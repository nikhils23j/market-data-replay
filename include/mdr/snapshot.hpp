#pragma once
// Snapshot file: FileHeader (16 B) then snapshots, each
//   SnapHeader (32 B): magic u32 | n_orders u32 | seq u64 | fnv1a(records) u64 | reserved u64
//   n_orders * Record (24 B): id u64 | price u32 | qty u32 | inst u16 | side u8 | pad(5)
// Records are written in per-level FIFO order, so re-adding them in file order
// reproduces queue priority exactly.

#include "mdr/book.hpp"
#include "mdr/wire.hpp"

#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace mdr {

struct SnapOrder {
  std::uint64_t id;
  std::uint32_t price;
  std::uint32_t qty;
  std::uint16_t inst;
  Side side;
};

inline void append_snapshot_file_header(std::vector<std::uint8_t>& out, const Session& s) {
  const auto at = out.size();
  out.resize(at + 16, 0);
  store_le<std::uint32_t>(out.data() + at, kSnapFileMagic);
  store_le<std::uint16_t>(out.data() + at + 4, kVersion);
  store_le<std::uint16_t>(out.data() + at + 6, s.instruments());
}

inline void append_snapshot(std::vector<std::uint8_t>& out, std::uint64_t seq,
                            const std::vector<SnapOrder>& orders) {
  const auto at = out.size();
  out.resize(at + kSnapHeaderSize + orders.size() * kSnapRecordSize, 0);
  auto* body = out.data() + at + kSnapHeaderSize;
  for (std::size_t i = 0; i < orders.size(); ++i) {
    auto* r = body + i * kSnapRecordSize;
    store_le<std::uint64_t>(r, orders[i].id);
    store_le<std::uint32_t>(r + 8, orders[i].price);
    store_le<std::uint32_t>(r + 12, orders[i].qty);
    store_le<std::uint16_t>(r + 16, orders[i].inst);
    r[18] = static_cast<std::uint8_t>(orders[i].side);
  }
  auto* h = out.data() + at;
  store_le<std::uint32_t>(h, kSnapMagic);
  store_le<std::uint32_t>(h + 4, static_cast<std::uint32_t>(orders.size()));
  store_le<std::uint64_t>(h + 8, seq);
  store_le<std::uint64_t>(h + 16, fnv1a(body, orders.size() * kSnapRecordSize));
}

class SnapshotStore {
 public:
  struct Entry {
    std::uint64_t seq;
    std::uint32_t n_orders;
    std::uint64_t checksum;
    const std::uint8_t* body;
  };

  SnapshotStore() = default;
  SnapshotStore(const std::uint8_t* data, std::size_t size, const Session& s) : session_(s) {
    if (size < 16 || load_le<std::uint32_t>(data) != kSnapFileMagic) throw std::runtime_error("snapshot file: bad magic");
    if (load_le<std::uint16_t>(data + 6) != s.instruments())
      throw std::runtime_error("snapshot file: instrument count does not match feed");
    std::size_t off = 16;
    while (off < size) {
      if (size - off < kSnapHeaderSize || load_le<std::uint32_t>(data + off) != kSnapMagic)
        throw std::runtime_error("snapshot file: bad snapshot header");
      Entry e{load_le<std::uint64_t>(data + off + 8), load_le<std::uint32_t>(data + off + 4),
              load_le<std::uint64_t>(data + off + 16), data + off + kSnapHeaderSize};
      const std::size_t body = std::size_t{e.n_orders} * kSnapRecordSize;
      if (size - off - kSnapHeaderSize < body) throw std::runtime_error("snapshot file: truncated");
      if (!entries_.empty() && e.seq <= entries_.back().seq) throw std::runtime_error("snapshot file: seq not increasing");
      entries_.push_back(e);
      off += kSnapHeaderSize + body;
    }
  }

  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  [[nodiscard]] const Entry& entry(std::size_t i) const { return entries_[i]; }

  // Checksum + per-record sanity + unique ids. Pure function of the bytes.
  [[nodiscard]] bool validate(std::size_t i) const {
    const auto& e = entries_[i];
    if (fnv1a(e.body, std::size_t{e.n_orders} * kSnapRecordSize) != e.checksum) return false;
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(e.n_orders);
    for (std::uint32_t k = 0; k < e.n_orders; ++k) {
      const auto o = record(e, k);
      if (o.id == 0 || o.qty == 0 || o.inst >= session_.instruments()) return false;
      if (o.side != Side::Bid && o.side != Side::Ask) return false;
      if (o.price - session_.base_tick[o.inst] >= kWindow) return false;
      if (!ids.insert(o.id).second) return false;
    }
    return true;
  }

  // Builds a fresh book from snapshot i and swaps it in only if every record
  // applied cleanly. Returns false (book untouched) otherwise.
  template <typename Book>
  bool restore(std::size_t i, Book& book) const {
    Book fresh(session_);
    const auto& e = entries_[i];
    for (std::uint32_t k = 0; k < e.n_orders; ++k) {
      const auto o = record(e, k);
      if (fresh.add(o.inst, o.id, o.side, o.price, o.qty) != Status::Ok) return false;
    }
    book = std::move(fresh);
    return true;
  }

  static SnapOrder record(const Entry& e, std::uint32_t k) {
    const auto* r = e.body + std::size_t{k} * kSnapRecordSize;
    return SnapOrder{load_le<std::uint64_t>(r), load_le<std::uint32_t>(r + 8), load_le<std::uint32_t>(r + 12),
                     load_le<std::uint16_t>(r + 16), static_cast<Side>(r[18])};
  }

 private:
  Session session_;
  std::vector<Entry> entries_;
};

}  // namespace mdr
