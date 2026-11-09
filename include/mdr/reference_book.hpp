#pragma once
// Deliberately boring reference book: no pools, no intrusive lists, no
// bitmaps. Same rules as OrderBook. Used as the oracle by the differential
// fuzzer and by the generator to know the "true" state of the market.

#include "mdr/book.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <unordered_map>
#include <vector>

namespace mdr {

class RefBook {
 public:
  struct Order {
    std::uint16_t inst;
    Side side;
    std::uint32_t price;
    std::uint32_t qty;
  };
  using LevelMap = std::map<std::uint32_t, std::vector<std::uint64_t>>;  // price -> FIFO of ids

  explicit RefBook(const Session& s) : session_(s), levels_(s.instruments()) {}

  Status apply(const Event& e) {
    switch (e.type) {
      case MsgType::Add: return add(e.inst, e.order_id, e.side, e.price, e.qty);
      case MsgType::Cancel:
      case MsgType::Execute: return reduce(e.order_id, e.qty);
      case MsgType::Delete: return del(e.order_id);
      case MsgType::Modify: return modify(e.order_id, e.price, e.qty);
      default: return Status::BadType;
    }
  }

  Status add(std::uint16_t inst, std::uint64_t id, Side side, std::uint32_t price, std::uint32_t qty) {
    if (inst >= levels_.size()) return Status::BadInstrument;
    if (side != Side::Bid && side != Side::Ask) return Status::BadType;
    if (id == 0 || orders_.count(id)) return Status::DuplicateOrder;
    if (qty == 0) return Status::BadQty;
    if (!in_window(inst, price)) return Status::BadPrice;
    orders_[id] = Order{inst, side, price, qty};
    side_map(inst, side)[price].push_back(id);
    return Status::Ok;
  }
  Status reduce(std::uint64_t id, std::uint32_t qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return Status::UnknownOrder;
    if (qty == 0 || qty > it->second.qty) return Status::BadQty;
    it->second.qty -= qty;
    if (it->second.qty == 0) return del(id);
    return Status::Ok;
  }
  Status del(std::uint64_t id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return Status::UnknownOrder;
    erase_from_level(id, it->second);
    orders_.erase(it);
    return Status::Ok;
  }
  Status modify(std::uint64_t id, std::uint32_t price, std::uint32_t qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return Status::UnknownOrder;
    auto& o = it->second;
    if (qty == 0) return Status::BadQty;
    if (!in_window(o.inst, price)) return Status::BadPrice;
    if (price == o.price && qty <= o.qty) {
      o.qty = qty;
      return Status::Ok;
    }
    erase_from_level(id, o);
    o.price = price;
    o.qty = qty;
    side_map(o.inst, o.side)[price].push_back(id);
    return Status::Ok;
  }

  [[nodiscard]] Top top(std::uint16_t inst, Side s) const {
    const auto& m = side_map(inst, s);
    if (m.empty()) return {};
    const auto& kv = (s == Side::Bid) ? *m.rbegin() : *m.begin();
    return Top{kv.first, level_qty(kv.second), static_cast<std::uint32_t>(kv.second.size())};
  }

  [[nodiscard]] std::uint64_t digest() const {
    Digest d;
    for (std::uint16_t i = 0; i < levels_.size(); ++i) {
      visit_levels(i, Side::Bid, [&](std::uint32_t p, const std::vector<std::uint64_t>& ids) { mix(d, p, ids); });
      d.add(0xABCDEFull);
      visit_levels(i, Side::Ask, [&](std::uint32_t p, const std::vector<std::uint64_t>& ids) { mix(d, p, ids); });
      d.add(0xABCDEFull);
    }
    return d.h;
  }

  // Best-first levels of one side.
  template <typename F>
  void visit_levels(std::uint16_t inst, Side s, F&& f) const {
    const auto& m = side_map(inst, s);
    if (s == Side::Bid) {
      for (auto it = m.rbegin(); it != m.rend(); ++it) f(it->first, it->second);
    } else {
      for (const auto& kv : m) f(kv.first, kv.second);
    }
  }

  // Id of the order at the front of the queue at the best price (0 if empty).
  [[nodiscard]] std::uint64_t front_order(std::uint16_t inst, Side s) const {
    const auto& m = side_map(inst, s);
    if (m.empty()) return 0;
    return (s == Side::Bid ? m.rbegin()->second : m.begin()->second).front();
  }

  [[nodiscard]] const Order* find(std::uint64_t id) const {
    auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] std::size_t live_orders() const { return orders_.size(); }
  [[nodiscard]] std::size_t level_count(std::uint16_t inst, Side s) const { return side_map(inst, s).size(); }
  [[nodiscard]] const Session& session() const { return session_; }

 private:
  Session session_;
  std::unordered_map<std::uint64_t, Order> orders_;
  std::vector<std::array<LevelMap, 2>> levels_;

  [[nodiscard]] bool in_window(std::uint16_t inst, std::uint32_t price) const {
    return price >= session_.base_tick[inst] && price < session_.base_tick[inst] + kWindow;
  }
  LevelMap& side_map(std::uint16_t inst, Side s) { return levels_[inst][s == Side::Bid ? 0 : 1]; }
  [[nodiscard]] const LevelMap& side_map(std::uint16_t inst, Side s) const {
    return levels_[inst][s == Side::Bid ? 0 : 1];
  }
  [[nodiscard]] std::uint64_t level_qty(const std::vector<std::uint64_t>& ids) const {
    std::uint64_t q = 0;
    for (auto id : ids) q += orders_.at(id).qty;
    return q;
  }
  void erase_from_level(std::uint64_t id, const Order& o) {
    auto& m = side_map(o.inst, o.side);
    auto lit = m.find(o.price);
    auto& ids = lit->second;
    ids.erase(std::find(ids.begin(), ids.end(), id));
    if (ids.empty()) m.erase(lit);
  }
  void mix(Digest& d, std::uint32_t price, const std::vector<std::uint64_t>& ids) const {
    d.add(price);
    d.add(level_qty(ids));
    d.add(ids.size());
    for (auto id : ids) {
      d.add(id);
      d.add(orders_.at(id).qty);
    }
  }
};

}  // namespace mdr
