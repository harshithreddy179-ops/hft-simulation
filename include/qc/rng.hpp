#pragma once
// Small, fast, seedable PRNG (xoshiro256**) with the few distributions the simulator needs.
// Deliberately not <random>: std distributions are not specified bit-for-bit, so the same seed
// can produce different streams on different standard libraries. This one is fully portable
// (modulo libm's log/cos for normal()/exponential()).

#include <cmath>
#include <cstdint>

namespace qc {

__extension__ typedef unsigned __int128 u128;  // portable-enough 64x64->128 multiply (GCC/Clang)

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 1) { reseed(seed); }

  void reseed(std::uint64_t seed) {
    // splitmix64 expands one word into the 256-bit state
    for (auto& w : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1D6E8EB9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
    have_spare_ = false;
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // uniform in [0, 1)
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

  // uniform integer in [lo, hi] (inclusive)
  std::int64_t range(std::int64_t lo, std::int64_t hi) {
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>((static_cast<u128>(next()) * span) >> 64);
  }

  bool chance(double p) { return uniform() < p; }

  // standard normal (Box-Muller)
  double normal() {
    if (have_spare_) {
      have_spare_ = false;
      return spare_;
    }
    double u1 = uniform();
    if (u1 < 1e-300) u1 = 1e-300;
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double th = 6.283185307179586 * u2;
    spare_ = r * std::sin(th);
    have_spare_ = true;
    return r * std::cos(th);
  }

  double exponential(double mean) {
    double u = uniform();
    if (u < 1e-300) u = 1e-300;
    return -mean * std::log(u);
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  std::uint64_t s_[4];
  double spare_ = 0.0;
  bool have_spare_ = false;
};

}  // namespace qc
