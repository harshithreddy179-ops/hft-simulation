#pragma once
// Example strategies for the simulator. They only use the Oms facade (send / cancel / modify /
// timers / read-only book and positions), exactly as a real strategy plugin would.

#include <cstdint>

#include "qc/oms.hpp"
#include "qc/rng.hpp"

namespace qc {

// ---- market maker -------------------------------------------------------------------------------
// Quotes a post-only bid and offer around the consolidated mid on one venue (by default the one
// with the best maker rebate). Both quotes are shifted against its inventory -- when long, they move
// down so it is likelier to sell than to buy more -- and a side stops quoting at the inventory cap.
struct MarketMakerConfig {
  double half_spread_ticks = 1.5;
  double skew_ticks_per_100 = 0.4;  // quote shift per 100 shares of inventory
  Qty quote_size = 100;
  Qty max_inventory = 1500;
  Nanos interval = 1 * kMilli;      // re-evaluate quotes this often
  Price requote_threshold = 1;      // only amend when the target moved by at least this many ticks
  VenueId venue = ~VenueId{0};      // ~0 = auto: the venue with the best maker rebate
};

class MarketMaker : public Strategy {
 public:
  MarketMaker(SymbolId symbol, MarketMakerConfig cfg) : symbol_(symbol), cfg_(cfg) {}
  const char* name() const override { return "market_maker"; }
  void on_start(Oms& oms) override;
  void on_timer(Oms& oms, std::uint32_t) override;
  void on_exec(Oms& oms, const ExecReport& r) override;

  std::uint64_t quotes_sent() const { return quotes_sent_; }
  std::uint64_t amends_sent() const { return amends_sent_; }

 private:
  struct Quote {
    OrderId id = 0;
    Price px = 0;
    Price target_px = 0;
    bool amend_pending = false;
    bool cancel_pending = false;
  };
  void update_side(Oms& oms, Side side, Price desired, bool allowed);

  SymbolId symbol_;
  MarketMakerConfig cfg_;
  Quote bid_, ask_;
  std::uint64_t quotes_sent_ = 0, amends_sent_ = 0;
};

// ---- parent order flow --------------------------------------------------------------------------
// Stand-in for client/agency flow: random-size parent orders at random times, executed through the
// smart order router. Its job is to exercise the router and produce execution-quality statistics.
struct ParentFlowConfig {
  double mean_interval_ms = 25.0;
  Qty min_qty = 200;
  Qty max_qty = 3000;
  Price limit_ticks = 4;  // limit = far touch +/- this
  std::uint64_t seed = 7;
};

class ParentFlow : public Strategy {
 public:
  ParentFlow(SymbolId symbol, ParentFlowConfig cfg) : symbol_(symbol), cfg_(cfg), rng_(cfg.seed) {}
  const char* name() const override { return "parent_flow"; }
  void on_start(Oms& oms) override;
  void on_timer(Oms& oms, std::uint32_t) override;
  std::uint64_t parents_sent() const { return sent_; }

 private:
  SymbolId symbol_;
  ParentFlowConfig cfg_;
  Rng rng_;
  std::uint64_t sent_ = 0;
};

// ---- TWAP ---------------------------------------------------------------------------------------
// Works one large parent order evenly over a time window in N slices, each slice routed by the SOR.
// A slice that under-fills is made up by later slices (remaining / slices_left).
struct TwapConfig {
  Side side = Side::Buy;
  Qty total_qty = 20000;
  Nanos start = 1 * kSecond;
  Nanos duration = 8 * kSecond;
  int slices = 40;
  Price limit_ticks = 6;
};

struct TwapSummary {
  Qty target = 0;
  Qty filled = 0;
  double avg_px_ticks = 0.0;
  double arrival_mid_ticks = 0.0;       // mid when the first slice was sent
  double interval_mid_ticks = 0.0;      // average of the mid sampled at every slice (the TWAP benchmark)
  double vs_arrival_bps = 0.0;          // positive = cost
  double vs_twap_bps = 0.0;
  double fees = 0.0;
  int slices_sent = 0;
};

class TwapExecutor : public Strategy {
 public:
  TwapExecutor(SymbolId symbol, TwapConfig cfg) : symbol_(symbol), cfg_(cfg) {}
  const char* name() const override { return "twap"; }
  void on_start(Oms& oms) override;
  void on_timer(Oms& oms, std::uint32_t) override;
  void on_parent_done(Oms& oms, const ParentResult& r) override;
  TwapSummary summary() const;

 private:
  SymbolId symbol_;
  TwapConfig cfg_;
  int slice_ = 0;
  Qty sent_qty_ = 0;       // quantity already handed to slices
  Qty filled_ = 0;
  double notional_ticks_ = 0.0;
  double fees_ = 0.0;
  double arrival_mid_ = 0.0;
  double mid_sum_ = 0.0;
  int mid_samples_ = 0;
  int slices_sent_ = 0;
};

}  // namespace qc
