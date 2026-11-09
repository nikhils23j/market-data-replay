#pragma once
// Order-by-order book. Orders live in a pooled store and form a FIFO queue at
// each price level; a custom open-addressing hash maps order id -> pool slot.
// The structure that indexes price levels is a template parameter so the
// benchmark changes exactly one thing between BitmapLevels / MapLevels /
// SortedVecLevels.

#include "mdr/wire.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <vector>

namespace mdr {

inline constexpr std::uint32_t NIL = 0xFFFFFFFFu;

enum class Status : std::uint8_t { Ok, UnknownOrder, DuplicateOrder, BadPrice, BadQty, BadInstrument, BadType };

inline const char* status_name(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::UnknownOrder: return "unknown_order";
    case Status::DuplicateOrder: return "duplicate_order";
    case Status::BadPrice: return "bad_price";
    case Status::BadQty: return "bad_qty";
    case Status::BadInstrument: return "bad_instrument";
    case Status::BadType: return "bad_type";
  }
  return "?";
}

struct Top {
  std::uint32_t price = 0;
  std::uint64_t qty = 0;
  std::uint32_t count = 0;  // 0 means the side is empty
  bool operator==(const Top&) const = default;
};

struct OrderNode {
  std::uint64_t id;
  std::uint32_t price;
  std::uint32_t qty;
  std::uint32_t prev;
  std::uint32_t next;
  std::uint16_t inst;
  Side side;
};

class OrderPool {
 public:
  std::uint32_t alloc() {
    if (free_ != NIL) {
      const auto i = free_;
      free_ = nodes_[i].next;
      ++live_;
      return i;
    }
    nodes_.push_back({});
    ++live_;
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }
  void release(std::uint32_t i) {
    nodes_[i].next = free_;
    free_ = i;
    --live_;
  }
  OrderNode& operator[](std::uint32_t i) { return nodes_[i]; }
  const OrderNode& operator[](std::uint32_t i) const { return nodes_[i]; }
  [[nodiscard]] std::size_t live() const { return live_; }

 private:
  std::vector<OrderNode> nodes_;
  std::uint32_t free_ = NIL;
  std::size_t live_ = 0;
};

// Linear probing, key 0 = empty, backward-shift deletion (no tombstones).
class OrderMap {
 public:
  OrderMap() { rehash(1u << 12); }

  [[nodiscard]] std::uint32_t find(std::uint64_t id) const {
    if (id == 0) return NIL;
    for (auto i = home(id);; i = (i + 1) & mask_) {
      if (slots_[i].key == id) return slots_[i].val;
      if (slots_[i].key == 0) return NIL;
    }
  }
  void insert(std::uint64_t id, std::uint32_t v) {  // caller guarantees id absent, id != 0
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    auto i = home(id);
    while (slots_[i].key != 0) i = (i + 1) & mask_;
    slots_[i] = {id, v};
    ++size_;
  }
  void erase(std::uint64_t id) {
    auto i = home(id);
    while (slots_[i].key != id) {
      if (slots_[i].key == 0) return;
      i = (i + 1) & mask_;
    }
    auto j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == 0) break;
      const auto k = home(slots_[j].key);
      // Move slot j back to i if its home is not cyclically within (i, j].
      const bool in_range = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (!in_range) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i] = {0, 0};
    --size_;
  }
  [[nodiscard]] std::size_t size() const { return size_; }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t val;
  };
  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  int shift_ = 0;

  [[nodiscard]] std::size_t home(std::uint64_t id) const {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
  }
  void rehash(std::size_t cap) {
    auto old = std::move(slots_);
    slots_.assign(cap, Slot{0, 0});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
    size_ = 0;
    for (const auto& s : old)
      if (s.key != 0) insert(s.key, s.val);
  }
};

struct Level {
  std::uint64_t qty = 0;
  std::uint32_t count = 0;
  std::uint32_t head = NIL;
  std::uint32_t tail = NIL;
  std::uint32_t price = 0;
};

