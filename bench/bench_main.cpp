// qc_bench: performance measurements for the hot path.
//
//   qc_bench book     [--ops N]      order book: throughput and per-operation latency percentiles
//   qc_bench risk     [--ops N]      pre-trade risk check latency
//   qc_bench exchange [--ops N]      whole stack (risk + match + ledger), one thread
//   qc_bench pipeline [--ops N]      sharded SPSC pipeline vs one mutex-protected exchange
//   qc_bench sor                     smart-order-router decision latency
//   qc_bench jitter                  how much the OS/VM steals the CPU from a spinning thread (read this
//                                    before trusting any tail latency printed by the other benches)
//   qc_bench all
//
// Latency numbers subtract the measured cost of the timer itself. Build with -DCMAKE_BUILD_TYPE=Release.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/cli_args.hpp"
#include "qc/exchange.hpp"
#include "qc/latency.hpp"
#include "qc/order_book.hpp"
#include "qc/risk.hpp"
#include "qc/router.hpp"
#include "qc/rng.hpp"
#include "qc/sharded_exchange.hpp"
#include "qc/workload.hpp"

using namespace qc;

namespace {

template <class T>
inline void keep(const T& v) {
  asm volatile("" : : "g"(&v) : "memory");
}

// Median cost of reading the clock twice back to back: what a timed region pays for the timer.
std::uint64_t timer_overhead_ns() {
  LatencyHistogram h;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t a = now_ns();
    const std::uint64_t b = now_ns();
    h.record(b - a);
  }
  return h.percentile(50);
}

std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

void print_latency(const char* name, const LatencyHistogram& h, std::uint64_t overhead) {
  auto adj = [&](std::uint64_t v) { return v > overhead ? v - overhead : 0; };
  std::cout << "  " << std::left << std::setw(26) << name << std::right << std::setw(10) << commas(h.count())
            << " ops   p50 " << std::setw(6) << adj(h.percentile(50)) << "  p90 " << std::setw(6) << adj(h.percentile(90))
            << "  p99 " << std::setw(7) << adj(h.percentile(99)) << "  p99.9 " << std::setw(8) << adj(h.percentile(99.9))
            << "  max " << std::setw(9) << adj(h.max()) << "  ns\n";
}

double elapsed(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------
// order book
// ---------------------------------------------------------------------------------------------

enum class Kind { Rest, Cross, Cancel, Modify, Reject };

Kind apply(OrderBook& book, const Command& c, BookEvents& ev) {
  const Order& o = c.order;
  switch (c.type) {
    case CommandType::New: {
      const SubmitResult r = book.submit(o, ev);
      if (r.status == OrderStatus::Rejected) return Kind::Reject;
      return r.filled > 0 ? Kind::Cross : Kind::Rest;
    }
    case CommandType::Cancel:
      return book.cancel(o.id, ev) ? Kind::Cancel : Kind::Reject;
    case CommandType::Modify:
      return book.modify(o.id, o.price, o.qty, o.ts, ev).status == OrderStatus::Rejected ? Kind::Reject : Kind::Modify;
  }
  return Kind::Reject;
}

void bench_book(std::size_t ops) {
  std::cout << "== order book (single symbol, " << commas(ops) << " mixed ops: ~70% new, 20% cancel, 5% amend)\n";
  WorkloadConfig wc;
  wc.count = ops;
  wc.symbols = 1;
  wc.clients = 8;
  wc.invalid_frac = 0;
  const auto cmds = generate_workload(wc);

  {  // throughput: no per-op timers
    OrderBook book(0, BookConfig{StpMode::None, 1 << 16});
    BookEvents ev;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Command& c : cmds) {
      ev.clear();
      keep(apply(book, c, ev));
    }
    const double s = elapsed(t0);
    std::cout << "  throughput: " << commas(static_cast<std::uint64_t>(static_cast<double>(ops) / s)) << " ops/s  ("
              << std::fixed << std::setprecision(1) << s * 1e9 / static_cast<double>(ops) << " ns/op average, "
              << commas(book.order_count()) << " resting at end)\n";
  }

  const std::uint64_t overhead = timer_overhead_ns();
  OrderBook book(0, BookConfig{StpMode::None, 1 << 16});
  BookEvents ev;
  LatencyHistogram rest, cross, cancel, modify, all;
  for (const Command& c : cmds) {
    ev.clear();
    const std::uint64_t t0 = now_ns();
    const Kind k = apply(book, c, ev);
    const std::uint64_t d = now_ns() - t0;
    all.record(d);
    switch (k) {
      case Kind::Rest: rest.record(d); break;
      case Kind::Cross: cross.record(d); break;
      case Kind::Cancel: cancel.record(d); break;
      case Kind::Modify: modify.record(d); break;
      case Kind::Reject: break;
    }
  }
  std::cout << "  latency (timer overhead " << overhead << " ns subtracted):\n";
  print_latency("add (rests)", rest, overhead);
  print_latency("add (crosses, >=1 fill)", cross, overhead);
  print_latency("cancel", cancel, overhead);
  print_latency("amend", modify, overhead);
  print_latency("all operations", all, overhead);
  std::cout << "\n";
}

