#include <functional>
#include <map>

#include "qc/csv_io.hpp"
#include "qc/engine.hpp"
#include "qc/exchange.hpp"
#include "qc/workload.hpp"
#include "testing.hpp"

using namespace qc;

namespace {

Command newc(OrderId id, ClientId c, Side s, Price px, Qty q, TimeInForce tif = TimeInForce::GTC, Nanos ts = 0) {
  Command cmd;
  cmd.type = CommandType::New;
  Order& o = cmd.order;
  o.id = id;
  o.client = c;
  o.symbol = 0;
  o.side = s;
  o.price = px;
  o.qty = q;
  o.tif = tif;
  o.ts = ts;
  return cmd;
}

Command cancelc(OrderId id, ClientId c, Nanos ts = 0) {
  Command cmd;
  cmd.type = CommandType::Cancel;
  cmd.order.id = id;
  cmd.order.client = c;
  cmd.order.symbol = 0;
  cmd.order.ts = ts;
  return cmd;
}

Command modifyc(OrderId id, ClientId c, Price px, Qty q, Nanos ts = 0) {
  Command cmd = cancelc(id, c, ts);
  cmd.type = CommandType::Modify;
  cmd.order.price = px;
  cmd.order.qty = q;
  return cmd;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// engine-level reports
// ------------------------------------------------------------------------------------------------

TEST(engine_report_sequence_for_cross) {
  MatchingEngine eng;
  EngineOutput out;
  eng.process(newc(1, 10, Side::Sell, 10000, 30), out);
  out.clear();
  eng.process(newc(2, 20, Side::Buy, 10000, 50), out);
  // New(2), Fill taker/maker pair, and the taker rests with 20
  CHECK_EQ(out.fills.size(), 1u);
  CHECK(out.reports[0].type == ExecType::New);
  CHECK_EQ(out.reports[0].order_id, 2u);
  const ExecReport& taker = out.reports[1];
  const ExecReport& maker = out.reports[2];
  CHECK(taker.type == ExecType::PartialFill);
  CHECK(taker.liquidity == Liquidity::Taker);
  CHECK_EQ(taker.leaves, 20);
  CHECK_EQ(taker.last_qty, 30);
  CHECK(maker.type == ExecType::Fill);
  CHECK(maker.liquidity == Liquidity::Maker);
  CHECK_EQ(maker.order_id, 1u);
  CHECK(maker.side == Side::Sell);
  CHECK_EQ(maker.leaves, 0);
}

TEST(engine_ioc_remainder_reports_expired) {
  MatchingEngine eng;
  EngineOutput out;
  eng.process(newc(1, 10, Side::Sell, 10000, 30), out);
  out.clear();
  eng.process(newc(2, 20, Side::Buy, 10000, 50, TimeInForce::IOC), out);
  CHECK(out.reports.back().type == ExecType::Expired);
  CHECK_EQ(out.reports.back().leaves, 0);
  CHECK(out.reports.back().is_terminal());
}

TEST(engine_cancel_ownership_and_unknown) {
  MatchingEngine eng;
  EngineOutput out;
  eng.process(newc(1, 10, Side::Buy, 10000, 30), out);
  out.clear();
  eng.process(cancelc(1, /*wrong client*/ 11), out);
  CHECK(out.reports.back().type == ExecType::CancelRejected);
  CHECK(out.reports.back().reason == RejectReason::NotOrderOwner);
  CHECK(!out.reports.back().is_terminal());
  CHECK(eng.book(0)->find(1).has_value());
  out.clear();
  eng.process(cancelc(99, 10), out);
  CHECK(out.reports.back().reason == RejectReason::UnknownOrder);
  out.clear();
  eng.process(cancelc(1, 10), out);
  CHECK(out.reports.back().type == ExecType::Cancelled);
  CHECK(!eng.book(0)->find(1).has_value());
}

TEST(engine_modify_reports) {
  MatchingEngine eng;
  EngineOutput out;
  eng.process(newc(1, 10, Side::Sell, 10000, 30), out);
  eng.process(newc(2, 20, Side::Buy, 9900, 10), out);
  out.clear();
  eng.process(modifyc(2, 20, 10000, 10), out);  // reprice through the ask
  CHECK(out.reports[0].type == ExecType::Replaced);
  CHECK_EQ(out.fills.size(), 1u);
  CHECK_EQ(out.reports[1].order_id, 2u);  // the modified order is now the taker...
  CHECK(out.reports[1].type == ExecType::Fill);
  CHECK(out.reports[2].type == ExecType::PartialFill);  // ...and order 1 (the maker) keeps 20
  CHECK_EQ(out.reports[2].leaves, 20);
  out.clear();
  eng.process(modifyc(2, 20, 10000, 10), out);  // order 2 is gone now
  CHECK(out.reports.back().type == ExecType::CancelRejected);
  CHECK(out.reports.back().reason == RejectReason::UnknownOrder);
}

TEST(engine_unknown_symbol) {
  MatchingEngine eng;
  EngineOutput out;
  Command c = newc(1, 10, Side::Buy, 10000, 30);
  c.order.symbol = 70000;
  eng.process(c, out);
  CHECK(out.reports.back().type == ExecType::Rejected);
  CHECK(out.reports.back().reason == RejectReason::UnknownSymbol);
}

// ------------------------------------------------------------------------------------------------
// exchange: risk + ledger integration
// ------------------------------------------------------------------------------------------------

TEST(exchange_fees_rebates_and_pnl) {
  ExchangeConfig cfg;
  cfg.fees.taker_bps = 3.0;
  cfg.fees.maker_bps = -1.0;
  Exchange ex(cfg);
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Sell, 10000, 100), out);  // maker (client 10)
  ex.process(newc(2, 20, Side::Buy, 10000, 100), out);   // taker (client 20)
  const Portfolio* maker = ex.portfolio(10);
  const Portfolio* taker = ex.portfolio(20);
  CHECK(maker && taker);
  CHECK_EQ(maker->position(0), -100);
  CHECK_EQ(taker->position(0), 100);
  CHECK_NEAR(taker->fees(), 100 * 100.0 * 3e-4, 1e-9);   // 10,000 notional * 3 bps = 3.00
  CHECK_NEAR(maker->fees(), -100 * 100.0 * 1e-4, 1e-9);  // rebate 1.00
  // price moves up: taker gains, maker loses
  ex.process(newc(3, 30, Side::Sell, 10100, 10), out);
  ex.process(newc(4, 40, Side::Buy, 10100, 10), out);
  CHECK_NEAR(taker->unrealized(ex.marks()), 100 * 1.0, 1e-9);
  CHECK_NEAR(maker->unrealized(ex.marks()), -100 * 1.0, 1e-9);
}

