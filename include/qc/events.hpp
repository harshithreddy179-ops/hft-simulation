#pragma once
// Output events produced by the matching core.

#include <vector>

#include "qc/types.hpp"

namespace qc {

// A trade. Executes at the *maker's* resting price (price improvement goes to the aggressor).
struct Fill {
  TradeId trade_id = 0;  // monotonically increasing per order book
  SymbolId symbol = 0;
  Price price = 0;
  Qty qty = 0;
  OrderId maker_order = 0;
  OrderId taker_order = 0;
  ClientId maker_client = 0;
  ClientId taker_client = 0;
  Side taker_side = Side::Buy;
  Qty maker_leaves = 0;  // remaining qty of each order after this fill
  Qty taker_leaves = 0;
  Nanos ts = 0;
};

enum class RemovalReason : std::uint8_t {
  UserCancel,
  SelfTrade,  // removed by self-trade prevention
  Replaced    // old state of an order that was modified (it may come back with new qty/price)
};

// A resting order (or part of one) leaving the book without trading.
struct Removal {
  OrderId id = 0;
  ClientId client = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Price price = 0;
  Qty qty = 0;  // quantity removed
  RemovalReason reason = RemovalReason::UserCancel;
};

struct BookEvents {
  std::vector<Fill> fills;
  std::vector<Removal> removals;
  void clear() {
    fills.clear();
    removals.clear();
  }
};

enum class ExecType : std::uint8_t {
  New,          // order accepted
  PartialFill,  // traded, qty remains
  Fill,         // traded, nothing remains
  Cancelled,    // removed (user cancel / self-trade prevention)
  Expired,      // IOC / market remainder dropped
  Replaced,     // modify accepted
  Rejected,     // new order refused (terminal)
  CancelRejected  // cancel/modify refused; the order is unchanged and still live (not terminal)
};

constexpr const char* to_cstr(ExecType t) {
  switch (t) {
    case ExecType::New: return "NEW";
    case ExecType::PartialFill: return "PARTIAL_FILL";
    case ExecType::Fill: return "FILL";
    case ExecType::Cancelled: return "CANCELLED";
    case ExecType::Expired: return "EXPIRED";
    case ExecType::Replaced: return "REPLACED";
    case ExecType::Rejected: return "REJECTED";
    case ExecType::CancelRejected: return "CANCEL_REJECTED";
  }
  return "?";
}

enum class Liquidity : std::uint8_t { None, Maker, Taker };

// What a client sees about one of its orders. `leaves` is *always* the order's remaining
// quantity after the event, so a consumer can mirror order state without bookkeeping.
struct ExecReport {
  Nanos ts = 0;
  OrderId order_id = 0;
  ClientId client = 0;
  SymbolId symbol = 0;
  VenueId venue = 0;
  Side side = Side::Buy;
  ExecType type = ExecType::New;
  RejectReason reason = RejectReason::None;
  Liquidity liquidity = Liquidity::None;
  Price last_px = 0;   // fills only
  Qty last_qty = 0;    // fills only
  Qty leaves = 0;

  bool is_fill() const { return type == ExecType::Fill || type == ExecType::PartialFill; }
  // After a terminal report the order is gone (no more reports will follow for it).
  bool is_terminal() const { return leaves == 0 && type != ExecType::New && type != ExecType::CancelRejected; }
};

}  // namespace qc
