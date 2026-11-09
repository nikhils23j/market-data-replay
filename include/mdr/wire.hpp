#pragma once
// Binary feed format. Everything on disk is little-endian and fixed-size.
//
// Line file:   LineHeader (16 B) + n_instruments * u32 base_tick + packets
// Packet:      PacketHeader (24 B) + count * Message (32 B)
// Message:     off 0 type u8 | 1 side u8 | 2 instrument u16 | 4 qty u32
//              off 8 order_id u64 | 16 price u32 | 20 reserved u32 (must be 0)
//              off 24 exchange timestamp ns u64
// Messages carry no sequence number; message i of a packet has seq first_seq + i
// (the same idea as MoldUDP64 framing). A packet with count == 0 is a heartbeat
// that announces the next sequence number.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdr {

inline constexpr std::uint32_t kWindow = 4096;  // price ticks per instrument
inline constexpr std::size_t kMsgSize = 32;
inline constexpr std::size_t kPacketHeaderSize = 24;
inline constexpr std::size_t kLineHeaderSize = 16;
inline constexpr std::uint32_t kLineMagic = 0x4C52444D;  // "MDRL"
inline constexpr std::uint32_t kSnapFileMagic = 0x5352444D;  // "MDRS"
inline constexpr std::uint32_t kSnapMagic = 0x50414E53;  // "SNAP"
inline constexpr std::size_t kSnapHeaderSize = 32;
inline constexpr std::size_t kSnapRecordSize = 24;
inline constexpr std::uint16_t kVersion = 1;

enum class MsgType : std::uint8_t {
  Add = 'A',
  Cancel = 'X',   // partial cancel: reduce qty, keep priority
  Delete = 'D',
  Execute = 'E',  // trade against a resting order
  Modify = 'M',   // replace price/qty
  // Internal control events (never on the wire).
  Recover = 'R',
  Skip = 'K',
  End = 'Z',
};

enum class Side : std::uint8_t { None = 0, Bid = 'B', Ask = 'S' };

// Decoded message as it travels through the pipeline. 32 bytes: four per
// 128-byte Apple M2 cache line.
struct Event {
  std::uint64_t seq = 0;
  std::uint64_t order_id = 0;
  std::uint32_t price = 0;
  std::uint32_t qty = 0;
  std::uint16_t inst = 0;
  MsgType type = MsgType::Add;
  Side side = Side::None;
  std::uint32_t stamp = 0;  // low 32 bits of the cycle counter (latency runs)
};
static_assert(sizeof(Event) == 32);

template <typename T>
inline T load_le(const std::uint8_t* p) {
  T v = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(p[i]) << (8 * i));
  return v;
}

