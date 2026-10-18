// qc: command-line front end.
//
//   qc gen    [options]        generate a synthetic order-flow CSV
//   qc replay FILE [options]   run a CSV through risk -> matching -> P&L, write trades/reports/pnl
//   qc help

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>

#include "cli_args.hpp"
#include "qc/csv_io.hpp"
#include "qc/exchange.hpp"
#include "qc/sharded_exchange.hpp"
#include "qc/simulator.hpp"
#include "qc/workload.hpp"

using namespace qc;

namespace {

using Clock = std::chrono::steady_clock;

double secs_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

std::string with_commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

void usage() {
  std::cout <<
R"(qc - Quantactive HFT simulation toolkit

USAGE
  qc gen    [--count N] [--symbols N] [--clients N] [--seed N] [--out FILE]
  qc replay FILE [--out-dir DIR] [--stp none|resting|taker] [--cost-basis avg|fifo]
                 [--taker-fee-bps X] [--maker-fee-bps X] [--no-risk]
                 [--max-order-qty N] [--max-notional X] [--collar-bps N] [--max-position N]
                 [--max-gross X] [--max-open-orders N] [--rate N] [--max-loss X]
                 [--threads N] [--quiet]
  qc sim    [--seconds S] [--seed N] [--policy smart|prorata|primary] [--stale-ms X]
                 [--no-mm] [--no-flow] [--no-twap] [--max-loss X] [--max-position N]
                 [--vol X] [--offset-ticks X] [--noise-ticks K] [--equity-csv FILE] [--compare]
  qc help

`qc gen` writes the order file format described in include/qc/csv_io.hpp.
`qc replay` writes trades.csv, reports.csv and pnl.csv into --out-dir.
)";
}

// ------------------------------------------------------------------------------------------------
// gen
// ------------------------------------------------------------------------------------------------

int cmd_gen(cli::Args& a) {
  WorkloadConfig wc;
  wc.count = static_cast<std::size_t>(a.get_int("count", 100000));
  wc.symbols = static_cast<std::uint32_t>(a.get_int("symbols", 4));
  wc.clients = static_cast<std::uint32_t>(a.get_int("clients", 8));
  wc.seed = static_cast<std::uint64_t>(a.get_int("seed", 1));
  if (wc.symbols == 0 || wc.clients == 0) {
    std::cerr << "--symbols and --clients must be > 0\n";
    return 2;
  }
  SymbolTable syms;
  static const char* names[] = {"AAPL", "MSFT", "GOOG", "AMZN", "NVDA", "TSLA", "META", "NFLX"};
  for (std::uint32_t s = 0; s < wc.symbols; ++s) syms.intern(s < 8 ? names[s] : "SYM" + std::to_string(s));
  const auto cmds = generate_workload(wc);
  const std::string out = a.get_str("out", "");
  if (out.empty()) {
    write_commands_csv(std::cout, cmds, syms);
  } else {
    std::ofstream f(out);
    if (!f) {
      std::cerr << "cannot open " << out << "\n";
      return 1;
    }
    write_commands_csv(f, cmds, syms);
    std::cerr << "wrote " << cmds.size() << " commands to " << out << "\n";
  }
  return 0;
}

// ------------------------------------------------------------------------------------------------
// replay
// ------------------------------------------------------------------------------------------------

ExchangeConfig exchange_config_from(cli::Args& a) {
  ExchangeConfig cfg;
  cfg.risk_enabled = !a.has("no-risk");
  const std::string stp = a.get_str("stp", "none");
  cfg.stp = stp == "resting" ? StpMode::CancelResting : stp == "taker" ? StpMode::CancelTaker : StpMode::None;
  cfg.cost_basis = a.get_str("cost-basis", "avg") == "fifo" ? CostBasis::Fifo : CostBasis::AverageCost;
  cfg.fees.taker_bps = a.get_double("taker-fee-bps", 0.0);
  cfg.fees.maker_bps = a.get_double("maker-fee-bps", 0.0);
  RiskLimits& l = cfg.limits;
  l.max_order_qty = a.get_int("max-order-qty", l.max_order_qty);
  l.max_order_notional = a.get_double("max-notional", l.max_order_notional);
  l.price_collar_bps = static_cast<int>(a.get_int("collar-bps", l.price_collar_bps));
  l.max_position = a.get_int("max-position", l.max_position);
  l.max_gross_notional = a.get_double("max-gross", l.max_gross_notional);
  l.max_open_orders = static_cast<std::uint32_t>(a.get_int("max-open-orders", l.max_open_orders));
  l.max_orders_per_sec = a.get_double("rate", l.max_orders_per_sec);
  l.max_daily_loss = a.get_double("max-loss", l.max_daily_loss);
  return cfg;
}

