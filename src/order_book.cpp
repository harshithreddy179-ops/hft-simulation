#include "qc/order_book.hpp"

#include <algorithm>
#include <sstream>

namespace qc {

OrderBook::OrderBook(SymbolId symbol, BookConfig cfg)
    : symbol_(symbol), cfg_(cfg), index_(cfg.reserve_orders * 2) {
  nodes_.reserve(cfg.reserve_orders);
}

// ---------------------------------------------------------------------------------------------
// validation
// ---------------------------------------------------------------------------------------------

RejectReason OrderBook::validate_order(const Order& o) const {
  if (o.qty <= 0 || o.qty > kMaxQty) return RejectReason::InvalidQty;
  if (o.id == kInvalidOrderId) return RejectReason::InvalidOrder;
  if (o.type == OrderType::Limit && (o.price <= 0 || o.price > kMaxLimitPrice)) return RejectReason::InvalidPrice;
  // A post-only order must be a resting limit order.
  if (o.post_only && (o.type != OrderType::Limit || o.tif != TimeInForce::GTC)) return RejectReason::InvalidOrder;
  if (index_.contains(o.id)) return RejectReason::DuplicateOrderId;
  return RejectReason::None;
}

// ---------------------------------------------------------------------------------------------
// node pool
// ---------------------------------------------------------------------------------------------

std::uint32_t OrderBook::alloc_node() {
  if (free_head_ != kNil) {
    const std::uint32_t idx = free_head_;
    free_head_ = nodes_[idx].next;
    return idx;
  }
  nodes_.emplace_back();
  return static_cast<std::uint32_t>(nodes_.size() - 1);
}

// Detach a node from its level's FIFO and recycle it. Does NOT erase an emptied level: callers
// decide when (the matching loop holds an iterator to the level it is draining).
void OrderBook::unlink(std::uint32_t idx) {
  Node& n = nodes_[idx];
  Level& lvl = *n.level;
  if (n.prev != kNil) nodes_[n.prev].next = n.next; else lvl.head = n.next;
  if (n.next != kNil) nodes_[n.next].prev = n.prev; else lvl.tail = n.prev;
  lvl.qty -= n.leaves;
  --lvl.count;
  index_.erase(n.id);
  n.level = nullptr;
  n.leaves = 0;
  n.prev = kNil;
  n.next = free_head_;
  free_head_ = idx;
}

void OrderBook::rest(const Order& o, Qty qty) {
  LevelMap& book = side_book(o.side);
  auto [it, inserted] = book.try_emplace(key_of(o.side, o.price));
  Level& lvl = it->second;
  if (inserted) lvl.price = o.price;

  const std::uint32_t idx = alloc_node();
  Node& n = nodes_[idx];
  n.id = o.id;
  n.ts = o.ts;
  n.leaves = qty;
  n.level = &lvl;
  n.client = o.client;
  n.side = o.side;
  n.prev = lvl.tail;
  n.next = kNil;
  if (lvl.tail != kNil) nodes_[lvl.tail].next = idx; else lvl.head = idx;
  lvl.tail = idx;
  lvl.qty += qty;
  ++lvl.count;
  index_.insert(o.id, idx);
}

// ---------------------------------------------------------------------------------------------
// matching
// ---------------------------------------------------------------------------------------------

// How much of `need` could `o` execute right now (stops counting once `need` is reached)?
// With self-trade prevention the answer depends on who owns the resting orders.
Qty OrderBook::fillable(const Order& o, Price limit, Qty need) const {
  const LevelMap& opp = side_book(opposite(o.side));
  Qty avail = 0;
  for (const auto& [key, lvl] : opp) {
    if (!crosses(o.side, limit, lvl.price)) break;
    if (cfg_.stp == StpMode::None) {
      avail += lvl.qty;
    } else {
      for (std::uint32_t i = lvl.head; i != kNil; i = nodes_[i].next) {
        const Node& n = nodes_[i];
        if (n.client == o.client) {
          if (cfg_.stp == StpMode::CancelTaker) return avail;  // taker would be stopped here
          continue;                                          // CancelResting: skipped, will be cancelled
        }
        avail += n.leaves;
      }
    }
    if (avail >= need) return avail;
  }
  return avail;
}

void OrderBook::match(const Order& o, Price limit, Qty& remaining, BookEvents& ev, SubmitResult& r,
                      bool& taker_stopped) {
  LevelMap& opp = side_book(opposite(o.side));
  while (remaining > 0 && !opp.empty()) {
    auto it = opp.begin();
    Level& lvl = it->second;
    if (!crosses(o.side, limit, lvl.price)) break;

    std::uint32_t idx = lvl.head;
    while (idx != kNil && remaining > 0) {
      Node& n = nodes_[idx];
      const std::uint32_t next = n.next;  // n is recycled below

      if (cfg_.stp != StpMode::None && n.client == o.client) {
        if (cfg_.stp == StpMode::CancelTaker) {
          taker_stopped = true;
          break;
        }
        ev.removals.push_back(Removal{n.id, n.client, symbol_, n.side, lvl.price, n.leaves, RemovalReason::SelfTrade});
        unlink(idx);
        idx = next;
        continue;
      }

      const Qty q = std::min(remaining, n.leaves);
      n.leaves -= q;
      lvl.qty -= q;
      remaining -= q;
      r.filled += q;
      r.filled_value += lvl.price * q;
      ev.fills.push_back(Fill{++trade_seq_, symbol_, lvl.price, q, n.id, o.id, n.client, o.client, o.side,
                              n.leaves, remaining, o.ts});
      if (n.leaves == 0) unlink(idx);
      idx = next;
    }

    if (lvl.count == 0) opp.erase(it);
    if (taker_stopped) break;
  }
}

SubmitResult OrderBook::submit(const Order& o, BookEvents& ev) {
  SubmitResult r;
  if (const RejectReason why = validate_order(o); why != RejectReason::None) {
    r.reason = why;
    return r;
  }

  // Market orders are limit orders at an unreachable price that never rest.
  const bool is_market = o.type == OrderType::Market;
  const Price limit = is_market ? (o.side == Side::Buy ? kMaxPrice : Price{0}) : o.price;
  const TimeInForce tif = is_market && o.tif == TimeInForce::GTC ? TimeInForce::IOC : o.tif;

  if (o.post_only) {
    const LevelMap& opp = side_book(opposite(o.side));
    if (!opp.empty() && crosses(o.side, limit, opp.begin()->second.price)) {
      r.reason = RejectReason::PostOnlyWouldCross;
      return r;
    }
  }
  if (tif == TimeInForce::FOK && fillable(o, limit, o.qty) < o.qty) {
    r.reason = RejectReason::FokUnfillable;
    return r;
  }

  Qty remaining = o.qty;
  bool taker_stopped = false;
  match(o, limit, remaining, ev, r, taker_stopped);

  if (remaining == 0) {
    r.status = OrderStatus::Filled;
  } else if (!is_market && tif == TimeInForce::GTC && !taker_stopped) {
    rest(o, remaining);
    r.status = OrderStatus::Resting;
    r.leaves = remaining;
  } else {
    r.status = OrderStatus::Expired;
    r.cancelled = remaining;
  }
  return r;
}

// ---------------------------------------------------------------------------------------------
// cancel / modify
// ---------------------------------------------------------------------------------------------

bool OrderBook::cancel(OrderId id, BookEvents& ev) {
  const std::uint32_t* slot = index_.find(id);
  if (!slot) return false;
  const std::uint32_t idx = *slot;
  const Node& n = nodes_[idx];
  const Price price = n.level->price;
  const Side side = n.side;
  ev.removals.push_back(Removal{n.id, n.client, symbol_, side, price, n.leaves, RemovalReason::UserCancel});
  Level* lvl = n.level;
  unlink(idx);
  if (lvl->count == 0) erase_level(side, price);
  return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Qty new_qty, Nanos ts, BookEvents& ev) {
  SubmitResult r;
  const std::uint32_t* slot = index_.find(id);
  if (!slot) {
    r.reason = RejectReason::UnknownOrder;
    return r;
  }
  if (new_qty <= 0 || new_qty > kMaxQty) {
    r.reason = RejectReason::InvalidQty;
    return r;
  }
  if (new_price <= 0 || new_price > kMaxLimitPrice) {
    r.reason = RejectReason::InvalidPrice;
    return r;
  }

  const std::uint32_t idx = *slot;
  Node& n = nodes_[idx];
  Level* lvl = n.level;
  const Price old_price = lvl->price;
  const Qty old_leaves = n.leaves;
  const Side side = n.side;
  ev.removals.push_back(Removal{n.id, n.client, symbol_, side, old_price, old_leaves, RemovalReason::Replaced});

  if (new_price == old_price && new_qty <= old_leaves) {
    // In-place reduction keeps queue position.
    lvl->qty -= old_leaves - new_qty;
    n.leaves = new_qty;
    r.status = OrderStatus::Resting;
    r.leaves = new_qty;
    return r;
  }

  Order replacement;
  replacement.id = id;
  replacement.client = n.client;
  replacement.symbol = symbol_;
  replacement.side = side;
  replacement.type = OrderType::Limit;
  replacement.tif = TimeInForce::GTC;
  replacement.price = new_price;
  replacement.qty = new_qty;
  replacement.ts = ts;

  unlink(idx);
  if (lvl->count == 0) erase_level(side, old_price);
  return submit(replacement, ev);
}

// ---------------------------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------------------------

std::optional<OrderBook::OrderInfo> OrderBook::find(OrderId id) const {
  const std::uint32_t* slot = index_.find(id);
  if (!slot) return std::nullopt;
  const Node& n = nodes_[*slot];
  return OrderInfo{n.id, n.client, n.side, n.level->price, n.leaves, n.ts};
}

OrderBook::Top OrderBook::top() const {
  Top t;
  if (!bids_.empty()) {
    const Level& l = bids_.begin()->second;
    t.has_bid = true;
    t.bid = l.price;
    t.bid_qty = l.qty;
  }
  if (!asks_.empty()) {
    const Level& l = asks_.begin()->second;
    t.has_ask = true;
    t.ask = l.price;
    t.ask_qty = l.qty;
  }
  return t;
}

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->second.price;
}
std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->second.price;
}

