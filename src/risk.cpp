#include "qc/risk.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

namespace {
inline Qty abs_qty(Qty q) { return q < 0 ? -q : q; }
}  // namespace

RiskEngine::ClientState& RiskEngine::client(ClientId c) {
  auto [it, inserted] = clients_.try_emplace(c);
  if (inserted) it->second.limits = defaults_;
  return it->second;
}

const RiskEngine::ClientState* RiskEngine::find_client(ClientId c) const {
  const auto it = clients_.find(c);
  return it == clients_.end() ? nullptr : &it->second;
}

RiskEngine::SymState& RiskEngine::sym_state(ClientState& c, SymbolId s) {
  if (s >= c.sym.size()) c.sym.resize(static_cast<std::size_t>(s) + 1);
  return c.sym[s];
}

const RiskLimits& RiskEngine::limits(ClientId c) const {
  const ClientState* cs = find_client(c);
  return cs ? cs->limits : defaults_;
}

void RiskEngine::set_reference_price(SymbolId s, Price px) {
  if (s >= kMaxSymbols) return;
  if (s >= ref_px_.size()) ref_px_.resize(static_cast<std::size_t>(s) + 1, 0);
  ref_px_[s] = px;
}

bool RiskEngine::is_killed(ClientId c) const {
  if (kill_all_) return true;
  const ClientState* cs = find_client(c);
  return cs && cs->killed;
}

RejectReason RiskEngine::check(const Order& o, Nanos now, Qty replacing_leaves) {
  ++checks_;
  ClientState& c = client(o.client);
  const RiskLimits& lim = c.limits;

  if (o.symbol >= kMaxSymbols) return reject(RejectReason::UnknownSymbol);
  if (kill_all_ || c.killed) return reject(RejectReason::RiskKillSwitch);
  if (!halted_.empty() && halted_.count(o.symbol)) return reject(RejectReason::RiskSymbolHalted);
  if (o.qty > lim.max_order_qty) return reject(RejectReason::RiskOrderSize);

  const Price ref = reference_price(o.symbol);
  // Market orders are priced at the reference; without one, price-based checks cannot run.
  const Price px = o.type == OrderType::Limit ? o.price : ref;
  if (px > 0 && notional(px, o.qty) > lim.max_order_notional) return reject(RejectReason::RiskOrderNotional);
  if (o.type == OrderType::Limit && ref > 0 && lim.price_collar_bps > 0) {
    const double dev_bps = std::fabs(static_cast<double>(o.price - ref)) * 10000.0 / static_cast<double>(ref);
    if (dev_bps > static_cast<double>(lim.price_collar_bps)) return reject(RejectReason::RiskPriceCollar);
  }

  double rate_cap = 0.0;
  if (lim.max_orders_per_sec > 0.0) {
    rate_cap = lim.burst_orders > 0.0 ? lim.burst_orders : std::max(1.0, lim.max_orders_per_sec);
    if (!c.tokens_init) {
      c.tokens = rate_cap;
      c.last_refill = now;
      c.tokens_init = true;
    } else if (now > c.last_refill) {
      c.tokens = std::min(rate_cap, c.tokens + static_cast<double>(now - c.last_refill) * 1e-9 * lim.max_orders_per_sec);
      c.last_refill = now;
    }
    if (c.tokens < 1.0) return reject(RejectReason::RiskRateLimit);
  }

  if (replacing_leaves == 0 && c.open_orders >= lim.max_open_orders) return reject(RejectReason::RiskOpenOrders);

  const SymState zero;
  const SymState& s = o.symbol < c.sym.size() ? c.sym[o.symbol] : zero;
  if (o.side == Side::Buy) {
    const Qty worst = s.pos + (s.open_buy - replacing_leaves + o.qty);
    if (worst > lim.max_position) return reject(RejectReason::RiskPositionLimit);
  } else {
    const Qty worst = s.pos - (s.open_sell - replacing_leaves + o.qty);
    if (worst < -lim.max_position) return reject(RejectReason::RiskPositionLimit);
  }

  if (lim.max_gross_notional > 0.0 && px > 0) {
    const double after = gross_exposure(o.client) + notional(px, o.qty) - notional(px, replacing_leaves);
    if (after > lim.max_gross_notional) return reject(RejectReason::RiskGrossExposure);
  }

  if (rate_cap > 0.0) c.tokens -= 1.0;
  return RejectReason::None;
}