// ---------------------------------------------------------------------------------------------
// risk
// ---------------------------------------------------------------------------------------------

void bench_risk(std::size_t ops) {
  std::cout << "== pre-trade risk (all 9 checks enabled)\n";
  RiskLimits l;
  l.max_order_qty = 10'000;
  l.max_order_notional = 1e7;
  l.price_collar_bps = 500;
  l.max_position = 1'000'000;
  l.max_gross_notional = 1e9;
  l.max_open_orders = 100'000;
  l.max_orders_per_sec = 1e9;
  l.burst_orders = 1e9;
  RiskEngine risk(l);
  risk.set_reference_price(0, 10000);
  const std::uint64_t overhead = timer_overhead_ns();
  LatencyHistogram h;
  Order o;
  o.symbol = 0;
  o.qty = 100;
  std::uint64_t rejects = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < ops; ++i) {
    o.id = i + 1;
    o.client = 1 + static_cast<ClientId>(i % 4);
    o.side = ((i / 4) & 1) ? Side::Buy : Side::Sell;  // each client alternates, so positions stay near flat
    o.price = 9990 + static_cast<Price>(i % 21);
    o.ts = static_cast<Nanos>(i) * 100;
    const std::uint64_t a = now_ns();
    const RejectReason r = risk.check(o, o.ts);
    const std::uint64_t d = now_ns() - a;
    h.record(d);
    if (r != RejectReason::None) {
      ++rejects;
      continue;
    }
    risk.track(o);
    ExecReport fill;
    fill.order_id = o.id;
    fill.client = o.client;
    fill.symbol = 0;
    fill.side = o.side;
    fill.type = ExecType::Fill;
    fill.last_qty = o.qty;
    fill.last_px = o.price;
    fill.leaves = 0;
    risk.on_report(fill);
  }
  const double s = elapsed(t0);
  print_latency("check()", h, overhead);
  std::cout << "  check+track+fill-report cycle: " << std::fixed << std::setprecision(1) << s * 1e9 / static_cast<double>(ops)
            << " ns/order (" << commas(rejects) << " of " << commas(ops) << " rejected";
  if (rejects * 100 > ops) std::cout << "  <-- WARNING: benchmark is mostly measuring early-exit rejects";
  std::cout << ")\n\n";
}

// ---------------------------------------------------------------------------------------------
// whole exchange, single thread
// ---------------------------------------------------------------------------------------------

void bench_exchange(std::size_t ops) {
  std::cout << "== exchange: risk + matching + ledger, single thread, 4 symbols\n";
  WorkloadConfig wc;
  wc.count = ops;
  wc.symbols = 4;
  const auto cmds = generate_workload(wc);
  Exchange ex;
  ExchangeOutput out;
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    ex.process(cmds[i], out);
    if ((i & 4095) == 4095) out.clear();
  }
  const double s = elapsed(t0);
  std::cout << "  " << commas(static_cast<std::uint64_t>(static_cast<double>(ops) / s)) << " commands/s  (" << std::fixed
            << std::setprecision(1) << s * 1e9 / static_cast<double>(ops) << " ns/command, " << commas(ex.stats().fills)
            << " trades)\n\n";
}

