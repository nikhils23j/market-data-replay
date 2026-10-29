#include "mdr/book.hpp"
#include "mdr/io.hpp"
#include "mdr/replay.hpp"

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int checks = 0;

void require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

template <typename F>
void throws(F action, const char* message) {
  bool did_throw = false;
  try { action(); } catch (const std::exception&) { did_throw = true; }
  require(did_throw, message);
}

void test_best_and_delete() {
  mdr::BitmapBook book(100, 130); // Crosses three bitmap words.
  require(!book.top().bid && !book.top().ask, "empty top");
  require(book.apply({1, mdr::Side::Bid, 100, 7}) == mdr::ApplyResult::Applied, "first bid");
  require(book.apply({2, mdr::Side::Bid, 164, 4}) == mdr::ApplyResult::Applied, "second bid");
  require(book.apply({3, mdr::Side::Bid, 229, 9}) == mdr::ApplyResult::Applied, "third bid");
  require(book.top().bid == mdr::Level{229, 9}, "highest bid across words");
  require(book.apply({4, mdr::Side::Ask, 200, 3}) == mdr::ApplyResult::Applied, "first ask");
  require(book.apply({5, mdr::Side::Ask, 101, 8}) == mdr::ApplyResult::Applied, "second ask");
  require(book.top().ask == mdr::Level{101, 8}, "lowest ask across words");
  require(book.apply({6, mdr::Side::Bid, 229, 0}) == mdr::ApplyResult::Applied, "delete bid");
  require(book.top().bid == mdr::Level{164, 4}, "next bid after delete");
  require(book.apply({7, mdr::Side::Ask, 101, 0}) == mdr::ApplyResult::Applied, "delete ask");
  require(book.top().ask == mdr::Level{200, 3}, "next ask after delete");
  require(book.apply({8, mdr::Side::Ask, 200, 0}) == mdr::ApplyResult::Applied, "delete last ask");
  require(!book.top().ask, "ask is empty");
}

void test_sequence_and_bounds() {
  mdr::BitmapBook book(100, 64);
  book.apply({1, mdr::Side::Bid, 100, 5});
  const auto before = book.top();
  require(book.apply({1, mdr::Side::Bid, 100, 9}) == mdr::ApplyResult::Duplicate, "duplicate");
  require(book.apply({3, mdr::Side::Bid, 101, 9}) == mdr::ApplyResult::Gap, "gap");
  require(book.last_sequence() == 1 && book.top() == before, "gap/duplicate do not mutate");
  throws([&] { book.apply({2, mdr::Side::Bid, 164, 9}); }, "reject out-of-range tick");
  require(book.last_sequence() == 1, "bad input does not advance sequence");
  require(book.apply({2, mdr::Side::Bid, 163, 1}) == mdr::ApplyResult::Applied, "last valid tick");
  throws([] { mdr::BitmapBook bad(0, 4097); }, "reject too many levels");
}

void test_snapshot_atomic_and_replay() {
  mdr::BitmapBook book(100, 64);
  book.apply({1, mdr::Side::Bid, 100, 5});
  const auto before = book.top();
  throws([&] { book.restore(2, {{mdr::Side::Bid, 101, 2}, {mdr::Side::Bid, 101, 3}}); },
         "duplicate snapshot level");
  require(book.top() == before && book.last_sequence() == 1, "invalid snapshot atomic");
  throws([&] { book.restore(0, {}); }, "reject old snapshot");

  mdr::BitmapBook recovered(100, 64);
  const std::vector<mdr::Update> updates = {
      {1, mdr::Side::Bid, 100, 10}, {2, mdr::Side::Ask, 120, 12},
      {2, mdr::Side::Ask, 120, 12}, {4, mdr::Side::Bid, 101, 5},
      {5, mdr::Side::Ask, 119, 7}};
  mdr::Snapshot snapshot{3, {{mdr::Side::Bid, 100, 20}, {mdr::Side::Ask, 120, 12}}};
  const auto stats = mdr::replay(updates, recovered, snapshot);
  require(stats.applied == 4 && stats.skipped_duplicate == 1 && stats.recoveries == 1,
          "replay counters");
  require(recovered.last_sequence() == 5, "replay sequence");
  require(recovered.top() == mdr::Top{mdr::Level{101, 5}, mdr::Level{119, 7}},
          "replay top");
  mdr::BitmapBook no_snapshot(100, 64);
  throws([&] { mdr::replay(updates, no_snapshot); }, "gap without snapshot fails");
  mdr::BitmapBook stale_snapshot(100, 64);
  throws([&] { mdr::replay(updates, stale_snapshot, mdr::Snapshot{2, {}}); },
         "stale snapshot cannot bridge gap");
}

void test_parsers() {
  std::istringstream events("# demo\n1,B,100,10\n2,A,120,0\n");
  const auto updates = mdr::read_updates(events);
  require(updates.size() == 2 && updates[1].quantity == 0, "parse updates");
  std::istringstream snapshot("seq,5\nB,100,10\nA,120,20\n");
  const auto parsed = mdr::read_snapshot(snapshot);
  require(parsed.sequence == 5 && parsed.levels.size() == 2, "parse snapshot");
  throws([] { std::istringstream bad("1,X,100,10\n"); mdr::read_updates(bad); },
         "reject side");
  throws([] { std::istringstream bad("1,B,100,-1\n"); mdr::read_updates(bad); },
         "reject negative quantity");
  throws([] { std::istringstream bad("seq,1\nB,100,0\n"); mdr::read_snapshot(bad); },
         "reject zero snapshot quantity");
  throws([] { std::istringstream bad("B,100,1\n"); mdr::read_snapshot(bad); },
         "reject missing snapshot sequence");
}

} // namespace

int main() {
  try {
    test_best_and_delete();
    test_sequence_and_bounds();
    test_snapshot_atomic_and_replay();
    test_parsers();
    std::cout << "passed " << checks << " checks\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test failed after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
