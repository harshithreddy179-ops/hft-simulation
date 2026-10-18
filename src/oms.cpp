#include "qc/oms.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

Oms::Oms(OmsConfig cfg, EventQueue& q, Network& net, const std::vector<VenueConfig>& venues, RiskEngine& risk,
         SmartOrderRouter& sor)
    : cfg_(cfg), q_(q), net_(net), venues_(venues), risk_(risk), sor_(sor) {
  for (const VenueConfig& v : venues_) {
    VenueFirmStats s;
    s.name = v.name;
    venue_stats_.push_back(std::move(s));
  }
}

void Oms::add_strategy(Strategy* s) {
  s->id = static_cast<StrategyId>(strategies_.size());
  strategies_.push_back(s);
  portfolios_.emplace_back(CostBasis::AverageCost);
  strat_equity_.emplace_back();
}

void Oms::start() {
  for (Strategy* s : strategies_) s->on_start(*this);
}

Qty Oms::firm_position(SymbolId sym) const {
  Qty p = 0;
  for (const Portfolio& pf : portfolios_) p += pf.position(sym);
  return p;
}

double Oms::firm_net_pnl() const {
  double t = 0.0;
  for (const Portfolio& pf : portfolios_) t += pf.net_pnl(marks_);
  return t;
}

void Oms::set_timer(StrategyId s, Nanos delay, std::uint32_t timer_id) {
  q_.schedule(now() + delay, EvTimer{s, timer_id});
}

void Oms::dispatch(VenueId v, const Command& cmd) {
  ++venue_stats_[v].orders_sent;
  q_.schedule(net_.to_venue(v, now()), EvOrderArrive{v, cmd});
}

// ---------------------------------------------------------------------------------------------
// strategy-facing order API
// ---------------------------------------------------------------------------------------------

OrderId Oms::send(const OrderRequest& req, RejectReason* why) {
  if (why) *why = RejectReason::None;

  Order chk;
  chk.id = next_id_;
  chk.client = cfg_.client;
  chk.symbol = req.symbol;
  chk.side = req.side;
  chk.type = req.limit > 0 ? OrderType::Limit : OrderType::Market;
  chk.price = req.limit;
  chk.qty = req.qty;
  chk.ts = now();
  const RejectReason r = risk_.check(chk, now());
  if (r != RejectReason::None) {
    ++rejected_requests_;
    if (why) *why = r;
    return 0;
  }

  Parent p;
  p.id = next_id_++;
  p.req = req;
  p.start = now();
  double mid = 0.0;
  if (book_.mid(req.symbol, mid)) p.arrival_mid = mid;

  if (req.algo == Algo::Direct) {
    Order o = chk;
    o.id = p.id;
    o.type = OrderType::Limit;
    o.tif = req.tif;
    o.post_only = req.post_only;
    auto [it, ok] = parents_.emplace(p.id, p);
    send_child(it->second, o);
    // Direct orders reuse the parent id as the child id
    return p.id;
  }

  auto [it, ok] = parents_.emplace(p.id, p);
  route_round(it->second);
  return p.id;
}

void Oms::send_child(Parent& p, const Order& o) {
  Child c;
  c.parent = p.id;
  c.venue = p.req.algo == Algo::Direct ? p.req.venue : 0;
  c.symbol = o.symbol;
  c.side = o.side;
  c.leaves = o.qty;
  c.sent_at = now();
  children_[o.id] = c;
  ++p.outstanding;
  ++p.children;
  risk_.track(o);
  Command cmd;
  cmd.type = CommandType::New;
  cmd.order = o;
  dispatch(c.venue, cmd);
}

// One routing round: plan from the *current* (possibly stale) consolidated book and fire IOC children.
void Oms::route_round(Parent& p) {
  const Qty remaining = p.req.qty - p.filled;
  RouteRequest rq;
  rq.symbol = p.req.symbol;
  rq.side = p.req.side;
  rq.qty = remaining;
  rq.limit = p.req.limit;
  rq.now = now();
  const RoutePlan plan = sor_.route(rq, book_);
  if (plan.n_children == 0) {
    finalize(p);
    return;
  }
  ++p.rounds;
  for (const ChildPlan& cp : plan) {
    Order o;
    o.id = next_id_++;
    o.client = cfg_.client;
    o.symbol = p.req.symbol;
    o.side = p.req.side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::IOC;
    o.price = cp.limit;
    o.qty = cp.qty;
    o.ts = now();
    Child c;
    c.parent = p.id;
    c.venue = cp.venue;
    c.symbol = o.symbol;
    c.side = o.side;
    c.leaves = o.qty;
    c.sent_at = now();
    children_[o.id] = c;
    ++p.outstanding;
    ++p.children;
    risk_.track(o);
    Command cmd;
    cmd.type = CommandType::New;
    cmd.order = o;
    dispatch(cp.venue, cmd);
  }
}

bool Oms::cancel(OrderId id) {
  const auto it = children_.find(id);
  if (it == children_.end()) return false;
  const Child& c = it->second;
  Command cmd;
  cmd.type = CommandType::Cancel;
  cmd.order.id = id;
  cmd.order.client = cfg_.client;
  cmd.order.symbol = c.symbol;
  cmd.order.side = c.side;
  cmd.order.ts = now();
  dispatch(c.venue, cmd);  // cancels are never risk-gated
  return true;
}

