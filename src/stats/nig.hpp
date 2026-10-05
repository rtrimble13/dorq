#pragma once

namespace dorq::stats {

// The posterior on the variance of zero-mean returns under a Normal-Inverse-Gamma
// model with exponential forgetting: each step multiplies the prior's weight by
// `discount` before adding one observation, so the estimate follows volatility
// regimes with an effective window of about 1 / (1 - discount) observations.
//
// sigma^2 ~ InverseGamma(a, b); the predictive for the next return is a Student t
// with 2a degrees of freedom and scale sqrt(b / a).
struct DiscountedNig {
  double a = 1.0;
  double b = 1.0;

  // A prior worth `weight` observations with variance `variance`.
  [[nodiscard]] static DiscountedNig prior(double variance, double weight) noexcept {
    return {weight / 2.0, weight / 2.0 * variance};
  }

  // Adds one return. When `clip` > 0 the return is first limited to `clip` times
  // the current scale, so one bad print or one crash cannot swamp the estimate.
  void update(double x, double discount, double clip) noexcept;

  // The variance estimate b / a.
  [[nodiscard]] double variance() const noexcept { return b / a; }
};

// Two estimates, as if both had seen their observations: weights and sums add.
[[nodiscard]] DiscountedNig combine(const DiscountedNig& x, const DiscountedNig& y) noexcept;

}  // namespace dorq::stats
