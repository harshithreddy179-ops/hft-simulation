#include <cmath>

#include "qc/rng.hpp"
#include "qc/router.hpp"
#include "testing.hpp"

using namespace qc;

namespace {

DepthSnapshot snap(VenueId v, std::vector<LevelQuote> bids, std::vector<LevelQuote> asks, Nanos ts = 0, SymbolId sym = 0) {
  DepthSnapshot s;
  s.venue = v;
  s.symbol = sym;
  s.venue_ts = ts;
  s.n_bids = static_cast<std::uint8_t>(bids.size());
  s.n_asks = static_cast<std::uint8_t>(asks.size());
  for (std::size_t i = 0; i < bids.size(); ++i) s.bids[i] = bids[i];
  for (std::size_t i = 0; i < asks.size(); ++i) s.asks[i] = asks[i];
  return s;
}

const ChildPlan* child_for(const RoutePlan& p, VenueId v) {
  for (const ChildPlan& c : p) {
    if (c.venue == v) return &c;
  }
  return nullptr;
}

RouteRequest buy(Qty q, Price limit = 0, Nanos now = 0) {
  RouteRequest r;
  r.side = Side::Buy;
  r.qty = q;
  r.limit = limit;
  r.now = now;
  return r;
}
RouteRequest sell(Qty q, Price limit = 0, Nanos now = 0) {
  RouteRequest r = buy(q, limit, now);
  r.side = Side::Sell;
  return r;
}

}  // namespace

TEST(consolidated_book_nbbo_mid_and_cross) {
  ConsolidatedBook cb;
  CHECK(!cb.best_bid(0).valid);
  cb.update(snap(0, {{9990, 100}}, {{10010, 50}}), 1);
  cb.update(snap(1, {{9995, 70}}, {{10005, 30}}), 1);
  cb.update(snap(2, {{9995, 10}}, {{10020, 40}}), 1);
  const auto bid = cb.best_bid(0), ask = cb.best_ask(0);
  CHECK_EQ(bid.price, 9995);
  CHECK_EQ(bid.qty, 80);  // 70 + 10 aggregated at the national best bid
  CHECK_EQ(bid.venue, 1u);
  CHECK_EQ(ask.price, 10005);
  CHECK_EQ(ask.venue, 1u);
  double mid = 0;
  CHECK(cb.mid(0, mid));
  CHECK_NEAR(mid, 10000.0, 1e-9);
  CHECK(!cb.crossed(0));
  cb.update(snap(2, {{10006, 10}}, {{10020, 40}}, 5), 6);  // a venue bids through another's offer
  CHECK(cb.crossed(0));
}

TEST(consolidated_book_ignores_out_of_order_updates) {
  ConsolidatedBook cb;
  cb.update(snap(0, {{9990, 100}}, {{10010, 50}}, /*ts=*/100), 100);
  cb.update(snap(0, {{9000, 1}}, {{11000, 1}}, /*ts=*/50), 101);  // older: dropped
  CHECK_EQ(cb.best_bid(0).price, 9990);
  CHECK_EQ(cb.received_at(0, 0), 100);
  cb.update(snap(0, {{9991, 100}}, {{10010, 50}}, /*ts=*/100), 102);  // same ts: accepted
  CHECK_EQ(cb.best_bid(0).price, 9991);
}

TEST(sor_takes_best_price_across_venues) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}, {10002, 100}}), 0);
  cb.update(snap(1, {}, {{10001, 100}, {10003, 100}}), 0);
  SmartOrderRouter sor({{0, 0, 0, 0}, {1, 0, 0, 0}});
  const RoutePlan p = sor.route(buy(250), cb);
  CHECK_EQ(p.planned, 250);
  CHECK_EQ(p.unplanned, 0);
  // consumption order: 10000(v0) 10001(v1) 10002(v0) -> 100 + 100 + 50 = 250 shares
  CHECK_EQ(child_for(p, 0)->qty, 150);  // 100 @10000 + 50 @10002
  CHECK_EQ(child_for(p, 0)->limit, 10002);
  CHECK_EQ(child_for(p, 1)->qty, 100);  // 100 @10001
  CHECK_EQ(child_for(p, 1)->limit, 10001);
  CHECK_NEAR(p.est_cost, 100.0 * 10000 + 100.0 * 10001 + 50.0 * 10002, 1e-6);
}

