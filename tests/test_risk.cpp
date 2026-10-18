#include "qc/risk.hpp"
#include "testing.hpp"

using namespace qc;

namespace {

constexpr ClientId C = 1;
constexpr SymbolId S = 0;

Order mk(OrderId id, Side side, Price px, Qty qty, ClientId c = C) {
  Order o;
  o.id = id;
  o.client = c;
  o.symbol = S;
  o.side = side;
  o.price = px;
  o.qty = qty;
  return o;
}

ExecReport fill_report(const Order& o, Qty q, Qty leaves) {
  ExecReport r;
  r.order_id = o.id;
  r.client = o.client;
  r.symbol = o.symbol;
  r.side = o.side;
  r.type = leaves == 0 ? ExecType::Fill : ExecType::PartialFill;
  r.last_px = o.price;
  r.last_qty = q;
  r.leaves = leaves;
  return r;
}

ExecReport terminal_report(const Order& o, ExecType t) {
  ExecReport r;
  r.order_id = o.id;
  r.client = o.client;
  r.symbol = o.symbol;
  r.side = o.side;
  r.type = t;
  r.leaves = 0;
  return r;
}

}  // namespace

TEST(risk_passes_ordinary_order) {
  RiskEngine r;
  CHECK(r.check(mk(1, Side::Buy, 10000, 100), 0) == RejectReason::None);
}

TEST(risk_order_size_and_notional) {
  RiskLimits l;
  l.max_order_qty = 500;
  l.max_order_notional = 10000.0;  // currency units
  RiskEngine r(l);
  CHECK(r.check(mk(1, Side::Buy, 10000, 501), 0) == RejectReason::RiskOrderSize);
  CHECK(r.check(mk(2, Side::Buy, 10000, 101), 0) == RejectReason::RiskOrderNotional);  // 101 * 100.00 = 10100
  CHECK(r.check(mk(3, Side::Buy, 10000, 100), 0) == RejectReason::None);               // exactly 10000 is allowed
  CHECK_EQ(r.reject_count(RejectReason::RiskOrderSize), 1u);
  CHECK_EQ(r.total_rejects(), 2u);
}

TEST(risk_price_collar) {
  RiskLimits l;
  l.price_collar_bps = 500;  // 5%
  RiskEngine r(l);
  CHECK(r.check(mk(1, Side::Buy, 10600, 10), 0) == RejectReason::None);  // no reference yet => unchecked
  r.set_reference_price(S, 10000);
  CHECK(r.check(mk(2, Side::Buy, 10600, 10), 0) == RejectReason::RiskPriceCollar);  // +6%
  CHECK(r.check(mk(3, Side::Sell, 9400, 10), 0) == RejectReason::RiskPriceCollar);  // -6%
  CHECK(r.check(mk(4, Side::Buy, 10400, 10), 0) == RejectReason::None);
  CHECK(r.check(mk(5, Side::Sell, 9600, 10), 0) == RejectReason::None);
}

TEST(risk_market_orders_are_priced_at_reference) {
  RiskLimits l;
  l.max_order_notional = 5000.0;
  RiskEngine r(l);
  Order m = mk(1, Side::Buy, 0, 100);
  m.type = OrderType::Market;
  CHECK(r.check(m, 0) == RejectReason::None);  // no reference: notional cannot be computed
  r.set_reference_price(S, 10000);
  CHECK(r.check(m, 0) == RejectReason::RiskOrderNotional);  // 100 * 100.00 = 10000 > 5000
}

TEST(risk_position_limit_counts_open_orders) {
  RiskLimits l;
  l.max_position = 100;
  RiskEngine r(l);
  const Order a = mk(1, Side::Buy, 10000, 60);
  CHECK(r.check(a, 0) == RejectReason::None);
  r.track(a);
  CHECK(r.check(mk(2, Side::Buy, 10000, 50), 0) == RejectReason::RiskPositionLimit);  // 60 open + 50 > 100
  CHECK(r.check(mk(3, Side::Buy, 10000, 40), 0) == RejectReason::None);               // exactly 100
  CHECK(r.check(mk(4, Side::Sell, 10000, 100), 0) == RejectReason::None);             // short side is separate
  CHECK(r.check(mk(5, Side::Sell, 10000, 101), 0) == RejectReason::RiskPositionLimit);
}

TEST(risk_position_tracks_fills_and_never_blocks_reducing_orders) {
  RiskLimits l;
  l.max_position = 100;
  RiskEngine r(l);
  const Order buy = mk(1, Side::Buy, 10000, 100);
  CHECK(r.check(buy, 0) == RejectReason::None);
  r.track(buy);
  r.on_report(fill_report(buy, 100, 0));
  CHECK_EQ(r.position(C, S), 100);
  CHECK_EQ(r.open_buy(C, S), 0);
  CHECK(r.check(mk(2, Side::Buy, 10000, 1), 0) == RejectReason::RiskPositionLimit);  // at the cap
  // a sell is risk-reducing: allowed, even with a pile of open buys it cannot exceed the cap
  CHECK(r.check(mk(3, Side::Sell, 10000, 100), 0) == RejectReason::None);
  CHECK(r.check(mk(4, Side::Sell, 10000, 200), 0) == RejectReason::None);   // ends at -100: within |max|
  CHECK(r.check(mk(5, Side::Sell, 10000, 201), 0) == RejectReason::RiskPositionLimit);
}

