#include "mdr/replay.hpp"

#include <stdexcept>
#include <string>

namespace mdr {

ReplayStats replay(const std::vector<Update>& updates, BitmapBook& book,
                   const std::optional<Snapshot>& snapshot) {
  ReplayStats stats;
  bool snapshot_used = false;
  for (const auto& update : updates) {
    auto result = book.apply(update);
    if (result == ApplyResult::Gap) {
      const auto expected = book.last_sequence() + 1;
      if (!snapshot || snapshot_used || snapshot->sequence < expected) {
        throw std::runtime_error("sequence gap: expected " + std::to_string(expected) +
                                 ", got " + std::to_string(update.sequence));
      }
      book.restore(snapshot->sequence, snapshot->levels);
      snapshot_used = true;
      ++stats.recoveries;
      result = book.apply(update);
      if (result == ApplyResult::Gap) {
        throw std::runtime_error("snapshot does not bridge sequence gap");
      }
    }
    if (result == ApplyResult::Duplicate) ++stats.skipped_duplicate;
    else ++stats.applied;
  }
  return stats;
}

} // namespace mdr
