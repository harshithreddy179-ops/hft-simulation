#pragma once
// Multi-threaded exchange: symbols are partitioned across shards, each shard is a complete
// single-threaded Exchange (risk + matching + ledger) pinned to its own worker thread.
//
//   producer 0 ──┐        ┌─► SPSC[0][0] ─┐
//   producer 1 ──┼─ hash(symbol) ─► SPSC[p][s] ─► worker s ─► Exchange s
//   producer P ──┘        └─► SPSC[P][S] ─┘
//
// Why this shape
//   * Single-writer principle: an order book is only ever touched by one thread, so there are no
//     locks and no atomics on the matching path. Scaling comes from partitioning, not locking.
//   * One SPSC queue per (producer, shard) pair keeps every queue single-producer/single-consumer,
//     which is what makes them wait-free; a shard polls its P inbound queues round-robin.
//   * Per-symbol ordering is preserved for a given producer (FIFO queue), and with one producer the
//     result per symbol is *identical* to the single-threaded Exchange (tested).
//
// Risk semantics under sharding: every limit that is per (client, symbol) -- order size/notional,
// price collar, position, open orders -- is exact, because that state lives in the shard that owns
// the symbol. Limits that span symbols (rate limit, gross exposure, daily loss) are enforced per
// shard, i.e. they are approximations of the global limit; a global limit would need a shared
// counter or a cross-shard feedback loop and costs latency. See docs/ARCHITECTURE.md.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "qc/exchange.hpp"
#include "qc/latency.hpp"
#include "qc/spsc_queue.hpp"

namespace qc {

struct ShardedConfig {
  std::size_t shards = 4;
  std::size_t producers = 1;
  std::size_t queue_capacity = 1 << 14;  // per (producer, shard) queue
  ExchangeConfig exchange;
  bool collect_outputs = false;          // keep every report/fill per shard (tests, replay)
  std::uint32_t latency_sample_every = 0;  // 0 = off, N = time every Nth message
  bool pin_threads = false;              // best effort, Linux only
};

class ShardedExchange {
 public:
  explicit ShardedExchange(ShardedConfig cfg);
  ~ShardedExchange();
  ShardedExchange(const ShardedExchange&) = delete;
  ShardedExchange& operator=(const ShardedExchange&) = delete;

  // Spawn the worker threads.
  void start();

  // Route a command to its shard. Each `producer` index must be driven by exactly one thread.
  // Spins (then yields) while the destination queue is full (back-pressure).
  void submit(std::size_t producer, const Command& cmd);

  // Call after every producer is done submitting. Drains all queues, stops and joins the workers.
  void finish();

  std::size_t shard_of(SymbolId s) const { return static_cast<std::size_t>(s) % cfg_.shards; }
  std::size_t shards() const { return cfg_.shards; }

  // --- results (valid after finish()) ---------------------------------------------------------
  const Exchange& exchange(std::size_t shard) const { return shards_[shard]->ex; }
  const ExchangeOutput& output(std::size_t shard) const { return shards_[shard]->out; }
  std::uint64_t processed(std::size_t shard) const { return shards_[shard]->processed; }
  std::uint64_t total_processed() const;
  ExchangeStats merged_stats() const;
  // enqueue -> start of processing, and processing time, in ns (sampled)
  LatencyHistogram queue_latency() const;
  LatencyHistogram service_latency() const;

 private:
  struct Msg {
    Command cmd;
    std::uint64_t enq_ns = 0;
  };
  struct Shard {
    explicit Shard(const ExchangeConfig& c) : ex(c) {}
    Exchange ex;
    ExchangeOutput out;
    std::uint64_t processed = 0;
    LatencyHistogram queue_lat, service_lat;
  };

  void worker(std::size_t shard);

  ShardedConfig cfg_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::vector<std::vector<std::unique_ptr<SpscQueue<Msg>>>> queues_;  // [producer][shard]
  std::vector<std::thread> threads_;
  std::atomic<bool> done_{false};
  bool started_ = false;
  std::vector<std::uint32_t> sample_counter_;  // per producer: counts submissions for sampling
};

}  // namespace qc