TEST(risk_exposure_released_on_terminal_reports) {
  RiskEngine r;
  const Order a = mk(1, Side::Buy, 10000, 100);
  const Order b = mk(2, Side::Sell, 10100, 50);
  r.track(a);
  r.track(b);
  CHECK_EQ(r.open_buy(C, S), 100);
  CHECK_EQ(r.open_sell(C, S), 50);
  CHECK_EQ(r.open_order_count(C), 2u);
  CHECK_NEAR(r.open_notional(C), 100 * 100.0 + 50 * 101.0, 1e-9);

  r.on_report(fill_report(a, 30, 70));
  CHECK_EQ(r.open_buy(C, S), 70);
  r.on_report(terminal_report(a, ExecType::Cancelled));
  r.on_report(terminal_report(b, ExecType::Expired));
  CHECK_EQ(r.open_buy(C, S), 0);
  CHECK_EQ(r.open_sell(C, S), 0);
  CHECK_EQ(r.open_order_count(C), 0u);
  CHECK_NEAR(r.open_notional(C), 0.0, 1e-9);
  CHECK_EQ(r.position(C, S), 30);
}

TEST(risk_cancel_reject_does_not_release_exposure) {
  RiskEngine r;
  const Order a = mk(1, Side::Buy, 10000, 100);
  r.track(a);
  ExecReport cr = terminal_report(a, ExecType::CancelRejected);
  cr.leaves = 100;
  r.on_report(cr);
  CHECK_EQ(r.open_buy(C, S), 100);
  cr.leaves = 0;  // even with a bogus leaves=0 the order must stay tracked
  r.on_report(cr);
  CHECK_EQ(r.open_buy(C, S), 100);
}

TEST(risk_duplicate_id_reject_does_not_release_original) {
  RiskEngine r;
  const Order a = mk(1, Side::Buy, 10000, 100);
  r.track(a);
  r.track(a);  // duplicate send
  CHECK_EQ(r.open_buy(C, S), 100);
  ExecReport rej = terminal_report(a, ExecType::Rejected);
  rej.reason = RejectReason::DuplicateOrderId;
  r.on_report(rej);
  CHECK_EQ(r.open_buy(C, S), 100);
  CHECK_EQ(r.open_order_count(C), 1u);
}

TEST(risk_open_order_limit) {
  RiskLimits l;
  l.max_open_orders = 2;
  RiskEngine r(l);
  r.track(mk(1, Side::Buy, 10000, 1));
  r.track(mk(2, Side::Buy, 10000, 1));
  CHECK(r.check(mk(3, Side::Buy, 10000, 1), 0) == RejectReason::RiskOpenOrders);
  CHECK(r.check(mk(1, Side::Buy, 10001, 1), 0, /*replacing_leaves=*/1) == RejectReason::None);  // an amend adds no slot
}

TEST(risk_rate_limit_token_bucket) {
  RiskLimits l;
  l.max_orders_per_sec = 10.0;
  l.burst_orders = 5.0;
  RiskEngine r(l);
  Nanos t = 0;
  for (int i = 0; i < 5; ++i) CHECK(r.check(mk(i + 1, Side::Buy, 10000, 1), t) == RejectReason::None);
  CHECK(r.check(mk(6, Side::Buy, 10000, 1), t) == RejectReason::RiskRateLimit);
  t += 50 * kMilli;  // 0.5 tokens
  CHECK(r.check(mk(7, Side::Buy, 10000, 1), t) == RejectReason::RiskRateLimit);
  t += 50 * kMilli;  // 1.0 tokens
  CHECK(r.check(mk(8, Side::Buy, 10000, 1), t) == RejectReason::None);
  CHECK(r.check(mk(9, Side::Buy, 10000, 1), t) == RejectReason::RiskRateLimit);
  t += 10 * kSecond;  // refill is capped at the burst size
  int ok = 0;
  for (int i = 0; i < 10; ++i) ok += r.check(mk(100 + i, Side::Buy, 10000, 1), t) == RejectReason::None;
  CHECK_EQ(ok, 5);
}

TEST(risk_rejected_orders_do_not_consume_rate_tokens) {
  RiskLimits l;
  l.max_orders_per_sec = 1.0;
  l.burst_orders = 1.0;
  l.max_order_qty = 10;
  RiskEngine r(l);
  CHECK(r.check(mk(1, Side::Buy, 10000, 11), 0) == RejectReason::RiskOrderSize);
  CHECK(r.check(mk(2, Side::Buy, 10000, 10), 0) == RejectReason::None);  // token still there
}

