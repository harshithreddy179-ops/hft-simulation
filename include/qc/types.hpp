#pragma once
// Core vocabulary types shared by every module.
//
// Conventions
//   * Prices are integer ticks (1 tick = 1 / kTicksPerUnit currency units). Integer prices
//     make level lookup exact and comparisons branch-free; floating point is only used for
//     money amounts (P&L, fees, notionals), which are always expressed in currency units.
//   * Quantities are signed 64-bit so position arithmetic never needs casts.
//   * Time is nanoseconds. In the simulator it is simulated time; in the benches it is wall time.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace qc {

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using ClientId = std::uint32_t;
using SymbolId = std::uint32_t;
using VenueId = std::uint32_t;
using Nanos = std::int64_t;

inline constexpr double kTicksPerUnit = 100.0;                  // tick = 0.01
inline constexpr Price kMaxPrice = std::numeric_limits<Price>::max() / 4;
inline constexpr OrderId kInvalidOrderId = ~OrderId{0};
inline constexpr SymbolId kMaxSymbols = 1u << 16;
// Hard sanity bounds so price*qty can never overflow int64 (1e9 * 1e9 = 1e18 < 9.2e18).
inline constexpr Price kMaxLimitPrice = 1'000'000'000;
inline constexpr Qty kMaxQty = 1'000'000'000;

inline constexpr Nanos kMicro = 1'000;
inline constexpr Nanos kMilli = 1'000'000;
inline constexpr Nanos kSecond = 1'000'000'000;

inline double to_units(Price ticks) { return static_cast<double>(ticks) / kTicksPerUnit; }
inline Price to_ticks(double units) { return static_cast<Price>(std::llround(units * kTicksPerUnit)); }
// Money amount (currency units) of `qty` shares at `px` ticks.
inline double notional(Price px, Qty qty) { return static_cast<double>(px) * static_cast<double>(qty) / kTicksPerUnit; }

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
constexpr int sign(Side s) { return s == Side::Buy ? 1 : -1; }
constexpr std::string_view to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
constexpr std::string_view to_string(OrderType t) { return t == OrderType::Limit ? "LIMIT" : "MARKET"; }

enum class TimeInForce : std::uint8_t {
  GTC = 0,  // good till cancelled: remainder rests on the book
  IOC = 1,  // immediate or cancel: remainder is dropped
  FOK = 2   // fill or kill: all or nothing
};
constexpr std::string_view to_string(TimeInForce t) {
  switch (t) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
  }
  return "?";
}

// Self-trade prevention: what to do when an aggressor would match a resting order of the same client.
enum class StpMode : std::uint8_t {
  None = 0,           // allow self trades
  CancelResting = 1,  // cancel the resting order, keep matching the aggressor
  CancelTaker = 2     // stop matching, cancel the aggressor's remainder
};

// One enum for every way an order can be refused (book validation + pre-trade risk), so an
// execution report needs a single reason field.
enum class RejectReason : std::uint8_t {
  None = 0,
  // order validation / book
  InvalidQty,
  InvalidPrice,
  InvalidOrder,
  DuplicateOrderId,
  UnknownOrder,
  UnknownSymbol,
  NotOrderOwner,
  PostOnlyWouldCross,
  FokUnfillable,
  // pre-trade risk
  RiskKillSwitch,
  RiskSymbolHalted,
  RiskOrderSize,
  RiskOrderNotional,
  RiskPriceCollar,
  RiskRateLimit,
  RiskOpenOrders,
  RiskPositionLimit,
  RiskGrossExposure,
  // routing
  NoLiquidity,
  Count_
};

constexpr std::string_view to_string(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "NONE";
    case RejectReason::InvalidQty: return "INVALID_QTY";
    case RejectReason::InvalidPrice: return "INVALID_PRICE";
    case RejectReason::InvalidOrder: return "INVALID_ORDER";
    case RejectReason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    case RejectReason::UnknownSymbol: return "UNKNOWN_SYMBOL";
    case RejectReason::NotOrderOwner: return "NOT_ORDER_OWNER";
    case RejectReason::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
    case RejectReason::FokUnfillable: return "FOK_UNFILLABLE";
    case RejectReason::RiskKillSwitch: return "RISK_KILL_SWITCH";
    case RejectReason::RiskSymbolHalted: return "RISK_SYMBOL_HALTED";
    case RejectReason::RiskOrderSize: return "RISK_ORDER_SIZE";
    case RejectReason::RiskOrderNotional: return "RISK_ORDER_NOTIONAL";
    case RejectReason::RiskPriceCollar: return "RISK_PRICE_COLLAR";
    case RejectReason::RiskRateLimit: return "RISK_RATE_LIMIT";
    case RejectReason::RiskOpenOrders: return "RISK_OPEN_ORDERS";
    case RejectReason::RiskPositionLimit: return "RISK_POSITION_LIMIT";
    case RejectReason::RiskGrossExposure: return "RISK_GROSS_EXPOSURE";
    case RejectReason::NoLiquidity: return "NO_LIQUIDITY";
    case RejectReason::Count_: break;
  }
  return "?";
}

constexpr bool is_risk_reject(RejectReason r) {
  return r >= RejectReason::RiskKillSwitch && r <= RejectReason::RiskGrossExposure;
}

struct Order {
  OrderId id = 0;
  ClientId client = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  bool post_only = false;  // reject instead of crossing the book (maker-only)
  Price price = 0;         // ticks; ignored for market orders
  Qty qty = 0;
  Nanos ts = 0;
};

enum class CommandType : std::uint8_t { New = 0, Cancel = 1, Modify = 2 };
constexpr std::string_view to_string(CommandType t) {
  switch (t) {
    case CommandType::New: return "NEW";
    case CommandType::Cancel: return "CANCEL";
    case CommandType::Modify: return "MODIFY";
  }
  return "?";
}

// An inbound message. New: `order` is the full order. Cancel: order.{id,client,symbol,ts} are used.
// Modify: order.{id,client,symbol,ts} identify the order; order.price / order.qty are the new
// price and the new *remaining* quantity.
struct Command {
  CommandType type = CommandType::New;
  Order order;
};

}  // namespace qc