int cmd_replay(cli::Args& a) {
  if (a.positional.empty()) {
    std::cerr << "usage: qc replay FILE [options]\n";
    return 2;
  }
  std::ifstream in(a.positional[0]);
  if (!in) {
    std::cerr << "cannot open " << a.positional[0] << "\n";
    return 1;
  }
  SymbolTable syms;
  std::vector<CsvError> errors;
  const std::vector<Command> cmds = read_commands_csv(in, syms, errors);
  if (!errors.empty()) {
    std::cerr << errors.size() << " malformed line(s) skipped:\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(errors.size(), 5); ++i) {
      std::cerr << "  line " << errors[i].line << ": " << errors[i].message << "\n";
    }
  }

  const ExchangeConfig cfg = exchange_config_from(a);
  const std::string out_dir = a.get_str("out-dir", "");
  const bool quiet = a.has("quiet");
  const auto threads = static_cast<std::size_t>(std::max<std::int64_t>(0, a.get_int("threads", 0)));

  std::ofstream trades_f, reports_f;
  if (!out_dir.empty()) {
    std::string mk = "mkdir -p '" + out_dir + "'";
    if (std::system(mk.c_str()) != 0) {
      std::cerr << "cannot create " << out_dir << "\n";
      return 1;
    }
    trades_f.open(out_dir + "/trades.csv");
    reports_f.open(out_dir + "/reports.csv");
    if (!trades_f || !reports_f) {
      std::cerr << "cannot write into " << out_dir << "\n";
      return 1;
    }
    write_trades_header(trades_f);
    write_reports_header(reports_f);
  }

  // ---- run -------------------------------------------------------------------------------------
  std::unique_ptr<Exchange> single;
  std::unique_ptr<ShardedExchange> sharded;
  std::vector<const Exchange*> shards;
  double busy = 0.0;
  const auto wall0 = Clock::now();

  if (threads == 0) {
    single = std::make_unique<Exchange>(cfg);
    ExchangeOutput out;
    constexpr std::size_t kBatch = 4096;
    for (std::size_t i = 0; i < cmds.size(); i += kBatch) {
      const std::size_t end = std::min(cmds.size(), i + kBatch);
      out.clear();
      const auto t0 = Clock::now();
      for (std::size_t j = i; j < end; ++j) single->process(cmds[j], out);
      busy += secs_since(t0);
      if (trades_f.is_open()) {
        write_trades(trades_f, out.fills, syms);
        write_reports(reports_f, out.reports, syms);
      }
    }
    shards.push_back(single.get());
  } else {
    ShardedConfig sc;
    sc.shards = threads;
    sc.exchange = cfg;
    sc.collect_outputs = trades_f.is_open();
    sharded = std::make_unique<ShardedExchange>(sc);
    const auto t0 = Clock::now();
    sharded->start();
    for (const Command& c : cmds) sharded->submit(0, c);
    sharded->finish();
    busy = secs_since(t0);
    for (std::size_t s = 0; s < sharded->shards(); ++s) shards.push_back(&sharded->exchange(s));
    if (trades_f.is_open()) {
      // merge the per-shard streams into one deterministic, time-ordered file
      std::vector<Fill> fills;
      std::vector<ExecReport> reports;
      for (std::size_t s = 0; s < sharded->shards(); ++s) {
        fills.insert(fills.end(), sharded->output(s).fills.begin(), sharded->output(s).fills.end());
        reports.insert(reports.end(), sharded->output(s).reports.begin(), sharded->output(s).reports.end());
      }
      std::stable_sort(fills.begin(), fills.end(), [](const Fill& x, const Fill& y) {
        return std::tie(x.ts, x.symbol, x.trade_id) < std::tie(y.ts, y.symbol, y.trade_id);
      });
      std::stable_sort(reports.begin(), reports.end(), [](const ExecReport& x, const ExecReport& y) { return x.ts < y.ts; });
      write_trades(trades_f, fills, syms);
      write_reports(reports_f, reports, syms);
    }
  }
  const double wall = secs_since(wall0);
  if (!out_dir.empty()) {
    std::ofstream pnl(out_dir + "/pnl.csv");
    write_pnl_csv(pnl, shards, syms);
  }
  if (quiet) return 0;

  // ---- summary ---------------------------------------------------------------------------------
  ExchangeStats st;
  std::map<RejectReason, std::uint64_t> rejects;
  for (const Exchange* ex : shards) {
    const ExchangeStats& e = ex->stats();
    st.commands += e.commands;
    st.new_orders += e.new_orders;
    st.cancels += e.cancels;
    st.modifies += e.modifies;
    st.book_rejects += e.book_rejects;
    st.risk_rejects += e.risk_rejects;
    st.fills += e.fills;
    st.volume += e.volume;
    st.kills += e.kills;
    for (std::size_t r = 1; r < static_cast<std::size_t>(RejectReason::Count_); ++r) {
      if (const auto n = ex->reject_count(static_cast<RejectReason>(r))) rejects[static_cast<RejectReason>(r)] += n;
    }
  }
  std::cout << std::fixed << std::setprecision(3);
  if (threads == 0) {
    std::cout << "Replayed " << with_commas(cmds.size()) << " commands single-threaded: engine time " << busy << "s ("
              << with_commas(busy > 0 ? static_cast<std::uint64_t>(static_cast<double>(cmds.size()) / busy) : 0) << " cmds/s), wall " << wall
              << "s\n";
  } else {
    std::cout << "Replayed " << with_commas(cmds.size()) << " commands on " << threads << " shard thread(s) (symbol-sharded, SPSC queues): "
              << busy << "s (" << with_commas(busy > 0 ? static_cast<std::uint64_t>(static_cast<double>(cmds.size()) / busy) : 0)
              << " cmds/s)\n"
              << "  note: per-symbol risk limits are exact; cross-symbol limits (rate, gross exposure, daily loss) apply per shard\n";
  }
  std::cout << "  new " << with_commas(st.new_orders) << "  cancel " << with_commas(st.cancels) << "  modify " << with_commas(st.modifies)
            << "\n";
  std::cout << "  trades " << with_commas(st.fills) << "  volume " << with_commas(st.volume) << "\n";
  std::cout << "  rejects: book " << with_commas(st.book_rejects) << ", risk " << with_commas(st.risk_rejects);
  if (st.kills) std::cout << ", kill-switch trips " << st.kills;
  std::cout << "\n";
  for (const auto& [why, n] : rejects) {
    std::cout << "      " << std::left << std::setw(24) << to_string(why) << std::right << with_commas(n) << "\n";
  }

  std::cout << "\nTop of book:\n";
  for (SymbolId s = 0; s < syms.size(); ++s) {
    const OrderBook* bk = shards[s % shards.size()]->engine().book(s);
    if (!bk) continue;
    const auto t = bk->top();
    std::cout << "  " << std::left << std::setw(6) << syms.name(s) << std::right;
    if (t.has_bid) std::cout << " bid " << format_price(t.bid) << " x " << std::setw(5) << t.bid_qty; else std::cout << " bid    -";
    if (t.has_ask) std::cout << "  |  ask " << format_price(t.ask) << " x " << std::setw(5) << t.ask_qty; else std::cout << "  |  ask    -";
    std::cout << "   (" << with_commas(bk->order_count()) << " resting orders)\n";
  }

  struct ClientPnl {
    double realized = 0, unrealized = 0, fees = 0;
    std::size_t fills = 0;
  };
  std::map<ClientId, ClientPnl> pnl;
  for (const Exchange* ex : shards) {
    for (const auto& [c, pf] : ex->ledger()) {
      ClientPnl& p = pnl[c];
      p.realized += pf.realized();
      p.unrealized += pf.unrealized(ex->marks());
      p.fees += pf.fees();
      p.fills += pf.fill_count();
    }
  }
  std::cout << "\nP&L by client (marked at last trade, " << (cfg.cost_basis == CostBasis::Fifo ? "FIFO" : "average cost") << "):\n";
  std::cout << "  client      realized    unrealized          fees           net   fills\n";
  std::cout << std::fixed << std::setprecision(2);
  double tot = 0.0;
  for (const auto& [c, p] : pnl) {
    const double net = p.realized + p.unrealized - p.fees;
    std::cout << "  " << std::setw(6) << c << std::setw(14) << p.realized << std::setw(14) << p.unrealized << std::setw(14) << p.fees
              << std::setw(14) << net << std::setw(8) << p.fills << "\n";
    tot += net;
  }
  std::cout << "  total net " << tot << "  (equals minus total fees: trading is zero-sum before fees)\n";
  if (!out_dir.empty()) std::cout << "\nWrote trades.csv, reports.csv, pnl.csv to " << out_dir << "\n";
  return 0;
}