// ---------------------------------------------------------------------------------------------
// concurrency: sharded SPSC pipeline vs one big lock
// ---------------------------------------------------------------------------------------------

void bench_pipeline(std::size_t ops) {
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  std::cout << "== concurrency (" << commas(ops) << " commands, 16 symbols, " << hw << " hardware threads)\n";
  WorkloadConfig wc;
  wc.count = ops;
  wc.symbols = 16;
  wc.clients = 32;
  const auto cmds = generate_workload(wc);

  // baseline: all threads share one Exchange behind a mutex (each thread owns a subset of symbols,
  // so the logical result is the same; only the synchronisation differs)
  for (unsigned threads : {1u, 2u, 4u}) {
    Exchange ex;
    std::mutex mu;
    std::vector<std::vector<const Command*>> slices(threads);
    for (const auto& c : cmds) slices[c.order.symbol % threads].push_back(&c);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) {
      ts.emplace_back([&, t] {
        ExchangeOutput out;
        std::size_t n = 0;
        for (const Command* c : slices[t]) {
          std::lock_guard<std::mutex> g(mu);
          ex.process(*c, out);
          if ((++n & 1023) == 0) out.clear();
        }
      });
    }
    for (auto& t : ts) t.join();
    const double s = elapsed(t0);
    std::cout << "  mutex-guarded exchange, " << threads << " thread(s):        " << std::setw(12)
              << commas(static_cast<std::uint64_t>(static_cast<double>(ops) / s)) << " commands/s\n";
  }

  // sharded: 1 producer thread feeding S shard workers through SPSC queues
  for (std::size_t shards : {1u, 2u, 3u}) {
    ShardedConfig sc;
    sc.shards = shards;
    sc.queue_capacity = 1 << 14;
    sc.latency_sample_every = 64;
    ShardedExchange sx(sc);
    sx.start();
    const auto t0 = std::chrono::steady_clock::now();
    for (const Command& c : cmds) sx.submit(0, c);
    sx.finish();
    const double s = elapsed(t0);
    const auto q = sx.queue_latency();
    const auto sv = sx.service_latency();
    std::cout << "  sharded SPSC, 1 producer + " << shards << " shard(s):    " << std::setw(12)
              << commas(static_cast<std::uint64_t>(static_cast<double>(ops) / s)) << " commands/s   queue wait p50/p99 "
              << commas(q.percentile(50)) << "/" << commas(q.percentile(99)) << " ns, service p50/p99 "
              << sv.percentile(50) << "/" << sv.percentile(99) << " ns\n";
  }
  std::cout << "  (the producer is a thread too: with " << hw << " cores, 1 producer + " << hw - 1
            << " shards is the most that fits without oversubscription)\n";
  std::cout << "  NB: at saturation the SPSC queue stays full, so 'queue wait' above is just queue depth x service\n"
               "      time (back-pressure), not a latency. Latency under a sustainable, paced load:\n";

  const std::size_t shards = std::max<std::size_t>(1, std::min<std::size_t>(2, hw > 2 ? 2 : 1));
  for (double rate : {250'000.0, 1'000'000.0, 2'000'000.0}) {
    ShardedConfig sc;
    sc.shards = shards;
    sc.queue_capacity = 1 << 14;
    sc.latency_sample_every = 8;
    ShardedExchange sx(sc);
    sx.start();
    const std::size_t n = std::min<std::size_t>(cmds.size(), static_cast<std::size_t>(rate * 1.5));  // ~1.5 s of load
    const std::uint64_t start = now_ns();
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t due = start + static_cast<std::uint64_t>(static_cast<double>(i) * 1e9 / rate);
      while (now_ns() < due) {
      }
      sx.submit(0, cmds[i]);
    }
    const double achieved = static_cast<double>(n) / (static_cast<double>(now_ns() - start) * 1e-9);
    sx.finish();
    const auto q = sx.queue_latency();
    const auto sv = sx.service_latency();
    std::cout << "  " << shards << " shards @ " << std::setw(9) << commas(static_cast<std::uint64_t>(rate)) << "/s offered ("
              << std::setw(9) << commas(static_cast<std::uint64_t>(achieved)) << " achieved):  enqueue->start p50 "
              << std::setw(7) << commas(q.percentile(50)) << "  p99 " << std::setw(9) << commas(q.percentile(99))
              << "  p99.9 " << std::setw(10) << commas(q.percentile(99.9)) << " ns;  processing p50 " << sv.percentile(50)
              << "  p99 " << sv.percentile(99) << " ns\n";
  }
  std::cout << "\n";
}

