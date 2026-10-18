#pragma once
// Single-symbol limit order book with strict price-time priority.
//
// Data structure
//   * Each side is a std::map<key, Level>. Bids are keyed by -price so that begin() is the best
//     price on *both* sides and the matching code is side-agnostic. std::map iterators/pointers
//     are stable, so orders keep a direct Level* and cancel never searches for the level.
//   * Orders live in a pooled array (indices, not pointers) and form an intrusive FIFO list per
//     level: add/cancel/fill are O(1) after the id lookup; no per-order heap allocation after warm-up.
//   * id -> pool slot is a FlatIdMap (open addressing).
//
// The book is single-threaded by design (single-writer principle). Concurrency is achieved by
// sharding books across threads (see sharded_exchange.hpp), never by locking one book.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "qc/events.hpp"
#include "qc/flat_id_map.hpp"
#include "qc/types.hpp"

namespace qc {

enum class OrderStatus : std::uint8_t {
  Rejected,  // nothing happened; see reason
  Resting,   // (part of) the order rests on the book; `filled` may be > 0
  Filled,    // fully executed on arrival
  Expired    // IOC/market/self-trade remainder dropped; `filled` may be > 0
};

struct SubmitResult {
  OrderStatus status = OrderStatus::Rejected;
  RejectReason reason = RejectReason::None;
  Qty filled = 0;          // executed on arrival
  Qty leaves = 0;          // now resting
  Qty cancelled = 0;       // dropped (Expired)
  std::int64_t filled_value = 0;  // sum(price_ticks * qty); VWAP = filled_value / filled
};

struct BookConfig {
  StpMode stp = StpMode::None;
  std::size_t reserve_orders = 4096;
};

class OrderBook {
 public:
  struct DepthLevel {
    Price price = 0;
    Qty qty = 0;
    std::uint32_t orders = 0;
  };
  struct OrderInfo {
    OrderId id = 0;
    ClientId client = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty leaves = 0;
    Nanos ts = 0;
  };
  struct Top {
    bool has_bid = false, has_ask = false;
    Price bid = 0, ask = 0;
    Qty bid_qty = 0, ask_qty = 0;
  };

  explicit OrderBook(SymbolId symbol, BookConfig cfg = {});

  SymbolId symbol() const { return symbol_; }

  // Match an order against the book, resting any GTC remainder. Fills/removals are appended to `ev`.
  SubmitResult submit(const Order& order, BookEvents& ev);

  // Remove a resting order. Appends a Removal(UserCancel). Returns false if the id is not live.
  bool cancel(OrderId id, BookEvents& ev);

  // Amend a resting order. `new_qty` is the new *remaining* quantity.
  //   same price and new_qty <= leaves  -> amended in place, time priority is kept
  //   otherwise                          -> cancel/replace: loses priority and may match immediately
  // Always appends Removal(Replaced, old leaves); the returned result describes the order afterwards.
  SubmitResult modify(OrderId id, Price new_price, Qty new_qty, Nanos ts, BookEvents& ev);

  // --- queries -----------------------------------------------------------------------------
  std::optional<OrderInfo> find(OrderId id) const;
  Top top() const;
  Qty volume_at(Side side, Price price) const;
  void depth(Side side, std::size_t max_levels, std::vector<DepthLevel>& out) const;
  std::size_t order_count() const { return index_.size(); }
  std::size_t level_count(Side side) const { return side_book(side).size(); }
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;

  // Structural self-check used by tests; returns false and fills `why` on the first violation.
  bool validate(std::string* why = nullptr) const;

 private:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  struct Level;
  struct Node {
    OrderId id = 0;
    Nanos ts = 0;
    Qty leaves = 0;
    Level* level = nullptr;
    ClientId client = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;
    Side side = Side::Buy;
  };
  struct Level {
    Price price = 0;
    Qty qty = 0;
    std::uint32_t count = 0;
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
  };
  using LevelMap = std::map<Price, Level>;

  static Price key_of(Side side, Price price) { return side == Side::Buy ? -price : price; }
  static bool crosses(Side taker, Price limit, Price level_price) {
    return taker == Side::Buy ? level_price <= limit : level_price >= limit;
  }

  LevelMap& side_book(Side s) { return s == Side::Buy ? bids_ : asks_; }
  const LevelMap& side_book(Side s) const { return s == Side::Buy ? bids_ : asks_; }

  RejectReason validate_order(const Order& o) const;
  Qty fillable(const Order& o, Price limit, Qty need) const;
  void match(const Order& o, Price limit, Qty& remaining, BookEvents& ev, SubmitResult& r, bool& taker_stopped);
  void rest(const Order& o, Qty qty);
  void unlink(std::uint32_t idx);
  std::uint32_t alloc_node();
  void erase_level(Side side, Price price) { side_book(side).erase(key_of(side, price)); }

  SymbolId symbol_;
  BookConfig cfg_;
  LevelMap bids_;  // key = -price
  LevelMap asks_;  // key = +price
  std::vector<Node> nodes_;
  std::uint32_t free_head_ = kNil;
  FlatIdMap index_;
  TradeId trade_seq_ = 0;
};

}  // namespace qc