template <typename T>
inline void store_le(std::uint8_t* p, T v) {
  for (std::size_t i = 0; i < sizeof(T); ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline void encode_msg(std::uint8_t* p, const Event& e, std::uint64_t ts_ns) {
  p[0] = static_cast<std::uint8_t>(e.type);
  p[1] = static_cast<std::uint8_t>(e.side);
  store_le<std::uint16_t>(p + 2, e.inst);
  store_le<std::uint32_t>(p + 4, e.qty);
  store_le<std::uint64_t>(p + 8, e.order_id);
  store_le<std::uint32_t>(p + 16, e.price);
  store_le<std::uint32_t>(p + 20, 0);
  store_le<std::uint64_t>(p + 24, ts_ns);
}

// Returns false for malformed messages (unknown type, bad side, bad instrument,
// nonzero reserved field).
inline bool decode_msg(const std::uint8_t* p, std::uint64_t seq, std::uint16_t n_instruments,
                       Event& e) {
  const auto type = static_cast<MsgType>(p[0]);
  const auto side = static_cast<Side>(p[1]);
  switch (type) {
    case MsgType::Add:
      if (side != Side::Bid && side != Side::Ask) return false;
      break;
    case MsgType::Cancel:
    case MsgType::Delete:
    case MsgType::Execute:
    case MsgType::Modify:
      if (side != Side::None) return false;
      break;
    default:
      return false;
  }
  if (load_le<std::uint32_t>(p + 20) != 0) return false;
  e.inst = load_le<std::uint16_t>(p + 2);
  if (e.inst >= n_instruments) return false;
  e.seq = seq;
  e.type = type;
  e.side = side;
  e.qty = load_le<std::uint32_t>(p + 4);
  e.order_id = load_le<std::uint64_t>(p + 8);
  e.price = load_le<std::uint32_t>(p + 16);
  e.stamp = 0;
  return true;
}

struct Session {
  std::vector<std::uint32_t> base_tick;  // window is [base, base + kWindow)
  [[nodiscard]] std::uint16_t instruments() const {
    return static_cast<std::uint16_t>(base_tick.size());
  }
  bool operator==(const Session&) const = default;
};

struct PacketHeader {
  std::uint64_t first_seq = 0;
  std::uint64_t arrival_ns = 0;
  std::uint16_t count = 0;
  std::uint8_t line = 0;
};

inline void encode_packet_header(std::uint8_t* p, const PacketHeader& h) {
  store_le<std::uint64_t>(p, h.first_seq);
  store_le<std::uint64_t>(p + 8, h.arrival_ns);
  store_le<std::uint16_t>(p + 16, h.count);
  p[18] = h.line;
  p[19] = 0;
  store_le<std::uint32_t>(p + 20, 0);
}

inline void append_line_header(std::vector<std::uint8_t>& out, const Session& s, std::uint8_t line) {
  const auto at = out.size();
  out.resize(at + kLineHeaderSize + 4 * s.base_tick.size());
  auto* p = out.data() + at;
  store_le<std::uint32_t>(p, kLineMagic);
  store_le<std::uint16_t>(p + 4, kVersion);
  store_le<std::uint16_t>(p + 6, s.instruments());
  p[8] = line;
  for (int i = 9; i < 12; ++i) p[i] = 0;
  store_le<std::uint32_t>(p + 12, kWindow);
  for (std::size_t i = 0; i < s.base_tick.size(); ++i) store_le<std::uint32_t>(p + 16 + 4 * i, s.base_tick[i]);
}

struct Packet {
  PacketHeader h;
  const std::uint8_t* msgs = nullptr;
};

// Read-only, bounds-checked view over a line file (usually mmap'd).
class LineView {
 public:
  LineView() = default;
  LineView(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {
    if (size < kLineHeaderSize || load_le<std::uint32_t>(data) != kLineMagic)
      throw std::runtime_error("line file: bad magic");
    if (load_le<std::uint16_t>(data + 4) != kVersion) throw std::runtime_error("line file: bad version");
    const auto n = load_le<std::uint16_t>(data + 6);
    line_ = data[8];
    if (n == 0 || load_le<std::uint32_t>(data + 12) != kWindow)
      throw std::runtime_error("line file: bad instrument count/window");
    if (size < kLineHeaderSize + 4u * n) throw std::runtime_error("line file: truncated header");
    for (std::uint16_t i = 0; i < n; ++i)
      session_.base_tick.push_back(load_le<std::uint32_t>(data + kLineHeaderSize + 4u * i));
    first_packet_ = kLineHeaderSize + 4u * n;
  }

  [[nodiscard]] const Session& session() const { return session_; }
  [[nodiscard]] std::uint8_t line() const { return line_; }
  [[nodiscard]] std::size_t begin_offset() const { return first_packet_; }
  [[nodiscard]] std::size_t size() const { return size_; }

  // Reads the packet at `offset`; advances offset. Returns false at end of file.
  bool next(std::size_t& offset, Packet& out) const {
    if (offset == size_) return false;
    if (size_ - offset < kPacketHeaderSize) throw std::runtime_error("line file: truncated packet header");
    const auto* p = data_ + offset;
    out.h.first_seq = load_le<std::uint64_t>(p);
    out.h.arrival_ns = load_le<std::uint64_t>(p + 8);
    out.h.count = load_le<std::uint16_t>(p + 16);
    out.h.line = p[18];
    const std::size_t body = std::size_t{out.h.count} * kMsgSize;
    if (out.h.first_seq == 0) throw std::runtime_error("line file: packet seq 0");
    if (size_ - offset - kPacketHeaderSize < body) throw std::runtime_error("line file: truncated packet body");
    out.msgs = p + kPacketHeaderSize;
    offset += kPacketHeaderSize + body;
    return true;
  }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t first_packet_ = 0;
  std::uint8_t line_ = 0;
  Session session_;
};

// 64-bit FNV-1a over bytes (snapshot checksums).
inline std::uint64_t fnv1a(const std::uint8_t* p, std::size_t n,
                           std::uint64_t h = 1469598103934665603ULL) {
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

// Order-sensitive digest of a whole book (word-at-a-time mixing; not crypto).
struct Digest {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  void add(std::uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
  }
};

}  // namespace mdr
