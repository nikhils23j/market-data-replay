// mdr_bench: measurements over a session written by mdr_gen.
//
//   apply_only     pre-decoded clean events -> book.apply(), one thread
//   apply_latency  same, but each apply() bracketed by timer reads
//   inline_1t      mmap'd A+B -> arbiter -> decode -> apply, one thread
//   pipeline_2t    mmap'd A+B -> arbiter/decoder thread -> SPSC ring -> book thread
//   paced_2t       pipeline_2t with the producer releasing messages at a fixed
//                  rate; latency = push (pre-ring) -> applied in book thread
//
// Every timed run also checks the final book digest against truth.txt.
#include "cli.hpp"
#include "mdr/arbiter.hpp"
#include "mdr/book.hpp"
#include "mdr/files.hpp"
#include "mdr/metrics.hpp"
#include "mdr/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

using Clock = std::chrono::steady_clock;

struct Series {
  std::string section, book;
  std::vector<double> runs;  // msgs/s
};

struct LatRow {
  std::string section, book;
  int rep;
  std::uint64_t n;
  double mean_ns, p50_ns, p90_ns, p99_ns, p999_ns, max_ns;
  std::uint64_t restores = 0;
  double restore_max_us = 0;
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const auto n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

LatRow lat_row(const std::string& sec, const std::string& book, int rep, const mdr::LogHistogram& h) {
  const double k = mdr::tick_ns();
  auto p = [&](double q) { return static_cast<double>(h.percentile(q)) * k; };
  return {sec, book, rep, h.count(), h.mean() * k, p(0.5), p(0.9), p(0.99), p(0.999), static_cast<double>(h.max()) * k, 0, 0};
}

std::string hex(std::uint64_t d) {
  char b[32];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(d));
  return b;
}

struct Ctx {
  std::vector<mdr::Event> events;
  const mdr::LineView* a;
  const mdr::LineView* b;
  const mdr::LineView* clean;
  const mdr::SnapshotStore* snaps;
  std::string truth;
  int reps, pace_reps;
  double pace;
  std::vector<Series> series;
  std::vector<LatRow> lat;
  mdr::ArbStats arb_example;
  std::uint64_t pipeline_events = 0;
};

void check(const Ctx& c, std::uint64_t digest, std::uint64_t errors, const char* what) {
  if (hex(digest) != c.truth || errors != 0)
    throw std::runtime_error(std::string(what) + ": digest mismatch or apply errors");
}

template <typename Book>
double apply_only(Ctx& c) {
  Book book(c.a->session());
  std::uint64_t errors = 0;
  const auto t0 = Clock::now();
  for (const auto& e : c.events) errors += book.apply(e) != mdr::Status::Ok;
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  check(c, book.digest(), errors, "apply_only");
  return static_cast<double>(c.events.size()) / s;
}

template <typename Book>
void apply_latency(Ctx& c) {
  Book book(c.a->session());
  mdr::LogHistogram h;
  std::uint64_t errors = 0;
  for (const auto& e : c.events) {
    const auto t0 = mdr::ticks();
    errors += book.apply(e) != mdr::Status::Ok;
    h.record(mdr::ticks() - t0);
  }
  check(c, book.digest(), errors, "apply_latency");
  c.lat.push_back(lat_row("apply_latency", Book::kName, 0, h));
}

template <typename Book>
double inline_1t(Ctx& c) {
  Book book(c.a->session());
  mdr::DirectSink<Book> sink{book, c.snaps};
  const auto t0 = Clock::now();
  mdr::Arbiter<mdr::DirectSink<Book>> arb(c.a, c.b, c.snaps, mdr::ArbConfig{}, sink);
  const auto st = arb.run();
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  check(c, book.digest(), sink.apply_errors, "inline_1t");
  c.arb_example = st;
  return static_cast<double>(st.msgs_delivered) / s;
}