TEST(exchange_risk_rejects_and_does_not_touch_book) {
  ExchangeConfig cfg;
  cfg.limits.max_order_qty = 100;
  Exchange ex(cfg);
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Buy, 10000, 101), out);
  CHECK_EQ(out.reports.size(), 1u);
  CHECK(out.reports[0].type == ExecType::Rejected);
  CHECK(out.reports[0].reason == RejectReason::RiskOrderSize);
  CHECK(ex.engine().book(0) == nullptr);  // never reached the engine
  CHECK_EQ(ex.stats().risk_rejects, 1u);
  CHECK_EQ(ex.risk().open_order_count(10), 0u);
}

TEST(exchange_risk_disabled_lets_everything_through) {
  ExchangeConfig cfg;
  cfg.risk_enabled = false;
  cfg.limits.max_order_qty = 1;
  Exchange ex(cfg);
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Buy, 10000, 500), out);
  CHECK(out.reports[0].type == ExecType::New);
}

TEST(exchange_cancel_never_gated_even_when_killed) {
  Exchange ex;
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Buy, 10000, 5), out);
  ex.risk().kill(10);
  out.clear();
  ex.process(newc(2, 10, Side::Buy, 10000, 5), out);
  CHECK(out.reports[0].reason == RejectReason::RiskKillSwitch);
  out.clear();
  ex.process(cancelc(1, 10), out);
  CHECK(out.reports[0].type == ExecType::Cancelled);
  CHECK_EQ(ex.risk().open_buy(10, 0), 0);
}

TEST(exchange_modify_is_risk_checked_against_released_quantity) {
  ExchangeConfig cfg;
  cfg.limits.max_position = 100;
  Exchange ex(cfg);
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Buy, 10000, 100), out);
  out.clear();
  ex.process(modifyc(1, 10, 9900, 100), out);  // same qty at a new price: fits (old 100 released)
  CHECK(out.reports[0].type == ExecType::Replaced);
  out.clear();
  ex.process(modifyc(1, 10, 9900, 101), out);  // would exceed the cap
  CHECK(out.reports[0].type == ExecType::CancelRejected);
  CHECK(out.reports[0].reason == RejectReason::RiskPositionLimit);
  CHECK_EQ(ex.risk().open_buy(10, 0), 100);
  CHECK_EQ(ex.engine().book(0)->find(1)->leaves, 100);  // untouched
}

