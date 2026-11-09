// mdr_replay: replay a generated session through the arbiter and a book, and
// check the final book against the generator's truth digest.
#include "cli.hpp"
#include "mdr/arbiter.hpp"
#include "mdr/book.hpp"
#include "mdr/files.hpp"
#include "mdr/pipeline.hpp"

#include <cstdio>
#include <iostream>
#include <memory>

namespace {

struct Outcome {
  mdr::ArbStats arb;
  std::uint64_t applied = 0, apply_errors = 0, digest = 0;
  double seconds = 0;
};

template <typename Book>
Outcome run(const mdr::LineView* a, const mdr::LineView* b, const mdr::SnapshotStore* snaps,
            const mdr::ArbConfig& cfg, int threads) {
  Outcome o;
  if (threads == 2) {
    mdr::PipelineConfig pc;
    pc.arb = cfg;
    const auto r = mdr::run_pipeline<Book>(a, b, snaps, pc);
    o.arb = r.arb;
    o.applied = r.events;
    o.apply_errors = r.apply_errors;
    o.digest = r.digest;
    o.seconds = r.seconds;
    return o;
  }
  Book book(a->session());
  mdr::DirectSink<Book> sink{book, snaps};
  const auto t0 = std::chrono::steady_clock::now();
  mdr::Arbiter<mdr::DirectSink<Book>> arb(a, b, snaps, cfg, sink);
  o.arb = arb.run();
  o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  o.applied = sink.applied;
  o.apply_errors = sink.apply_errors;
  o.digest = book.digest();
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const mdr::Args args(argc, argv);
    const auto dir = args.str("dir", "build/data");
    const auto book = args.str("book", "bitmap");
    const auto lines = args.str("lines", "ab");
    const int threads = static_cast<int>(args.u64("threads", 1));
    mdr::ArbConfig cfg;
    cfg.gap_timeout_ns = args.u64("gap-timeout-us", 50) * 1000;
    cfg.max_buffered = args.u64("max-buffered", cfg.max_buffered);
    cfg.recover = !args.flag("no-snapshots");
    args.reject_unknown();
    if (threads != 1 && threads != 2) throw std::invalid_argument("--threads must be 1 or 2");

    const auto truth = mdr::read_kv(dir + "/truth.txt");
    std::unique_ptr<mdr::MappedFile> fa, fb;
    std::unique_ptr<mdr::LineView> va, vb;
    if (lines.find('a') != std::string::npos) {
      fa = std::make_unique<mdr::MappedFile>(dir + "/line_a.bin");
      va = std::make_unique<mdr::LineView>(fa->data(), fa->size());
    }
    if (lines.find('b') != std::string::npos) {
      fb = std::make_unique<mdr::MappedFile>(dir + "/line_b.bin");
      vb = std::make_unique<mdr::LineView>(fb->data(), fb->size());
    }
    if (!va && !vb) throw std::invalid_argument("--lines must contain a and/or b");
    const mdr::LineView* first = va ? va.get() : vb.get();
    const mdr::LineView* second = va ? vb.get() : nullptr;
    mdr::MappedFile fs(dir + "/snapshots.bin");
    const mdr::SnapshotStore snaps(fs.data(), fs.size(), first->session());

    Outcome o;
    if (book == "bitmap") o = run<mdr::BitmapBook>(first, second, &snaps, cfg, threads);
    else if (book == "map") o = run<mdr::MapBook>(first, second, &snaps, cfg, threads);
    else if (book == "vector") o = run<mdr::VectorBook>(first, second, &snaps, cfg, threads);
    else throw std::invalid_argument("--book must be bitmap, map or vector");

    char digest[32];
    std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(o.digest));
    const auto& s = o.arb;
    std::cout << "book=" << book << " threads=" << threads << " lines=" << lines
              << " gap_timeout_us=" << cfg.gap_timeout_ns / 1000 << " snapshots=" << (cfg.recover ? "on" : "off") << '\n'
              << "packets_in: A=" << s.packets_in[0] << " B=" << s.packets_in[1] << '\n'
              << "delivered_from: first_line=" << s.first_copy[0] << " second_line=" << s.first_copy[1]
              << " stale_or_duplicate=" << s.stale_or_duplicate << '\n'
              << "out_of_order: buffered=" << s.buffered << " healed=" << s.healed << '\n'
              << "gaps_declared=" << s.gaps_declared << " recoveries=" << s.recoveries
              << " snapshots_rejected=" << s.snapshots_rejected << " unrecoverable=" << s.unrecoverable_gaps << '\n'
              << "msgs: delivered=" << s.msgs_delivered << " covered_by_snapshot=" << s.msgs_covered_by_snapshot
              << " skipped=" << s.msgs_skipped << " decode_errors=" << s.decode_errors << '\n'
              << "book: applied=" << o.applied << " apply_errors=" << o.apply_errors << " seconds=" << o.seconds << '\n'
              << "final_digest=" << digest << " truth=" << truth.at("digest")
              << " match=" << (truth.at("digest") == digest ? "yes" : "NO") << '\n';
    return truth.at("digest") == digest ? 0 : 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
