#include "stats/proportion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "stats/special.hpp"

namespace dorq::stats {

double effective_size(double sum, double sum_of_squares) noexcept {
  return sum_of_squares > 0.0 ? sum * sum / sum_of_squares : 0.0;
}

Interval wilson(double p, double n, double z) noexcept {
  if (std::isnan(n) || n <= 0.0) {
    return {};
  }
  const double z2 = z * z;
  const double denominator = 1.0 + z2 / n;
  const double centre = (p + z2 / (2.0 * n)) / denominator;
  const double half =
      z * std::sqrt(std::max(0.0, p * (1.0 - p) / n + z2 / (4.0 * n * n))) / denominator;
  return {std::max(0.0, centre - half), std::min(1.0, centre + half)};
}

double PairedSums::difference() const noexcept {
  const double w = total();
  return w > 0.0 ? (first_only - second_only) / w : 0.0;
}

double paired_difference_lower(const PairedSums& s, double z) noexcept {
  const double w = s.total();
  if (w <= 0.0) {
    return 0.0;
  }
  const double p1 = (s.both + s.first_only) / w;
  const double p2 = (s.both + s.second_only) / w;
  const Interval i1 = wilson(p1, s.effective_size, z);
  const Interval i2 = wilson(p2, s.effective_size, z);
  const double denominator = (s.both + s.first_only) * (s.second_only + s.neither) *
                             (s.both + s.second_only) * (s.first_only + s.neither);
  const double phi = denominator > 0.0 ? (s.both * s.neither - s.first_only * s.second_only) /
                                             std::sqrt(denominator)
                                       : 0.0;
  const double dl = p1 - i1.low;
  const double eu = i2.high - p2;
  return (p1 - p2) - std::sqrt(std::max(0.0, dl * dl - 2.0 * phi * dl * eu + eu * eu));
}

double mcnemar_one_sided(std::int64_t first_only, std::int64_t second_only) noexcept {
  const std::int64_t n = first_only + second_only;
  if (n <= 0 || first_only <= 0) {
    return 1.0;
  }
  const auto nd = static_cast<double>(n);
  const double log_half = -nd * std::numbers::ln2;
  const double log_n = log_gamma(nd + 1.0);
  double tail = 0.0;
  for (std::int64_t k = n; k >= first_only; --k) {
    const auto kd = static_cast<double>(k);
    tail += std::exp(log_n - log_gamma(kd + 1.0) - log_gamma(nd - kd + 1.0) + log_half);
  }
  return std::min(1.0, tail);
}

}  // namespace dorq::stats
