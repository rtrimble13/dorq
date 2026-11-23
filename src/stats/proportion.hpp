#pragma once

#include <cstdint>

namespace dorq::stats {

// Estimates of a proportion from a weighted sample, and of the difference between
// two paired proportions (DE-01; tools/eval/README.md, "Intervals"). The weights
// are survey weights, 1 / inclusion probability: a weighted proportion is a Hajek
// ratio, and its interval is the Wilson score interval on Kish's effective sample
// size. With every weight 1 these are the textbook unweighted forms.

// z for a two-sided 95% interval, and for a one-sided 95% bound.
inline constexpr double kZTwoSided95 = 1.959964;
inline constexpr double kZOneSided95 = 1.644854;

struct Interval {
  double low = 0.0;
  double high = 1.0;
};

// Kish's effective sample size of weights whose sum is `sum` and whose sum of
// squares is `sum_of_squares`: sum^2 / sum_of_squares; 0 when there are none.
[[nodiscard]] double effective_size(double sum, double sum_of_squares) noexcept;

// The Wilson score interval for a proportion p observed on n (effective)
// observations, at z. [0, 1] when n is 0.
[[nodiscard]] Interval wilson(double p, double n, double z) noexcept;

// Two engines on the same items: the weighted sums of the items both found, only
// the first found, only the second found and neither found, and the effective size
// of their weights.
struct PairedSums {
  double both = 0.0;
  double first_only = 0.0;
  double second_only = 0.0;
  double neither = 0.0;
  double effective_size = 0.0;

  [[nodiscard]] double total() const noexcept { return both + first_only + second_only + neither; }
  // The first engine's share found minus the second's: (b - c) / W.
  [[nodiscard]] double difference() const noexcept;
};

// The lower confidence bound at z of the paired difference, by Newcombe's hybrid
// score method 10 (Newcombe 1998, Statistics in Medicine 17:2635): Wilson bounds on
// each engine's share at z on the effective size, joined through the phi
// correlation of the 2x2 table, uncorrected. At z = kZOneSided95 it is the
// one-sided 95% lower bound. 0 when there are no items.
[[nodiscard]] double paired_difference_lower(const PairedSums& sums, double z) noexcept;

// The exact one-sided McNemar test on unweighted counts of discordant items:
// P(X >= first_only) for X ~ Binomial(first_only + second_only, 1/2), the p-value
// against "the first engine finds no more than the second". 1 when no item is
// discordant.
[[nodiscard]] double mcnemar_one_sided(std::int64_t first_only, std::int64_t second_only) noexcept;

}  // namespace dorq::stats
