#include "stats/student_t.hpp"

#include <cmath>
#include <numbers>

#include "stats/special.hpp"

namespace dorq::stats {

double student_t_log_pdf(double x, double nu, double scale) noexcept {
  const double z = x / scale;
  return log_gamma((nu + 1.0) / 2.0) - log_gamma(nu / 2.0) - 0.5 * std::log(nu * std::numbers::pi) -
         std::log(scale) - (nu + 1.0) / 2.0 * std::log1p(z * z / nu);
}

double student_t_two_sided_tail(double x, double nu) noexcept {
  if (!std::isfinite(x)) {
    return 0.0;
  }
  // P(|T| >= |x|) = I_{nu / (nu + x^2)}(nu / 2, 1 / 2).
  return incomplete_beta(nu / 2.0, 0.5, nu / (nu + x * x));
}

double student_t_cdf(double x, double nu) noexcept {
  const double tail = student_t_two_sided_tail(x, nu) / 2.0;
  return x < 0.0 ? tail : 1.0 - tail;
}

}  // namespace dorq::stats
