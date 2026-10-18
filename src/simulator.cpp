#include "qc/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <ostream>

namespace qc {

namespace {
template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;
}  // namespace

std::vector<VenueConfig> default_venues() {
  VenueConfig a;
  a.id = 0;
  a.name = "ALPHA";  // fast and deep, but the priciest to take from
  a.taker_fee_bps = 1.2;
  a.maker_fee_bps = -0.8;
  a.latency_in = 60 * kMicro;
  a.latency_out = 60 * kMicro;
  a.md_latency = 30 * kMicro;
  a.jitter = 15 * kMicro;
  a.flow_rate_per_sec = 6000;
  a.qty_scale = 1.2;
  a.quote_noise_ticks = 0.8;

  VenueConfig b;
  b.id = 1;
  b.name = "BRAVO";  // slow, cheap
  b.taker_fee_bps = 0.4;
  b.maker_fee_bps = 0.0;
  b.latency_in = 250 * kMicro;
  b.latency_out = 250 * kMicro;
  b.md_latency = 150 * kMicro;
  b.jitter = 60 * kMicro;
  b.flow_rate_per_sec = 4000;
  b.qty_scale = 1.0;
  b.quote_noise_ticks = 1.2;

  VenueConfig c;
  c.id = 2;
  c.name = "CHARLIE";  // middling everything, thinner book, noisier quotes
  c.taker_fee_bps = 0.8;
  c.maker_fee_bps = -0.3;
  c.latency_in = 140 * kMicro;
  c.latency_out = 140 * kMicro;
  c.md_latency = 90 * kMicro;
  c.jitter = 40 * kMicro;
  c.flow_rate_per_sec = 3000;
  c.qty_scale = 0.8;
  c.quote_noise_ticks = 1.5;
  return {a, b, c};
}

SimConfig::SimConfig() {
  firm_limits.max_order_qty = 5000;
  firm_limits.max_order_notional = 2'000'000.0;
  firm_limits.price_collar_bps = 300;
  firm_limits.max_position = 40'000;
  firm_limits.max_gross_notional = 10'000'000.0;
  firm_limits.max_open_orders = 200;
  firm_limits.max_orders_per_sec = 5000.0;
  firm_limits.burst_orders = 500.0;
  firm_limits.max_daily_loss = 0.0;  // off unless requested
}

struct Simulator::Venue {
  Venue(const VenueConfig& vc, const FlowConfig& fc, std::uint64_t seed) : engine(vc.id), flow(vc.id, vc, fc, seed) {}
  MatchingEngine engine;
  FlowGenerator flow;
  DepthSnapshot last_pub;
  bool has_pub = false;
  EngineOutput out;
  std::uint64_t noise_trades = 0;
};

Simulator::Simulator(SimConfig cfg)
    : cfg_(std::move(cfg)), risk_(cfg_.firm_limits), fundamental_rng_(cfg_.seed ^ 0xF00DF00Dull) {
  if (cfg_.venues.empty()) cfg_.venues = default_venues();
  for (std::size_t i = 0; i < cfg_.venues.size(); ++i) cfg_.venues[i].id = static_cast<VenueId>(i);
  fundamental_ = cfg_.flow.fundamental_start;

  net_ = std::make_unique<Network>(cfg_.venues, cfg_.seed ^ 0xBEEFBEEFull);
  std::vector<VenueInfo> infos;
  for (const VenueConfig& vc : cfg_.venues) {
    venues_.push_back(std::make_unique<Venue>(vc, cfg_.flow, cfg_.seed * 1'000'003ull + vc.id));
    infos.push_back(VenueInfo{vc.id, vc.taker_fee_bps, vc.maker_fee_bps, vc.latency_in});
  }
  cfg_.sor.primary = std::min<VenueId>(cfg_.sor.primary, static_cast<VenueId>(cfg_.venues.size() - 1));
  sor_ = std::make_unique<SmartOrderRouter>(infos, cfg_.sor);
  oms_ = std::make_unique<Oms>(cfg_.oms, q_, *net_, cfg_.venues, risk_, *sor_);

  if (cfg_.enable_market_maker) {
    MarketMakerConfig mc = cfg_.mm;
    if (mc.venue >= cfg_.venues.size()) {
      mc.venue = 0;
      for (const VenueConfig& vc : cfg_.venues) {
        if (vc.maker_fee_bps < cfg_.venues[mc.venue].maker_fee_bps) mc.venue = vc.id;
      }
    }
    auto s = std::make_unique<MarketMaker>(0, mc);
    mm_ = s.get();
    oms_->add_strategy(s.get());
    strategies_.push_back(std::move(s));
  }
  if (cfg_.enable_parent_flow) {
    auto s = std::make_unique<ParentFlow>(0, cfg_.parent_flow);
    oms_->add_strategy(s.get());
    strategies_.push_back(std::move(s));
  }
  if (cfg_.enable_twap) {
    auto s = std::make_unique<TwapExecutor>(0, cfg_.twap);
    twap_ = s.get();
    oms_->add_strategy(s.get());
    strategies_.push_back(std::move(s));
  }
}

Simulator::~Simulator() = default;

// ---------------------------------------------------------------------------------------------
// venues
// ---------------------------------------------------------------------------------------------

void Simulator::venue_process(VenueId v, const Command& cmd) {
  Venue& ve = *venues_[v];
  ve.out.clear();
  ve.engine.process(cmd, ve.out);
  for (const ExecReport& r : ve.out.reports) {
    if (r.client == kFirmClient) q_.schedule(net_->from_venue(v, q_.now()), EvReportArrive{r});
  }
  for (const Fill& f : ve.out.fills) {
    if (f.maker_client != kFirmClient && f.taker_client != kFirmClient) ++ve.noise_trades;
  }
  publish(v);
  if (!in_arbitrage_) arbitrage();
}

// A zero-latency arbitrageur: whenever the best bid at one venue is strictly above the best ask at
// another it buys the cheap side and sells the rich side (IOC at the displayed prices). Real markets
// have such participants, which is why cross-venue crossed books are rare and short-lived; without
// one the simulated venues drift apart and the router comparison would be dominated by free money.
void Simulator::arbitrage() {
  in_arbitrage_ = true;
  for (int guard = 0; guard < 16; ++guard) {
    int bid_v = -1, ask_v = -1;
    OrderBook::Top bt, at;
    for (std::size_t i = 0; i < venues_.size(); ++i) {
      const OrderBook* bk = venues_[i]->engine.book(0);
      if (!bk) continue;
      const auto t = bk->top();
      if (t.has_bid && (bid_v < 0 || t.bid > bt.bid)) {
        bid_v = static_cast<int>(i);
        bt = t;
      }
      if (t.has_ask && (ask_v < 0 || t.ask < at.ask)) {
        ask_v = static_cast<int>(i);
        at = t;
      }
    }
    if (bid_v < 0 || ask_v < 0 || bid_v == ask_v || bt.bid <= at.ask) break;
    const Qty q = std::min(bt.bid_qty, at.ask_qty);
    for (const bool buy_leg : {true, false}) {
      Command c;
      c.type = CommandType::New;
      Order& o = c.order;
      o.id = arb_next_id_++;
      o.client = 900;
      o.symbol = 0;
      o.side = buy_leg ? Side::Buy : Side::Sell;
      o.type = OrderType::Limit;
      o.tif = TimeInForce::IOC;
      o.price = buy_leg ? at.ask : bt.bid;
      o.qty = q;
      o.ts = q_.now();
      venue_process(static_cast<VenueId>(buy_leg ? ask_v : bid_v), c);
    }
    ++arb_trades_;
  }
  in_arbitrage_ = false;
}

// Publish top-of-book depth if it changed; it reaches the firm after the market-data latency.
void Simulator::publish(VenueId v) {
  Venue& ve = *venues_[v];
  const OrderBook* bk = ve.engine.book(0);
  if (!bk) return;
  std::vector<OrderBook::DepthLevel> lv;
  DepthSnapshot s;
  s.venue = v;
  s.symbol = 0;
  s.venue_ts = q_.now();
  bk->depth(Side::Buy, kDepthLevels, lv);
  s.n_bids = static_cast<std::uint8_t>(lv.size());
  for (std::size_t i = 0; i < lv.size(); ++i) s.bids[i] = LevelQuote{lv[i].price, lv[i].qty};
  bk->depth(Side::Sell, kDepthLevels, lv);
  s.n_asks = static_cast<std::uint8_t>(lv.size());
  for (std::size_t i = 0; i < lv.size(); ++i) s.asks[i] = LevelQuote{lv[i].price, lv[i].qty};

  if (ve.has_pub && s.n_bids == ve.last_pub.n_bids && s.n_asks == ve.last_pub.n_asks) {
    bool same = true;
    for (std::size_t i = 0; same && i < s.n_bids; ++i) same = s.bids[i].price == ve.last_pub.bids[i].price && s.bids[i].qty == ve.last_pub.bids[i].qty;
    for (std::size_t i = 0; same && i < s.n_asks; ++i) same = s.asks[i].price == ve.last_pub.asks[i].price && s.asks[i].qty == ve.last_pub.asks[i].qty;
    if (same) return;
  }
  ve.last_pub = s;
  ve.has_pub = true;
  q_.schedule(net_->market_data(v, q_.now()), EvQuoteArrive{s});
}

// ---------------------------------------------------------------------------------------------
// event loop
// ---------------------------------------------------------------------------------------------

void Simulator::handle(const Event& e, bool draining) {
  std::visit(
      Overloaded{
          [&](const EvFundamentalStep&) {
            if (draining) return;
            const double dt = static_cast<double>(cfg_.flow.fundamental_step) * 1e-9;
            fundamental_ = std::max(100.0, fundamental_ + cfg_.flow.vol_ticks_per_sqrt_s * std::sqrt(dt) * fundamental_rng_.normal());
            for (auto& v : venues_) v->flow.step_offset();
            q_.schedule(q_.now() + cfg_.flow.fundamental_step, EvFundamentalStep{});
          },
          [&](const EvNoise& n) {
            if (draining) return;
            Venue& ve = *venues_[n.venue];
            venue_process(n.venue, ve.flow.next(q_.now(), fundamental_));
            q_.schedule(q_.now() + ve.flow.next_gap(), EvNoise{n.venue});
          },
          [&](const EvOrderArrive& a) { venue_process(a.venue, a.cmd); },
          [&](const EvReportArrive& r) { oms_->on_report(r.rep); },
          [&](const EvQuoteArrive& qd) { oms_->on_quote(qd.snap); },
          [&](const EvTimer& t) {
            if (!draining) oms_->on_timer(t);
          },
          [&](const EvRetry& r) { oms_->on_retry(r.parent); },
          [&](const EvSample&) {
            if (draining) return;
            oms_->on_sample();
            ++samples_;
            const auto bid = oms_->book().best_bid(0), ask = oms_->book().best_ask(0);
            if (bid.valid && ask.valid) {
              ++spread_samples_;
              spread_sum_ += static_cast<double>(ask.price - bid.price);
              if (bid.price > ask.price) ++crossed_samples_;
            }
            q_.schedule(q_.now() + cfg_.sample_interval, EvSample{});
          },
      },
      e.payload);
}

SimReport Simulator::run() {
  const auto wall0 = std::chrono::steady_clock::now();

  for (auto& v : venues_) {
    for (const Command& c : v->flow.initial_book(0, fundamental_)) {
      v->out.clear();
      v->engine.process(c, v->out);
    }
    publish(v->engine.venue());
    q_.schedule(v->flow.next_gap(), EvNoise{v->engine.venue()});
  }
  q_.schedule(cfg_.flow.fundamental_step, EvFundamentalStep{});
  q_.schedule(cfg_.sample_interval, EvSample{});
  oms_->start();

  while (!q_.empty() && q_.next_time() <= cfg_.duration) {
    const Event e = q_.pop();
    ++events_;
    handle(e, false);
  }
  // Let in-flight orders, reports and routing retries settle; no new background flow or strategy timers.
  const Nanos grace = cfg_.duration + 50 * kMilli;
  while (!q_.empty() && q_.next_time() <= grace) {
    const Event e = q_.pop();
    ++events_;
    handle(e, true);
  }

  SimReport rep;
  rep.policy = cfg_.sor.policy;
  rep.seed = cfg_.seed;
  rep.duration = cfg_.duration;
  rep.events = events_;
  rep.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();

  const MarkTable& marks = oms_->marks();
  for (StrategyId i = 0; i < oms_->strategies(); ++i) {
    const Portfolio& pf = oms_->portfolio(i);
    StrategyReport sr;
    sr.name = oms_->strategy(i)->name();
    sr.position = pf.position(0);
    sr.realized = pf.realized();
    sr.unrealized = pf.unrealized(marks);
    sr.fees = pf.fees();
    sr.net = pf.net_pnl(marks);
    sr.max_drawdown = pf.max_drawdown();
    sr.fills = pf.fill_count();
    sr.volume = pf.volume();
    rep.strategies.push_back(sr);
  }
  rep.firm_net = oms_->firm_net_pnl();

  rep.parents = oms_->results();
  ExecutionStats& ex = rep.execution;
  double slip_w = 0, fee_w = 0, children = 0, rounds = 0, lat = 0;
  for (const ParentResult& p : rep.parents) {
    ++ex.parents;
    ex.requested += p.qty;
    ex.filled += p.filled;
    slip_w += p.slippage_bps() * static_cast<double>(p.filled);
    fee_w += p.fee_bps() * static_cast<double>(p.filled);
    ex.cost_currency += p.cost_currency();
    children += p.children;
    rounds += p.rounds;
    lat += static_cast<double>(p.end - p.start) / 1e3;
  }
  if (ex.parents > 0) {
    const double f = static_cast<double>(std::max<Qty>(1, ex.filled));
    ex.fill_rate = ex.requested > 0 ? static_cast<double>(ex.filled) / static_cast<double>(ex.requested) : 0.0;
    ex.slippage_bps = slip_w / f;
    ex.fee_bps = fee_w / f;
    ex.total_cost_bps = ex.slippage_bps + ex.fee_bps;
    ex.avg_children = children / static_cast<double>(ex.parents);
    ex.avg_rounds = rounds / static_cast<double>(ex.parents);
    ex.avg_latency_us = lat / static_cast<double>(ex.parents);
  }

  if (twap_) {
    rep.has_twap = true;
    rep.twap = twap_->summary();
  }
  rep.venues = oms_->venue_stats();
  for (const auto& v : venues_) rep.noise_trades.push_back(v->noise_trades);
  rep.avg_spread_ticks = spread_samples_ ? spread_sum_ / static_cast<double>(spread_samples_) : 0.0;
  rep.crossed_fraction = spread_samples_ ? static_cast<double>(crossed_samples_) / static_cast<double>(spread_samples_) : 0.0;
  rep.risk_rejects = risk_.total_rejects();
  for (std::size_t r = 1; r < static_cast<std::size_t>(RejectReason::Count_); ++r) {
    const auto reason = static_cast<RejectReason>(r);
    if (risk_.reject_count(reason) > 0) rep.reject_breakdown.emplace_back(reason, risk_.reject_count(reason));
  }
  for (const StrategyReport& sr : rep.strategies) rep.firm_position += sr.position;
  rep.risk_position = risk_.position(kFirmClient, 0);
  rep.open_orders = risk_.open_order_count(kFirmClient);
  rep.arb_trades = arb_trades_;
  rep.kills = oms_->kills();
  rep.kill_time = oms_->kill_time();
  rep.last_fill_after_kill = oms_->last_fill_after_kill();
  if (mm_) {
    rep.mm_quotes = mm_->quotes_sent();
    rep.mm_amends = mm_->amends_sent();
  }
  rep.equity = oms_->equity_curve();
  rep.strategy_equity = oms_->strategy_equity();
  return rep;
}

// ---------------------------------------------------------------------------------------------
// reporting
// ---------------------------------------------------------------------------------------------

namespace {
std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}
}  // namespace

