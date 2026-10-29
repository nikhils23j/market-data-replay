#pragma once

#include "mdr/book.hpp"
#include "mdr/io.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace mdr {

struct ReplayStats {
  std::size_t applied = 0;
  std::size_t skipped_duplicate = 0;
  std::size_t recoveries = 0;
};

ReplayStats replay(const std::vector<Update>& updates, BitmapBook& book,
                   const std::optional<Snapshot>& snapshot = std::nullopt);

} // namespace mdr
