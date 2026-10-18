#pragma once
// Pre-trade risk engine.
//
// Every order passes `check()` before it may reach a venue. Checks run cheapest-first and fail
// fast; the first violated limit is the reject reason.
//
//   1. kill switch (client / global)        6. rate limit (token bucket, sim/wall time)
//   2. symbol halt                          7. open order count
//   3. max order size (fat finger: qty)     8. position limit (worst case incl. open orders)
//   4. max order notional                   9. gross exposure (positions + open orders)
//   5. price collar vs reference price (fat finger: price)
//
// Position and exposure are *worst case*: position + every open buy (or - every open sell) +
// this order. That keeps the gate safe even when fills arrive late (decoupled pipelines, venue
// latency in the simulator), because open/in-flight quantity is counted until a terminal report
// says otherwise. Risk-reducing flow is never blocked by the opposite side's open orders.
//
// Orders are mirrored in a tracked-order table updated from ExecReports (each report carries
// `leaves`), so the engine also knows exactly which orders to cancel on a kill.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "qc/events.hpp"
#include "qc/types.hpp"

namespace qc {

struct RiskLimits {
  Qty max_order_qty = 1'000'000;
  double max_order_notional = 1e9;       // currency units
  int price_collar_bps = 0;              // 0 = off; |px - ref| / ref in basis points
  Qty max_position = 100'000'000;        // per symbol, absolute
  double max_gross_notional = 0.0;       // 0 = off
  std::uint32_t max_open_orders = 1'000'000;
  double max_orders_per_sec = 0.0;       // 0 = off
  double burst_orders = 0.0;             // bucket size; 0 => max(1, rate)
  double max_daily_loss = 0.0;           // 0 = off; kill switch when net P&L <= -max_daily_loss
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits defaults = {}) : defaults_(defaults), has_loss_limits_(defaults.max_daily_loss > 0.0) {}

  void set_default_limits(const RiskLimits& l) {
    defaults_ = l;
    has_loss_limits_ |= l.max_daily_loss > 0.0;
  }
  void set_limits(ClientId c, const RiskLimits& l) {
    client(c).limits = l;
    has_loss_limits_ |= l.max_daily_loss > 0.0;
  }
  // True if any client may have a daily-loss limit (lets callers skip mark-to-market sweeps otherwise).
  bool has_loss_limits() const { return has_loss_limits_; }
  const RiskLimits& limits(ClientId c) const;

  void set_reference_price(SymbolId s, Price px);
  Price reference_price(SymbolId s) const { return s < ref_px_.size() ? ref_px_[s] : 0; }

  // Admission control. Returns RejectReason::None when the order may be sent. A pass consumes
  // one rate-limit token. `replacing_leaves` > 0 marks an amend of a live order of the same side
  // (its old quantity is released from the exposure computation and no extra open-order slot is used).
  RejectReason check(const Order& o, Nanos now, Qty replacing_leaves = 0);

  // Register an order that has been (or is about to be) sent. Exposure counts from this moment.
  void track(const Order& o);
  // A modify was sent: until the Replaced report arrives, count max(old, new) quantity at the new price.
  void on_modify_sent(OrderId id, Price new_px, Qty new_qty);
  // Feed every execution report back. Keeps positions and open-order exposure exact.
  void on_report(const ExecReport& r);

  // Returns true if this update tripped the daily-loss kill switch (caller should mass-cancel).
  bool on_pnl(ClientId c, double net_pnl);

  void kill(ClientId c) { client(c).killed = true; }
  void resume(ClientId c) { client(c).killed = false; }
  void kill_all() { kill_all_ = true; }
  void resume_all() { kill_all_ = false; }
  void halt_symbol(SymbolId s) { halted_.insert(s); }
  void resume_symbol(SymbolId s) { halted_.erase(s); }
  bool is_killed(ClientId c) const;

  Qty position(ClientId c, SymbolId s) const;
  Qty open_buy(ClientId c, SymbolId s) const;
  Qty open_sell(ClientId c, SymbolId s) const;
  std::size_t open_order_count(ClientId c) const;
  double open_notional(ClientId c) const;
  double gross_exposure(ClientId c) const;
  std::vector<std::pair<OrderId, SymbolId>> open_orders(ClientId c) const;

  std::uint64_t reject_count(RejectReason r) const { return reject_counts_[static_cast<std::size_t>(r)]; }
  std::uint64_t total_rejects() const;
  std::uint64_t checks() const { return checks_; }

 private:
  struct SymState {
    Qty pos = 0;
    Qty open_buy = 0;
    Qty open_sell = 0;
  };
  struct ClientState {
    RiskLimits limits;
    bool killed = false;
    double tokens = 0.0;
    Nanos last_refill = 0;
    bool tokens_init = false;
    std::vector<SymState> sym;  // indexed by SymbolId
    std::size_t open_orders = 0;
    double open_notional = 0.0;
  };
  struct Tracked {
    ClientId client;
    SymbolId symbol;
    Side side;
    Price px;
    Qty remaining;
  };

  ClientState& client(ClientId c);
  const ClientState* find_client(ClientId c) const;
  static SymState& sym_state(ClientState& c, SymbolId s);
  RejectReason reject(RejectReason r) {
    ++reject_counts_[static_cast<std::size_t>(r)];
    return r;
  }
  void set_remaining(Tracked& t, Qty new_remaining);

  RiskLimits defaults_;
  std::unordered_map<ClientId, ClientState> clients_;
  std::unordered_map<OrderId, Tracked> orders_;
  std::unordered_set<SymbolId> halted_;
  std::vector<Price> ref_px_;
  bool kill_all_ = false;
  bool has_loss_limits_ = false;
  std::array<std::uint64_t, static_cast<std::size_t>(RejectReason::Count_)> reject_counts_{};
  std::uint64_t checks_ = 0;
};

}  // namespace qc