template <typename Book>
double pipeline_2t(Ctx& c) {
  mdr::PipelineConfig pc;
  const auto r = mdr::run_pipeline<Book>(c.a, c.b, c.snaps, pc);
  check(c, r.digest, r.apply_errors, "pipeline_2t");
  c.pipeline_events = r.events;
  return static_cast<double>(r.events) / r.seconds;
}

// clean_input: single fault-free line, so no snapshot restores happen.
template <typename Book>
void paced_2t(Ctx& c, int rep, bool clean_input) {
  mdr::PipelineConfig pc;
  pc.pace_msgs_per_sec = c.pace;
  pc.measure_latency = true;
  const auto r = clean_input ? mdr::run_pipeline<Book>(c.clean, nullptr, c.snaps, pc)
                             : mdr::run_pipeline<Book>(c.a, c.b, c.snaps, pc);
  check(c, r.digest, r.apply_errors, "paced_2t");
  auto row = lat_row(clean_input ? "paced_2t_clean" : "paced_2t_ab", Book::kName, rep, r.latency_ticks);
  row.restores = r.restores;
  row.restore_max_us = r.restore_max_us;
  c.lat.push_back(row);
}

struct RestoreRow {
  std::string book;
  std::uint32_t orders;
  std::vector<double> us;
};