// ---------------------------------------------------------------------------------------------
// smart order router
// ---------------------------------------------------------------------------------------------

void bench_sor() {
  std::cout << "== smart order router (3 venues x 5 levels, sweep ~1,000 shares)\n";
  std::vector<VenueInfo> venues = {{0, 0.30, -0.20, 50'000}, {1, 0.10, 0.00, 250'000}, {2, 0.25, -0.10, 120'000}};
  SmartOrderRouter sor(venues, SorConfig{});
  ConsolidatedBook cb;
  Rng rng(3);
  for (VenueId v = 0; v < 3; ++v) {
    DepthSnapshot s;
    s.venue = v;
    s.symbol = 0;
    s.venue_ts = 0;
    s.n_bids = s.n_asks = 5;
    for (int i = 0; i < 5; ++i) {
      s.asks[static_cast<std::size_t>(i)] = {10010 + i + static_cast<Price>(v), 100 + rng.range(0, 200)};
      s.bids[static_cast<std::size_t>(i)] = {10000 - i - static_cast<Price>(v), 100 + rng.range(0, 200)};
    }
    cb.update(s, 0);
  }
  const std::uint64_t overhead = timer_overhead_ns();
  LatencyHistogram h;
  RouteRequest rq;
  rq.symbol = 0;
  rq.side = Side::Buy;
  rq.qty = 1000;
  rq.limit = 10100;
  rq.now = 1000;
  std::uint64_t sink = 0;
  for (int i = 0; i < 500000; ++i) {
    rq.side = (i & 1) ? Side::Buy : Side::Sell;
    rq.limit = rq.side == Side::Buy ? 10100 : 9900;
    const std::uint64_t a = now_ns();
    const RoutePlan p = sor.route(rq, cb);
    h.record(now_ns() - a);
    sink += static_cast<std::uint64_t>(p.planned);
  }
  keep(sink);
  print_latency("route()", h, overhead);
  std::cout << "\n";
}

// ---------------------------------------------------------------------------------------------
// scheduler jitter probe
// ---------------------------------------------------------------------------------------------

void bench_jitter() {
  std::cout << "== scheduler jitter: threads that do nothing but read the clock for 1 s each\n";
  for (unsigned threads : {1u, 3u}) {
    std::vector<LatencyHistogram> hs(threads);
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < threads; ++t) {
      ts.emplace_back([&hs, t] {
        const std::uint64_t end = now_ns() + 1'000'000'000ull;
        std::uint64_t last = now_ns();
        while (last < end) {
          const std::uint64_t n = now_ns();
          hs[t].record(n - last);
          last = n;
        }
      });
    }
    for (auto& t : ts) t.join();
    LatencyHistogram all;
    for (const auto& h : hs) all.merge(h);
    std::cout << "  " << threads << " thread(s): gap between clock reads  p50 " << all.percentile(50) << " ns   p99.99 "
              << commas(all.percentile(99.99)) << " ns   max " << commas(all.max())
              << " ns  (anything beyond ~100 ns is the OS taking the core away)\n";
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string what = argc > 1 ? argv[1] : "all";
  cli::Args a(argc > 2 ? argc - 2 : 0, argv + 2);
  const auto ops = static_cast<std::size_t>(a.get_int("ops", 2'000'000));
  const bool all = what == "all";
  if (all || what == "book") bench_book(ops);
  if (all || what == "risk") bench_risk(ops);
  if (all || what == "exchange") bench_exchange(ops);
  if (all || what == "pipeline") bench_pipeline(ops);
  if (all || what == "sor") bench_sor();
  if (all || what == "jitter") bench_jitter();
  if (!all && what != "book" && what != "risk" && what != "exchange" && what != "pipeline" && what != "sor" &&
      what != "jitter") {
    std::cerr << "usage: qc_bench [book|risk|exchange|pipeline|sor|jitter|all] [--ops N]\n";
    return 2;
  }
  return 0;
}
