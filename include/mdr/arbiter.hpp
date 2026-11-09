#pragma once
// A/B line arbitration + gap handling.
//
// Packets from both lines are merged in arrival-time order (the files are
// "captures" sorted by arrival). The first copy of each sequence range wins;
// later copies are dropped. A packet that arrives ahead of a hole is buffered,
// because the other line (or a late packet) may still fill it. If the hole is
// still open after `gap_timeout_ns` of feed time, or the buffer gets too big,
// the data is declared lost and we recover from the earliest *valid* snapshot
// that covers the hole; everything up to the snapshot's sequence is dropped.

#include "mdr/snapshot.hpp"
#include "mdr/wire.hpp"

#include <array>
#include <map>
#include <vector>

namespace mdr {

struct ArbConfig {
  std::uint64_t gap_timeout_ns = 50'000;
  std::size_t max_buffered = 4096;
  bool recover = true;  // false: skip the hole and keep going on a stale book
};

struct ArbStats {
  std::array<std::uint64_t, 2> packets_in{};   // packets read per input line
  std::array<std::uint64_t, 2> first_copy{};   // packets whose data was delivered from this line
  std::uint64_t stale_or_duplicate = 0;        // dropped: already have these seqs
  std::uint64_t buffered = 0;                  // arrived ahead of a hole
  std::uint64_t healed = 0;                    // buffered packets later delivered in order
  std::uint64_t gaps_declared = 0;
  std::uint64_t recoveries = 0;
  std::uint64_t snapshots_rejected = 0;
  std::uint64_t unrecoverable_gaps = 0;
  std::uint64_t msgs_delivered = 0;
  std::uint64_t msgs_covered_by_snapshot = 0;  // seqs never applied one by one
  std::uint64_t msgs_skipped = 0;              // seqs lost with no recovery
  std::uint64_t decode_errors = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t next_expected = 1;
};

// Sink concept: on_event(const Event&), on_recover(size_t snapshot, uint64_t seq),
// on_skip(uint64_t next_seq).
template <typename Sink>
class Arbiter {
 public:
  Arbiter(const LineView* a, const LineView* b, const SnapshotStore* snaps, ArbConfig cfg, Sink& sink)
      : lines_{a, b}, snaps_(snaps), cfg_(cfg), sink_(sink) {
    if (!a) throw std::invalid_argument("arbiter needs at least one line");
    if (b && !(a->session() == b->session())) throw std::invalid_argument("lines describe different sessions");
    n_inst_ = a->session().instruments();
    if (snaps_) valid_.assign(snaps_->size(), 0);
  }

  ArbStats run() {
    std::array<std::size_t, 2> off{};
    std::array<Packet, 2> nxt{};
    std::array<bool, 2> has{};
    for (int k = 0; k < 2; ++k) {
      if (!lines_[k]) continue;
      off[k] = lines_[k]->begin_offset();
      has[k] = lines_[k]->next(off[k], nxt[k]);
    }
    while (has[0] || has[1]) {
      const int k = (has[0] && (!has[1] || nxt[0].h.arrival_ns <= nxt[1].h.arrival_ns)) ? 0 : 1;
      const Packet p = nxt[k];
      has[k] = lines_[k]->next(off[k], nxt[k]);
      ++st_.packets_in[k];
      on_packet(p, k);
    }
    while (!buf_.empty()) declare_gap();  // end of input: nothing else will arrive
    st_.next_expected = expected_;
    return st_;
  }

 private:
  struct Held {
    Packet p;
    int line;
  };
  std::array<const LineView*, 2> lines_;
  const SnapshotStore* snaps_;
  ArbConfig cfg_;
  Sink& sink_;
  std::uint16_t n_inst_ = 0;
  std::uint64_t expected_ = 1;
  std::map<std::uint64_t, Held> buf_;  // keyed by first_seq
  std::uint64_t gap_since_ = 0;        // arrival time of the oldest buffered packet
  std::vector<std::int8_t> valid_;     // snapshot validation cache: 0 unknown, 1 ok, -1 bad
  ArbStats st_;