Qty OrderBook::volume_at(Side side, Price price) const {
  const LevelMap& m = side_book(side);
  const auto it = m.find(key_of(side, price));
  return it == m.end() ? 0 : it->second.qty;
}

void OrderBook::depth(Side side, std::size_t max_levels, std::vector<DepthLevel>& out) const {
  out.clear();
  for (const auto& [key, lvl] : side_book(side)) {
    if (out.size() >= max_levels) break;
    out.push_back(DepthLevel{lvl.price, lvl.qty, lvl.count});
  }
}

bool OrderBook::validate(std::string* why) const {
  auto fail = [&](const std::string& msg) {
    if (why) *why = msg;
    return false;
  };
  std::size_t live = 0;
  for (const Side side : {Side::Buy, Side::Sell}) {
    const LevelMap& m = side_book(side);
    for (const auto& [key, lvl] : m) {
      if (key != key_of(side, lvl.price)) return fail("level key does not match price");
      if (lvl.count == 0 || lvl.head == kNil || lvl.tail == kNil) return fail("empty level left in map");
      Qty sum = 0;
      std::uint32_t cnt = 0;
      std::uint32_t prev = kNil;
      for (std::uint32_t i = lvl.head; i != kNil; i = nodes_[i].next) {
        const Node& n = nodes_[i];
        if (n.prev != prev) return fail("broken prev link");
        if (n.level != &lvl) return fail("node points at wrong level");
        if (n.side != side) return fail("node on wrong side");
        if (n.leaves <= 0) return fail("non-positive leaves on live order");
        const std::uint32_t* slot = index_.find(n.id);
        if (!slot || *slot != i) return fail("index does not point at node");
        sum += n.leaves;
        ++cnt;
        prev = i;
      }
      if (prev != lvl.tail) return fail("tail mismatch");
      if (sum != lvl.qty) return fail("level qty != sum of order leaves");
      if (cnt != lvl.count) return fail("level count mismatch");
      live += cnt;
    }
  }
  if (live != index_.size()) return fail("index size != live orders");
  std::size_t free_n = 0;
  for (std::uint32_t i = free_head_; i != kNil; i = nodes_[i].next) ++free_n;
  if (free_n + live != nodes_.size()) return fail("node pool leak");
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->second.price >= asks_.begin()->second.price) {
    return fail("book is crossed");
  }
  return true;
}

}  // namespace qc
