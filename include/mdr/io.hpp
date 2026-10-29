#pragma once

#include "mdr/book.hpp"

#include <istream>
#include <vector>

namespace mdr {

struct Snapshot {
  std::uint64_t sequence;
  std::vector<SnapshotLevel> levels;
};

std::vector<Update> read_updates(std::istream& input);
Snapshot read_snapshot(std::istream& input);

} // namespace mdr
