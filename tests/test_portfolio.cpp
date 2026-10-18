#include "qc/portfolio.hpp"
#include "qc/rng.hpp"
#include "testing.hpp"

using namespace qc;

namespace {
constexpr SymbolId S = 0;
Price px(double units) { return to_ticks(units); }
}  // namespace

TEST(pnl_long_round_trip) {
  for (CostBasis cb : {CostBasis::AverageCost, CostBasis::Fifo}) {
    Portfolio p(cb);
    p.on_fill(S, Side::Buy, 100, px(100.0));
    p.on_fill(S, Side::Sell, 100, px(101.0));
    MarkTable m;
    m.set(S, px(101.0));
    CHECK_NEAR(p.realized(), 100.0, 1e-9);
    CHECK_NEAR(p.unrealized(m), 0.0, 1e-9);
    CHECK_EQ(p.position(S), 0);
    CHECK_NEAR(p.cash(), 100.0, 1e-9);
  }
}

TEST(pnl_short_round_trip) {
  for (CostBasis cb : {CostBasis::AverageCost, CostBasis::Fifo}) {
    Portfolio p(cb);
    p.on_fill(S, Side::Sell, 100, px(101.0));
    p.on_fill(S, Side::Buy, 100, px(100.0));
    CHECK_NEAR(p.realized(), 100.0, 1e-9);  // short profits when price falls
    CHECK_EQ(p.position(S), 0);
  }
}

TEST(pnl_flip_through_zero) {
  for (CostBasis cb : {CostBasis::AverageCost, CostBasis::Fifo}) {
    Portfolio p(cb);
    p.on_fill(S, Side::Buy, 100, px(100.0));
    p.on_fill(S, Side::Sell, 150, px(102.0));  // closes 100 (+200) and opens short 50 @ 102
    CHECK_EQ(p.position(S), -50);
    CHECK_NEAR(p.realized(), 200.0, 1e-9);
    CHECK_NEAR(p.find(S)->avg_cost(), 10200.0, 1e-9);
    MarkTable m;
    m.set(S, px(101.0));
    CHECK_NEAR(p.unrealized(m), 50.0, 1e-9);  // short 50 from 102, now 101
    p.on_fill(S, Side::Buy, 50, px(103.0));   // cover at a loss: -50
    CHECK_NEAR(p.realized(), 150.0, 1e-9);
    CHECK_EQ(p.position(S), 0);
  }
}

TEST(pnl_fifo_vs_average_cost_split_but_same_total) {
  Portfolio avg(CostBasis::AverageCost), fifo(CostBasis::Fifo);
  for (Portfolio* p : {&avg, &fifo}) {
    p->on_fill(S, Side::Buy, 100, px(100.0));
    p->on_fill(S, Side::Buy, 100, px(110.0));
    p->on_fill(S, Side::Sell, 100, px(120.0));
  }
  MarkTable m;
  m.set(S, px(120.0));
  CHECK_NEAR(avg.realized(), 1500.0, 1e-9);   // (120 - 105) * 100
  CHECK_NEAR(fifo.realized(), 2000.0, 1e-9);  // (120 - 100) * 100
  CHECK_NEAR(avg.unrealized(m), 1500.0, 1e-9);
  CHECK_NEAR(fifo.unrealized(m), 1000.0, 1e-9);  // remaining lot cost 110
  CHECK_NEAR(avg.net_pnl(m), 3000.0, 1e-9);
  CHECK_NEAR(fifo.net_pnl(m), 3000.0, 1e-9);
}

TEST(pnl_fees_and_rebates) {
  Portfolio p;
  p.on_fill(S, Side::Buy, 100, px(10.0), 0.30);    // taker fee
  p.on_fill(S, Side::Sell, 100, px(10.0), -0.10);  // maker rebate
  MarkTable m;
  m.set(S, px(10.0));
  CHECK_NEAR(p.realized(), 0.0, 1e-9);
  CHECK_NEAR(p.fees(), 0.20, 1e-9);
  CHECK_NEAR(p.net_pnl(m), -0.20, 1e-9);
  CHECK_NEAR(p.cash(), -0.20, 1e-9);
}

TEST(pnl_unmarked_symbol_has_no_unrealized) {
  Portfolio p;
  p.on_fill(S, Side::Buy, 10, px(50.0));
  MarkTable m;  // no mark set
  CHECK_NEAR(p.unrealized(m), 0.0, 1e-12);
}

TEST(pnl_exposure_and_breakdown) {
  Portfolio p;
  p.on_fill(0, Side::Buy, 100, px(10.0));
  p.on_fill(1, Side::Sell, 50, px(20.0));
  MarkTable m;
  m.set(0, px(11.0));
  m.set(1, px(19.0));
  CHECK_NEAR(p.gross_exposure(m), 100 * 11.0 + 50 * 19.0, 1e-9);
  CHECK_NEAR(p.net_exposure(m), 100 * 11.0 - 50 * 19.0, 1e-9);
  const auto b = p.breakdown(m);
  CHECK_EQ(b.size(), 2u);
  CHECK_NEAR(b[0].unrealized, 100.0, 1e-9);  // long 100, +1.00
  CHECK_NEAR(b[1].unrealized, 50.0, 1e-9);   // short 50, price fell 1.00
  CHECK_NEAR(p.net_pnl(m), 150.0, 1e-9);
}

TEST(pnl_drawdown_tracking) {
  Portfolio p;
  MarkTable m;
  m.set(S, px(100.0));
  p.on_fill(S, Side::Buy, 100, px(100.0));
  p.snapshot(m);
  m.set(S, px(110.0));
  p.snapshot(m);  // equity +1000 (peak)
  m.set(S, px(104.0));
  p.snapshot(m);  // equity +400
  m.set(S, px(108.0));
  p.snapshot(m);
  CHECK_NEAR(p.peak_equity(), 1000.0, 1e-9);
  CHECK_NEAR(p.max_drawdown(), 600.0, 1e-9);
}

// The accounting identity must hold for any sequence of fills, in both cost-basis modes, and the
// two modes must agree on total P&L.
TEST(pnl_identity_holds_under_random_fills) {
  Rng rng(2024);
  for (int trial = 0; trial < 50; ++trial) {
    Portfolio avg(CostBasis::AverageCost, 1000.0), fifo(CostBasis::Fifo, 1000.0);
    MarkTable m;
    for (int i = 0; i < 400; ++i) {
      const SymbolId s = static_cast<SymbolId>(rng.range(0, 2));
      const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      const Qty q = rng.range(1, 60);
      const Price price = 9000 + rng.range(0, 2000);
      const double fee = rng.chance(0.5) ? rng.uniform() * 0.05 : -rng.uniform() * 0.02;
      avg.on_fill(s, side, q, price, fee);
      fifo.on_fill(s, side, q, price, fee);
      m.set(s, price);
    }
    for (Portfolio* p : {&avg, &fifo}) {
      const double lhs = p->equity(m) - p->initial_cash();
      const double rhs = p->realized() + p->unrealized(m) - p->fees();
      CHECK_NEAR(lhs, rhs, 1e-6);
    }
    CHECK_NEAR(avg.net_pnl(m), fifo.net_pnl(m), 1e-6);
    for (SymbolId s = 0; s < 3; ++s) CHECK_EQ(avg.position(s), fifo.position(s));
    CHECK_NEAR(avg.fees(), fifo.fees(), 1e-9);
  }
}
