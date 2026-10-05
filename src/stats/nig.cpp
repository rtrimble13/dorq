#include "stats/nig.hpp"

#include <algorithm>
#include <cmath>

namespace dorq::stats {

void DiscountedNig::update(double x, double discount, double clip) noexcept {
  if (clip > 0.0) {
    const double limit = clip * std::sqrt(variance());
    x = std::clamp(x, -limit, limit);
  }
  a = discount * a + 0.5;
  b = discount * b + 0.5 * x * x;
}

DiscountedNig combine(const DiscountedNig& x, const DiscountedNig& y) noexcept {
  return {x.a + y.a, x.b + y.b};
}

}  // namespace dorq::stats
