#pragma once
// Timing helpers and a log-bucket latency histogram.
//
// On arm64 we read the generic timer (CNTVCT_EL0) directly. On Apple M2 it runs
// at 24 MHz, i.e. one tick = 41.67 ns. That is the hard resolution floor of
// every per-message number we report.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>

namespace mdr {

inline std::uint64_t ticks() {
#if defined(__aarch64__)
  std::uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

inline std::uint64_t tick_hz() {
#if defined(__aarch64__)
  std::uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
#else
  return 1'000'000'000ull;
#endif
}

inline double tick_ns() { return 1e9 / static_cast<double>(tick_hz()); }

// Values < 16 get exact buckets; above that, 8 linear sub-buckets per power of
// two (<= 12.5% relative bucket width). Percentiles report the bucket's upper
// bound, so they are conservative; max is exact.
class LogHistogram {
 public:
  static constexpr int kBuckets = 16 + 60 * 8;

  void record(std::uint64_t v) {
    ++counts_[index(v)];
    ++n_;
    sum_ += v;
    max_ = std::max(max_, v);
  }
  void merge(const LogHistogram& o) {
    for (int i = 0; i < kBuckets; ++i) counts_[static_cast<std::size_t>(i)] += o.counts_[static_cast<std::size_t>(i)];
    n_ += o.n_;
    sum_ += o.sum_;
    max_ = std::max(max_, o.max_);
  }
  [[nodiscard]] std::uint64_t percentile(double q) const {
    if (n_ == 0) return 0;
    const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(n_ - 1)) + 1;
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts_[static_cast<std::size_t>(i)];
      if (seen >= rank) return std::min(upper(i), max_);
    }
    return max_;
  }
  [[nodiscard]] std::uint64_t count() const { return n_; }
  [[nodiscard]] std::uint64_t max() const { return max_; }
  [[nodiscard]] double mean() const { return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0; }

  static int index(std::uint64_t v) {
    if (v < 16) return static_cast<int>(v);
    const int e = 63 - std::countl_zero(v);  // >= 4
    const int sub = static_cast<int>((v >> (e - 3)) & 7);
    return std::min(16 + (e - 4) * 8 + sub, kBuckets - 1);
  }
  static std::uint64_t upper(int i) {
    if (i < 16) return static_cast<std::uint64_t>(i);
    const int e = (i - 16) / 8 + 4;
    const int sub = (i - 16) % 8;
    return ((std::uint64_t{8} + static_cast<std::uint64_t>(sub) + 1) << (e - 3)) - 1;
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t n_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace mdr
