#include "mdr/book.hpp"

#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mdr {

BitmapBook::BitmapBook(std::int64_t min_tick, std::size_t level_count)
    : min_tick_(min_tick), level_count_(level_count) {
  if (min_tick < 0 || level_count == 0 || level_count > 4096 ||
      min_tick > std::numeric_limits<std::int64_t>::max() -
                     static_cast<std::int64_t>(level_count - 1)) {
    throw std::invalid_argument("invalid price window (requires 1..4096 nonnegative ticks)");
  }
  const auto words = (level_count + 63) / 64;
  bids_.quantity.resize(level_count);
  asks_.quantity.resize(level_count);
  bids_.bits.resize(words);
  asks_.bits.resize(words);
}

std::size_t BitmapBook::index_of(std::int64_t price_tick) const {
  if (price_tick < min_tick_ ||
      price_tick - min_tick_ >= static_cast<std::int64_t>(level_count_)) {
    throw std::out_of_range("price tick outside configured window");
  }
  return static_cast<std::size_t>(price_tick - min_tick_);
}

void BitmapBook::set_level(SideState& state, std::size_t index,
                           std::uint32_t quantity) {
  state.quantity[index] = quantity;
  const auto word = index / 64;
  const auto mask = std::uint64_t{1} << (index % 64);
  if (quantity == 0) {
    state.bits[word] &= ~mask;
    if (state.bits[word] == 0) state.active_words &= ~(std::uint64_t{1} << word);
  } else {
    state.bits[word] |= mask;
    state.active_words |= std::uint64_t{1} << word;
  }
}

ApplyResult BitmapBook::apply(const Update& update) {
  // Validate before any sequence or book mutation. Bad feed data is never skipped.
  const auto index = index_of(update.price_tick);
  if (update.sequence <= last_sequence_) return ApplyResult::Duplicate;
  if (update.sequence != last_sequence_ + 1) return ApplyResult::Gap;
  set_level(update.side == Side::Bid ? bids_ : asks_, index, update.quantity);
  last_sequence_ = update.sequence;
  return ApplyResult::Applied;
}

void BitmapBook::restore(std::uint64_t sequence,
                         const std::vector<SnapshotLevel>& levels) {
  if (sequence < last_sequence_) throw std::invalid_argument("snapshot is older than book");
  BitmapBook replacement(min_tick_, level_count_);
  for (const auto& level : levels) {
    if (level.quantity == 0) throw std::invalid_argument("snapshot contains zero quantity");
    const auto index = replacement.index_of(level.price_tick);
    auto& state = level.side == Side::Bid ? replacement.bids_ : replacement.asks_;
    if (state.quantity[index] != 0) throw std::invalid_argument("duplicate snapshot level");
    set_level(state, index, level.quantity);
  }
  replacement.last_sequence_ = sequence;
  *this = std::move(replacement); // Snapshot replacement is atomic after validation.
}

std::optional<Level> BitmapBook::best(const SideState& state, bool highest) const {
  if (state.active_words == 0) return std::nullopt;
  const auto word = highest ? 63U - std::countl_zero(state.active_words)
                            : std::countr_zero(state.active_words);
  const auto bitmap = state.bits[word];
  const auto bit = highest ? 63U - std::countl_zero(bitmap)
                           : std::countr_zero(bitmap);
  const auto index = static_cast<std::size_t>(word) * 64 + bit;
  return Level{min_tick_ + static_cast<std::int64_t>(index), state.quantity[index]};
}

Top BitmapBook::top() const { return Top{best(bids_, true), best(asks_, false)}; }

} // namespace mdr