// ---- Price-level index policies (one side of one instrument) --------------
// Contract: find() -> nullptr if absent; upsert() returns existing or a new
// empty level; remove() is called when a level's count reaches zero. Prices
// are already validated to be inside the window. upsert() may invalidate
// other Level pointers (sorted vector), so callers never hold them across it.

class BitmapLevels {
 public:
  static constexpr const char* kName = "bitmap";
  void init(std::uint32_t base, bool is_bid) {
    base_ = base;
    bid_ = is_bid;
    levels_.assign(kWindow, Level{});
    std::fill(std::begin(bits_), std::end(bits_), 0);
    summary_ = 0;
  }
  Level* find(std::uint32_t price) {
    auto& l = levels_[price - base_];
    return l.count ? &l : nullptr;
  }
  Level& upsert(std::uint32_t price) {
    const auto i = price - base_;
    auto& l = levels_[i];
    if (l.count == 0) {
      l.price = price;
      bits_[i >> 6] |= std::uint64_t{1} << (i & 63);
      summary_ |= std::uint64_t{1} << (i >> 6);
    }
    return l;
  }
  void remove(std::uint32_t price) {
    const auto i = price - base_;
    levels_[i] = Level{};
    auto& w = bits_[i >> 6];
    w &= ~(std::uint64_t{1} << (i & 63));
    if (w == 0) summary_ &= ~(std::uint64_t{1} << (i >> 6));
  }
  [[nodiscard]] const Level* best() const {
    if (summary_ == 0) return nullptr;
    unsigned w, b;
    if (bid_) {
      w = 63u - static_cast<unsigned>(std::countl_zero(summary_));
      b = 63u - static_cast<unsigned>(std::countl_zero(bits_[w]));
    } else {
      w = static_cast<unsigned>(std::countr_zero(summary_));
      b = static_cast<unsigned>(std::countr_zero(bits_[w]));
    }
    return &levels_[w * 64 + b];
  }
  template <typename F>
  void for_each_best_first(F&& f) const {
    if (bid_) {
      for (int i = static_cast<int>(kWindow) - 1; i >= 0; --i)
        if (levels_[static_cast<std::size_t>(i)].count) f(levels_[static_cast<std::size_t>(i)]);
    } else {
      for (std::size_t i = 0; i < kWindow; ++i)
        if (levels_[i].count) f(levels_[i]);
    }
  }

 private:
  std::vector<Level> levels_;
  std::uint64_t bits_[kWindow / 64] = {};
  std::uint64_t summary_ = 0;
  std::uint32_t base_ = 0;
  bool bid_ = true;
};
static_assert(kWindow == 64 * 64, "two-level bitmap assumes 64x64 ticks");

class MapLevels {
 public:
  static constexpr const char* kName = "std_map";
  void init(std::uint32_t, bool is_bid) {
    bid_ = is_bid;
    m_.clear();
  }
  Level* find(std::uint32_t price) {
    auto it = m_.find(price);
    return it == m_.end() ? nullptr : &it->second;
  }
  Level& upsert(std::uint32_t price) {
    auto [it, inserted] = m_.try_emplace(price);
    if (inserted) it->second.price = price;
    return it->second;
  }
  void remove(std::uint32_t price) { m_.erase(price); }
  [[nodiscard]] const Level* best() const {
    if (m_.empty()) return nullptr;
    return bid_ ? &std::prev(m_.end())->second : &m_.begin()->second;
  }
  template <typename F>
  void for_each_best_first(F&& f) const {
    if (bid_) {
      for (auto it = m_.rbegin(); it != m_.rend(); ++it) f(it->second);
    } else {
      for (const auto& kv : m_) f(kv.second);
    }
  }

 private:
  std::map<std::uint32_t, Level> m_;
  bool bid_ = true;
};

