#pragma once
// Smart order router (SOR) and the consolidated multi-venue book it reads.
//
// Given a parent order and the latest depth snapshot from every venue, `route()` decides how many
// shares to send where. For the default BestPrice policy:
//
//   1. Flatten every visible contra-side level of every venue into candidates.
//   2. Price each candidate *all-in*:   buy  -> px * (1 + taker_fee)      sell -> px * (1 - taker_fee)
//      (fees are fractions of a tick at typical prices, so they mostly decide ties -- and they are
//      exactly what a naive "best displayed price" router gets wrong).
//   3. (Optional, EXPERIMENTAL, off by default.) Discount displayed size for staleness: expected size
//      = qty * exp(-(age + latency)/decay). Measured in the simulator it did not improve execution
//      cost in any regime tried (latency x1..x30, volatility 3..15 ticks/sqrt(s)) and can crater the
//      fill rate; see docs/EXPERIMENTS.md. Retrying unfilled remainders with fresh data works better.
//   4. Greedily take the cheapest candidates until the parent is filled or the limit price is
//      reached. Greedy-by-cost is optimal here because each level's cost is independent of the others.
//   5. Emit one IOC child per venue. Its limit is the worst price planned at that venue (plus an
//      optional slippage allowance), so a child can never trade through the plan.
//
// Two baselines exist for comparison: ProRata (split by displayed size, price-blind) and
// Primary (everything to one venue).
//
// route() is allocation-free (fixed-size arrays on the stack) and pure: same inputs, same plan.

#include <array>
#include <cstdint>
#include <vector>

#include "qc/types.hpp"

namespace qc {

inline constexpr std::size_t kDepthLevels = 5;
inline constexpr std::size_t kMaxVenues = 8;

struct LevelQuote {
  Price price = 0;
  Qty qty = 0;
};

// Top-N depth of one venue's book for one symbol, as published by the venue.
struct DepthSnapshot {
  VenueId venue = 0;
  SymbolId symbol = 0;
  Nanos venue_ts = 0;  // when the venue generated it
  std::uint8_t n_bids = 0;
  std::uint8_t n_asks = 0;
  std::array<LevelQuote, kDepthLevels> bids{};  // best first
  std::array<LevelQuote, kDepthLevels> asks{};  // best first
};

// The firm's view of the market: the latest snapshot per (symbol, venue), possibly stale.
class ConsolidatedBook {
 public:
  struct Best {
    bool valid = false;
    Price price = 0;
    Qty qty = 0;
    VenueId venue = 0;
  };

  // Out-of-order delivery is ignored: a snapshot older than the stored one is dropped.
  void update(const DepthSnapshot& s, Nanos recv_ts);
  const DepthSnapshot* snapshot(VenueId v, SymbolId s) const;
  Nanos received_at(VenueId v, SymbolId s) const;

  Best best_bid(SymbolId s) const;  // national best bid across venues
  Best best_ask(SymbolId s) const;
  bool mid(SymbolId s, double& mid_ticks) const;  // false until both sides exist
  bool crossed(SymbolId s) const;                 // best bid >= best ask across venues (arbitrage)

 private:
  struct Entry {
    DepthSnapshot snap;
    Nanos recv_ts = 0;
    bool valid = false;
  };
  const Entry* entry(VenueId v, SymbolId s) const;
  std::vector<std::vector<Entry>> entries_;  // [symbol][venue]
};

struct VenueInfo {
  VenueId id = 0;
  double taker_fee_bps = 0.0;
  double maker_fee_bps = 0.0;  // negative = rebate
  Nanos latency_in_ns = 0;     // one-way, firm -> venue
};

enum class RoutingPolicy : std::uint8_t { BestPrice, ProRata, Primary };
constexpr const char* to_cstr(RoutingPolicy p) {
  switch (p) {
    case RoutingPolicy::BestPrice: return "smart";
    case RoutingPolicy::ProRata: return "prorata";
    case RoutingPolicy::Primary: return "primary";
  }
  return "?";
}

struct SorConfig {
  RoutingPolicy policy = RoutingPolicy::BestPrice;
  double stale_decay_ns = 0.0;  // EXPERIMENTAL; 0 = trust displayed size; else qty *= exp(-(age+latency)/decay)
  Price slippage_ticks = 0;     // child limit = worst planned price +/- this
  VenueId primary = 0;          // Primary policy target
  Qty min_child_qty = 1;        // drop dust children
};

struct RouteRequest {
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Qty qty = 0;
  Price limit = 0;  // 0 = no limit price
  Nanos now = 0;
};

struct ChildPlan {
  VenueId venue = 0;
  Qty qty = 0;
  Price limit = 0;          // IOC limit for the child order
  double est_cost = 0.0;    // fee-adjusted notional in tick*share units (side-signed cost, buy positive)
  double est_avg_px = 0.0;  // ticks, raw (no fees)
};

struct RoutePlan {
  std::array<ChildPlan, kMaxVenues> child{};
  std::uint8_t n_children = 0;
  Qty planned = 0;
  Qty unplanned = 0;      // could not be placed within the limit / visible liquidity
  double est_cost = 0.0;  // sum of child est_cost
  const ChildPlan* begin() const { return child.data(); }
  const ChildPlan* end() const { return child.data() + n_children; }
};

class SmartOrderRouter {
 public:
  SmartOrderRouter(std::vector<VenueInfo> venues, SorConfig cfg = {});

  RoutePlan route(const RouteRequest& rq, const ConsolidatedBook& book) const;

  const SorConfig& config() const { return cfg_; }
  void set_policy(RoutingPolicy p) { cfg_.policy = p; }
  const std::vector<VenueInfo>& venues() const { return venues_; }

 private:
  RoutePlan route_best_price(const RouteRequest& rq, const ConsolidatedBook& book) const;
  RoutePlan route_pro_rata(const RouteRequest& rq, const ConsolidatedBook& book) const;
  RoutePlan route_primary(const RouteRequest& rq, const ConsolidatedBook& book) const;

  std::vector<VenueInfo> venues_;
  SorConfig cfg_;
};

}  // namespace qc
