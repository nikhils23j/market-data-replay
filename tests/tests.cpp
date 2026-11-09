// Unit + integration checks. Plain asserts with a counter; no framework.
#include "mdr/arbiter.hpp"
#include "mdr/book.hpp"
#include "mdr/generator.hpp"
#include "mdr/metrics.hpp"
#include "mdr/pipeline.hpp"
#include "mdr/reference_book.hpp"
#include "mdr/snapshot.hpp"
#include "mdr/spsc.hpp"

#include <iostream>
#include <random>
#include <thread>

namespace {

int checks = 0;
void require(bool ok, const char* what) {
  ++checks;
  if (!ok) throw std::runtime_error(what);
}

const mdr::Session kS{{1000, 50000}};

mdr::Event ev(mdr::MsgType t, std::uint64_t id, std::uint32_t price = 0, std::uint32_t qty = 0,
              mdr::Side side = mdr::Side::None, std::uint16_t inst = 0) {
  mdr::Event e;
  e.type = t;
  e.order_id = id;
  e.price = price;
  e.qty = qty;
  e.side = side;
  e.inst = inst;
  return e;
}

void test_wire() {
  mdr::Event e = ev(mdr::MsgType::Add, 0x0102030405060708ULL, 0x0A0B0C0D, 300, mdr::Side::Bid, 1);
  std::uint8_t buf[32];
  mdr::encode_msg(buf, e, 0x1122334455667788ULL);
  require(buf[0] == 'A' && buf[1] == 'B' && buf[2] == 1 && buf[3] == 0, "header bytes little-endian");
  require(buf[8] == 0x08 && buf[15] == 0x01, "order id little-endian");
  require(buf[16] == 0x0D && buf[19] == 0x0A, "price little-endian");
  mdr::Event d;
  require(mdr::decode_msg(buf, 77, 2, d), "decode ok");
  require(d.seq == 77 && d.order_id == e.order_id && d.price == e.price && d.qty == 300 && d.inst == 1, "roundtrip");
  require(!mdr::decode_msg(buf, 77, 1, d), "instrument out of range rejected");
  buf[20] = 1;
  require(!mdr::decode_msg(buf, 77, 2, d), "nonzero reserved rejected");
  buf[20] = 0;
  buf[0] = 'Q';
  require(!mdr::decode_msg(buf, 77, 2, d), "unknown type rejected");
  buf[0] = 'D';
  require(!mdr::decode_msg(buf, 77, 2, d), "delete with a side byte rejected");
}

void test_order_map() {
  mdr::OrderMap m;
  std::mt19937_64 rng(5);
  std::vector<std::uint64_t> ids;
  for (std::uint32_t i = 0; i < 20000; ++i) {
    const auto id = 1 + (rng() % 1'000'000'000);
    if (m.find(id) != mdr::NIL) continue;
    m.insert(id, i);
    ids.push_back(id);
  }
  for (std::size_t i = 0; i < ids.size(); i += 2) m.erase(ids[i]);
  bool ok = true;
  for (std::size_t i = 0; i < ids.size(); ++i) ok &= (m.find(ids[i]) == mdr::NIL) == (i % 2 == 0);
  require(ok, "hash map erase keeps other keys reachable (backward shift)");
  require(m.size() == ids.size() / 2, "hash map size");
  require(m.find(0) == mdr::NIL, "id 0 never found");
}

template <typename Book>
void test_book_rules() {
  Book b(kS);
  using mdr::Side;
  using mdr::Status;
  require(b.add(0, 1, Side::Bid, 1100, 100) == Status::Ok, "add 1");
  require(b.add(0, 2, Side::Bid, 1100, 200) == Status::Ok, "add 2 same level");
  require(b.add(0, 3, Side::Bid, 1127, 50) == Status::Ok, "add bid tick 127 (end of word 1)");
  require(b.add(0, 4, Side::Bid, 1128, 70) == Status::Ok, "add bid tick 128 (start of word 2)");
  require(b.top(0, Side::Bid) == mdr::Top{1128, 70, 1}, "best bid across bitmap word boundary");
  require(b.add(0, 5, Side::Ask, 5095, 10) == Status::Ok && b.add(0, 6, Side::Ask, 1200, 20) == Status::Ok, "asks");
  require(b.top(0, Side::Ask) == mdr::Top{1200, 20, 1}, "best ask");
  require(b.add(0, 1, Side::Ask, 1300, 5) == Status::DuplicateOrder, "duplicate id");
  require(b.add(0, 9, Side::Ask, 999, 5) == Status::BadPrice, "below window");
  require(b.add(0, 9, Side::Ask, 5096, 5) == Status::BadPrice, "above window");
  require(b.add(0, 9, Side::Ask, 1300, 0) == Status::BadQty, "zero qty");
  require(b.add(2, 9, Side::Ask, 1300, 5) == Status::BadInstrument, "bad instrument");
  require(b.reduce(42, 1) == Status::UnknownOrder, "unknown order");
  require(b.reduce(1, 101) == Status::BadQty, "over-cancel");
  // FIFO priority at 1100 is 1 then 2. Execute front fully, then check order.
  require(b.reduce(1, 100) == Status::Ok, "full execute removes order");
  require(b.del(4) == Status::Ok && b.del(3) == Status::Ok, "delete top two levels");
  require(b.top(0, Side::Bid) == mdr::Top{1100, 200, 1}, "remaining level");
  require(b.add(0, 7, Side::Bid, 1100, 30) == Status::Ok, "join queue behind 2");
  require(b.modify(2, 1100, 150) == Status::Ok, "size down keeps priority");
  std::vector<std::uint64_t> fifo;
  b.for_each_order([&](const mdr::OrderNode& o) { if (o.price == 1100) fifo.push_back(o.id); });
  require(fifo == std::vector<std::uint64_t>{2, 7}, "priority kept after size-down");
  require(b.modify(2, 1100, 151) == Status::Ok, "size up loses priority");
  fifo.clear();
  b.for_each_order([&](const mdr::OrderNode& o) { if (o.price == 1100) fifo.push_back(o.id); });
  require(fifo == std::vector<std::uint64_t>{7, 2}, "size up goes to back");
  require(b.top(0, Side::Bid) == mdr::Top{1100, 181, 2}, "level qty after modifies");
  require(b.modify(7, 1101, 30) == Status::Ok && b.top(0, Side::Bid) == mdr::Top{1101, 30, 1}, "price move");
  require(b.modify(7, 999, 30) == Status::BadPrice, "modify out of window");
  require(b.live_orders() == 4, "live orders");
  mdr::RefBook r(kS);
  b.for_each_order([&](const mdr::OrderNode& o) { r.add(o.inst, o.id, o.side, o.price, o.qty); });
  require(r.digest() == b.digest(), "digest equals reference after rebuild");
}

std::vector<std::uint8_t> make_line(const mdr::Session& s, std::uint8_t line,
                                    const std::vector<std::pair<std::uint64_t, std::uint16_t>>& pkts,
                                    const std::vector<mdr::Event>& msgs) {
  std::vector<std::uint8_t> out;
  mdr::append_line_header(out, s, line);
  std::uint64_t t = 100;
  for (const auto& [first, count] : pkts) {
    const auto at = out.size();
    out.resize(at + mdr::kPacketHeaderSize + count * mdr::kMsgSize);
    mdr::encode_packet_header(out.data() + at, {first, t += 10, count, line});
    for (std::uint16_t i = 0; i < count; ++i)
      mdr::encode_msg(out.data() + at + mdr::kPacketHeaderSize + i * mdr::kMsgSize, msgs[first - 1 + i], 0);
  }
  return out;
}

void test_arbiter() {
  const mdr::Session s{{1000}};
  std::vector<mdr::Event> msgs;
  for (std::uint32_t i = 0; i < 6; ++i) msgs.push_back(ev(mdr::MsgType::Add, i + 1, 1100 + i, 10, mdr::Side::Bid));
  // Line A: 1-2, [3 lost], 4, 4 (dup), 5-6.  Line B: 3 arrives after A's 4 (buffered then healed).
  const auto A = make_line(s, 0, {{1, 2}, {4, 1}, {4, 1}, {5, 2}}, msgs);
  auto B = make_line(s, 1, {{3, 1}}, msgs);
  mdr::store_le<std::uint64_t>(B.data() + mdr::kLineHeaderSize + 4 + 8, 125);  // arrival between A's packets
  const mdr::LineView va(A.data(), A.size()), vb(B.data(), B.size());
  mdr::BitmapBook book(s);
  mdr::DirectSink<mdr::BitmapBook> sink{book};
  const auto st = mdr::Arbiter<decltype(sink)>(&va, &vb, nullptr, {}, sink).run();
  require(st.msgs_delivered == 6 && sink.seq_contiguous, "all six delivered in order");
  require(st.healed == 1 && st.buffered == 1, "packet 4 buffered then healed by line B");
  require(st.stale_or_duplicate == 1 && st.recoveries == 0, "duplicate dropped, no recovery");
  require(st.first_copy[1] == 1, "line B supplied one packet");

  // Both lines lose 3: recover from snapshot at seq 3, discarding nothing else.
  const auto A2 = make_line(s, 0, {{1, 2}, {4, 1}, {5, 2}}, msgs);
  std::vector<std::uint8_t> snap;
  mdr::append_snapshot_file_header(snap, s);
  mdr::append_snapshot(snap, 2, {{1, 1100, 10, 0, mdr::Side::Bid}});  // too old to bridge the gap
  std::vector<mdr::SnapOrder> three;
  for (std::uint32_t i = 0; i < 3; ++i) three.push_back({i + 1, 1100 + i, 10, 0, mdr::Side::Bid});
  mdr::append_snapshot(snap, 3, three);  // corrupted below
  mdr::append_snapshot(snap, 4, {{1, 1100, 10, 0, mdr::Side::Bid}, {2, 1101, 10, 0, mdr::Side::Bid},
                                 {3, 1102, 10, 0, mdr::Side::Bid}, {4, 1103, 10, 0, mdr::Side::Bid}});
  const auto second_snap_body = 16 + mdr::kSnapHeaderSize + mdr::kSnapRecordSize + mdr::kSnapHeaderSize;
  snap[second_snap_body + 12] ^= 0xFF;  // flip qty byte of seq-3 snapshot
  const mdr::SnapshotStore store(snap.data(), snap.size(), s);
  require(store.validate(0) && !store.validate(1) && store.validate(2), "checksum catches corruption");
  const mdr::LineView va2(A2.data(), A2.size());
  mdr::BitmapBook b2(s);
  mdr::DirectSink<mdr::BitmapBook> sink2{b2, &store};
  mdr::ArbConfig cfg;
  cfg.gap_timeout_ns = 0;
  const auto st2 = mdr::Arbiter<decltype(sink2)>(&va2, nullptr, &store, cfg, sink2).run();
  require(st2.recoveries == 1 && st2.snapshots_rejected == 1, "skipped bad snapshot, used next valid one");
  require(st2.msgs_covered_by_snapshot == 2 && st2.healed == 0 && st2.stale_or_duplicate == 1,
          "seq 3,4 come from the snapshot; buffered packet 4 is now stale");
  require(b2.top(0, mdr::Side::Bid) == mdr::Top{1105, 10, 1} && b2.live_orders() == 6, "book complete after recovery");

  // Without snapshots the gap is skipped and the book is missing an order.
  mdr::BitmapBook b3(s);
  mdr::DirectSink<mdr::BitmapBook> sink3{b3};
  cfg.recover = false;
  const auto st3 = mdr::Arbiter<decltype(sink3)>(&va2, nullptr, nullptr, cfg, sink3).run();
  require(st3.msgs_skipped == 1 && b3.live_orders() == 5, "no-snapshot mode loses data visibly");

  // Restore of a bad snapshot leaves the book untouched.
  std::vector<std::uint8_t> bad;
  mdr::append_snapshot_file_header(bad, s);
  mdr::append_snapshot(bad, 9, {{1, 1100, 10, 0, mdr::Side::Bid}, {1, 1101, 10, 0, mdr::Side::Bid}});
  const mdr::SnapshotStore bad_store(bad.data(), bad.size(), s);
  const auto before = b3.digest();
  require(!bad_store.validate(0) && !bad_store.restore(0, b3) && b3.digest() == before, "duplicate-id snapshot rejected atomically");
}

void test_truncated_line() {
  const mdr::Session s{{1000}};
  std::vector<mdr::Event> msgs{ev(mdr::MsgType::Add, 1, 1100, 10, mdr::Side::Bid)};
  auto A = make_line(s, 0, {{1, 1}}, msgs);
  A.pop_back();
  const mdr::LineView v(A.data(), A.size());
  std::size_t off = v.begin_offset();
  mdr::Packet p;
  bool threw = false;
  try { v.next(off, p); } catch (const std::exception&) { threw = true; }
  require(threw, "truncated packet detected");
}

void test_spsc() {
  mdr::SpscRing<std::uint64_t> ring(8);
  constexpr std::uint64_t N = 500'000;
  std::uint64_t bad = 0;
  std::thread consumer([&] {
    std::uint64_t expect = 0, v;
    while (expect < N) {
      if (ring.try_pop(v)) {
        if (v != expect) ++bad;
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < N;)
    if (ring.try_push(i)) ++i;
  consumer.join();
  require(bad == 0, "SPSC ring preserves order across threads");
}

void test_histogram() {
  mdr::LogHistogram h;
  for (std::uint64_t v = 1; v <= 1000; ++v) h.record(v);
  require(h.max() == 1000 && h.count() == 1000, "histogram count/max");
  const auto p50 = h.percentile(0.5);
  require(p50 >= 500 && p50 <= 563, "p50 within one bucket (12.5%)");
  require(h.percentile(1.0) == 1000, "p100 is max");
}

void test_end_to_end() {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    mdr::GenParams p;
    p.seed = seed;
    p.messages = 60'000;
    p.instruments = 3;
    p.loss_a = 0.02;
    p.loss_b = 0.02;
    p.outage_every = 2000;
    p.outage_len = 50;
    p.snapshot_every = 5000;
    p.corrupt_snapshot = 2;
    const auto g = mdr::generate(p);
    const mdr::LineView a(g.line_a.data(), g.line_a.size()), b(g.line_b.data(), g.line_b.size());
    const mdr::SnapshotStore snaps(g.snapshots.data(), g.snapshots.size(), g.session);
    mdr::VectorBook book(g.session);
    mdr::DirectSink<mdr::VectorBook> sink{book, &snaps};
    const auto st = mdr::Arbiter<decltype(sink)>(&a, &b, &snaps, {}, sink).run();
    require(g.stats.lost_both > 0 && st.recoveries > 0, "workload actually needed recovery");
    require(book.digest() == g.truth_digest && sink.apply_errors == 0, "1-thread replay matches truth");
    const auto r = mdr::run_pipeline<mdr::BitmapBook>(&a, &b, &snaps, {});
    require(r.digest == g.truth_digest && r.apply_errors == 0, "2-thread pipeline matches truth");
    mdr::BitmapBook only_a(g.session);
    mdr::DirectSink<mdr::BitmapBook> sa{only_a, &snaps};
    const auto sta = mdr::Arbiter<decltype(sa)>(&a, nullptr, &snaps, {}, sa).run();
    require(sta.recoveries > st.recoveries && only_a.digest() == g.truth_digest, "single line needs more recoveries");
  }
}

}  // namespace

int main() {
  try {
    test_wire();
    test_order_map();
    test_book_rules<mdr::BitmapBook>();
    test_book_rules<mdr::MapBook>();
    test_book_rules<mdr::VectorBook>();
    test_arbiter();
    test_truncated_line();
    test_spsc();
    test_histogram();
    test_end_to_end();
    std::cout << "passed " << checks << " checks\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAILED after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