// Sorted worst -> best so the busy end (the touch) is at the back, where
// inserts and erases move the fewest elements. Lookup is a binary search.
class SortedVecLevels {
 public:
  static constexpr const char* kName = "sorted_vector";
  void init(std::uint32_t, bool is_bid) {
    bid_ = is_bid;
    v_.clear();
  }
  Level* find(std::uint32_t price) {
    auto it = lb(price);
    return (it != v_.end() && it->price == price) ? &*it : nullptr;
  }
  Level& upsert(std::uint32_t price) {
    auto it = lb(price);
    if (it != v_.end() && it->price == price) return *it;
    Level l;
    l.price = price;
    return *v_.insert(it, l);
  }
  void remove(std::uint32_t price) {
    auto it = lb(price);
    if (it != v_.end() && it->price == price) v_.erase(it);
  }
  [[nodiscard]] const Level* best() const { return v_.empty() ? nullptr : &v_.back(); }
  template <typename F>
  void for_each_best_first(F&& f) const {
    for (auto it = v_.rbegin(); it != v_.rend(); ++it) f(*it);
  }

 private:
  std::vector<Level> v_;
  bool bid_ = true;
  [[nodiscard]] std::uint32_t key(std::uint32_t p) const { return bid_ ? p : ~p; }
  std::vector<Level>::iterator lb(std::uint32_t price) {
    const auto k = key(price);
    return std::lower_bound(v_.begin(), v_.end(), k,
                            [this](const Level& l, std::uint32_t kk) { return key(l.price) < kk; });
  }
};

// ---- The book ---------------------------------------------------------------

template <typename Levels>
class OrderBook {
 public:
  using LevelsType = Levels;
  static constexpr const char* kName = Levels::kName;

  explicit OrderBook(const Session& s) : session_(s), books_(s.instruments()) {
    for (std::size_t i = 0; i < books_.size(); ++i) {
      books_[i].bid.init(s.base_tick[i], true);
      books_[i].ask.init(s.base_tick[i], false);
    }
  }

  Status apply(const Event& e) {
    switch (e.type) {
      case MsgType::Add: return add(e.inst, e.order_id, e.side, e.price, e.qty);
      case MsgType::Cancel: return reduce(e.order_id, e.qty);
      case MsgType::Execute: {
        const auto s = reduce(e.order_id, e.qty);
        if (s == Status::Ok) traded_ += e.qty;
        return s;
      }
      case MsgType::Delete: return del(e.order_id);
      case MsgType::Modify: return modify(e.order_id, e.price, e.qty);
      default: return Status::BadType;
    }
  }

  Status add(std::uint16_t inst, std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty) {
    if (inst >= books_.size()) return Status::BadInstrument;
    if (side != Side::Bid && side != Side::Ask) return Status::BadType;
    if (id == 0 || map_.find(id) != NIL) return Status::DuplicateOrder;
    if (qty == 0) return Status::BadQty;
    if (!in_window(inst, price)) return Status::BadPrice;
    const auto idx = pool_.alloc();
    pool_[idx] = OrderNode{id, price, qty, NIL, NIL, inst, side};
    map_.insert(id, idx);
    link_tail(levels(inst, side).upsert(price), idx);
    return Status::Ok;
  }

  // Cancel / execute: remove `qty` from the order, deleting it at zero.
  Status reduce(std::uint64_t id, std::uint32_t qty) {
    const auto idx = map_.find(id);
    if (idx == NIL) return Status::UnknownOrder;
    auto& o = pool_[idx];
    if (qty == 0 || qty > o.qty) return Status::BadQty;
    if (qty == o.qty) return remove_order(idx);
    o.qty -= qty;
    levels(o.inst, o.side).find(o.price)->qty -= qty;
    return Status::Ok;
  }

  Status del(std::uint64_t id) {
    const auto idx = map_.find(id);
    if (idx == NIL) return Status::UnknownOrder;
    return remove_order(idx);
  }

