#pragma once

#include <cstdint>

namespace dorq::stats {

// Special functions, implemented here rather than taken from <cmath> where the C
// library's version is not thread-safe (lgamma sets the global signgam) or not
// available (incomplete beta and gamma). Using one implementation on every
// platform also keeps results from depending on the C library (doc/adr/0001).

// log Gamma(x) for x > 0 (Lanczos, g = 7; ~15 significant digits).
[[nodiscard]] double log_gamma(double x) noexcept;

// log B(a, b).
[[nodiscard]] double log_beta(double a, double b) noexcept;

// The regularized incomplete beta function I_x(a, b), for a, b > 0 and x in [0, 1]:
// the CDF of a Beta(a, b) distribution at x.
[[nodiscard]] double incomplete_beta(double a, double b, double x) noexcept;

// The q-quantile of Beta(a, b), q in [0, 1].
[[nodiscard]] double beta_quantile(double a, double b, double q) noexcept;

// The regularized lower incomplete gamma function P(a, x), a > 0, x >= 0.
[[nodiscard]] double incomplete_gamma(double a, double x) noexcept;

// P(X >= k) for X ~ Poisson(lambda).
[[nodiscard]] double poisson_upper_tail(std::int64_t k, double lambda) noexcept;

// The standard normal CDF.
[[nodiscard]] double normal_cdf(double x) noexcept;

// log(exp(a) + exp(b)) without overflow; -inf is the identity.
[[nodiscard]] double log_add(double a, double b) noexcept;

}  // namespace dorq::stats
