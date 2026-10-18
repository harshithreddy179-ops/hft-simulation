#pragma once
// Order management system (the firm side of the simulation).
//
//   Strategy ──send()──► RiskEngine::check ──► SOR (parent -> IOC children) ──► venues
//      ▲                                                                           │
//      └── on_exec / on_parent_done ◄── fills, fees, per-strategy P&L ◄── reports ◄┘
//
// * Parent orders: a strategy asks for "buy 2,000 with limit L". With Algo::Sor the OMS routes IOC
//   children across venues. Because market data lags (stale quotes / phantom liquidity), a round
//   often fills only part of the parent; the OMS then waits `retry_delay` for fresh data and routes
//   the remainder, up to `max_rounds`. Algo::Direct sends one order to one venue (market making).
// * Risk is checked once per parent (rate token + limits) and every child is tracked for exposure
//   from the moment it is sent, so in-flight orders already count against position limits.
// * P&L is attributed per strategy; the daily-loss kill switch watches the firm total and, when it
//   trips, cancels every open order.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "qc/latency.hpp"
#include "qc/portfolio.hpp"
#include "qc/risk.hpp"
#include "qc/router.hpp"
#include "qc/sim_types.hpp"

namespace qc {

enum class Algo : std::uint8_t { Sor, Direct };

struct OrderRequest {
  StrategyId strategy = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Qty qty = 0;
  Price limit = 0;                      // 0 = no limit (Sor only)
  Algo algo = Algo::Sor;
  VenueId venue = 0;                    // Direct only
  TimeInForce tif = TimeInForce::GTC;   // Direct only (Sor children are always IOC)
  bool post_only = false;               // Direct only
};

struct ParentResult {
  OrderId id = 0;
  StrategyId strategy = 0;
  SymbolId symbol = 0;
  Side side = Side::Buy;
  Qty qty = 0;
  Qty filled = 0;
  double avg_px_ticks = 0.0;
  double arrival_mid_ticks = 0.0;  // consolidated mid when the parent was submitted (0 = unknown)
  double fees = 0.0;               // currency units (negative = net rebate)
  int rounds = 0;
  int children = 0;
  Nanos start = 0;
  Nanos end = 0;

  double fill_rate() const { return qty > 0 ? static_cast<double>(filled) / static_cast<double>(qty) : 0.0; }
  // Implementation shortfall of the filled part vs the arrival mid, in basis points (positive = cost).
  double slippage_bps() const {
    if (filled == 0 || arrival_mid_ticks <= 0.0) return 0.0;
    return sign(side) * (avg_px_ticks - arrival_mid_ticks) / arrival_mid_ticks * 1e4;
  }
  double fee_bps() const {
    const double n = notional(static_cast<Price>(avg_px_ticks), filled);
    return n > 0 ? fees / n * 1e4 : 0.0;
  }
  double total_cost_bps() const { return slippage_bps() + fee_bps(); }
  // Currency cost of the filled shares vs arrival mid (price impact + fees); positive = cost.
  double cost_currency() const {
    if (filled == 0 || arrival_mid_ticks <= 0.0) return fees;
    return sign(side) * (avg_px_ticks - arrival_mid_ticks) * static_cast<double>(filled) / kTicksPerUnit + fees;
  }
};

class Oms;

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual const char* name() const = 0;
  virtual void on_start(Oms&) {}
  virtual void on_timer(Oms&, std::uint32_t /*timer_id*/) {}
  virtual void on_exec(Oms&, const ExecReport&) {}
  virtual void on_parent_done(Oms&, const ParentResult&) {}
  StrategyId id = 0;
};

struct OmsConfig {
  ClientId client = kFirmClient;
  int max_rounds = 4;
  Nanos retry_delay = 300 * kMicro;
};

struct VenueFirmStats {
  std::string name;
  std::uint64_t orders_sent = 0;
  std::uint64_t fills = 0;
  Qty qty = 0;
  Qty maker_qty = 0;
  Qty taker_qty = 0;
  double fees = 0.0;
  LatencyHistogram round_trip;  // send -> first report back, simulated ns
};