TEST(sor_sell_side_is_symmetric) {
  ConsolidatedBook cb;
  cb.update(snap(0, {{10000, 100}, {9998, 100}}, {}), 0);
  cb.update(snap(1, {{9999, 100}, {9997, 100}}, {}), 0);
  SmartOrderRouter sor({{0, 0, 0, 0}, {1, 0, 0, 0}});
  const RoutePlan p = sor.route(sell(250), cb);
  CHECK_EQ(p.planned, 250);
  // consumption order: 10000(v0) 9999(v1) 9998(v0)
  CHECK_EQ(child_for(p, 0)->qty, 150);
  CHECK_EQ(child_for(p, 0)->limit, 9998);  // worst price taken at venue 0
  CHECK_EQ(child_for(p, 1)->qty, 100);
  CHECK_EQ(child_for(p, 1)->limit, 9999);
}

TEST(sor_prefers_cheaper_all_in_price_over_better_displayed_price) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}}), 0);  // 100.00 but 3 bps taker fee => 100.03 all-in
  cb.update(snap(1, {}, {{10001, 100}}), 0);  // 100.01 and 0.1 bps          => 100.011
  SmartOrderRouter sor({{0, 3.0, 0, 0}, {1, 0.1, 0, 0}});
  const RoutePlan p = sor.route(buy(100), cb);
  CHECK_EQ(p.n_children, 1);
  CHECK_EQ(p.child[0].venue, 1u);
  // for a seller the fee eats proceeds, so the fee-heavy venue is penalised the same way
  ConsolidatedBook cs;
  cs.update(snap(0, {{10000, 100}}, {}), 0);
  cs.update(snap(1, {{9999, 100}}, {}), 0);
  SmartOrderRouter sor2({{0, 3.0, 0, 0}, {1, 0.1, 0, 0}});
  CHECK_EQ(sor2.route(sell(100), cs).child[0].venue, 1u);
}

TEST(sor_ties_go_to_the_faster_venue) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}}), 0);
  cb.update(snap(1, {}, {{10000, 100}}), 0);
  SmartOrderRouter sor({{0, 0, 0, 500'000}, {1, 0, 0, 50'000}});
  const RoutePlan p = sor.route(buy(100), cb);
  CHECK_EQ(p.n_children, 1);
  CHECK_EQ(p.child[0].venue, 1u);
}

TEST(sor_respects_limit_price_and_reports_unplanned) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 50}, {10005, 50}}), 0);
  cb.update(snap(1, {}, {{10002, 50}, {10010, 500}}), 0);
  SmartOrderRouter sor({{0, 0, 0, 0}, {1, 0, 0, 0}});
  const RoutePlan p = sor.route(buy(1000, /*limit=*/10005), cb);
  CHECK_EQ(p.planned, 150);  // 50@10000 + 50@10002 + 50@10005; the 10010 level is out of bounds
  CHECK_EQ(p.unplanned, 850);
  for (const ChildPlan& c : p) CHECK(c.limit <= 10005);
  const RoutePlan none = sor.route(buy(10, /*limit=*/9000), cb);
  CHECK_EQ(none.planned, 0);
  CHECK_EQ(none.unplanned, 10);
  CHECK_EQ(none.n_children, 0);
}

TEST(sor_slippage_allowance_widens_child_limit_but_never_past_parent_limit) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}}), 0);
  SorConfig cfg;
  cfg.slippage_ticks = 3;
  SmartOrderRouter sor({{0, 0, 0, 0}}, cfg);
  CHECK_EQ(sor.route(buy(100), cb).child[0].limit, 10003);
  CHECK_EQ(sor.route(buy(100, 10001), cb).child[0].limit, 10001);
  ConsolidatedBook cs;
  cs.update(snap(0, {{10000, 100}}, {}), 0);
  CHECK_EQ(sor.route(sell(100), cs).child[0].limit, 9997);
}