TEST(exchange_daily_loss_kills_and_mass_cancels) {
  ExchangeConfig cfg;
  cfg.limits.max_daily_loss = 500.0;
  Exchange ex(cfg);
  ExchangeOutput out;
  ex.process(newc(1, 2, Side::Sell, 10000, 100), out);        // B sells 100 @ 100.00
  ex.process(newc(2, 1, Side::Buy, 10000, 100), out);         // A buys 100 @ 100.00
  ex.process(newc(3, 1, Side::Sell, 12000, 50), out);         // A rests an offer far away
  CHECK_EQ(ex.risk().open_sell(1, 0), 50);
  out.clear();
  ex.process(newc(4, 3, Side::Buy, 9000, 10), out);           // E bids 90.00
  ex.process(newc(5, 4, Side::Sell, 9000, 10), out);          // D sells to E at 90.00 -> A marked at 90: -1000
  CHECK(ex.risk().is_killed(1));
  CHECK_EQ(ex.stats().kills, 1u);
  bool saw_cancel = false;
  for (const auto& r : out.reports) saw_cancel |= (r.order_id == 3 && r.type == ExecType::Cancelled);
  CHECK(saw_cancel);
  CHECK(!ex.engine().book(0)->find(3).has_value());
  CHECK_EQ(ex.risk().open_order_count(1), 0u);
  out.clear();
  ex.process(newc(6, 1, Side::Buy, 9000, 1), out);
  CHECK(out.reports[0].reason == RejectReason::RiskKillSwitch);
}

TEST(exchange_duplicate_order_id_keeps_original_tracked) {
  Exchange ex;
  ExchangeOutput out;
  ex.process(newc(1, 10, Side::Buy, 10000, 50), out);
  out.clear();
  ex.process(newc(1, 10, Side::Buy, 10000, 50), out);
  CHECK(out.reports[0].reason == RejectReason::DuplicateOrderId);
  CHECK_EQ(ex.risk().open_buy(10, 0), 50);
  CHECK_EQ(ex.risk().open_order_count(10), 1u);
}

// ------------------------------------------------------------------------------------------------
// global invariants under random flow
// ------------------------------------------------------------------------------------------------

namespace {

void run_invariant_fuzz(StpMode stp, std::uint64_t seed) {
  ExchangeConfig cfg;
  cfg.stp = stp;
  cfg.fees.taker_bps = 2.0;
  cfg.fees.maker_bps = -0.5;
  Exchange ex(cfg);
  WorkloadConfig wc;
  wc.seed = seed;
  wc.count = 30000;
  wc.symbols = 3;
  wc.clients = 6;
  wc.aggressive_frac = 0.25;  // lots of crossing, and with few clients lots of self-trade attempts
  const auto cmds = generate_workload(wc);

  ExchangeOutput out;
  std::map<std::pair<ClientId, SymbolId>, Qty> pos_from_fills;
  auto check = [&](std::size_t step) {
    std::size_t resting_orders = 0, tracked_orders = 0;
    for (SymbolId s = 0; s < wc.symbols; ++s) {
      Qty resting_buy = 0, resting_sell = 0, open_buy = 0, open_sell = 0, net_pos = 0;
      if (const OrderBook* bk = ex.engine().book(s)) {
        std::string why;
        CHECK_MSG(bk->validate(&why), "step " << step << ": " << why);
        std::vector<OrderBook::DepthLevel> d;
        bk->depth(Side::Buy, 1u << 20, d);
        for (auto& l : d) resting_buy += l.qty;
        bk->depth(Side::Sell, 1u << 20, d);
        for (auto& l : d) resting_sell += l.qty;
        resting_orders += bk->order_count();
      }
      for (ClientId c = 1; c <= wc.clients; ++c) {
        open_buy += ex.risk().open_buy(c, s);
        open_sell += ex.risk().open_sell(c, s);
        net_pos += ex.risk().position(c, s);
        const Qty expected_pos = pos_from_fills[std::make_pair(c, s)];
        CHECK_EQ(ex.risk().position(c, s), expected_pos);
      }
      CHECK_EQ(open_buy, resting_buy);    // risk's view of open exposure == what is really resting
      CHECK_EQ(open_sell, resting_sell);
      CHECK_EQ(net_pos, 0);               // every trade has a buyer and a seller
    }
    for (ClientId c = 1; c <= wc.clients; ++c) tracked_orders += ex.risk().open_order_count(c);
    CHECK_EQ(tracked_orders, resting_orders);

    // zero-sum: all clients' trading P&L (before fees) nets to zero when marked at one price
    double sum = 0.0, fees = 0.0;
    for (const auto& [c, pf] : ex.ledger()) {
      sum += pf.realized() + pf.unrealized(ex.marks());
      fees += pf.fees();
      CHECK_NEAR(pf.equity(ex.marks()), pf.realized() + pf.unrealized(ex.marks()) - pf.fees(), 1e-5);
    }
    CHECK_NEAR(sum, 0.0, 1e-4);
    (void)fees;
  };

  std::size_t step = 0;
  for (const Command& c : cmds) {
    out.clear();
    ex.process(c, out);
    for (const Fill& f : out.fills) {
      const Qty q = f.qty;
      pos_from_fills[std::make_pair(f.taker_client, f.symbol)] += f.taker_side == Side::Buy ? q : -q;
      pos_from_fills[std::make_pair(f.maker_client, f.symbol)] += f.taker_side == Side::Buy ? -q : q;
    }
    if (++step % 2500 == 0) check(step);
  }
  check(step);
  CHECK(ex.stats().fills > 1000);  // the fuzz actually traded
}

}  // namespace

