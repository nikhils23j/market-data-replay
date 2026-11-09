#include "mdr/generator.hpp"

#include "mdr/reference_book.hpp"
#include "mdr/snapshot.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_map>

namespace mdr {
namespace {

struct Rng {
  std::mt19937_64 g;
  explicit Rng(std::uint64_t seed) : g(seed) {}
  double u01() { return static_cast<double>(g() >> 11) * 0x1.0p-53; }
  std::uint64_t below(std::uint64_t n) { return g() % n; }  // tiny bias is fine here
  std::uint64_t operator()() { return g(); }
};

struct PacketPlan {
  std::uint64_t first_seq;
  std::uint32_t count;
  std::uint64_t send_ns;
};

void append_packet(std::vector<std::uint8_t>& out, const PacketHeader& h, const std::uint8_t* msgs) {
  const auto at = out.size();
  out.resize(at + kPacketHeaderSize + std::size_t{h.count} * kMsgSize);
  encode_packet_header(out.data() + at, h);
  if (h.count) std::copy_n(msgs, std::size_t{h.count} * kMsgSize, out.data() + at + kPacketHeaderSize);
}

}  // namespace

GenOutput generate(const GenParams& p) {
  if (p.messages == 0 || p.instruments == 0 || p.max_per_packet == 0 || p.max_per_packet > 65535 ||
      p.snapshot_every == 0)
    throw std::invalid_argument("generator: messages, instruments, max_per_packet, snapshot_every must be positive");
  GenOutput out;
  Rng rng(p.seed);
  for (std::uint16_t i = 0; i < p.instruments; ++i) out.session.base_tick.push_back(100'000u + 10'000u * i);
  const auto& S = out.session;
  RefBook ref(S);

  // Busier symbols first: weight 1/(i+1).
  std::vector<double> w;
  for (std::uint16_t i = 0; i < p.instruments; ++i) w.push_back(1.0 / (i + 1));
  std::discrete_distribution<int> pick_inst(w.begin(), w.end());

  std::vector<std::int64_t> fair(p.instruments);
  for (std::uint16_t i = 0; i < p.instruments; ++i) fair[i] = S.base_tick[i] + kWindow / 2;
  std::vector<std::vector<std::uint64_t>> live(p.instruments);
  std::unordered_map<std::uint64_t, std::uint32_t> live_pos;
  live_pos.reserve(p.target_orders * p.instruments * 2u);
  std::uint64_t next_id = 1;

  auto forget = [&](std::uint64_t id, std::uint16_t inst) {
    auto it = live_pos.find(id);
    auto& v = live[inst];
    const auto idx = it->second;
    v[idx] = v.back();
    live_pos[v[idx]] = idx;
    v.pop_back();
    live_pos.erase(id);
  };
  auto clamp_price = [&](std::int64_t px, std::uint16_t inst) {
    const std::int64_t lo = S.base_tick[inst], hi = lo + kWindow - 1;
    return static_cast<std::uint32_t>(std::clamp(px, lo, hi));
  };

  std::vector<std::uint8_t> canon(p.messages * kMsgSize);
  std::vector<PacketPlan> packets;
  packets.reserve(p.messages / 2 + 1);
  out.snapshots.reserve(1 << 20);
  append_snapshot_file_header(out.snapshots, S);

  std::uint64_t send_ns = 1'000'000;
  std::uint32_t left_in_packet = 0;
  double live_sum = 0, levels_sum = 0;
  std::uint64_t samples = 0;
  std::exponential_distribution<double> gap_dist(1.0);

  for (std::uint64_t seq = 1; seq <= p.messages; ++seq) {
    if (left_in_packet == 0) {
      const auto remaining = p.messages - seq + 1;
      left_in_packet = static_cast<std::uint32_t>(std::min<std::uint64_t>(1 + rng.below(p.max_per_packet), remaining));
      send_ns += static_cast<std::uint64_t>(gap_dist(rng.g) * p.mean_ns_per_msg * left_in_packet) + 1;
      packets.push_back({seq, left_in_packet, send_ns});
    }
    --left_in_packet;

    const auto inst = static_cast<std::uint16_t>(pick_inst(rng.g));
    if (rng.u01() < 0.1) {
      fair[inst] += (rng() & 1) ? 1 : -1;
      const std::int64_t lo = S.base_tick[inst] + 1000, hi = S.base_tick[inst] + kWindow - 1000;
      fair[inst] = std::clamp(fair[inst], lo, hi);
    }
    auto& lv = live[inst];
    Event e;
    e.seq = seq;
    e.inst = inst;
    const double p_add = lv.size() < p.target_orders ? 0.5 : 0.3;
    const double r = lv.size() < 10 ? 0.0 : rng.u01();
    const double rest = (r - p_add) / (1.0 - p_add);  // position inside the non-add mass
    if (r < p_add) {
      e.type = MsgType::Add;
      e.side = (rng() & 1) ? Side::Bid : Side::Ask;
      std::int64_t off = 0;
      if (rng.u01() < 0.03) off = 20 + static_cast<std::int64_t>(rng.below(200));
      else while (off < 60 && rng.u01() < 0.7) ++off;
      std::int64_t px;
      if (e.side == Side::Bid) {
        px = fair[inst] - 1 - off;
        const auto ask = ref.top(inst, Side::Ask);
        if (ask.count) px = std::min<std::int64_t>(px, static_cast<std::int64_t>(ask.price) - 1);
      } else {
        px = fair[inst] + 1 + off;
        const auto bid = ref.top(inst, Side::Bid);
        if (bid.count) px = std::max<std::int64_t>(px, static_cast<std::int64_t>(bid.price) + 1);
      }
      e.price = clamp_price(px, inst);
      e.qty = static_cast<std::uint32_t>((1 + rng.below(10)) * 100);
      e.order_id = next_id++;
      ++out.stats.adds;
    } else if (rest < 0.42) {
      e.type = MsgType::Delete;
      e.order_id = lv[rng.below(lv.size())];
      ++out.stats.deletes;
    } else if (rest < 0.59) {
      e.type = MsgType::Cancel;
      e.order_id = lv[rng.below(lv.size())];
      const auto q = ref.find(e.order_id)->qty;
      e.qty = q > 1 ? static_cast<std::uint32_t>(1 + rng.below(q - 1)) : q;
      ++out.stats.cancels;
    } else if (rest < 0.79) {
      e.type = MsgType::Execute;
      Side s = (rng() & 1) ? Side::Bid : Side::Ask;
      if (ref.top(inst, s).count == 0) s = (s == Side::Bid) ? Side::Ask : Side::Bid;
      e.order_id = ref.front_order(inst, s);
      const auto q = ref.find(e.order_id)->qty;
      e.qty = (q == 1 || rng.u01() < 0.5) ? q : static_cast<std::uint32_t>(1 + rng.below(q - 1));
      ++out.stats.executes;
    } else {
      e.type = MsgType::Modify;
      e.order_id = lv[rng.below(lv.size())];
      const auto* o = ref.find(e.order_id);
      const double m = rng.u01();
      e.price = o->price;
      e.qty = o->qty;
      if (m < 0.5 && o->qty > 1) {
        e.qty = static_cast<std::uint32_t>(1 + rng.below(o->qty - 1));  // keeps priority
      } else if (m < 0.8) {
        const auto step = static_cast<std::int64_t>(1 + rng.below(3));
        e.price = clamp_price(static_cast<std::int64_t>(o->price) + ((rng() & 1) ? step : -step), inst);
      } else {
        e.qty = o->qty + 100;  // size up: loses priority
      }
      ++out.stats.modifies;
    }

    std::uint16_t order_inst = inst;
    if (e.type != MsgType::Add) order_inst = ref.find(e.order_id)->inst;
    if (ref.apply(e) != Status::Ok) throw std::logic_error("generator produced an invalid event");
    if (e.type == MsgType::Add) {
      live_pos[e.order_id] = static_cast<std::uint32_t>(live[inst].size());
      live[inst].push_back(e.order_id);
    } else if (!ref.find(e.order_id)) {
      forget(e.order_id, order_inst);
    }
    encode_msg(canon.data() + (seq - 1) * kMsgSize, e, packets.back().send_ns);

    if (seq % 997 == 0) {
      ++samples;
      live_sum += static_cast<double>(ref.live_orders());
      for (std::uint16_t i = 0; i < p.instruments; ++i)
        for (Side s : {Side::Bid, Side::Ask}) {
          const auto n = ref.level_count(i, s);
          levels_sum += static_cast<double>(n);
          out.stats.max_levels_per_side = std::max<std::uint64_t>(out.stats.max_levels_per_side, n);
        }
    }
    if (seq % p.snapshot_every == 0 || seq == p.messages) {
      std::vector<SnapOrder> orders;
      orders.reserve(ref.live_orders());
      for (std::uint16_t i = 0; i < p.instruments; ++i)
        for (Side s : {Side::Bid, Side::Ask})
          ref.visit_levels(i, s, [&](std::uint32_t price, const std::vector<std::uint64_t>& ids) {
            for (auto id : ids) orders.push_back({id, price, ref.find(id)->qty, i, s});
          });
      const auto at = out.snapshots.size();
      append_snapshot(out.snapshots, seq, orders);
      if (static_cast<int>(out.stats.snapshots) == p.corrupt_snapshot && !orders.empty())
        out.snapshots[at + kSnapHeaderSize + 8] ^= 0x01;  // flip a price bit; checksum now fails
      ++out.stats.snapshots;
    }
  }
  out.messages = p.messages;
  out.truth_digest = ref.digest();
  out.stats.packets = packets.size();
  if (samples) {
    out.stats.avg_live_orders = live_sum / static_cast<double>(samples);
    out.stats.avg_levels_per_side = levels_sum / static_cast<double>(samples * p.instruments * 2u);
  }

  // ---- Derive the captures ------------------------------------------------
  const std::uint64_t hb_seq = p.messages + 1;
  const std::uint64_t hb_ns = send_ns + 1'000'000;
  const std::size_t approx = p.messages * kMsgSize + packets.size() * kPacketHeaderSize + 4096;

  out.clean.reserve(approx);
  append_line_header(out.clean, S, 2);
  for (const auto& pk : packets)
    append_packet(out.clean, {pk.first_seq, pk.send_ns, static_cast<std::uint16_t>(pk.count), 2},
                  canon.data() + (pk.first_seq - 1) * kMsgSize);
  append_packet(out.clean, {hb_seq, hb_ns, 0, 2}, nullptr);

  std::vector<std::uint8_t> lost(packets.size(), 0);
  for (int line = 0; line < 2; ++line) {
    struct Copy {
      std::uint64_t arrival;
      std::size_t pkt;
    };
    std::vector<Copy> copies;
    copies.reserve(packets.size() + packets.size() / 100);
    const double loss = line == 0 ? p.loss_a : p.loss_b;
    const std::uint64_t lag = line == 0 ? 0 : p.line_b_lag_ns;
    for (std::size_t i = 0; i < packets.size(); ++i) {
      bool drop = rng.u01() < loss;
      if (p.outage_every > 0) {
        const auto k = (i + p.outage_every / 2) / p.outage_every;  // outage number
        const auto start = k * p.outage_every - p.outage_every / 2;
        const int outage_line = (k % 2 == 1) ? 1 : 0;  // first outage hits B
        if (k > 0 && outage_line == line && i >= start && i < start + p.outage_len) drop = true;
      }
      if (drop) {
        (line == 0 ? out.stats.lost_a : out.stats.lost_b)++;
        if (++lost[i] == 2) ++out.stats.lost_both;
        continue;
      }
      std::uint64_t arrival = packets[i].send_ns + 5000 + lag + rng.below(p.jitter_ns + 1);
      if (rng.u01() < p.reorder) {
        arrival += 3000 + rng.below(27'000);
        (line == 0 ? out.stats.late_a : out.stats.late_b)++;
      }
      copies.push_back({arrival, i});
      if (rng.u01() < p.dup) {
        copies.push_back({arrival + 500 + rng.below(20'000), i});
        (line == 0 ? out.stats.dup_a : out.stats.dup_b)++;
      }
    }
    std::stable_sort(copies.begin(), copies.end(), [](const Copy& x, const Copy& y) { return x.arrival < y.arrival; });
    auto& buf = line == 0 ? out.line_a : out.line_b;
    buf.reserve(approx);
    append_line_header(buf, S, static_cast<std::uint8_t>(line));
    for (const auto& c : copies) {
      const auto& pk = packets[c.pkt];
      append_packet(buf, {pk.first_seq, c.arrival, static_cast<std::uint16_t>(pk.count), static_cast<std::uint8_t>(line)},
                    canon.data() + (pk.first_seq - 1) * kMsgSize);
    }
    append_packet(buf, {hb_seq, hb_ns + lag, 0, static_cast<std::uint8_t>(line)}, nullptr);
  }
  return out;
}

}  // namespace mdr
