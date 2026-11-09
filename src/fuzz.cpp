// mdr_fuzz: seeded differential fuzzer (Apple clang ships no libFuzzer).
//
// Part 1 (books): random add/cancel/delete/execute/modify streams, ~10% of
//   them invalid on purpose, applied to RefBook and all three OrderBook
//   variants. Every event: status codes must match and top-of-book for the
//   touched instrument must match. Every --check-every events: full digest
//   (every level, every order in FIFO order) and order counts must match, plus
//   a snapshot write -> validate -> restore round trip.
// Part 2 (sessions): small generated sessions with random loss / duplicates /
//   reordering / outages / gap timeouts; replay through the arbiter (and the
//   threaded pipeline) and require final digest == truth and zero apply errors.
#include "cli.hpp"
#include "mdr/arbiter.hpp"
#include "mdr/book.hpp"
#include "mdr/generator.hpp"
#include "mdr/pipeline.hpp"
#include "mdr/reference_book.hpp"
#include "mdr/snapshot.hpp"

#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_map>

namespace {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::uint64_t seed, std::uint64_t step, const std::string& what) {
  std::ostringstream os;
  os << "seed " << seed << " step " << step << ": " << what;
  throw Failure(os.str());
}

struct Counters {
  std::uint64_t events = 0, invalid = 0, digest_checks = 0, snapshot_roundtrips = 0;
};

template <typename Book>
void check_snapshot_roundtrip(const Book& b, const mdr::RefBook& ref, std::uint64_t seed, std::uint64_t step) {
  std::vector<mdr::SnapOrder> orders;
  b.for_each_order([&](const mdr::OrderNode& o) { orders.push_back({o.id, o.price, o.qty, o.inst, o.side}); });
  std::vector<std::uint8_t> buf;
  mdr::append_snapshot_file_header(buf, ref.session());
  mdr::append_snapshot(buf, step + 1, orders);
  const mdr::SnapshotStore store(buf.data(), buf.size(), ref.session());
  if (!store.validate(0)) fail(seed, step, "fresh snapshot failed validation");
  mdr::BitmapBook r1(ref.session());
  mdr::MapBook r2(ref.session());
  mdr::VectorBook r3(ref.session());
  if (!store.restore(0, r1) || !store.restore(0, r2) || !store.restore(0, r3)) fail(seed, step, "restore failed");
  const auto d = ref.digest();
  if (r1.digest() != d || r2.digest() != d || r3.digest() != d) fail(seed, step, "snapshot round trip digest mismatch");
}