TEST(exchange_invariants_no_stp) { run_invariant_fuzz(StpMode::None, 5); }
TEST(exchange_invariants_stp_cancel_resting) { run_invariant_fuzz(StpMode::CancelResting, 6); }
TEST(exchange_invariants_stp_cancel_taker) { run_invariant_fuzz(StpMode::CancelTaker, 7); }

TEST(exchange_is_deterministic) {
  WorkloadConfig wc;
  wc.seed = 77;
  wc.count = 20000;
  const auto cmds = generate_workload(wc);
  auto digest = [&] {
    Exchange ex;
    ExchangeOutput out;
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (const Command& c : cmds) {
      out.clear();
      ex.process(c, out);
      for (const auto& r : out.reports) {
        mix(r.order_id);
        mix(static_cast<std::uint64_t>(r.type));
        mix(static_cast<std::uint64_t>(r.last_qty));
        mix(static_cast<std::uint64_t>(r.last_px));
        mix(static_cast<std::uint64_t>(r.leaves));
      }
    }
    return h;
  };
  CHECK_EQ(digest(), digest());
}

// ------------------------------------------------------------------------------------------------
// workload + csv
// ------------------------------------------------------------------------------------------------

TEST(workload_is_reproducible_and_well_formed) {
  WorkloadConfig wc;
  wc.seed = 3;
  wc.count = 5000;
  const auto a = generate_workload(wc);
  const auto b = generate_workload(wc);
  CHECK_EQ(a.size(), 5000u);
  bool same = a.size() == b.size();
  for (std::size_t i = 0; same && i < a.size(); ++i) {
    same = a[i].type == b[i].type && a[i].order.id == b[i].order.id && a[i].order.price == b[i].order.price &&
           a[i].order.qty == b[i].order.qty && a[i].order.ts == b[i].order.ts;
  }
  CHECK(same);
  Nanos last = 0;
  bool monotone = true;
  for (const auto& c : a) {
    monotone &= c.order.ts >= last;
    last = c.order.ts;
  }
  CHECK(monotone);
  wc.seed = 4;
  CHECK(generate_workload(wc)[10].order.price != a[10].order.price || generate_workload(wc)[11].order.qty != a[11].order.qty);
}

TEST(csv_parses_header_comments_and_defaults) {
  const std::string text =
      "ts_ns,action,order_id,client,symbol,side,type,tif,price,qty,flags\n"
      "# a comment\n"
      "\n"
      "1000,NEW,1,10,AAPL,BUY,LIMIT,GTC,150.25,100\n"
      "1500,new,2,11,aapl,s,market,,,50\n"
      "2000,MODIFY,1,10,AAPL,,,,150.30,80\n"
      "2500,CANCEL,1,10,AAPL\n"
      "3000,NEW,3,10,MSFT,BUY,LIMIT,GTC,10.00,10,POST_ONLY\n";
  std::istringstream in(text);
  SymbolTable syms;
  std::vector<CsvError> errs;
  const auto cmds = read_commands_csv(in, syms, errs);
  CHECK_EQ(errs.size(), 0u);
  CHECK_EQ(cmds.size(), 5u);
  CHECK_EQ(syms.size(), 3u);  // AAPL, aapl (case-sensitive), MSFT
  CHECK_EQ(cmds[0].order.price, 15025);
  CHECK(cmds[0].order.side == Side::Buy);
  CHECK(cmds[1].order.type == OrderType::Market);
  CHECK(cmds[1].order.tif == TimeInForce::IOC);  // market default
  CHECK(cmds[1].order.side == Side::Sell);
  CHECK(cmds[2].type == CommandType::Modify);
  CHECK_EQ(cmds[2].order.price, 15030);
  CHECK_EQ(cmds[2].order.qty, 80);
  CHECK(cmds[3].type == CommandType::Cancel);
  CHECK(cmds[4].order.post_only);
}