TEST(sor_discounts_stale_liquidity) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}}, /*ts=*/0), 0);                   // 5 ms old
  cb.update(snap(1, {}, {{10001, 100}}, /*ts=*/4'900'000), 4'900'000);  // 0.1 ms old
  SorConfig cfg;
  cfg.stale_decay_ns = 1'000'000;  // 1 ms
  SmartOrderRouter sor({{0, 0, 0, 0}, {1, 0, 0, 0}}, cfg);
  const RoutePlan p = sor.route(buy(100, 0, /*now=*/5'000'000), cb);
  // venue 0: e^-5 => ~0 shares expected, so the order goes to the fresher (slightly worse) venue
  CHECK(child_for(p, 0) == nullptr);
  CHECK_EQ(child_for(p, 1)->qty, 90);  // floor(100 * e^-0.1) = 90
  CHECK_EQ(p.unplanned, 10);
}

TEST(sor_min_child_qty_drops_dust) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 100}}), 0);
  cb.update(snap(1, {}, {{10001, 3}}), 0);
  SorConfig cfg;
  cfg.min_child_qty = 5;
  SmartOrderRouter sor({{0, 0, 0, 0}, {1, 0, 0, 0}}, cfg);
  const RoutePlan p = sor.route(buy(103), cb);
  CHECK_EQ(p.n_children, 1);
  CHECK_EQ(p.planned, 100);
  CHECK_EQ(p.unplanned, 3);
}

TEST(sor_baseline_policies) {
  ConsolidatedBook cb;
  cb.update(snap(0, {}, {{10000, 300}}), 0);
  cb.update(snap(1, {}, {{10050, 100}}), 0);
  SorConfig pr;
  pr.policy = RoutingPolicy::ProRata;
  SmartOrderRouter prorata({{0, 0, 0, 0}, {1, 0, 0, 0}}, pr);
  const RoutePlan a = prorata.route(buy(200), cb);
  CHECK_EQ(a.planned, 200);
  CHECK_EQ(child_for(a, 0)->qty, 150);  // 3:1 by displayed size, price-blind
  CHECK_EQ(child_for(a, 1)->qty, 50);

  SorConfig pm;
  pm.policy = RoutingPolicy::Primary;
  pm.primary = 1;
  SmartOrderRouter primary({{0, 0, 0, 0}, {1, 0, 0, 0}}, pm);
  const RoutePlan b = primary.route(buy(200), cb);
  CHECK_EQ(b.planned, 100);  // only what the primary displays
  CHECK_EQ(b.unplanned, 100);
  CHECK_EQ(b.n_children, 1);
  CHECK_EQ(b.child[0].venue, 1u);
}

TEST(sor_empty_inputs) {
  ConsolidatedBook cb;
  SmartOrderRouter sor({{0, 0, 0, 0}});
  CHECK_EQ(sor.route(buy(100), cb).planned, 0);  // no snapshots at all
  cb.update(snap(0, {{9990, 10}}, {}), 0);
  CHECK_EQ(sor.route(buy(100), cb).planned, 0);  // no asks
  CHECK_EQ(sor.route(buy(0), cb).n_children, 0);
  CHECK_EQ(sor.route(buy(-5), cb).n_children, 0);
}

// ------------------------------------------------------------------------------------------------
// properties over random markets
// ------------------------------------------------------------------------------------------------

namespace {

struct RandomMarket {
  ConsolidatedBook cb;
  std::vector<VenueInfo> venues;
};

RandomMarket make_market(Rng& rng) {
  RandomMarket m;
  const int nv = static_cast<int>(rng.range(1, 5));
  for (int v = 0; v < nv; ++v) {
    m.venues.push_back(VenueInfo{static_cast<VenueId>(v), rng.uniform() * 4.0, -rng.uniform(), rng.range(10'000, 500'000)});
    std::vector<LevelQuote> bids, asks;
    Price a = 10000 + rng.range(0, 6), b = 10000 - rng.range(0, 6);
    const int nl = static_cast<int>(rng.range(0, kDepthLevels));
    for (int i = 0; i < nl; ++i) {
      asks.push_back({a, rng.range(1, 300)});
      bids.push_back({b, rng.range(1, 300)});
      a += rng.range(1, 3);
      b -= rng.range(1, 3);
    }
    m.cb.update(snap(static_cast<VenueId>(v), bids, asks, 0), 0);
  }
  return m;
}

Qty displayed_within(const ConsolidatedBook& cb, VenueId v, Side side, Price limit) {
  const DepthSnapshot* s = cb.snapshot(v, 0);
  if (!s) return 0;
  Qty t = 0;
  const auto& lv = side == Side::Buy ? s->asks : s->bids;
  const std::size_t n = side == Side::Buy ? s->n_asks : s->n_bids;
  for (std::size_t i = 0; i < n; ++i) {
    const bool ok = limit == 0 || (side == Side::Buy ? lv[i].price <= limit : lv[i].price >= limit);
    if (ok) t += lv[i].qty;
  }
  return t;
}

}  // namespace

