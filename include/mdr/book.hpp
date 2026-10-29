#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mdr {

enum class Side { Bid, Ask };

struct Level {
  std::int64_t price_tick;
  std::uint32_t quantity;
  bool operator==(const Level&) const = default;
};

struct SnapshotLevel {
  Side side;
  std::int64_t price_tick;
  std::uint32_t quantity;
};

struct Update {
  std::uint64_t sequence;
  Side side;
  std::int64_t price_tick;
  std::uint32_t quantity; // Absolute level quantity; zero deletes a level.
};

struct Top {
  std::optional<Level> bid;
  std::optional<Level> ask;
  bool operator==(const Top&) const = default;
};

enum class ApplyResult { Applied, Duplicate, Gap };

// Fixed 1..4096 tick window. A two-level bitmap makes best-price lookup O(1).
class BitmapBook {
 public:
  BitmapBook(std::int64_t min_tick, std::size_t level_count);

  ApplyResult apply(const Update& update);
  void restore(std::uint64_t sequence, const std::vector<SnapshotLevel>& levels);
  [[nodiscard]] Top top() const;
  [[nodiscard]] std::uint64_t last_sequence() const { return last_sequence_; }
  [[nodiscard]] std::int64_t min_tick() const { return min_tick_; }
  [[nodiscard]] std::size_t level_count() const { return level_count_; }

 private:
  struct SideState {
    std::vector<std::uint32_t> quantity;
    std::vector<std::uint64_t> bits;
    std::uint64_t active_words = 0;
  };

  std::int64_t min_tick_;
  std::size_t level_count_;
  std::uint64_t last_sequence_ = 0;
  SideState bids_;
  SideState asks_;

  [[nodiscard]] std::size_t index_of(std::int64_t price_tick) const;
  static void set_level(SideState& state, std::size_t index, std::uint32_t quantity);
  [[nodiscard]] std::optional<Level> best(const SideState& state, bool highest) const;
};

} // namespace mdr