bool Oms::modify(OrderId id, Price new_price, Qty new_qty) {
  const auto it = children_.find(id);
  if (it == children_.end()) return false;
  Child& c = it->second;
  Order probe;
  probe.id = id;
  probe.client = cfg_.client;
  probe.symbol = c.symbol;
  probe.side = c.side;
  probe.type = OrderType::Limit;
  probe.price = new_price;
  probe.qty = new_qty;
  probe.ts = now();
  if (risk_.check(probe, now(), c.leaves) != RejectReason::None) {
    ++rejected_requests_;
    return false;
  }
  risk_.on_modify_sent(id, new_price, new_qty);
  Command cmd;
  cmd.type = CommandType::Modify;
  cmd.order = probe;
  dispatch(c.venue, cmd);
  return true;
}

void Oms::cancel_all() {
  for (const auto& [id, sym] : risk_.open_orders(cfg_.client)) cancel(id);
}

// ---------------------------------------------------------------------------------------------
// events from the venues
// ---------------------------------------------------------------------------------------------

double Oms::fee_for(const ExecReport& r) const {
  const VenueConfig& v = venues_[r.venue];
  const double bps = r.liquidity == Liquidity::Taker ? v.taker_fee_bps : r.liquidity == Liquidity::Maker ? v.maker_fee_bps : 0.0;
  return notional(r.last_px, r.last_qty) * bps * 1e-4;
}

void Oms::on_report(const ExecReport& r) {
  if (r.client != cfg_.client) return;
  risk_.on_report(r);

  const auto cit = children_.find(r.order_id);
  if (cit == children_.end()) return;
  Child& child = cit->second;
  const OrderId parent_id = child.parent;
  const auto pit = parents_.find(parent_id);
  if (pit == parents_.end()) return;
  Parent& p = pit->second;

  if (!child.reported && r.type != ExecType::CancelRejected) {
    child.reported = true;
    venue_stats_[r.venue].round_trip.record(static_cast<std::uint64_t>(now() - child.sent_at));
  }
  if (r.type != ExecType::CancelRejected) child.leaves = r.leaves;

  if (r.is_fill()) {
    if (kill_time_ >= 0) last_fill_after_kill_ = now();
    const double fee = fee_for(r);
    portfolios_[p.req.strategy].on_fill(r.symbol, r.side, r.last_qty, r.last_px, fee);
    p.filled += r.last_qty;
    p.notional_ticks += static_cast<double>(r.last_px) * static_cast<double>(r.last_qty);
    p.fees += fee;
    VenueFirmStats& vs = venue_stats_[r.venue];
    ++vs.fills;
    vs.qty += r.last_qty;
    (r.liquidity == Liquidity::Maker ? vs.maker_qty : vs.taker_qty) += r.last_qty;
    vs.fees += fee;

    if (risk_.limits(cfg_.client).max_daily_loss > 0.0 && risk_.on_pnl(cfg_.client, firm_net_pnl())) {
      ++kills_;
      if (kill_time_ < 0) kill_time_ = now();
      cancel_all();
    }
  }

  Strategy* strat = strategies_[p.req.strategy];
  strat->on_exec(*this, r);

  if (r.is_terminal()) {
    // erase by key, not by `cit`: the strategy callback above may have inserted new orders, and a
    // rehash invalidates iterators (element references such as `p` stay valid)
    children_.erase(r.order_id);
    --p.outstanding;
    if (p.req.algo == Algo::Direct) {
      finalize(p);
    } else if (p.outstanding == 0) {
      const Qty remaining = p.req.qty - p.filled;
      if (remaining > 0 && p.rounds < cfg_.max_rounds && !risk_.is_killed(cfg_.client)) {
        q_.schedule(now() + cfg_.retry_delay, EvRetry{p.id});  // let market data catch up with our own fills
      } else {
        finalize(p);
      }
    }
  }
}

void Oms::on_retry(OrderId parent) {
  const auto it = parents_.find(parent);
  if (it == parents_.end()) return;
  if (risk_.is_killed(cfg_.client)) {
    finalize(it->second);
    return;
  }
  route_round(it->second);
}

void Oms::finalize(Parent& p) {
  ParentResult r;
  r.id = p.id;
  r.strategy = p.req.strategy;
  r.symbol = p.req.symbol;
  r.side = p.req.side;
  r.qty = p.req.qty;
  r.filled = p.filled;
  r.avg_px_ticks = p.filled > 0 ? p.notional_ticks / static_cast<double>(p.filled) : 0.0;
  r.arrival_mid_ticks = p.arrival_mid;
  r.fees = p.fees;
  r.rounds = p.rounds;
  r.children = p.children;
  r.start = p.start;
  r.end = now();
  const StrategyId sid = p.req.strategy;
  const bool direct = p.req.algo == Algo::Direct;
  const OrderId pid = p.id;  // copy: the element is destroyed by the erase below
  parents_.erase(pid);       // `p` is invalid from here on
  if (!direct) results_.push_back(r);  // resting quotes are not "executions": keep the stats about parent orders
  strategies_[sid]->on_parent_done(*this, r);
}

void Oms::on_quote(const DepthSnapshot& s) {
  book_.update(s, now());
  double mid = 0.0;
  if (book_.mid(s.symbol, mid)) {
    const Price m = static_cast<Price>(std::llround(mid));
    marks_.set(s.symbol, m);
    risk_.set_reference_price(s.symbol, m);
  }
}

void Oms::on_timer(const EvTimer& t) {
  if (t.strategy < strategies_.size()) strategies_[t.strategy]->on_timer(*this, t.id);
}

void Oms::on_sample() {
  double total = 0.0;
  for (std::size_t i = 0; i < portfolios_.size(); ++i) {
    portfolios_[i].snapshot(marks_);
    const double e = portfolios_[i].net_pnl(marks_);
    strat_equity_[i].emplace_back(now(), e);
    total += e;
  }
  equity_.emplace_back(now(), total);
}

}  // namespace qc