TEST(sor_properties_hold_on_random_markets) {
  Rng rng(808);
  for (int trial = 0; trial < 5000; ++trial) {
    RandomMarket m = make_market(rng);
    const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    RouteRequest rq;
    rq.side = side;
    rq.qty = rng.range(1, 1500);
    rq.limit = rng.chance(0.5) ? 0 : (side == Side::Buy ? 10000 + rng.range(0, 12) : 10000 - rng.range(0, 12));
    for (RoutingPolicy pol : {RoutingPolicy::BestPrice, RoutingPolicy::ProRata, RoutingPolicy::Primary}) {
      SorConfig cfg;
      cfg.policy = pol;
      SmartOrderRouter sor(m.venues, cfg);
      const RoutePlan p = sor.route(rq, m.cb);
      Qty sum = 0;
      for (const ChildPlan& c : p) {
        CHECK(c.qty > 0);
        CHECK(c.qty <= displayed_within(m.cb, c.venue, side, rq.limit));  // never more than is shown
        if (rq.limit > 0) CHECK(side == Side::Buy ? c.limit <= rq.limit : c.limit >= rq.limit);
        sum += c.qty;
      }
      CHECK_EQ(sum, p.planned);
      CHECK_EQ(p.planned + p.unplanned, rq.qty);
      CHECK(p.planned <= rq.qty);
    }
  }
}

TEST(sor_best_price_is_never_costlier_than_baselines_when_all_fully_fill) {
  Rng rng(909);
  int compared = 0;
  for (int trial = 0; trial < 5000; ++trial) {
    RandomMarket m = make_market(rng);
    RouteRequest rq;
    rq.side = rng.chance(0.5) ? Side::Buy : Side::Sell;
    rq.qty = rng.range(1, 400);
    SorConfig best_cfg, pr_cfg, pm_cfg;
    pr_cfg.policy = RoutingPolicy::ProRata;
    pm_cfg.policy = RoutingPolicy::Primary;
    pm_cfg.primary = m.venues[0].id;
    const RoutePlan best = SmartOrderRouter(m.venues, best_cfg).route(rq, m.cb);
    for (const SorConfig& other_cfg : {pr_cfg, pm_cfg}) {
      const RoutePlan other = SmartOrderRouter(m.venues, other_cfg).route(rq, m.cb);
      if (best.planned != rq.qty || other.planned != rq.qty) continue;
      ++compared;
      if (rq.side == Side::Buy) {
        CHECK_MSG(best.est_cost <= other.est_cost + 1e-6, "buy: " << best.est_cost << " vs " << other.est_cost);
      } else {
        CHECK_MSG(best.est_cost >= other.est_cost - 1e-6, "sell: " << best.est_cost << " vs " << other.est_cost);
      }
    }
  }
  CHECK(compared > 1000);  // the property was actually exercised
}

TEST(sor_is_a_pure_function) {
  Rng rng(1);
  RandomMarket m = make_market(rng);
  SmartOrderRouter sor(m.venues);
  RouteRequest rq = buy(500);
  const RoutePlan a = sor.route(rq, m.cb);
  const RoutePlan b = sor.route(rq, m.cb);
  CHECK_EQ(a.n_children, b.n_children);
  CHECK_EQ(a.planned, b.planned);
  CHECK_NEAR(a.est_cost, b.est_cost, 0.0);
}
