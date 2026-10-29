#include "mdr/io.hpp"
#include "mdr/replay.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

std::string show(const std::optional<mdr::Level>& level) {
  if (!level) return "none";
  return std::to_string(level->price_tick) + "@" + std::to_string(level->quantity);
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::string events_path;
    std::string snapshot_path;
    std::int64_t min_tick = 100000;
    std::size_t levels = 4096;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
      const std::string value = argv[++i];
      if (arg == "--events") events_path = value;
      else if (arg == "--snapshot") snapshot_path = value;
      else if (arg == "--min-tick") min_tick = std::stoll(value);
      else if (arg == "--levels") levels = std::stoull(value);
      else throw std::invalid_argument("unknown option: " + arg);
    }
    if (events_path.empty()) throw std::invalid_argument("usage: mdr_replay --events FILE [--snapshot FILE] [--min-tick N] [--levels N]");
    std::ifstream events_file(events_path);
    if (!events_file) throw std::runtime_error("cannot open events file: " + events_path);
    const auto updates = mdr::read_updates(events_file);
    std::optional<mdr::Snapshot> snapshot;
    if (!snapshot_path.empty()) {
      std::ifstream snapshot_file(snapshot_path);
      if (!snapshot_file) throw std::runtime_error("cannot open snapshot file: " + snapshot_path);
      snapshot = mdr::read_snapshot(snapshot_file);
    }
    mdr::BitmapBook book(min_tick, levels);
    const auto stats = mdr::replay(updates, book, snapshot);
    const auto top = book.top();
    std::cout << "applied=" << stats.applied << " skipped_duplicate="
              << stats.skipped_duplicate << " recoveries=" << stats.recoveries
              << " last_seq=" << book.last_sequence() << " best_bid=" << show(top.bid)
              << " best_ask=" << show(top.ask) << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
