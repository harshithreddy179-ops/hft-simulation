#include "qc/sharded_exchange.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace qc {

namespace {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

void pin_to_cpu([[maybe_unused]] std::size_t cpu) {
#if defined(__linux__)
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpu % hw), &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);  // best effort; failure is harmless
#endif
}

}  // namespace

ShardedExchange::ShardedExchange(ShardedConfig cfg) : cfg_(cfg) {
  if (cfg_.shards == 0) cfg_.shards = 1;
  if (cfg_.producers == 0) cfg_.producers = 1;
  for (std::size_t s = 0; s < cfg_.shards; ++s) shards_.push_back(std::make_unique<Shard>(cfg_.exchange));
  queues_.resize(cfg_.producers);
  for (auto& row : queues_) {
    for (std::size_t s = 0; s < cfg_.shards; ++s) row.push_back(std::make_unique<SpscQueue<Msg>>(cfg_.queue_capacity));
  }
  sample_counter_.assign(cfg_.producers, 0);
}

ShardedExchange::~ShardedExchange() {
  if (started_) finish();
}

void ShardedExchange::start() {
  if (started_) return;
  started_ = true;
  done_.store(false, std::memory_order_relaxed);
  for (std::size_t s = 0; s < cfg_.shards; ++s) threads_.emplace_back([this, s] { worker(s); });
}

void ShardedExchange::submit(std::size_t producer, const Command& cmd) {
  SpscQueue<Msg>& q = *queues_[producer][shard_of(cmd.order.symbol)];
  Msg m;
  m.cmd = cmd;
  if (cfg_.latency_sample_every != 0 && ++sample_counter_[producer] % cfg_.latency_sample_every == 0) {
    m.enq_ns = now_ns();
  }
  unsigned spins = 0;
  while (!q.try_push(m)) {
    if (++spins < 128) cpu_relax(); else std::this_thread::yield();  // queue full: back-pressure
  }
}

void ShardedExchange::finish() {
  if (!started_) return;
  done_.store(true, std::memory_order_release);
  for (auto& t : threads_) t.join();
  threads_.clear();
  started_ = false;
}

void ShardedExchange::worker(std::size_t sh) {
  if (cfg_.pin_threads) pin_to_cpu(sh);
  Shard& shard = *shards_[sh];
  const bool collect = cfg_.collect_outputs;
  ExchangeOutput scratch;
  Msg m;

  auto handle = [&](Msg& msg) {
    if (msg.enq_ns != 0) {
      const std::uint64_t t0 = now_ns();
      shard.queue_lat.record(t0 - msg.enq_ns);
      shard.ex.process(msg.cmd, collect ? shard.out : scratch);
      shard.service_lat.record(now_ns() - t0);
    } else {
      shard.ex.process(msg.cmd, collect ? shard.out : scratch);
    }
    if (!collect) scratch.clear();
    ++shard.processed;
  };

  unsigned idle = 0;
  for (;;) {
    bool any = false;
    for (std::size_t p = 0; p < cfg_.producers; ++p) {
      SpscQueue<Msg>& q = *queues_[p][sh];
      for (int burst = 0; burst < 64 && q.try_pop(m); ++burst) {
        handle(m);
        any = true;
      }
    }
    if (any) {
      idle = 0;
      continue;
    }
    if (done_.load(std::memory_order_acquire)) {
      // Producers are finished (happens-before via done_): one last full drain, then exit.
      bool again = false;
      for (std::size_t p = 0; p < cfg_.producers; ++p) {
        SpscQueue<Msg>& q = *queues_[p][sh];
        while (q.try_pop(m)) {
          handle(m);
          again = true;
        }
      }
      if (!again) break;
      continue;
    }
    if (++idle < 256) cpu_relax(); else std::this_thread::yield();
  }
}

std::uint64_t ShardedExchange::total_processed() const {
  std::uint64_t n = 0;
  for (const auto& s : shards_) n += s->processed;
  return n;
}

ExchangeStats ShardedExchange::merged_stats() const {
  ExchangeStats m;
  for (const auto& s : shards_) {
    const ExchangeStats& e = s->ex.stats();
    m.commands += e.commands;
    m.new_orders += e.new_orders;
    m.cancels += e.cancels;
    m.modifies += e.modifies;
    m.book_rejects += e.book_rejects;
    m.risk_rejects += e.risk_rejects;
    m.fills += e.fills;
    m.volume += e.volume;
    m.kills += e.kills;
  }
  return m;
}

LatencyHistogram ShardedExchange::queue_latency() const {
  LatencyHistogram h;
  for (const auto& s : shards_) h.merge(s->queue_lat);
  return h;
}

LatencyHistogram ShardedExchange::service_latency() const {
  LatencyHistogram h;
  for (const auto& s : shards_) h.merge(s->service_lat);
  return h;
}

}  // namespace qc
