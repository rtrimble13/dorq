#pragma once

namespace dorq::stats {

// Student's t distribution with `nu` degrees of freedom, located at 0, with scale
// `scale` (the standard deviation is scale * sqrt(nu / (nu - 2)) for nu > 2).

// log density at x.
[[nodiscard]] double student_t_log_pdf(double x, double nu, double scale) noexcept;

// P(|X| >= |x|) for the standard t (scale 1).
[[nodiscard]] double student_t_two_sided_tail(double x, double nu) noexcept;

// P(X <= x) for the standard t (scale 1).
[[nodiscard]] double student_t_cdf(double x, double nu) noexcept;

}  // namespace dorq::stats
