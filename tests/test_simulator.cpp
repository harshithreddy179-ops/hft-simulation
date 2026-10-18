#include <cmath>

#include "qc/flow.hpp"
#include "qc/simulator.hpp"
#include "testing.hpp"

using namespace qc;

namespace {

SimConfig quick(std::uint64_t seed = 42, Nanos secs = 5) {
  SimConfig c;
  c.seed = seed;
  c.duration = secs * kSecond;
  c.twap.start = 500 * kMilli;
  c.twap.duration = 3 * kSecond;
  c.twap.total_qty = 8000;
  c.twap.slices = 20;
  return c;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// building blocks
// ------------------------------------------------------------------------------------------------

TEST(event_queue_orders_by_time_then_scheduling_order) {
  EventQueue q;
  q.schedule(100, EvTimer{0, 1});
  q.schedule(50, EvTimer{0, 2});
  q.schedule(100, EvTimer{0, 3});
  q.schedule(100, EvTimer{0, 4});
  std::vector<std::uint32_t> order;
  while (!q.empty()) order.push_back(std::get<EvTimer>(q.pop().payload).id);
  CHECK((order == std::vector<std::uint32_t>{2, 1, 3, 4}));  // FIFO among simultaneous events
  CHECK_EQ(q.now(), 100);
  q.schedule(10, EvTimer{0, 5});  // scheduling into the past is clamped to "now"
  CHECK_EQ(q.next_time(), 100);
}

TEST(network_channels_are_fifo_despite_jitter) {
  std::vector<VenueConfig> v(1);
  v[0].latency_in = 10 * kMicro;
  v[0].jitter = 500 * kMicro;  // jitter far larger than the send spacing
  Network net(v, 1);
  Nanos last = 0;
  bool monotone = true;
  for (int i = 0; i < 20000; ++i) {
    const Nanos at = net.to_venue(0, static_cast<Nanos>(i) * kMicro);
    monotone &= at >= last;
    monotone &= at >= static_cast<Nanos>(i) * kMicro + 10 * kMicro;  // never faster than the base latency
    last = at;
  }
  CHECK(monotone);
}

TEST(flow_generator_is_reproducible_and_seeds_a_two_sided_book) {
  const VenueConfig vc = default_venues()[0];
  FlowConfig fc;
  FlowGenerator a(0, vc, fc, 5), b(0, vc, fc, 5);
  const auto ia = a.initial_book(0, 10000.0);
  const auto ib = b.initial_book(0, 10000.0);
  CHECK_EQ(ia.size(), 2 * fc.seed_levels);
  bool same = ia.size() == ib.size();
  Price best_bid = 0, best_ask = kMaxPrice;
  for (std::size_t i = 0; same && i < ia.size(); ++i) {
    same = ia[i].order.price == ib[i].order.price && ia[i].order.qty == ib[i].order.qty;
    if (ia[i].order.side == Side::Buy) best_bid = std::max(best_bid, ia[i].order.price);
    else best_ask = std::min(best_ask, ia[i].order.price);
  }
  CHECK(same);
  CHECK(best_bid < best_ask);
  for (int i = 0; i < 1000; ++i) {
    const Command x = a.next(i, 10000.0), y = b.next(i, 10000.0);
    CHECK(x.type == y.type && x.order.price == y.order.price && x.order.qty == y.order.qty);
  }
}

// ------------------------------------------------------------------------------------------------
// whole-simulation properties
// ------------------------------------------------------------------------------------------------

TEST(simulation_is_deterministic_for_a_seed) {
  const SimReport a = Simulator(quick(7)).run();
  const SimReport b = Simulator(quick(7)).run();
  CHECK_EQ(a.events, b.events);
  CHECK_NEAR(a.firm_net, b.firm_net, 0.0);
  CHECK_EQ(a.execution.parents, b.execution.parents);
  CHECK_EQ(a.risk_rejects, b.risk_rejects);
  for (std::size_t i = 0; i < a.venues.size(); ++i) CHECK_EQ(a.venues[i].qty, b.venues[i].qty);
  const SimReport c = Simulator(quick(8)).run();
  CHECK(c.events != a.events || std::fabs(c.firm_net - a.firm_net) > 1e-9);  // a different seed is a different world
}

TEST(simulation_books_balance) {
  const SimReport r = Simulator(quick()).run();
  CHECK(r.execution.parents > 50);
  // risk engine and portfolios are independent bookkeepers of the same position
  CHECK_EQ(r.risk_position, r.firm_position);
  // every fill is counted once per venue and once per strategy
  Qty venue_qty = 0, strat_qty = 0;
  double venue_fees = 0, strat_fees = 0;
  for (const auto& v : r.venues) {
    venue_qty += v.qty;
    venue_fees += v.fees;
  }
  for (const auto& s : r.strategies) {
    strat_qty += s.volume;
    strat_fees += s.fees;
  }
  CHECK_EQ(venue_qty, strat_qty);
  CHECK_NEAR(venue_fees, strat_fees, 1e-6);
  // maker + taker volume partition total volume
  for (const auto& v : r.venues) CHECK_EQ(v.maker_qty + v.taker_qty, v.qty);
  // only the market maker's two resting quotes may still be live after the drain
  CHECK(r.open_orders <= 2);
  // total P&L is the sum of the parts
  double sum = 0;
  for (const auto& s : r.strategies) sum += s.net;
  CHECK_NEAR(sum, r.firm_net, 1e-6);
  for (const auto& s : r.strategies) CHECK_NEAR(s.net, s.realized + s.unrealized - s.fees, 1e-6);
}

TEST(simulation_market_is_sane) {
  const SimReport r = Simulator(quick()).run();
  CHECK(r.crossed_fraction < 0.01);  // the arbitrageur keeps the consolidated book uncrossed
  CHECK(r.avg_spread_ticks > 0.3 && r.avg_spread_ticks < 5.0);
  CHECK(r.arb_trades > 0);
  for (std::uint64_t n : r.noise_trades) CHECK(n > 100);  // every venue actually traded
  // round trips reflect the configured latencies: ALPHA is the fastest venue, BRAVO the slowest
  CHECK(r.venues[0].round_trip.percentile(50) < r.venues[2].round_trip.percentile(50));
  CHECK(r.venues[2].round_trip.percentile(50) < r.venues[1].round_trip.percentile(50));
}

TEST(simulation_twap_completes_and_reports_benchmarks) {
  const SimReport r = Simulator(quick()).run();
  CHECK(r.has_twap);
  CHECK(r.twap.filled >= r.twap.target * 99 / 100);
  CHECK(r.twap.slices_sent >= 19);
  CHECK(r.twap.arrival_mid_ticks > 0 && r.twap.interval_mid_ticks > 0);
  CHECK(std::fabs(r.twap.vs_twap_bps) < 25.0);  // bought within a few bps of the interval mid
}

TEST(simulation_market_maker_quotes_and_respects_inventory_cap) {
  SimConfig cfg = quick();
  cfg.enable_parent_flow = false;
  cfg.enable_twap = false;
  cfg.mm.max_inventory = 300;
  const SimReport r = Simulator(cfg).run();
  CHECK(r.mm_quotes > 0);
  CHECK(r.mm_amends > 0);
  CHECK(r.strategies.size() == 1);
  // each side stops at the cap, but fills can overshoot by one quote's size while orders are in flight
  CHECK(std::llabs(r.strategies[0].position) <= cfg.mm.max_inventory + 3 * cfg.mm.quote_size);
  CHECK(r.strategies[0].fills > 50);
  CHECK(r.strategies[0].fees < 0);  // it posts on the rebate venue: fees are net negative (income)
}

TEST(simulation_kill_switch_cancels_everything_and_blocks_new_orders) {
  SimConfig cfg = quick(42, 6);
  cfg.firm_limits.max_daily_loss = 300.0;  // the taker flow burns this quickly
  const SimReport r = Simulator(cfg).run();
  CHECK_EQ(r.kills, 1u);
  CHECK(r.kill_time > 0 && r.kill_time < 5 * kSecond);
  std::uint64_t blocked = 0;
  for (const auto& [why, n] : r.reject_breakdown) {
    if (why == RejectReason::RiskKillSwitch) blocked = n;
  }
  CHECK(blocked > 0);           // later orders were refused
  CHECK_EQ(r.open_orders, 0u);  // nothing is left resting
  // The decisive check: the book is dead after a kill. Only orders already in flight (a couple of
  // round trips, ~1 ms) may still fill; resting quotes that failed to cancel would be hit for much longer.
  CHECK(r.last_fill_after_kill - r.kill_time < 3 * kMilli);
}

TEST(simulation_risk_position_limit_is_enforced) {
  SimConfig cfg = quick();
  cfg.enable_market_maker = false;
  cfg.firm_limits.max_position = 3000;
  const SimReport r = Simulator(cfg).run();
  std::uint64_t rejected = 0;
  for (const auto& [why, n] : r.reject_breakdown) {
    if (why == RejectReason::RiskPositionLimit) rejected = n;
  }
  CHECK(rejected > 0);
  // worst-case accounting (open + in-flight counted) means the cap is never breached by fills
  CHECK(std::llabs(r.firm_position) <= 3000);
}

TEST(smart_router_beats_the_baselines_on_execution_cost) {
  int beat_prorata = 0, beat_primary = 0;
  for (std::uint64_t seed : {1, 2, 3}) {
    double cost[3];
    int i = 0;
    for (RoutingPolicy p : {RoutingPolicy::BestPrice, RoutingPolicy::ProRata, RoutingPolicy::Primary}) {
      SimConfig cfg = quick(seed, 6);
      cfg.enable_market_maker = false;
      cfg.sor.policy = p;
      cost[i++] = Simulator(cfg).run().execution.total_cost_bps;
    }
    beat_prorata += cost[0] < cost[1];
    beat_primary += cost[0] < cost[2];
  }
  CHECK_EQ(beat_prorata, 3);
  CHECK_EQ(beat_primary, 3);
}

TEST(simulation_runs_with_a_single_venue) {
  SimConfig cfg = quick();
  cfg.venues = {default_venues()[0]};
  const SimReport r = Simulator(cfg).run();
  CHECK_EQ(r.venues.size(), 1u);
  CHECK(r.execution.parents > 10);
  CHECK_EQ(r.risk_position, r.firm_position);
}
