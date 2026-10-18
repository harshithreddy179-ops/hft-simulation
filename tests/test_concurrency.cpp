#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <tuple>

#include "qc/latency.hpp"
#include "qc/rng.hpp"
#include "qc/sharded_exchange.hpp"
#include "qc/spsc_queue.hpp"
#include "qc/workload.hpp"
#include "testing.hpp"

using namespace qc;

// ------------------------------------------------------------------------------------------------
// SPSC queue
// ------------------------------------------------------------------------------------------------

TEST(spsc_single_thread_semantics) {
  SpscQueue<int> q(5);  // rounds up to 8
  CHECK_EQ(q.capacity(), 8u);
  int x = 0;
  CHECK(!q.try_pop(x));
  for (int i = 0; i < 8; ++i) CHECK(q.try_push(i));
  CHECK(!q.try_push(99));  // full
  CHECK_EQ(q.size_approx(), 8u);
  for (int i = 0; i < 8; ++i) {
    CHECK(q.try_pop(x));
    CHECK_EQ(x, i);
  }
  CHECK(!q.try_pop(x));
  // wrap-around many times with a partially filled ring
  for (int round = 0; round < 1000; ++round) {
    CHECK(q.try_push(round));
    CHECK(q.try_push(round + 1));
    CHECK(q.try_pop(x));
    CHECK_EQ(x, round);
    CHECK(q.try_pop(x));
    CHECK_EQ(x, round + 1);
  }
}

TEST(spsc_two_threads_preserve_order_and_lose_nothing) {
  constexpr std::uint64_t N = 2'000'000;
  SpscQueue<std::uint64_t> q(1024);  // small ring => constant full/empty transitions
  std::atomic<bool> ok{true};
  std::thread consumer([&] {
    std::uint64_t expect = 0, v = 0;
    while (expect < N) {
      if (q.try_pop(v)) {
        if (v != expect) {
          ok.store(false);
          return;
        }
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < N; ++i) {
    while (!q.try_push(i)) {
    }
  }
  consumer.join();
  CHECK(ok.load());
  CHECK_EQ(q.size_approx(), 0u);
}

// ------------------------------------------------------------------------------------------------
// latency histogram
// ------------------------------------------------------------------------------------------------

TEST(histogram_index_is_monotone_and_bounds_value) {
  unsigned last = 0;
  Rng rng(5);
  std::vector<std::uint64_t> vals;
  for (int i = 0; i < 20000; ++i) vals.push_back(static_cast<std::uint64_t>(rng.next() >> rng.range(0, 63)));
  vals.push_back(0);
  vals.push_back(~std::uint64_t{0});
  std::sort(vals.begin(), vals.end());
  for (std::uint64_t v : vals) {
    const unsigned idx = LatencyHistogram::index(v);
    CHECK(idx >= last);
    CHECK(idx < LatencyHistogram::kBuckets);
    CHECK(LatencyHistogram::upper_bound(idx) >= v);
    // relative error bound: bucket width <= value / 16
    if (v >= 16) CHECK(static_cast<double>(LatencyHistogram::upper_bound(idx) - v) <= static_cast<double>(v) / 15.0 + 1);
    last = idx;
  }
}

TEST(histogram_percentiles_match_exact_within_bucket_error) {
  Rng rng(11);
  LatencyHistogram h;
  std::vector<std::uint64_t> v;
  for (int i = 0; i < 100000; ++i) {
    const auto x = static_cast<std::uint64_t>(50.0 + rng.exponential(400.0));
    v.push_back(x);
    h.record(x);
  }
  std::sort(v.begin(), v.end());
  CHECK_EQ(h.count(), v.size());
  CHECK_EQ(h.min(), v.front());
  CHECK_EQ(h.max(), v.back());
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const auto rank = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size()));
    const double exact = static_cast<double>(v[std::min(rank, v.size() - 1)]);
    const double got = static_cast<double>(h.percentile(p));
    CHECK_MSG(got >= exact * 0.99 && got <= exact * 1.07 + 1, "p" << p << " exact " << exact << " got " << got);
  }
  CHECK_EQ(h.percentile(100.0), v.back());

  LatencyHistogram a, b;
  for (int i = 0; i < 1000; ++i) (i % 2 ? a : b).record(static_cast<std::uint64_t>(i));
  a.merge(b);
  CHECK_EQ(a.count(), 1000u);
  CHECK_EQ(a.max(), 999u);
}

// ------------------------------------------------------------------------------------------------
// sharded exchange
// ------------------------------------------------------------------------------------------------

namespace {

using FillKey = std::tuple<OrderId, OrderId, Price, Qty>;  // maker, taker, price, qty

std::map<SymbolId, std::vector<FillKey>> fills_by_symbol(const std::vector<const ExchangeOutput*>& outs) {
  std::map<SymbolId, std::vector<FillKey>> m;
  for (const ExchangeOutput* o : outs) {
    for (const Fill& f : o->fills) m[f.symbol].emplace_back(f.maker_order, f.taker_order, f.price, f.qty);
  }
  return m;
}

}  // namespace

