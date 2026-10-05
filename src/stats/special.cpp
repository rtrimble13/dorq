#include "stats/special.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace dorq::stats {
namespace {

constexpr double kEpsilon = 1e-15;
constexpr double kTiny = 1e-300;
constexpr int kMaxIterations = 1000;

// Continued fraction for the incomplete beta (Numerical Recipes, betacf).
double beta_continued_fraction(double a, double b, double x) noexcept {
  const double qab = a + b;
  const double qap = a + 1.0;
  const double qam = a - 1.0;
  double c = 1.0;
  double d = 1.0 - qab * x / qap;
  d = std::fabs(d) < kTiny ? kTiny : d;
  d = 1.0 / d;
  double h = d;
  for (int m = 1; m <= kMaxIterations; ++m) {
    const double dm = m;
    const double m2 = 2.0 * dm;
    double aa = dm * (b - dm) * x / ((qam + m2) * (a + m2));
    d = 1.0 + aa * d;
    d = std::fabs(d) < kTiny ? kTiny : d;
    c = 1.0 + aa / c;
    c = std::fabs(c) < kTiny ? kTiny : c;
    d = 1.0 / d;
    h *= d * c;
    aa = -(a + dm) * (qab + dm) * x / ((a + m2) * (qap + m2));
    d = 1.0 + aa * d;
    d = std::fabs(d) < kTiny ? kTiny : d;
    c = 1.0 + aa / c;
    c = std::fabs(c) < kTiny ? kTiny : c;
    d = 1.0 / d;
    const double delta = d * c;
    h *= delta;
    if (std::fabs(delta - 1.0) < kEpsilon) {
      break;
    }
  }
  return h;
}

}  // namespace

namespace {

// log Gamma(x) for x >= 0.5: Lanczos approximation, g = 7, n = 9 (Godfrey's
// coefficients).
double lanczos_log_gamma(double x) noexcept {
  constexpr std::array<double, 9> kCoefficients = {
      0.99999999999980993,  676.5203681218851,     -1259.1392167224028,
      771.32342877765313,   -176.61502916214059,   12.507343278686905,
      -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7};
  const double z = x - 1.0;
  double sum = kCoefficients[0];
  for (std::size_t i = 1; i < kCoefficients.size(); ++i) {
    sum += kCoefficients.at(i) / (z + static_cast<double>(i));
  }
  const double t = z + 7.5;
  return 0.5 * std::log(2.0 * std::numbers::pi) + (z + 0.5) * std::log(t) - t + std::log(sum);
}

}  // namespace

double log_gamma(double x) noexcept {
  if (x < 0.5) {
    // Reflection: Gamma(x) Gamma(1 - x) = pi / sin(pi x).
    return std::log(std::numbers::pi / std::fabs(std::sin(std::numbers::pi * x))) -
           lanczos_log_gamma(1.0 - x);
  }
  return lanczos_log_gamma(x);
}

double log_beta(double a, double b) noexcept {
  return log_gamma(a) + log_gamma(b) - log_gamma(a + b);
}

double incomplete_beta(double a, double b, double x) noexcept {
  if (x <= 0.0) {
    return 0.0;
  }
  if (x >= 1.0) {
    return 1.0;
  }
  const double front = std::exp(a * std::log(x) + b * std::log1p(-x) - log_beta(a, b));
  if (x < (a + 1.0) / (a + b + 2.0)) {
    return front * beta_continued_fraction(a, b, x) / a;
  }
  return 1.0 - front * beta_continued_fraction(b, a, 1.0 - x) / b;
}

double beta_quantile(double a, double b, double q) noexcept {
  if (q <= 0.0) {
    return 0.0;
  }
  if (q >= 1.0) {
    return 1.0;
  }
  double lo = 0.0;
  double hi = 1.0;
  for (int i = 0; i < 200 && hi - lo > 1e-15; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (incomplete_beta(a, b, mid) < q) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

double incomplete_gamma(double a, double x) noexcept {
  if (x <= 0.0) {
    return 0.0;
  }
  const double log_front = -x + a * std::log(x) - log_gamma(a);
  if (x < a + 1.0) {
    // Series.
    double term = 1.0 / a;
    double sum = term;
    double ap = a;
    for (int n = 0; n < kMaxIterations; ++n) {
      ap += 1.0;
      term *= x / ap;
      sum += term;
      if (std::fabs(term) < std::fabs(sum) * kEpsilon) {
        break;
      }
    }
    return std::exp(log_front) * sum;
  }
  // Continued fraction for Q, then P = 1 - Q.
  double b = x + 1.0 - a;
  double c = 1.0 / kTiny;
  double d = 1.0 / b;
  double h = d;
  for (int i = 1; i <= kMaxIterations; ++i) {
    const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
    b += 2.0;
    d = an * d + b;
    d = std::fabs(d) < kTiny ? kTiny : d;
    c = b + an / c;
    c = std::fabs(c) < kTiny ? kTiny : c;
    d = 1.0 / d;
    const double delta = d * c;
    h *= delta;
    if (std::fabs(delta - 1.0) < kEpsilon) {
      break;
    }
  }
  return 1.0 - std::exp(log_front) * h;
}

double poisson_upper_tail(std::int64_t k, double lambda) noexcept {
  if (k <= 0) {
    return 1.0;
  }
  if (lambda <= 0.0) {
    return 0.0;
  }
  // P(X >= k) = P(k, lambda), the regularized lower incomplete gamma.
  return incomplete_gamma(static_cast<double>(k), lambda);
}

double normal_cdf(double x) noexcept { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

double log_add(double a, double b) noexcept {
  if (a == -std::numeric_limits<double>::infinity()) {
    return b;
  }
  if (b == -std::numeric_limits<double>::infinity()) {
    return a;
  }
  const double hi = std::max(a, b);
  return hi + std::log1p(std::exp(std::min(a, b) - hi));
}

}  // namespace dorq::stats