// ------------------------------------------------------------------------------------------------
// sim
// ------------------------------------------------------------------------------------------------

SimConfig sim_config_from(cli::Args& a) {
  SimConfig cfg;
  cfg.seed = static_cast<std::uint64_t>(a.get_int("seed", 42));
  cfg.duration = static_cast<Nanos>(a.get_double("seconds", 10.0) * 1e9);
  const std::string pol = a.get_str("policy", "smart");
  cfg.sor.policy = pol == "prorata" ? RoutingPolicy::ProRata : pol == "primary" ? RoutingPolicy::Primary : RoutingPolicy::BestPrice;
  cfg.sor.stale_decay_ns = a.get_double("stale-ms", 0.0) * 1e6;
  cfg.enable_market_maker = !a.has("no-mm");
  cfg.enable_parent_flow = !a.has("no-flow");
  cfg.enable_twap = !a.has("no-twap");
  cfg.flow.vol_ticks_per_sqrt_s = a.get_double("vol", cfg.flow.vol_ticks_per_sqrt_s);
  cfg.flow.mean_offset_ticks = a.get_double("offset-ticks", cfg.flow.mean_offset_ticks);
  if (a.has("noise-ticks")) {  // scale every venue's idiosyncratic quote offset
    cfg.venues = default_venues();
    const double k = a.get_double("noise-ticks", 1.0);
    for (VenueConfig& v : cfg.venues) v.quote_noise_ticks *= k;
  }
  cfg.firm_limits.max_daily_loss = a.get_double("max-loss", cfg.firm_limits.max_daily_loss);
  cfg.firm_limits.max_position = a.get_int("max-position", cfg.firm_limits.max_position);
  return cfg;
}