TEST(sharded_matches_single_threaded_per_symbol) {
  WorkloadConfig wc;
  wc.seed = 31;
  wc.count = 60000;
  wc.symbols = 6;
  wc.clients = 8;
  const auto cmds = generate_workload(wc);

  // reference
  ExchangeConfig xc;
  xc.stp = StpMode::CancelResting;
  Exchange ref(xc);
  ExchangeOutput ref_out;
  for (const auto& c : cmds) ref.process(c, ref_out);

  ShardedConfig sc;
  sc.shards = 4;
  sc.exchange = xc;
  sc.collect_outputs = true;
  sc.queue_capacity = 256;  // tiny queues: exercise back-pressure
  ShardedExchange sx(sc);
  sx.start();
  for (const auto& c : cmds) sx.submit(0, c);
  sx.finish();

  CHECK_EQ(sx.total_processed(), cmds.size());

  std::vector<const ExchangeOutput*> outs;
  for (std::size_t s = 0; s < sx.shards(); ++s) outs.push_back(&sx.output(s));
  const auto got = fills_by_symbol(outs);
  const auto want = fills_by_symbol({&ref_out});
  CHECK(got == want);
  CHECK(!want.empty());

  const ExchangeStats ms = sx.merged_stats();
  CHECK_EQ(ms.fills, ref.stats().fills);
  CHECK_EQ(ms.volume, ref.stats().volume);
  CHECK_EQ(ms.new_orders, ref.stats().new_orders);
  CHECK_EQ(ms.book_rejects, ref.stats().book_rejects);

  // per-client P&L adds up across shards to the single-threaded ledger
  for (const auto& [client, pf] : ref.ledger()) {
    double realized = 0.0, fees = 0.0;
    for (std::size_t s = 0; s < sx.shards(); ++s) {
      if (const Portfolio* p = sx.exchange(s).portfolio(client)) {
        realized += p->realized();
        fees += p->fees();
      }
    }
    CHECK_NEAR(realized, pf.realized(), 1e-6);
    CHECK_NEAR(fees, pf.fees(), 1e-6);
  }
}

TEST(sharded_multi_producer_stress_keeps_global_invariants) {
  constexpr std::size_t P = 3;
  std::vector<std::vector<Command>> streams(P);
  for (std::size_t p = 0; p < P; ++p) {
    WorkloadConfig wc;
    wc.seed = 100 + p;
    wc.count = 40000;
    wc.symbols = 5;
    wc.clients = 4;
    wc.aggressive_frac = 0.3;
    streams[p] = generate_workload(wc);
    for (Command& c : streams[p]) {  // disjoint ids and clients per producer
      c.order.id += static_cast<OrderId>(p) * 1'000'000'000ull;
      c.order.client += static_cast<ClientId>(p) * 100;
    }
  }

  ShardedConfig sc;
  sc.shards = 3;
  sc.producers = P;
  sc.queue_capacity = 512;
  sc.exchange.stp = StpMode::CancelResting;
  ShardedExchange sx(sc);
  sx.start();
  std::vector<std::thread> producers;
  for (std::size_t p = 0; p < P; ++p) {
    producers.emplace_back([&, p] {
      for (const Command& c : streams[p]) sx.submit(p, c);
    });
  }
  for (auto& t : producers) t.join();
  sx.finish();

  CHECK_EQ(sx.total_processed(), P * 40000);

  for (SymbolId s = 0; s < 5; ++s) {
    const Exchange& ex = sx.exchange(sx.shard_of(s));
    Qty net = 0, open_buy = 0, resting_buy = 0;
    for (std::size_t p = 0; p < P; ++p) {
      for (ClientId c = 1; c <= 4; ++c) {
        const ClientId cid = c + static_cast<ClientId>(p) * 100;
        net += ex.risk().position(cid, s);
        open_buy += ex.risk().open_buy(cid, s);
      }
    }
    CHECK_EQ(net, 0);  // trades are zero-sum in position
    if (const OrderBook* bk = ex.engine().book(s)) {
      std::string why;
      CHECK_MSG(bk->validate(&why), why);
      std::vector<OrderBook::DepthLevel> d;
      bk->depth(Side::Buy, 1u << 20, d);
      for (auto& l : d) resting_buy += l.qty;
    }
    CHECK_EQ(open_buy, resting_buy);
  }
  CHECK(sx.merged_stats().fills > 1000);
}

TEST(sharded_records_latency_when_sampling) {
  WorkloadConfig wc;
  wc.count = 20000;
  const auto cmds = generate_workload(wc);
  ShardedConfig sc;
  sc.shards = 2;
  sc.latency_sample_every = 4;
  ShardedExchange sx(sc);
  sx.start();
  for (const auto& c : cmds) sx.submit(0, c);
  sx.finish();
  const auto q = sx.queue_latency();
  const auto s = sx.service_latency();
  CHECK_EQ(q.count(), 5000u);
  CHECK_EQ(s.count(), 5000u);
  CHECK(s.percentile(50) > 0);
}

TEST(sharded_restart_and_empty_run_are_safe) {
  ShardedConfig sc;
  sc.shards = 2;
  ShardedExchange sx(sc);
  sx.finish();  // finish before start is a no-op
  sx.start();
  sx.finish();  // nothing submitted
  CHECK_EQ(sx.total_processed(), 0u);
}
