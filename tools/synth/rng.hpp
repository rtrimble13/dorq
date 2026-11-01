#pragma once

// dorq-synth's random numbers: its own PRNG (xoshiro256**) and its own normal and t
// samplers, so a seed gives the same data with any C++ library.

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace synth {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) {
    for (auto& word : state_) {
      seed += 0x9e3779b97f4a7c15ULL;  // splitmix64
      std::uint64_t z = seed;
      z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
      word = z ^ (z >> 31U);
    }
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17U;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
  }

  // Uniform on (0, 1).
  double uniform() { return (static_cast<double>(next() >> 11U) + 0.5) * 0x1.0p-53; }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  // Log-uniform on [lo, hi].
  double log_uniform(double lo, double hi) { return std::exp(uniform(std::log(lo), std::log(hi))); }
  int integer(int lo, int hi) {  // inclusive
    return lo + static_cast<int>(uniform() * (hi - lo + 1));
  }
  bool chance(double p) { return uniform() < p; }
  double normal() {  // Box-Muller, one of the pair
    return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * std::numbers::pi * uniform());
  }
  // Student t with 4 degrees of freedom, scaled to unit variance.
  double t4() {
    const double chi2 = -2.0 * std::log(uniform() * uniform());  // chi-square, 4 dof
    return normal() / std::sqrt(chi2 / 4.0) / std::numbers::sqrt2;
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << static_cast<unsigned>(k)) | (x >> static_cast<unsigned>(64 - k));
  }
  std::array<std::uint64_t, 4> state_{};
};

}  // namespace synth
