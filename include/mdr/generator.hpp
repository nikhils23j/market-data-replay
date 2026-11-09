#pragma once
// Synthetic session generator. Produces one canonical order-flow stream and
// derives two "captures" of it (line A and line B) with independent loss,
// line outages, duplicates, jitter and late (reordered) packets, plus a clean
// single-line copy, periodic snapshots and the true final book digest.

#include "mdr/wire.hpp"

#include <cstdint>
#include <vector>

namespace mdr {

struct GenParams {
  std::uint64_t messages = 1'000'000;
  std::uint16_t instruments = 8;
  std::uint64_t seed = 1;
  double loss_a = 0.002;       // per-packet independent loss
  double loss_b = 0.002;
  double dup = 0.001;          // per-packet chance a line delivers it twice
  double reorder = 0.005;      // per-packet chance it is delayed 3-30 us
  std::uint32_t max_per_packet = 8;
  std::uint64_t snapshot_every = 100'000;  // messages; a final snapshot is always written
  std::uint64_t outage_every = 200'000;    // packets between line outages (0 = none)
  std::uint32_t outage_len = 300;          // packets lost per outage (alternates B, A, B, ...)
  double mean_ns_per_msg = 1000;           // nominal feed rate 1M msgs/s
  std::uint64_t line_b_lag_ns = 2000;
  std::uint64_t jitter_ns = 1000;
  int corrupt_snapshot = -1;               // flip a byte in this snapshot index
  std::uint32_t target_orders = 2000;      // resting orders per instrument (approx.)
};

struct GenStats {
  std::uint64_t packets = 0;
  std::uint64_t lost_a = 0, lost_b = 0, lost_both = 0;
  std::uint64_t dup_a = 0, dup_b = 0;
  std::uint64_t late_a = 0, late_b = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t adds = 0, cancels = 0, deletes = 0, executes = 0, modifies = 0;
  double avg_live_orders = 0;
  double avg_levels_per_side = 0;
  std::uint64_t max_levels_per_side = 0;
};

struct GenOutput {
  Session session;
  std::vector<std::uint8_t> line_a, line_b, clean, snapshots;
  std::uint64_t messages = 0;
  std::uint64_t truth_digest = 0;
  GenStats stats;
};

GenOutput generate(const GenParams& p);

}  // namespace mdr