TEST(risk_kill_switch_halts_and_resumes) {
  RiskEngine r;
  r.kill(C);
  CHECK(r.check(mk(1, Side::Buy, 10000, 1), 0) == RejectReason::RiskKillSwitch);
  CHECK(r.check(mk(2, Side::Buy, 10000, 1, /*client=*/2), 0) == RejectReason::None);  // other clients unaffected
  r.resume(C);
  CHECK(r.check(mk(3, Side::Buy, 10000, 1), 0) == RejectReason::None);
  r.kill_all();
  CHECK(r.check(mk(4, Side::Buy, 10000, 1, 2), 0) == RejectReason::RiskKillSwitch);
  r.resume_all();
  r.halt_symbol(S);
  CHECK(r.check(mk(5, Side::Buy, 10000, 1), 0) == RejectReason::RiskSymbolHalted);
  r.resume_symbol(S);
  CHECK(r.check(mk(6, Side::Buy, 10000, 1), 0) == RejectReason::None);
}

TEST(risk_daily_loss_trips_kill_switch_once) {
  RiskLimits l;
  l.max_daily_loss = 1000.0;
  RiskEngine r(l);
  CHECK(!r.on_pnl(C, -999.0));
  CHECK(r.on_pnl(C, -1000.0));   // trips
  CHECK(!r.on_pnl(C, -5000.0));  // already killed: reported only once
  CHECK(r.is_killed(C));
  CHECK(r.check(mk(1, Side::Buy, 10000, 1), 0) == RejectReason::RiskKillSwitch);
}

TEST(risk_gross_exposure_limit) {
  RiskLimits l;
  l.max_gross_notional = 20000.0;
  RiskEngine r(l);
  r.set_reference_price(S, 10000);
  const Order a = mk(1, Side::Buy, 10000, 100);  // 10,000
  CHECK(r.check(a, 0) == RejectReason::None);
  r.track(a);
  CHECK(r.check(mk(2, Side::Sell, 10000, 100), 0) == RejectReason::None);  // 20,000 total
  r.track(mk(2, Side::Sell, 10000, 100));
  CHECK(r.check(mk(3, Side::Buy, 10000, 1), 0) == RejectReason::RiskGrossExposure);
  r.on_report(fill_report(a, 100, 0));  // open order becomes a position of equal notional
  CHECK_NEAR(r.gross_exposure(C), 20000.0, 1e-9);
}

TEST(risk_modify_releases_old_quantity) {
  RiskLimits l;
  l.max_position = 100;
  RiskEngine r(l);
  const Order a = mk(1, Side::Buy, 10000, 100);
  r.track(a);
  CHECK(r.check(mk(1, Side::Buy, 10001, 100), 0) == RejectReason::RiskPositionLimit);        // as a new order: 200
  CHECK(r.check(mk(1, Side::Buy, 10001, 100), 0, /*replacing_leaves=*/100) == RejectReason::None);
  CHECK(r.check(mk(1, Side::Buy, 10001, 101), 0, /*replacing_leaves=*/100) == RejectReason::RiskPositionLimit);

  r.on_modify_sent(1, 10001, 150);  // in flight: counted at the larger quantity
  CHECK_EQ(r.open_buy(C, S), 150);
  ExecReport rep = terminal_report(a, ExecType::Replaced);
  rep.leaves = 90;
  r.on_report(rep);
  CHECK_EQ(r.open_buy(C, S), 90);
  CHECK_NEAR(r.open_notional(C), 90 * 100.01, 1e-9);
}

TEST(risk_open_orders_listed_for_mass_cancel) {
  RiskEngine r;
  r.track(mk(5, Side::Buy, 10000, 1));
  r.track(mk(3, Side::Sell, 10000, 1));
  r.track(mk(9, Side::Sell, 10000, 1, /*client=*/2));
  const auto v = r.open_orders(C);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0].first, 3u);
  CHECK_EQ(v[1].first, 5u);
}

TEST(risk_per_client_limits) {
  RiskEngine r;
  RiskLimits tight;
  tight.max_order_qty = 10;
  r.set_limits(2, tight);
  CHECK(r.check(mk(1, Side::Buy, 10000, 100, 1), 0) == RejectReason::None);
  CHECK(r.check(mk(2, Side::Buy, 10000, 100, 2), 0) == RejectReason::RiskOrderSize);
}

TEST(risk_rejects_absurd_symbol_without_allocating) {
  RiskEngine r;
  Order o = mk(1, Side::Buy, 10000, 1);
  o.symbol = 4'000'000'000u;
  CHECK(r.check(o, 0) == RejectReason::UnknownSymbol);
}