  // Same price and smaller-or-equal qty keeps queue priority; anything else
  // moves the order to the back of the queue at the new price.
  Status modify(std::uint64_t id, std::uint32_t price, std::uint32_t qty) {
    const auto idx = map_.find(id);
    if (idx == NIL) return Status::UnknownOrder;
    auto& o = pool_[idx];
    if (qty == 0) return Status::BadQty;
    if (!in_window(o.inst, price)) return Status::BadPrice;
    auto& side = levels(o.inst, o.side);
    if (price == o.price && qty <= o.qty) {
      side.find(price)->qty -= (o.qty - qty);
      o.qty = qty;
      return Status::Ok;
    }
    unlink(side, idx);
    o.price = price;
    o.qty = qty;
    link_tail(side.upsert(price), idx);
    return Status::Ok;
  }

  [[nodiscard]] Top top(std::uint16_t inst, Side s) const {
    const auto* l = levels(inst, s).best();
    return l ? Top{l->price, l->qty, l->count} : Top{};
  }

  [[nodiscard]] std::uint64_t digest() const {
    Digest d;
    for (const auto& b : books_) {
      for (const Levels* side : {&b.bid, &b.ask}) {
        side->for_each_best_first([&](const Level& l) {
          d.add(l.price);
          d.add(l.qty);
          d.add(l.count);
          for (auto i = l.head; i != NIL; i = pool_[i].next) {
            d.add(pool_[i].id);
            d.add(pool_[i].qty);
          }
        });
        d.add(0xABCDEFull);
      }
    }
    return d.h;
  }

  // Visits every order in level FIFO order (used to write snapshots).
  template <typename F>
  void for_each_order(F&& f) const {
    for (const auto& b : books_)
      for (const Levels* side : {&b.bid, &b.ask})
        side->for_each_best_first([&](const Level& l) {
          for (auto i = l.head; i != NIL; i = pool_[i].next) f(pool_[i]);
        });
  }

  [[nodiscard]] std::size_t live_orders() const { return pool_.live(); }
  [[nodiscard]] std::uint64_t traded_qty() const { return traded_; }
  [[nodiscard]] const Session& session() const { return session_; }

 private:
  struct InstBook {
    Levels bid, ask;
  };
  Session session_;
  std::vector<InstBook> books_;
  OrderPool pool_;
  OrderMap map_;
  std::uint64_t traded_ = 0;

  [[nodiscard]] bool in_window(std::uint16_t inst, std::uint32_t price) const {
    return price - session_.base_tick[inst] < kWindow;  // unsigned wrap rejects price < base
  }
  Levels& levels(std::uint16_t inst, Side s) { return s == Side::Bid ? books_[inst].bid : books_[inst].ask; }
  const Levels& levels(std::uint16_t inst, Side s) const {
    return s == Side::Bid ? books_[inst].bid : books_[inst].ask;
  }
  void link_tail(Level& l, std::uint32_t idx) {
    auto& o = pool_[idx];
    o.prev = l.tail;
    o.next = NIL;
    if (l.tail != NIL) pool_[l.tail].next = idx;
    else l.head = idx;
    l.tail = idx;
    ++l.count;
    l.qty += o.qty;
  }
  void unlink(Levels& side, std::uint32_t idx) {
    auto& o = pool_[idx];
    auto& l = *side.find(o.price);
    if (o.prev != NIL) pool_[o.prev].next = o.next;
    else l.head = o.next;
    if (o.next != NIL) pool_[o.next].prev = o.prev;
    else l.tail = o.prev;
    l.qty -= o.qty;
    if (--l.count == 0) side.remove(o.price);
  }
  Status remove_order(std::uint32_t idx) {
    auto& o = pool_[idx];
    unlink(levels(o.inst, o.side), idx);
    map_.erase(o.id);
    pool_.release(idx);
    return Status::Ok;
  }
};

using BitmapBook = OrderBook<BitmapLevels>;
using MapBook = OrderBook<MapLevels>;
using VectorBook = OrderBook<SortedVecLevels>;

}  // namespace mdr
