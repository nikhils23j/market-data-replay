#include "mdr/book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct MapBook {
  std::map<std::int64_t, std::uint32_t, std::greater<>> bids;
  std::map<std::int64_t, std::uint32_t> asks;
  std::uint64_t last_sequence = 0;

  void apply(const mdr::Update& update) {
    if (update.sequence != last_sequence + 1) throw std::logic_error("bad benchmark sequence");
    if (update.side == mdr::Side::Bid) {
      if (update.quantity == 0) bids.erase(update.price_tick);
      else bids[update.price_tick] = update.quantity;
    } else {
      if (update.quantity == 0) asks.erase(update.price_tick);
      else asks[update.price_tick] = update.quantity;
    }
    last_sequence = update.sequence;
  }

  mdr::Top top() const {
    mdr::Top result;
    if (!bids.empty()) result.bid = mdr::Level{bids.begin()->first, bids.begin()->second};
    if (!asks.empty()) result.ask = mdr::Level{asks.begin()->first, asks.begin()->second};
    return result;
  }
};

std::uint64_t mix(std::uint64_t checksum, const mdr::Top& top) {
  const auto bid_price = top.bid ? static_cast<std::uint64_t>(top.bid->price_tick) : 0;
  const auto ask_price = top.ask ? static_cast<std::uint64_t>(top.ask->price_tick) : 0;
  const auto bid_qty = top.bid ? top.bid->quantity : 0;
  const auto ask_qty = top.ask ? top.ask->quantity : 0;
  checksum ^= bid_price + (static_cast<std::uint64_t>(bid_qty) << 32);
  checksum *= 1099511628211ULL;
  checksum ^= ask_price + (static_cast<std::uint64_t>(ask_qty) << 32);
  checksum *= 1099511628211ULL;
  return checksum;
}

struct Result {
  double events_per_second;
  std::int64_t p50_ns;
  std::int64_t p99_ns;
  std::uint64_t checksum;
  mdr::Top final_top;
};

template <typename Book, typename Apply>
Result measure(Book& book, const std::vector<mdr::Update>& updates, Apply apply) {
  std::vector<std::int64_t> samples;
  samples.reserve(updates.size());
  std::uint64_t checksum = 1469598103934665603ULL;
  const auto total_start = Clock::now();
  for (const auto& update : updates) {
    const auto start = Clock::now();
    apply(book, update);
    const auto top = book.top();
    checksum = mix(checksum, top);
    const auto end = Clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  }
  const auto total_end = Clock::now();
  std::sort(samples.begin(), samples.end());
  const auto seconds = std::chrono::duration<double>(total_end - total_start).count();
  const auto percentile = [&](double rank) {
    return samples[static_cast<std::size_t>(rank * (samples.size() - 1))];
  };
  return Result{updates.size() / seconds, percentile(0.50), percentile(0.99), checksum,
                book.top()};
}

void print_result(const char* name, const Result& result) {
  std::cout << name << ",events_per_sec=" << static_cast<std::uint64_t>(result.events_per_second)
            << ",p50_ns=" << result.p50_ns << ",p99_ns=" << result.p99_ns
            << ",checksum=" << result.checksum << '\n';
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::size_t count = 100000;
    std::size_t levels = 4096;
    std::uint64_t seed = 42;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
      const std::string value = argv[++i];
      if (arg == "--events") count = std::stoull(value);
      else if (arg == "--levels") levels = std::stoull(value);
      else if (arg == "--seed") seed = std::stoull(value);
      else throw std::invalid_argument("unknown option: " + arg);
    }
    if (count == 0 || levels < 2 || levels > 4096) {
      throw std::invalid_argument("events must be positive and levels must be 2..4096");
    }
    constexpr std::int64_t min_tick = 100000;
    std::mt19937_64 rng(seed);
    std::vector<mdr::Update> updates;
    updates.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto side = (i & 1) == 0 ? mdr::Side::Bid : mdr::Side::Ask;
      const auto index = static_cast<std::int64_t>(rng() % levels);
      const auto quantity = rng() % 10 == 0 ? 0U : static_cast<std::uint32_t>(1 + rng() % 100);
      updates.push_back(mdr::Update{static_cast<std::uint64_t>(i + 1), side,
                                    min_tick + index, quantity});
    }
    mdr::BitmapBook bitmap(min_tick, levels);
    MapBook baseline;
    const auto optimized = measure(bitmap, updates, [](auto& book, const auto& update) {
      if (book.apply(update) != mdr::ApplyResult::Applied) {
        throw std::logic_error("unexpected benchmark update result");
      }
    });
    const auto mapped = measure(baseline, updates, [](auto& book, const auto& update) {
      book.apply(update);
    });
    if (optimized.checksum != mapped.checksum || optimized.final_top != mapped.final_top) {
      throw std::logic_error("bitmap/map result mismatch");
    }
    std::cout << "workload,events=" << count << ",levels=" << levels << ",seed=" << seed
              << ",operation=absolute_level_update_plus_top_of_book\n";
    print_result("bitmap", optimized);
    print_result("std_map", mapped);
    std::cout << "checksum_match=yes\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