int cmd_sim(cli::Args& a) {
  const SimConfig base = sim_config_from(a);

  if (a.has("compare")) {
    // Same market, same client orders, only the router differs. The market maker is switched off so
    // the strategies' own fills cannot make the runs diverge for reasons other than routing.
    struct Variant {
      const char* label;
      RoutingPolicy policy;
      double stale_ms;
    };
    const Variant variants[] = {{"smart", RoutingPolicy::BestPrice, 0.0},
                                {"smart+haircut (experimental)", RoutingPolicy::BestPrice, 1.0},
                                {"prorata (price-blind)", RoutingPolicy::ProRata, 0.0},
                                {"primary venue only", RoutingPolicy::Primary, 0.0}};
    std::cout << "Router comparison: " << static_cast<double>(base.duration) / 1e9 << " s, seed " << base.seed
              << ", identical market and parent orders (market maker off)\n\n";
    std::cout << std::left << std::setw(31) << "policy" << std::right << std::setw(10) << "parents" << std::setw(10) << "fill rate"
              << std::setw(12) << "slippage" << std::setw(10) << "fees" << std::setw(10) << "all-in" << std::setw(14) << "cost" << std::setw(10)
              << "children" << std::setw(9) << "rounds" << std::setw(15) << "TWAP vs arr.\n";
    std::cout << std::left << std::setw(31) << "" << std::right << std::setw(10) << "" << std::setw(10) << "" << std::setw(12) << "(bps)"
              << std::setw(10) << "(bps)" << std::setw(10) << "(bps)" << std::setw(14) << "(currency)" << std::setw(10) << "/parent"
              << std::setw(9) << "/parent" << std::setw(15) << "(bps)\n";
    for (const Variant& v : variants) {
      SimConfig cfg = base;
      cfg.enable_market_maker = false;
      cfg.sor.policy = v.policy;
      cfg.sor.stale_decay_ns = v.stale_ms * 1e6;
      Simulator sim(cfg);
      const SimReport r = sim.run();
      const ExecutionStats& e = r.execution;
      std::cout << std::left << std::setw(31) << v.label << std::right << std::setw(10) << e.parents << std::fixed << std::setprecision(1)
                << std::setw(9) << e.fill_rate * 100.0 << "%" << std::setprecision(2) << std::setw(12) << e.slippage_bps << std::setw(10)
                << e.fee_bps << std::setw(10) << e.total_cost_bps << std::setw(14) << e.cost_currency << std::setw(10) << e.avg_children
                << std::setw(9) << e.avg_rounds << std::setw(15) << (r.has_twap ? r.twap.vs_arrival_bps : 0.0) << "\n";
    }
    std::cout << "\n(slippage = fill price vs consolidated mid at order arrival, qty-weighted; lower is better)\n";
    return 0;
  }

  Simulator sim(base);
  const SimReport r = sim.run();
  print_report(std::cout, r);
  const std::string eq = a.get_str("equity-csv", "");
  if (!eq.empty()) {
    std::ofstream f(eq);
    write_equity_csv(f, r);
    std::cout << "equity curve written to " << eq << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  cli::Args args(argc - 2, argv + 2);
  if (cmd == "gen") return cmd_gen(args);
  if (cmd == "replay") return cmd_replay(args);
  if (cmd == "sim") return cmd_sim(args);
  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    usage();
    return 0;
  }
  std::cerr << "unknown command '" << cmd << "'\n\n";
  usage();
  return 2;
}