  void on_packet(const Packet& p, int k) {
    const auto now = p.h.arrival_ns;
    if (!buf_.empty() && now - gap_since_ > cfg_.gap_timeout_ns) declare_gap();
    const auto first = p.h.first_seq;
    const auto end = first + p.h.count;
    if (p.h.count == 0) {
      ++st_.heartbeats;
      if (first <= expected_) return;
    } else if (end <= expected_) {
      ++st_.stale_or_duplicate;
      return;
    }
    if (first > expected_) {
      if (buf_.empty()) gap_since_ = now;
      if (buf_.try_emplace(first, Held{p, k}).second) {
        if (p.h.count) ++st_.buffered;
      } else {
        ++st_.stale_or_duplicate;
      }
      if (buf_.size() > cfg_.max_buffered) declare_gap();
      return;
    }
    deliver(p, k);
    drain();
  }

  void deliver(const Packet& p, int k) {
    const auto first = p.h.first_seq;
    for (auto seq = expected_; seq < first + p.h.count; ++seq) {
      Event e;
      if (decode_msg(p.msgs + (seq - first) * kMsgSize, seq, n_inst_, e)) {
        sink_.on_event(e);
        ++st_.msgs_delivered;
      } else {
        ++st_.decode_errors;
      }
    }
    expected_ = first + p.h.count;
    ++st_.first_copy[k];
  }

  void drain() {
    while (!buf_.empty() && buf_.begin()->first <= expected_) {
      const Held h = buf_.begin()->second;
      buf_.erase(buf_.begin());
      if (h.p.h.count == 0) continue;
      if (h.p.h.first_seq + h.p.h.count > expected_) {
        deliver(h.p, h.line);
        ++st_.healed;
      } else {
        ++st_.stale_or_duplicate;
      }
    }
    if (!buf_.empty()) {
      gap_since_ = buf_.begin()->second.p.h.arrival_ns;
      for (const auto& kv : buf_) gap_since_ = std::min(gap_since_, kv.second.p.h.arrival_ns);
    }
  }

  void declare_gap() {
    ++st_.gaps_declared;
    const auto hole_end = buf_.begin()->first;  // first seq we do have after the hole
    if (cfg_.recover && snaps_) {
      for (std::size_t i = 0; i < snaps_->size(); ++i) {
        const auto s = snaps_->entry(i).seq;
        if (s + 1 < hole_end) continue;  // too old to cover the hole
        if (valid_[i] == 0) {
          valid_[i] = snaps_->validate(i) ? 1 : -1;
          if (valid_[i] < 0) ++st_.snapshots_rejected;
        }
        if (valid_[i] < 0) continue;
        ++st_.recoveries;
        st_.msgs_covered_by_snapshot += s + 1 - expected_;
        sink_.on_recover(i, s);
        expected_ = s + 1;
        drain();
        return;
      }
      ++st_.unrecoverable_gaps;
    }
    st_.msgs_skipped += hole_end - expected_;
    expected_ = hole_end;
    sink_.on_skip(expected_);
    drain();
  }
};

// Single-threaded sink: apply straight into a book.
template <typename Book>
struct DirectSink {
  Book& book;
  const SnapshotStore* snaps = nullptr;
  std::uint64_t applied = 0;
  std::uint64_t apply_errors = 0;
  std::uint64_t last_seq = 0;
  bool seq_contiguous = true;  // every event is last+1 (or right after a recover/skip)

  void on_event(const Event& e) {
    if (e.seq != last_seq + 1) seq_contiguous = false;
    last_seq = e.seq;
    if (book.apply(e) != Status::Ok) ++apply_errors;
    ++applied;
  }
  void on_recover(std::size_t i, std::uint64_t seq) {
    if (!snaps->restore(i, book)) throw std::runtime_error("validated snapshot failed to restore");
    last_seq = seq;
  }
  void on_skip(std::uint64_t next) { last_seq = next - 1; }
};

}  // namespace mdr