void print_report(std::ostream& os, const SimReport& r) {
  os << std::fixed;
  os << "=== simulation: " << std::setprecision(1) << static_cast<double>(r.duration) / 1e9 << " s simulated, seed " << r.seed
     << ", router policy '" << to_cstr(r.policy) << "', " << r.venues.size() << " venues ===\n";
  os << "events " << commas(r.events) << " in " << std::setprecision(2) << r.wall_seconds << " s wall ("
     << commas(static_cast<std::uint64_t>(static_cast<double>(r.events) / std::max(1e-9, r.wall_seconds))) << " events/s)\n";
  os << "market: avg NBBO spread " << std::setprecision(2) << r.avg_spread_ticks << " ticks, consolidated book crossed in "
     << std::setprecision(1) << r.crossed_fraction * 100.0 << "% of samples (stale data / arbitrage windows; "
     << commas(r.arb_trades) << " background arbitrage trades)\n\n";

  os << "P&L by strategy (currency units, marked at consolidated mid)\n";
  os << "  " << std::left << std::setw(14) << "strategy" << std::right << std::setw(9) << "position" << std::setw(12) << "realized"
     << std::setw(12) << "unrealized" << std::setw(11) << "fees" << std::setw(12) << "net" << std::setw(11) << "max DD" << std::setw(8)
     << "fills" << "\n";
  os << std::setprecision(2);
  for (const StrategyReport& s : r.strategies) {
    os << "  " << std::left << std::setw(14) << s.name << std::right << std::setw(9) << s.position << std::setw(12) << s.realized
       << std::setw(12) << s.unrealized << std::setw(11) << s.fees << std::setw(12) << s.net << std::setw(11) << s.max_drawdown
       << std::setw(8) << s.fills << "\n";
  }
  os << "  " << std::left << std::setw(14) << "FIRM" << std::right << std::setw(56) << r.firm_net << "\n\n";

  const ExecutionStats& e = r.execution;
  os << "Execution quality (" << commas(e.parents) << " parent orders routed by the SOR)\n";
  os << "  fill rate " << std::setprecision(1) << e.fill_rate * 100.0 << "%   slippage vs arrival mid " << std::setprecision(2)
     << e.slippage_bps << " bps   fees " << e.fee_bps << " bps   all-in " << e.total_cost_bps << " bps   cost " << e.cost_currency
     << "\n";
  os << "  avg " << std::setprecision(2) << e.avg_children << " child orders and " << e.avg_rounds << " routing rounds per parent, "
     << std::setprecision(0) << e.avg_latency_us << " us parent latency (simulated)\n";
  if (r.has_twap) {
    const TwapSummary& t = r.twap;
    os << "  TWAP: filled " << commas(static_cast<std::uint64_t>(t.filled)) << " / " << commas(static_cast<std::uint64_t>(t.target))
       << " in " << t.slices_sent << " slices, avg " << std::setprecision(2) << t.avg_px_ticks / kTicksPerUnit << " vs arrival mid "
       << t.arrival_mid_ticks / kTicksPerUnit << " (" << t.vs_arrival_bps << " bps) and vs interval-avg mid "
       << t.interval_mid_ticks / kTicksPerUnit << " (" << t.vs_twap_bps << " bps), fees " << t.fees << "\n";
  }
  os << "\n";

  os << "Venues\n";
  os << "  " << std::left << std::setw(9) << "venue" << std::right << std::setw(9) << "orders" << std::setw(8) << "fills" << std::setw(10)
     << "qty" << std::setw(10) << "maker" << std::setw(10) << "taker" << std::setw(10) << "fees" << std::setw(17) << "rtt p50/p99 us"
     << std::setw(13) << "bg trades" << "\n";
  for (std::size_t i = 0; i < r.venues.size(); ++i) {
    const VenueFirmStats& v = r.venues[i];
    os << "  " << std::left << std::setw(9) << v.name << std::right << std::setw(9) << commas(v.orders_sent) << std::setw(8) << commas(v.fills)
       << std::setw(10) << commas(static_cast<std::uint64_t>(v.qty)) << std::setw(10) << commas(static_cast<std::uint64_t>(v.maker_qty))
       << std::setw(10) << commas(static_cast<std::uint64_t>(v.taker_qty)) << std::setw(10) << std::setprecision(1) << v.fees
       << std::setw(12) << std::to_string(v.round_trip.percentile(50) / 1000) + "/" + std::to_string(v.round_trip.percentile(99) / 1000)
       << std::setw(13) << commas(i < r.noise_trades.size() ? r.noise_trades[i] : 0) << "\n";
  }
  os << "\nRisk gate: " << commas(r.risk_rejects) << " rejects";
  for (const auto& [why, n] : r.reject_breakdown) os << "  " << to_string(why) << "=" << commas(n);
  if (r.kills) os << "  KILL SWITCH TRIPPED x" << r.kills;
  os << "\n";
  if (r.mm_quotes) os << "Market maker: " << commas(r.mm_quotes) << " quotes, " << commas(r.mm_amends) << " amends\n";
}

void write_equity_csv(std::ostream& os, const SimReport& r) {
  os << "time_s,firm";
  for (const StrategyReport& s : r.strategies) os << ',' << s.name;
  os << "\n" << std::fixed << std::setprecision(4);
  for (std::size_t i = 0; i < r.equity.size(); ++i) {
    os << static_cast<double>(r.equity[i].first) / 1e9 << ',' << r.equity[i].second;
    for (const auto& se : r.strategy_equity) os << ',' << (i < se.size() ? se[i].second : 0.0);
    os << "\n";
  }
}

}  // namespace qc
