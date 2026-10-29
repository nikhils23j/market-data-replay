#include "mdr/io.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdr {
namespace {

std::vector<std::string_view> split_csv(const std::string& line) {
  std::vector<std::string_view> fields;
  std::string_view view(line);
  while (true) {
    const auto pos = view.find(',');
    fields.push_back(view.substr(0, pos));
    if (pos == std::string_view::npos) return fields;
    view.remove_prefix(pos + 1);
  }
}

template <typename T>
T parse_integer(std::string_view value, const char* name) {
  T result{};
  const auto* first = value.data();
  const auto* last = first + value.size();
  const auto [end, error] = std::from_chars(first, last, result);
  if (error != std::errc{} || end != last || value.empty()) {
    throw std::invalid_argument(std::string("invalid ") + name);
  }
  return result;
}

Side parse_side(std::string_view value) {
  if (value == "B") return Side::Bid;
  if (value == "A") return Side::Ask;
  throw std::invalid_argument("side must be B or A");
}

bool skip_line(const std::string& line) {
  return line.empty() || line.front() == '#';
}

} // namespace

std::vector<Update> read_updates(std::istream& input) {
  std::vector<Update> updates;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (skip_line(line)) continue;
    try {
      const auto fields = split_csv(line);
      if (fields.size() != 4) throw std::invalid_argument("expected 4 fields");
      const auto seq = parse_integer<std::uint64_t>(fields[0], "sequence");
      const auto side = parse_side(fields[1]);
      const auto price = parse_integer<std::int64_t>(fields[2], "price tick");
      const auto qty = parse_integer<std::uint32_t>(fields[3], "quantity");
      if (seq == 0 || price < 0) throw std::invalid_argument("sequence and price must be positive/nonnegative");
      updates.push_back(Update{seq, side, price, qty});
    } catch (const std::exception& e) {
      throw std::invalid_argument("updates line " + std::to_string(line_number) + ": " + e.what());
    }
  }
  if (!input.eof() && input.fail()) throw std::runtime_error("failed reading updates");
  return updates;
}

Snapshot read_snapshot(std::istream& input) {
  Snapshot snapshot{};
  std::string line;
  std::size_t line_number = 0;
  bool has_sequence = false;
  while (std::getline(input, line)) {
    ++line_number;
    if (skip_line(line)) continue;
    try {
      const auto fields = split_csv(line);
      if (!has_sequence) {
        if (fields.size() != 2 || fields[0] != "seq") {
          throw std::invalid_argument("first data row must be seq,<number>");
        }
        snapshot.sequence = parse_integer<std::uint64_t>(fields[1], "snapshot sequence");
        has_sequence = true;
      } else {
        if (fields.size() != 3) throw std::invalid_argument("expected 3 fields");
        const auto side = parse_side(fields[0]);
        const auto price = parse_integer<std::int64_t>(fields[1], "price tick");
        const auto qty = parse_integer<std::uint32_t>(fields[2], "quantity");
        if (price < 0 || qty == 0) throw std::invalid_argument("snapshot price/quantity invalid");
        snapshot.levels.push_back(SnapshotLevel{side, price, qty});
      }
    } catch (const std::exception& e) {
      throw std::invalid_argument("snapshot line " + std::to_string(line_number) + ": " + e.what());
    }
  }
  if (!input.eof() && input.fail()) throw std::runtime_error("failed reading snapshot");
  if (!has_sequence) throw std::invalid_argument("missing snapshot sequence");
  return snapshot;
}

} // namespace mdr
