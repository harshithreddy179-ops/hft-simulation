#pragma once
// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Memory ordering: the producer publishes a slot with a release-store of `tail_`, the consumer
// acquires it; symmetrically for `head_` when a slot is recycled. Each side keeps a *cached copy*
// of the other side's index and only re-reads the shared atomic when the cache says full/empty,
// so in steady state a push or pop touches one cache line it already owns (no ping-pong).
// The producer-owned and consumer-owned fields live on separate cache lines to avoid false sharing.
//
// Exactly one thread may call try_push and exactly one thread may call try_pop.

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace qc {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscQueue {
 public:
  // `capacity` is rounded up to a power of two (so indexing is a mask, not a modulo).
  explicit SpscQueue(std::size_t capacity)
      : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)), mask_(capacity_ - 1), buf_(capacity_) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const { return capacity_; }

  bool try_push(const T& v) {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_cache_ >= capacity_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ >= capacity_) return false;  // full
    }
    buf_[t & mask_] = v;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return false;  // empty
    }
    out = buf_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Racy snapshot, for diagnostics only.
  std::size_t size_approx() const {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::vector<T> buf_;

  // consumer-owned line
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
  // producer-owned line
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;
  // keep the next object off the producer line
  char pad_[kCacheLine - sizeof(std::atomic<std::size_t>) - sizeof(std::size_t)];
};

}  // namespace qc
