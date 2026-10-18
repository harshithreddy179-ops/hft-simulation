#pragma once
// Latency measurement: a monotonic nanosecond clock and a fixed-memory log-linear histogram.
//
// The histogram has 16 linear sub-buckets per power of two (HdrHistogram-style), i.e. a worst-case
// relative error of 1/16 = 6.25% on any percentile, a constant 976-slot footprint (7.8 KB) that
// stays in L1, and a branch-light O(1) record() so measuring does not distort what is measured.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>

namespace qc {

inline std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

class LatencyHistogram {
 public:
  static constexpr unsigned kSubBits = 4;
  static constexpr unsigned kSubCount = 1u << kSubBits;
  static constexpr unsigned kBuckets = (64 - kSubBits + 1) * kSubCount;

  void record(std::uint64_t v) {
    ++counts_[index(v)];
    ++total_;
    sum_ += static_cast<double>(v);
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
  }

  void merge(const LatencyHistogram& o) {
    for (unsigned i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    total_ += o.total_;
    sum_ += o.sum_;
    min_ = std::min(min_, o.min_);
    max_ = std::max(max_, o.max_);
  }

  std::uint64_t count() const { return total_; }
  std::uint64_t min() const { return total_ ? min_ : 0; }
  std::uint64_t max() const { return max_; }
  double mean() const { return total_ ? sum_ / static_cast<double>(total_) : 0.0; }

  // p in [0, 100]. Returns the upper edge of the bucket holding the p-th percentile sample
  // (never above the observed max).
  std::uint64_t percentile(double p) const {
    if (total_ == 0) return 0;
    auto rank = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_) + 0.999999999);
    rank = std::clamp<std::uint64_t>(rank, 1, total_);
    std::uint64_t seen = 0;
    for (unsigned i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen >= rank) return std::min(upper_bound(i), max_);
    }
    return max_;
  }

  static unsigned index(std::uint64_t v) {
    if (v < kSubCount) return static_cast<unsigned>(v);
    const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(v));
    const unsigned shift = msb - kSubBits;
    return ((shift + 1) << kSubBits) + static_cast<unsigned>((v >> shift) - kSubCount);
  }

  static std::uint64_t upper_bound(unsigned idx) {
    if (idx < kSubCount) return idx;
    const unsigned shift = (idx >> kSubBits) - 1;
    const std::uint64_t lo = static_cast<std::uint64_t>(kSubCount + (idx & (kSubCount - 1))) << shift;
    return lo + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  double sum_ = 0.0;
  std::uint64_t min_ = ~std::uint64_t{0};
  std::uint64_t max_ = 0;
};

}  // namespace qc