void fuzz_books(std::uint64_t seed, std::uint64_t events, std::uint64_t check_every, Counters& c) {
  std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ULL + 12345);
  auto below = [&](std::uint64_t n) { return rng() % n; };
  auto u01 = [&] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };

  mdr::Session s;
  const auto n_inst = 1 + below(3);
  for (std::uint64_t i = 0; i < n_inst; ++i)
    s.base_tick.push_back(seed % 5 == 0 && i == 0 ? 0u : static_cast<std::uint32_t>(1 + below(1'000'000)));
  // Each seed concentrates on a different zone/width so we hit dense books,
  // sparse books, and bitmap word boundaries.
  const std::int64_t widths[] = {1, 3, 64, 130, 700, 4096};
  const std::int64_t width = widths[below(6)];
  const auto center = static_cast<std::int64_t>(below(mdr::kWindow));

  mdr::RefBook ref(s);
  mdr::BitmapBook b1(s);
  mdr::MapBook b2(s);
  mdr::VectorBook b3(s);
  std::vector<std::uint64_t> live;
  std::unordered_map<std::uint64_t, std::size_t> pos;
  std::uint64_t next_id = 1;

  auto price_for = [&](std::uint16_t inst) -> std::uint32_t {
    const std::int64_t base = s.base_tick[inst];
    const double r = u01();
    std::int64_t off;
    if (r < 0.02) {
      const std::int64_t bad[] = {-1, mdr::kWindow, mdr::kWindow + 1, 1'000'000'000};
      off = bad[below(4)];
    } else if (r < 0.07) {
      const std::int64_t edge[] = {0, 1, 63, 64, 65, 127, 128, 4031, 4032, 4094, 4095};
      off = edge[below(11)];
    } else {
      off = std::clamp<std::int64_t>(center - width / 2 + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(width))), 0,
                                     mdr::kWindow - 1);
    }
    const auto px = base + off;
    return px < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(px);
  };
  auto pick_id = [&]() -> std::uint64_t {
    if (live.empty() || u01() < 0.08) {
      const std::uint64_t junk[] = {0, next_id, next_id + 1000, 1 + below(next_id)};
      return junk[below(4)];
    }
    return live[below(live.size())];
  };
  auto track = [&](std::uint64_t id) {
    const bool alive = ref.find(id) != nullptr;
    auto it = pos.find(id);
    if (alive && it == pos.end()) {
      pos[id] = live.size();
      live.push_back(id);
    } else if (!alive && it != pos.end()) {
      const auto i = it->second;
      live[i] = live.back();
      pos[live[i]] = i;
      live.pop_back();
      pos.erase(id);
    }
  };

  for (std::uint64_t step = 0; step < events; ++step) {
    mdr::Event e;
    e.seq = step + 1;
    e.inst = static_cast<std::uint16_t>(below(n_inst));
    const double r = live.size() < 5 ? 0.0 : u01();
    if (r < 0.40) {
      e.type = mdr::MsgType::Add;
      e.side = (rng() & 1) ? mdr::Side::Bid : mdr::Side::Ask;
      e.order_id = u01() < 0.03 ? pick_id() : next_id++;
      e.price = price_for(e.inst);
      e.qty = u01() < 0.03 ? 0 : static_cast<std::uint32_t>(1 + below(1000));
      if (u01() < 0.01) e.inst = static_cast<std::uint16_t>(n_inst + below(3));  // bad instrument
    } else if (r < 0.95) {
      const double k = u01();
      e.order_id = pick_id();
      const auto* o = ref.find(e.order_id);
      const std::uint32_t cur = o ? o->qty : 100;
      if (k < 0.25) {
        e.type = mdr::MsgType::Cancel;
        e.qty = static_cast<std::uint32_t>(below(cur + 2));  // 0 and cur+1 are invalid
      } else if (k < 0.5) {
        e.type = mdr::MsgType::Execute;
        e.qty = static_cast<std::uint32_t>(below(cur + 2));
      } else if (k < 0.72) {
        e.type = mdr::MsgType::Delete;
      } else {
        e.type = mdr::MsgType::Modify;
        const auto inst = o ? o->inst : e.inst;
        const double m = u01();
        e.price = (o && m < 0.5) ? o->price : price_for(inst);
        e.qty = u01() < 0.02 ? 0 : (o && m < 0.3 ? static_cast<std::uint32_t>(1 + below(cur)) : static_cast<std::uint32_t>(1 + below(1000)));
      }
    } else {
      e.type = (rng() & 1) ? mdr::MsgType::Recover : static_cast<mdr::MsgType>('?');  // not a book op
      e.order_id = pick_id();
    }

    const auto want = ref.apply(e);
    const auto g1 = b1.apply(e), g2 = b2.apply(e), g3 = b3.apply(e);
    if (g1 != want || g2 != want || g3 != want) {
      fail(seed, step, std::string("status mismatch: ref=") + mdr::status_name(want) + " bitmap=" + mdr::status_name(g1) +
                           " map=" + mdr::status_name(g2) + " vector=" + mdr::status_name(g3));
    }
    if (want != mdr::Status::Ok) ++c.invalid;
    track(e.order_id);
    if (e.inst < n_inst) {
      for (auto side : {mdr::Side::Bid, mdr::Side::Ask}) {
        const auto t = ref.top(e.inst, side);
        if (b1.top(e.inst, side) != t || b2.top(e.inst, side) != t || b3.top(e.inst, side) != t)
          fail(seed, step, "top-of-book mismatch");
      }
    }
    if ((step + 1) % check_every == 0 || step + 1 == events) {
      const auto d = ref.digest();
      if (b1.digest() != d || b2.digest() != d || b3.digest() != d) fail(seed, step, "digest mismatch");
      if (b1.live_orders() != ref.live_orders() || b2.live_orders() != ref.live_orders() ||
          b3.live_orders() != ref.live_orders())
        fail(seed, step, "live order count mismatch");
      ++c.digest_checks;
      if (((step + 1) / check_every) % 10 == 0 || step + 1 == events) {
        check_snapshot_roundtrip(b1, ref, seed, step);
        ++c.snapshot_roundtrips;
      }
    }
    ++c.events;
  }
}