void RiskEngine::set_remaining(Tracked& t, Qty new_remaining) {
  ClientState& c = client(t.client);
  SymState& s = sym_state(c, t.symbol);
  const Qty delta = new_remaining - t.remaining;
  (t.side == Side::Buy ? s.open_buy : s.open_sell) += delta;
  c.open_notional += notional(t.px, delta);
  t.remaining = new_remaining;
}

void RiskEngine::track(const Order& o) {
  const Price px = o.type == OrderType::Limit ? o.price : reference_price(o.symbol);
  const auto [it, inserted] = orders_.try_emplace(o.id, Tracked{o.client, o.symbol, o.side, px, 0});
  if (!inserted) return;  // already tracked (duplicate id): the venue will reject the duplicate
  client(o.client).open_orders += 1;
  set_remaining(it->second, o.qty);
}

void RiskEngine::on_modify_sent(OrderId id, Price new_px, Qty new_qty) {
  const auto it = orders_.find(id);
  if (it == orders_.end()) return;
  Tracked& t = it->second;
  ClientState& c = client(t.client);
  // re-price the open notional of what is currently tracked, then widen quantity if needed
  c.open_notional += notional(new_px - t.px, t.remaining);
  t.px = new_px;
  if (new_qty > t.remaining) set_remaining(t, new_qty);
}

void RiskEngine::on_report(const ExecReport& r) {
  if (r.type == ExecType::CancelRejected) return;  // order unchanged
  // A rejected duplicate id says nothing about the original live order that owns the id.
  if (r.type == ExecType::Rejected && r.reason == RejectReason::DuplicateOrderId) return;

  if (r.is_fill()) {
    ClientState& c = client(r.client);
    sym_state(c, r.symbol).pos += static_cast<Qty>(sign(r.side)) * r.last_qty;
  }

  const auto it = orders_.find(r.order_id);
  if (it == orders_.end()) return;
  set_remaining(it->second, r.leaves);
  if (r.leaves == 0) {
    ClientState& c = client(it->second.client);
    if (c.open_orders > 0) --c.open_orders;
    orders_.erase(it);
  }
}

bool RiskEngine::on_pnl(ClientId id, double net_pnl) {
  ClientState& c = client(id);
  if (c.limits.max_daily_loss > 0.0 && !c.killed && net_pnl <= -c.limits.max_daily_loss) {
    c.killed = true;
    return true;
  }
  return false;
}

Qty RiskEngine::position(ClientId c, SymbolId s) const {
  const ClientState* cs = find_client(c);
  return cs && s < cs->sym.size() ? cs->sym[s].pos : 0;
}
Qty RiskEngine::open_buy(ClientId c, SymbolId s) const {
  const ClientState* cs = find_client(c);
  return cs && s < cs->sym.size() ? cs->sym[s].open_buy : 0;
}
Qty RiskEngine::open_sell(ClientId c, SymbolId s) const {
  const ClientState* cs = find_client(c);
  return cs && s < cs->sym.size() ? cs->sym[s].open_sell : 0;
}
std::size_t RiskEngine::open_order_count(ClientId c) const {
  const ClientState* cs = find_client(c);
  return cs ? cs->open_orders : 0;
}
double RiskEngine::open_notional(ClientId c) const {
  const ClientState* cs = find_client(c);
  return cs ? cs->open_notional : 0.0;
}

// positions at reference prices + notional of every open order (conservative: both sides add up)
double RiskEngine::gross_exposure(ClientId c) const {
  const ClientState* cs = find_client(c);
  if (!cs) return 0.0;
  double g = cs->open_notional;
  for (std::size_t s = 0; s < cs->sym.size(); ++s) {
    if (cs->sym[s].pos != 0) g += notional(reference_price(static_cast<SymbolId>(s)), abs_qty(cs->sym[s].pos));
  }
  return g;
}

std::vector<std::pair<OrderId, SymbolId>> RiskEngine::open_orders(ClientId c) const {
  std::vector<std::pair<OrderId, SymbolId>> v;
  for (const auto& [id, t] : orders_) {
    if (t.client == c) v.emplace_back(id, t.symbol);
  }
  std::sort(v.begin(), v.end());  // deterministic order for mass-cancel
  return v;
}

std::uint64_t RiskEngine::total_rejects() const {
  std::uint64_t n = 0;
  for (std::size_t i = 1; i < reject_counts_.size(); ++i) n += reject_counts_[i];
  return n;
}

}  // namespace qc
