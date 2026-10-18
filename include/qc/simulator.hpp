#pragma once
// Discrete-event multi-venue market simulation.
//
//   noise traders ──► Venue 0..N (matching engines, fees, latency) ──► depth snapshots ──► firm's
//   (random walk fundamental,                                          consolidated book (stale!)
//    per-venue quote offsets)        ▲                                       │
//                                    │ orders (latency+jitter, FIFO)          ▼
//                                    └──────── OMS: risk ─ SOR ◄──── strategies (MM, TWAP, parent flow)
//
// Everything is driven by one seeded event queue, so a run is exactly reproducible. Simulated time
// is independent of wall-clock time: a 10 s simulation takes well under a second to compute.

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "qc/engine.hpp"
#include "qc/flow.hpp"
#include "qc/oms.hpp"
#include "qc/risk.hpp"
#include "qc/router.hpp"
#include "qc/sim_types.hpp"
#include "qc/strategies.hpp"

namespace qc {

std::vector<VenueConfig> default_venues();

struct SimConfig {
  std::uint64_t seed = 42;
  Nanos duration = 10 * kSecond;
  std::vector<VenueConfig> venues;  // empty => default_venues()
  FlowConfig flow;
  RiskLimits firm_limits;
  SorConfig sor;
  OmsConfig oms;
  bool enable_market_maker = true;
  MarketMakerConfig mm;
  bool enable_parent_flow = true;
  ParentFlowConfig parent_flow;
  bool enable_twap = true;
  TwapConfig twap;
  Nanos sample_interval = 100 * kMilli;

  SimConfig();  // sets sensible firm risk limits
};

struct StrategyReport {
  std::string name;
  Qty position = 0;
  double realized = 0.0, unrealized = 0.0, fees = 0.0, net = 0.0, max_drawdown = 0.0;
  std::size_t fills = 0;
  Qty volume = 0;
};

struct ExecutionStats {
  std::size_t parents = 0;
  Qty requested = 0;
  Qty filled = 0;
  double fill_rate = 0.0;          // filled / requested
  double slippage_bps = 0.0;       // qty-weighted vs arrival mid (price only)
  double fee_bps = 0.0;            // qty-weighted
  double total_cost_bps = 0.0;
  double cost_currency = 0.0;      // total price impact + fees vs arrival mid
  double avg_children = 0.0;
  double avg_rounds = 0.0;
  double avg_latency_us = 0.0;     // parent send -> completion, simulated time
};

struct SimReport {
  RoutingPolicy policy = RoutingPolicy::BestPrice;
  std::uint64_t seed = 0;
  Nanos duration = 0;
  std::uint64_t events = 0;
  double wall_seconds = 0.0;

  std::vector<StrategyReport> strategies;
  double firm_net = 0.0;
  ExecutionStats execution;
  std::vector<ParentResult> parents;
  bool has_twap = false;
  TwapSummary twap;
  std::vector<VenueFirmStats> venues;
  std::vector<std::uint64_t> noise_trades;  // background trades per venue

  double avg_spread_ticks = 0.0;
  double crossed_fraction = 0.0;  // share of samples where the consolidated book was crossed
  std::uint64_t risk_rejects = 0;
  std::vector<std::pair<RejectReason, std::uint64_t>> reject_breakdown;
  Qty firm_position = 0;      // sum of strategy positions (portfolio view)
  Qty risk_position = 0;      // the risk engine's independent view of the same number
  std::size_t open_orders = 0;  // orders the risk engine still tracks as live at the end
  std::uint64_t arb_trades = 0;                 // cross-venue arbitrage executions by the background arbitrageur
  std::uint64_t kills = 0;
  Nanos kill_time = -1;             // simulated time of the first kill-switch trip (-1 = none)
  Nanos last_fill_after_kill = 0;   // time of the last fill that arrived after the kill (0 = none)
  std::uint64_t mm_quotes = 0, mm_amends = 0;

  std::vector<std::pair<Nanos, double>> equity;  // firm net P&L over time
  std::vector<std::vector<std::pair<Nanos, double>>> strategy_equity;
};

class Simulator {
 public:
  explicit Simulator(SimConfig cfg);
  ~Simulator();
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  SimReport run();

 private:
  struct Venue;
  void venue_process(VenueId v, const Command& cmd);
  void publish(VenueId v);
  void arbitrage();
  void handle(const Event& e, bool draining);

  SimConfig cfg_;
  EventQueue q_;
  std::unique_ptr<Network> net_;
  std::vector<std::unique_ptr<Venue>> venues_;
  RiskEngine risk_;
  std::unique_ptr<SmartOrderRouter> sor_;
  std::unique_ptr<Oms> oms_;
  std::vector<std::unique_ptr<Strategy>> strategies_;
  MarketMaker* mm_ = nullptr;
  TwapExecutor* twap_ = nullptr;
  Rng fundamental_rng_;
  double fundamental_ = 0.0;
  double spread_sum_ = 0.0;
  std::uint64_t spread_samples_ = 0, crossed_samples_ = 0, samples_ = 0;
  std::uint64_t events_ = 0;
  std::uint64_t arb_trades_ = 0;
  OrderId arb_next_id_ = 5'000'000'000'000ull;
  bool in_arbitrage_ = false;
};

void print_report(std::ostream& os, const SimReport& r);
void write_equity_csv(std::ostream& os, const SimReport& r);

}  // namespace qc
