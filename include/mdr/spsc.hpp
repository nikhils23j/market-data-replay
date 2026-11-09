#pragma once
// Bounded single-producer / single-consumer ring.
// Each side owns its index on its own 128-byte line (Apple M2 cache lines are
// 128 B, not 64) and keeps a cached copy of the other side's index, so in the
// common case push/pop touch no shared cache line except the slot itself.

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace mdr {

inline constexpr std::size_t kCacheLine = 128;

template <typename T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity_pow2) : buf_(capacity_pow2), mask_(capacity_pow2 - 1) {
    if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) throw std::invalid_argument("ring capacity must be a power of two");
  }

  bool try_push(const T& v) {
    if (!try_push_deferred(v)) return false;
    flush();
    return true;
  }

  // Batched publication: write the slot but don't make it visible yet. The
  // consumer sees nothing until flush(), so the shared tail line moves between
  // cores once per batch instead of once per message.
  bool try_push_deferred(const T& v) {
    if (local_tail_ - head_cache_ == buf_.size()) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (local_tail_ - head_cache_ == buf_.size()) {
        flush();  // full: make sure the consumer can see what we have
        return false;
      }
    }
    buf_[local_tail_ & mask_] = v;
    ++local_tail_;
    return true;
  }
  void flush() { tail_.store(local_tail_, std::memory_order_release); }

  bool try_pop(T& out) {
    const auto h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return false;
    }
    out = buf_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const { return buf_.size(); }

 private:
  std::vector<T> buf_;
  std::size_t mask_;
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer-owned
  std::size_t head_cache_ = 0;                             // producer's view of head
  std::size_t local_tail_ = 0;                             // producer's unpublished tail
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer-owned
  std::size_t tail_cache_ = 0;                             // consumer's view of tail
  char pad_[kCacheLine - sizeof(std::size_t)]{};
};

}  // namespace mdr