TEST(csv_reports_bad_lines_with_line_numbers_and_continues) {
  const std::string text =
      "1000,NEW,1,10,AAPL,BUY,LIMIT,GTC,150.25,100\n"
      "1001,NEW,2,10,AAPL,BUY,LIMIT,GTC,150.255,100\n"   // off the tick grid
      "1002,NEW,3,10,AAPL,SIDEWAYS,LIMIT,GTC,150.25,100\n"
      "1003,FLY,4,10,AAPL\n"
      "xyz,NEW,5,10,AAPL,BUY,LIMIT,GTC,150.25,100\n"
      "1005,NEW,6,10,AAPL,BUY,LIMIT,GTC,150.25,abc\n"
      "1006,NEW,7,10\n"
      "1007,NEW,8,10,AAPL,BUY,LIMIT,GTC,150.25,100\n";
  std::istringstream in(text);
  SymbolTable syms;
  std::vector<CsvError> errs;
  const auto cmds = read_commands_csv(in, syms, errs);
  CHECK_EQ(cmds.size(), 2u);
  CHECK_EQ(errs.size(), 6u);
  CHECK_EQ(errs[0].line, 2u);
  CHECK_EQ(errs[5].line, 7u);
}

TEST(csv_round_trip) {
  WorkloadConfig wc;
  wc.seed = 9;
  wc.count = 3000;
  wc.invalid_frac = 0.0;
  SymbolTable syms;
  for (std::uint32_t s = 0; s < wc.symbols; ++s) syms.intern("SYM" + std::to_string(s));
  const auto cmds = generate_workload(wc);
  std::ostringstream os;
  write_commands_csv(os, cmds, syms);
  std::istringstream is(os.str());
  SymbolTable syms2;
  std::vector<CsvError> errs;
  const auto back = read_commands_csv(is, syms2, errs);
  CHECK_EQ(errs.size(), 0u);
  CHECK_EQ(back.size(), cmds.size());
  bool same = back.size() == cmds.size();
  for (std::size_t i = 0; same && i < cmds.size(); ++i) {
    const Order& a = cmds[i].order;
    const Order& b = back[i].order;
    same = cmds[i].type == back[i].type && a.id == b.id && a.client == b.client && a.ts == b.ts &&
           syms.name(a.symbol) == syms2.name(b.symbol);
    if (cmds[i].type == CommandType::New) {
      same = same && a.side == b.side && a.type == b.type && a.tif == b.tif && a.qty == b.qty &&
             (a.type == OrderType::Market || a.price == b.price);
    }
    if (cmds[i].type == CommandType::Modify) same = same && a.price == b.price && a.qty == b.qty;
  }
  CHECK(same);
}

TEST(price_formatting) {
  CHECK(format_price(15025) == "150.25");
  CHECK(format_price(5) == "0.05");
  CHECK(format_price(100) == "1.00");
  CHECK(format_price(0) == "0.00");
}

// ------------------------------------------------------------------------------------------------
// golden files: the checked-in sample scenario must reproduce byte-for-byte
// ------------------------------------------------------------------------------------------------

#include <fstream>

namespace {
std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

TEST(golden_sample_scenario) {
  const std::string dir = QC_DATA_DIR;
  std::ifstream in(dir + "/sample_orders.csv");
  CHECK(in.good());
  SymbolTable syms;
  std::vector<CsvError> errs;
  const auto cmds = read_commands_csv(in, syms, errs);
  CHECK_EQ(errs.size(), 0u);
  CHECK_EQ(cmds.size(), 18u);

  Exchange ex;
  ExchangeOutput out;
  for (const auto& c : cmds) ex.process(c, out);

  std::ostringstream trades, reports, pnl;
  write_trades_header(trades);
  write_trades(trades, out.fills, syms);
  write_reports_header(reports);
  write_reports(reports, out.reports, syms);
  write_pnl_csv(pnl, ex, syms);
  CHECK(trades.str() == slurp(dir + "/expected/sample_trades.csv"));
  CHECK(reports.str() == slurp(dir + "/expected/sample_reports.csv"));
  CHECK(pnl.str() == slurp(dir + "/expected/sample_pnl.csv"));
}