template <typename Book>
void replay_session(const mdr::GenOutput& g, const mdr::ArbConfig& cfg, std::uint64_t seed, bool threaded) {
  const mdr::LineView a(g.line_a.data(), g.line_a.size());
  const mdr::LineView b(g.line_b.data(), g.line_b.size());
  const mdr::SnapshotStore snaps(g.snapshots.data(), g.snapshots.size(), g.session);
  if (threaded) {
    mdr::PipelineConfig pc;
    pc.arb = cfg;
    pc.ring_capacity = 64;  // tiny ring: forces lots of full/empty transitions
    const auto r = mdr::run_pipeline<Book>(&a, &b, &snaps, pc);
    if (r.digest != g.truth_digest) fail(seed, 0, std::string("pipeline digest mismatch, book=") + Book::kName);
    if (r.apply_errors) fail(seed, 0, "pipeline apply errors");
    return;
  }
  Book book(g.session);
  mdr::DirectSink<Book> sink{book, &snaps};
  mdr::Arbiter<mdr::DirectSink<Book>> arb(&a, &b, &snaps, cfg, sink);
  const auto st = arb.run();
  if (book.digest() != g.truth_digest) fail(seed, 0, std::string("session digest mismatch, book=") + Book::kName);
  if (sink.apply_errors) fail(seed, 0, "apply errors after arbitration");
  if (!sink.seq_contiguous) fail(seed, 0, "delivered sequence numbers not contiguous");
  if (st.decode_errors || st.unrecoverable_gaps) fail(seed, 0, "decode errors or unrecoverable gaps");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const mdr::Args args(argc, argv);
    const auto seeds = args.u64("seeds", 8);
    const auto first_seed = args.u64("first-seed", 1);
    const auto events = args.u64("events", 500'000);
    const auto check_every = args.u64("check-every", 1000);
    const auto sessions = args.u64("sessions", 40);
    const auto session_msgs = args.u64("session-messages", 20'000);
    args.reject_unknown();

    const auto t0 = std::chrono::steady_clock::now();
    Counters c;
    for (auto seed = first_seed; seed < first_seed + seeds; ++seed) fuzz_books(seed, events, check_every, c);
    const auto t1 = std::chrono::steady_clock::now();
    std::cout << "book_fuzz: seeds=" << first_seed << ".." << first_seed + seeds - 1 << " events=" << c.events
              << " invalid_events=" << c.invalid << " digest_checks=" << c.digest_checks
              << " snapshot_roundtrips=" << c.snapshot_roundtrips << " books=ref,bitmap,std_map,sorted_vector"
              << " mismatches=0 seconds=" << std::chrono::duration<double>(t1 - t0).count() << '\n';

    std::uint64_t recov = 0, rejected = 0, healed = 0, msgs = 0;
    for (std::uint64_t i = 0; i < sessions; ++i) {
      const auto seed = 1000 + first_seed + i;
      std::mt19937_64 rng(seed);
      auto u01 = [&] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };
      mdr::GenParams p;
      p.seed = seed;
      p.messages = session_msgs;
      p.instruments = static_cast<std::uint16_t>(1 + rng() % 4);
      p.loss_a = u01() * 0.3;
      p.loss_b = u01() * 0.3;
      p.dup = u01() * 0.05;
      p.reorder = u01() * 0.1;
      p.max_per_packet = static_cast<std::uint32_t>(1 + rng() % 16);
      p.snapshot_every = 200 + rng() % 5000;
      p.outage_every = (rng() & 1) ? 50 + rng() % 500 : 0;
      p.outage_len = static_cast<std::uint32_t>(1 + rng() % 100);
      p.target_orders = static_cast<std::uint32_t>(20 + rng() % 500);
      const auto n_snaps = p.messages / p.snapshot_every + (p.messages % p.snapshot_every ? 1 : 0);
      if (n_snaps > 1 && (rng() & 1)) p.corrupt_snapshot = static_cast<int>(rng() % (n_snaps - 1));  // never the final one
      const std::uint64_t timeouts[] = {0, 1'000, 10'000, 50'000, 1'000'000};
      mdr::ArbConfig cfg;
      cfg.gap_timeout_ns = timeouts[rng() % 5];
      cfg.max_buffered = 1 + rng() % 4096;
      const auto g = mdr::generate(p);
      replay_session<mdr::BitmapBook>(g, cfg, seed, false);
      replay_session<mdr::MapBook>(g, cfg, seed, false);
      replay_session<mdr::VectorBook>(g, cfg, seed, false);
      replay_session<mdr::BitmapBook>(g, cfg, seed, true);
      // collect a few counters for the summary
      const mdr::LineView a(g.line_a.data(), g.line_a.size()), b(g.line_b.data(), g.line_b.size());
      const mdr::SnapshotStore snaps(g.snapshots.data(), g.snapshots.size(), g.session);
      mdr::BitmapBook book(g.session);
      mdr::DirectSink<mdr::BitmapBook> sink{book, &snaps};
      const auto st = mdr::Arbiter<mdr::DirectSink<mdr::BitmapBook>>(&a, &b, &snaps, cfg, sink).run();
      recov += st.recoveries;
      rejected += st.snapshots_rejected;
      healed += st.healed;
      msgs += g.messages;
    }
    const auto t2 = std::chrono::steady_clock::now();
    std::cout << "session_fuzz: sessions=" << sessions << " messages_total=" << msgs
              << " replays_per_session=4(bitmap,std_map,sorted_vector,bitmap_2thread) recoveries=" << recov
              << " corrupt_snapshots_rejected=" << rejected << " healed_out_of_order=" << healed
              << " digest_mismatches=0 seconds=" << std::chrono::duration<double>(t2 - t1).count() << '\n';
    std::cout << "FUZZ PASS\n";
    return 0;
  } catch (const Failure& f) {
    std::cerr << "FUZZ FAIL: " << f.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