// How long does the book thread stall while rebuilding from a snapshot?
template <typename Book>
RestoreRow restore_cost(Ctx& c) {
  const auto idx = c.snaps->size() / 2;
  RestoreRow row{Book::kName, c.snaps->entry(idx).n_orders, {}};
  Book book(c.a->session());
  for (int i = 0; i < 15; ++i) {
    const auto t0 = Clock::now();
    if (!c.snaps->restore(idx, book)) throw std::runtime_error("restore failed");
    row.us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  return row;
}

using Runner = std::function<double(Ctx&)>;

void run_series(Ctx& c, const std::string& section,
                const std::vector<std::pair<std::string, Runner>>& books) {
  std::vector<Series> out;
  for (const auto& b : books) out.push_back({section, b.first, {}});
  for (int r = 0; r < c.reps; ++r)
    for (std::size_t j = 0; j < books.size(); ++j) {  // rotate order each rep
      const auto k = (j + static_cast<std::size_t>(r)) % books.size();
      out[k].runs.push_back(books[k].second(c));
    }
  for (auto& s : out) {
    std::cout << std::left << std::setw(13) << section << std::setw(15) << s.book << std::right << std::fixed
              << std::setprecision(2) << " median " << std::setw(7) << median(s.runs) / 1e6 << " M msg/s   min "
              << *std::min_element(s.runs.begin(), s.runs.end()) / 1e6 << "   max "
              << *std::max_element(s.runs.begin(), s.runs.end()) / 1e6 << "   (n=" << s.runs.size() << ")\n";
    c.series.push_back(s);
  }
}

void print_lat(const LatRow& l) {
  std::cout << std::left << std::setw(16) << l.section << std::setw(15) << l.book << std::right << " rep " << l.rep
            << std::fixed << std::setprecision(0) << "  n=" << l.n << "  mean " << l.mean_ns << "  p50 " << l.p50_ns
            << "  p90 " << l.p90_ns << "  p99 " << l.p99_ns << "  p99.9 " << l.p999_ns << "  max " << l.max_ns
            << " ns";
  if (l.restores) std::cout << "  (restores " << l.restores << ", longest " << l.restore_max_us << " us)";
  std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const mdr::Args args(argc, argv);
    const auto dir = args.str("dir", "build/data");
    const auto json_path = args.str("json", "");
    Ctx c{};
    c.reps = static_cast<int>(args.u64("reps", 5));
    c.pace_reps = static_cast<int>(args.u64("pace-reps", 3));
    c.pace = args.f64("pace", 1e6);
    args.reject_unknown();
    mdr::prefer_fast_cores();

    const auto truth = mdr::read_kv(dir + "/truth.txt");
    c.truth = truth.at("digest");
    mdr::MappedFile fa(dir + "/line_a.bin"), fb(dir + "/line_b.bin"), fc(dir + "/clean.bin"), fs(dir + "/snapshots.bin");
    const mdr::LineView a(fa.data(), fa.size()), b(fb.data(), fb.size()), clean(fc.data(), fc.size());
    const mdr::SnapshotStore snaps(fs.data(), fs.size(), a.session());
    c.a = &a;
    c.b = &b;
    c.clean = &clean;
    c.snaps = &snaps;
    {
      std::size_t off = clean.begin_offset();
      mdr::Packet p;
      c.events.reserve(std::stoull(truth.at("messages")));
      while (clean.next(off, p))
        for (std::uint16_t i = 0; i < p.h.count; ++i) {
          mdr::Event e;
          if (!mdr::decode_msg(p.msgs + i * mdr::kMsgSize, p.h.first_seq + i, clean.session().instruments(), e))
            throw std::runtime_error("clean stream decode error");
          c.events.push_back(e);
        }
    }
    std::cout << "session: " << c.events.size() << " messages, " << a.session().instruments()
              << " instruments, timer " << mdr::tick_hz() << " Hz (" << std::setprecision(4) << mdr::tick_ns()
              << " ns/tick)\n";

    // Timer overhead: two back-to-back reads.
    mdr::LogHistogram overhead;
    for (int i = 0; i < 1'000'000; ++i) {
      const auto t0 = mdr::ticks();
      overhead.record(mdr::ticks() - t0);
    }
    const auto ov = lat_row("timer_overhead", "-", 0, overhead);
    print_lat(ov);

    run_series(c, "apply_only",
               {{mdr::BitmapBook::kName, apply_only<mdr::BitmapBook>},
                {mdr::MapBook::kName, apply_only<mdr::MapBook>},
                {mdr::VectorBook::kName, apply_only<mdr::VectorBook>}});
    apply_latency<mdr::BitmapBook>(c);
    apply_latency<mdr::MapBook>(c);
    apply_latency<mdr::VectorBook>(c);
    for (std::size_t i = c.lat.size() - 3; i < c.lat.size(); ++i) print_lat(c.lat[i]);
    run_series(c, "inline_1t",
               {{mdr::BitmapBook::kName, inline_1t<mdr::BitmapBook>},
                {mdr::MapBook::kName, inline_1t<mdr::MapBook>},
                {mdr::VectorBook::kName, inline_1t<mdr::VectorBook>}});
    run_series(c, "pipeline_2t",
               {{mdr::BitmapBook::kName, pipeline_2t<mdr::BitmapBook>},
                {mdr::MapBook::kName, pipeline_2t<mdr::MapBook>},
                {mdr::VectorBook::kName, pipeline_2t<mdr::VectorBook>}});
    std::vector<RestoreRow> restores{restore_cost<mdr::BitmapBook>(c), restore_cost<mdr::MapBook>(c),
                                     restore_cost<mdr::VectorBook>(c)};
    for (const auto& r : restores)
      std::cout << "snapshot_restore " << std::left << std::setw(15) << r.book << std::right << " orders=" << r.orders
                << std::fixed << std::setprecision(0) << "  median " << median(r.us) << " us  min "
                << *std::min_element(r.us.begin(), r.us.end()) << "  max " << *std::max_element(r.us.begin(), r.us.end())
                << " us (n=" << r.us.size() << ")\n";
    const auto before = c.lat.size();
    for (int r = 0; r < c.pace_reps; ++r)
      for (bool clean_input : {false, true}) {
        paced_2t<mdr::BitmapBook>(c, r, clean_input);
        paced_2t<mdr::MapBook>(c, r, clean_input);
        paced_2t<mdr::VectorBook>(c, r, clean_input);
      }
    for (std::size_t i = before; i < c.lat.size(); ++i) print_lat(c.lat[i]);
    const auto& s = c.arb_example;
    std::cout << "arbiter: packets A=" << s.packets_in[0] << " B=" << s.packets_in[1] << " from_A=" << s.first_copy[0]
              << " from_B=" << s.first_copy[1] << " stale_or_dup=" << s.stale_or_duplicate << " healed=" << s.healed
              << " gaps=" << s.gaps_declared << " recoveries=" << s.recoveries
              << " covered_by_snapshot=" << s.msgs_covered_by_snapshot << " delivered=" << s.msgs_delivered << '\n';
    std::cout << "all timed runs matched truth digest " << c.truth << '\n';

    if (!json_path.empty()) {
      std::ofstream j(json_path);
      j << std::setprecision(10);
      j << "{\n  \"messages\": " << c.events.size() << ",\n  \"truth_digest\": \"" << c.truth << "\",\n";
      j << "  \"timer_hz\": " << mdr::tick_hz() << ",\n  \"reps\": " << c.reps << ",\n  \"pace_reps\": " << c.pace_reps
        << ",\n  \"pace_msgs_per_sec\": " << c.pace << ",\n";
      j << "  \"throughput_msgs_per_sec\": [\n";
      for (std::size_t i = 0; i < c.series.size(); ++i) {
        const auto& x = c.series[i];
        j << "    {\"section\": \"" << x.section << "\", \"book\": \"" << x.book << "\", \"median\": " << median(x.runs)
          << ", \"min\": " << *std::min_element(x.runs.begin(), x.runs.end())
          << ", \"max\": " << *std::max_element(x.runs.begin(), x.runs.end()) << ", \"runs\": [";
        for (std::size_t k = 0; k < x.runs.size(); ++k) j << (k ? ", " : "") << x.runs[k];
        j << "]}" << (i + 1 < c.series.size() ? "," : "") << "\n";
      }
      j << "  ],\n  \"latency_ns\": [\n";
      std::vector<LatRow> all{ov};
      all.insert(all.end(), c.lat.begin(), c.lat.end());
      for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& l = all[i];
        j << "    {\"section\": \"" << l.section << "\", \"book\": \"" << l.book << "\", \"rep\": " << l.rep
          << ", \"n\": " << l.n << ", \"mean\": " << l.mean_ns << ", \"p50\": " << l.p50_ns << ", \"p90\": " << l.p90_ns
          << ", \"p99\": " << l.p99_ns << ", \"p99_9\": " << l.p999_ns << ", \"max\": " << l.max_ns << ", \"restores\": " << l.restores
          << ", \"restore_max_us\": " << l.restore_max_us << "}"
          << (i + 1 < all.size() ? "," : "") << "\n";
      }
      j << "  ],\n  \"snapshot_restore_us\": [\n";
      for (std::size_t i = 0; i < restores.size(); ++i) {
        const auto& r = restores[i];
        j << "    {\"book\": \"" << r.book << "\", \"orders\": " << r.orders << ", \"median\": " << median(r.us)
          << ", \"min\": " << *std::min_element(r.us.begin(), r.us.end())
          << ", \"max\": " << *std::max_element(r.us.begin(), r.us.end()) << "}" << (i + 1 < restores.size() ? "," : "")
          << "\n";
      }
      j << "  ],\n  \"arbiter\": {\"packets_a\": " << s.packets_in[0] << ", \"packets_b\": " << s.packets_in[1]
        << ", \"from_a\": " << s.first_copy[0] << ", \"from_b\": " << s.first_copy[1]
        << ", \"stale_or_duplicate\": " << s.stale_or_duplicate << ", \"buffered\": " << s.buffered
        << ", \"healed\": " << s.healed << ", \"gaps_declared\": " << s.gaps_declared
        << ", \"recoveries\": " << s.recoveries << ", \"snapshots_rejected\": " << s.snapshots_rejected
        << ", \"msgs_delivered\": " << s.msgs_delivered << ", \"msgs_covered_by_snapshot\": " << s.msgs_covered_by_snapshot
        << "}\n}\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
