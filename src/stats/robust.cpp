#include "stats/robust.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace dorq::stats {

double median(std::vector<double> values) noexcept {
  if (values.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>((values.size() - 1) / 2);
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

double mad(std::span<const double> values) {
  const double center = median({values.begin(), values.end()});
  std::vector<double> deviation;
  deviation.reserve(values.size());
  for (const double v : values) {
    deviation.push_back(std::fabs(v - center));
  }
  return 1.4826 * median(std::move(deviation));
}

}  // namespace dorq::stats
