#pragma once
// Two-thread pipeline:
//   [mmap'd line A + line B] -> decoder/arbiter thread -> SpscRing<Event> -> book thread
// Recovery and skips travel through the ring as control events, so the book
// thread sees one ordered stream and never shares the book with anyone.

#include "mdr/arbiter.hpp"
#include "mdr/metrics.hpp"
#include "mdr/spsc.hpp"

#include <atomic>
#include <memory>
#include <thread>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace mdr {

inline void cpu_relax() {
#if defined(__aarch64__)
  asm volatile("yield");
#endif
}

// Spin briefly, then give the core away. Pure spinning is fastest on an idle
// machine but collapses when cores are oversubscribed.
struct Backoff {
  unsigned spins = 0;
  void pause() {
    if (++spins < (1u << 14)) cpu_relax();
    else std::this_thread::yield();
  }
  void reset() { spins = 0; }
};

// macOS cannot pin threads to cores; USER_INTERACTIVE QoS asks the scheduler
// to prefer performance cores.
inline void prefer_fast_cores() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

struct PipelineConfig {
  std::size_t ring_capacity = 1u << 14;
  double pace_msgs_per_sec = 0;  // 0 = as fast as possible
  bool measure_latency = false;  // stamp at push, measure after apply
  std::size_t batch = 64;        // publish the ring tail every `batch` events (1 = every event)
  ArbConfig arb;
};

struct PipelineResult {
  double seconds = 0;
  std::uint64_t events = 0;
  std::uint64_t apply_errors = 0;
  std::uint64_t digest = 0;
  std::uint64_t ring_full_spins = 0;
  std::uint64_t restores = 0;
  double restore_max_us = 0;    // longest book-thread stall rebuilding from a snapshot
  double restore_total_us = 0;
  ArbStats arb;
  LogHistogram latency_ticks;  // push -> applied, in timer ticks
};

template <typename Book>
PipelineResult run_pipeline(const LineView* a, const LineView* b, const SnapshotStore* snaps,
                            const PipelineConfig& cfg) {
  auto ring = std::make_unique<SpscRing<Event>>(cfg.ring_capacity);
  PipelineResult res;
  std::atomic<bool> ready{false};
  const Session session = a->session();

  std::thread consumer([&] {
    prefer_fast_cores();
    Book book(session);
    std::uint64_t events = 0, errors = 0;
    LogHistogram hist;
    ready.store(true, std::memory_order_release);
    Event e;
    Backoff backoff;
    while (true) {
      if (!ring->try_pop(e)) {
        backoff.pause();
        continue;
      }
      backoff.reset();
      if (e.type == MsgType::End) break;
      if (e.type == MsgType::Recover) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!snaps->restore(static_cast<std::size_t>(e.order_id), book)) ++errors;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ++res.restores;
        res.restore_total_us += us;
        res.restore_max_us = std::max(res.restore_max_us, us);
        continue;
      }
      if (e.type == MsgType::Skip) continue;
      if (book.apply(e) != Status::Ok) ++errors;
      ++events;
      if (cfg.measure_latency) hist.record(static_cast<std::uint32_t>(static_cast<std::uint32_t>(ticks()) - e.stamp));
    }
    res.events = events;
    res.apply_errors = errors;
    res.digest = book.digest();
    res.latency_ticks = hist;
  });

  struct RingSink {
    SpscRing<Event>& ring;
    const PipelineConfig& cfg;
    double ticks_per_msg = 0;
    double t0 = 0;
    std::uint64_t n = 0;
    std::uint64_t full = 0;
    void push(Event e) {
      if (ticks_per_msg > 0) {
        const auto target = static_cast<std::uint64_t>(t0 + static_cast<double>(n) * ticks_per_msg);
        if (ticks() < target) {
          ring.flush();  // about to idle: publish whatever is pending first
          while (ticks() < target) cpu_relax();
        }
      }
      if (cfg.measure_latency) e.stamp = static_cast<std::uint32_t>(ticks());
      Backoff backoff;
      while (!ring.try_push_deferred(e)) {
        ++full;
        backoff.pause();
      }
      const bool control = e.type == MsgType::Recover || e.type == MsgType::Skip || e.type == MsgType::End;
      if (++n % cfg.batch == 0 || control || ticks_per_msg > 0) ring.flush();  // paced: publish every event
    }
    void on_event(const Event& e) { push(e); }
    void on_recover(std::size_t i, std::uint64_t seq) {
      Event c;
      c.type = MsgType::Recover;
      c.order_id = i;
      c.seq = seq;
      push(c);
    }
    void on_skip(std::uint64_t next) {
      Event c;
      c.type = MsgType::Skip;
      c.seq = next;
      push(c);
    }
  };

  prefer_fast_cores();
  while (!ready.load(std::memory_order_acquire)) cpu_relax();
  RingSink sink{*ring, cfg};
  if (cfg.pace_msgs_per_sec > 0) sink.ticks_per_msg = static_cast<double>(tick_hz()) / cfg.pace_msgs_per_sec;
  const auto start = std::chrono::steady_clock::now();
  sink.t0 = static_cast<double>(ticks());
  Arbiter<RingSink> arb(a, b, snaps, cfg.arb, sink);
  res.arb = arb.run();
  Event end;
  end.type = MsgType::End;
  sink.ticks_per_msg = 0;
  sink.push(end);
  consumer.join();
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  res.ring_full_spins = sink.full;
  return res;
}

}  // namespace mdr