class Oms {
 public:
  Oms(OmsConfig cfg, EventQueue& q, Network& net, const std::vector<VenueConfig>& venues, RiskEngine& risk,
      SmartOrderRouter& sor);

  void add_strategy(Strategy* s);
  void start();

  // ---- API for strategies ------------------------------------------------------------------
  Nanos now() const { return q_.now(); }
  const ConsolidatedBook& book() const { return book_; }
  const MarkTable& marks() const { return marks_; }
  const Portfolio& portfolio(StrategyId s) const { return portfolios_[s]; }
  Qty position(StrategyId s, SymbolId sym) const { return portfolios_[s].position(sym); }
  Qty firm_position(SymbolId sym) const;
  const OmsConfig& config() const { return cfg_; }

  // Returns the order id (usable with cancel/modify for Direct orders) or 0 if risk rejected it.
  OrderId send(const OrderRequest& req, RejectReason* why = nullptr);
  bool cancel(OrderId id);
  bool modify(OrderId id, Price new_price, Qty new_qty);
  void set_timer(StrategyId s, Nanos delay, std::uint32_t timer_id);

  // ---- hooks called by the simulator ---------------------------------------------------------
  void on_report(const ExecReport& r);
  void on_quote(const DepthSnapshot& s);
  void on_timer(const EvTimer& t);
  void on_retry(OrderId parent);
  void on_sample();

  // ---- results ----------------------------------------------------------------------------
  const std::vector<ParentResult>& results() const { return results_; }
  const std::vector<VenueFirmStats>& venue_stats() const { return venue_stats_; }
  const std::vector<std::pair<Nanos, double>>& equity_curve() const { return equity_; }
  const std::vector<std::vector<std::pair<Nanos, double>>>& strategy_equity() const { return strat_equity_; }
  double firm_net_pnl() const;
  std::uint64_t kills() const { return kills_; }
  Nanos kill_time() const { return kill_time_; }                      // -1 if never killed
  Nanos last_fill_after_kill() const { return last_fill_after_kill_; }  // 0 if none
  std::uint64_t rejected_requests() const { return rejected_requests_; }
  std::size_t strategies() const { return strategies_.size(); }
  Strategy* strategy(StrategyId s) { return strategies_[s]; }

 private:
  struct Parent {
    OrderId id = 0;
    OrderRequest req;
    Qty filled = 0;
    double notional_ticks = 0.0;  // sum price_ticks * qty
    double fees = 0.0;
    double arrival_mid = 0.0;
    Nanos start = 0;
    int rounds = 0;
    int outstanding = 0;
    int children = 0;
  };
  struct Child {
    OrderId parent = 0;
    VenueId venue = 0;
    SymbolId symbol = 0;
    Side side = Side::Buy;
    Qty leaves = 0;
    Nanos sent_at = 0;
    bool reported = false;
  };

  void route_round(Parent& p);
  void finalize(Parent& p);
  void send_child(Parent& p, const Order& o);
  void dispatch(VenueId v, const Command& cmd);
  void cancel_all();
  double fee_for(const ExecReport& r) const;

  OmsConfig cfg_;
  EventQueue& q_;
  Network& net_;
  const std::vector<VenueConfig>& venues_;
  RiskEngine& risk_;
  SmartOrderRouter& sor_;

  std::vector<Strategy*> strategies_;
  std::vector<Portfolio> portfolios_;
  ConsolidatedBook book_;
  MarkTable marks_;
  OrderId next_id_ = 1;
  std::unordered_map<OrderId, Parent> parents_;
  std::unordered_map<OrderId, Child> children_;
  std::vector<ParentResult> results_;
  std::vector<VenueFirmStats> venue_stats_;
  std::vector<std::pair<Nanos, double>> equity_;
  std::vector<std::vector<std::pair<Nanos, double>>> strat_equity_;
  std::uint64_t kills_ = 0;
  Nanos kill_time_ = -1;
  Nanos last_fill_after_kill_ = 0;
  std::uint64_t rejected_requests_ = 0;
};

}  // namespace qc
