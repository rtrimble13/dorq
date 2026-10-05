#pragma once

#include <span>
#include <vector>

namespace dorq::stats {

// Robust location and scale (the Hampel filter's ingredients).

// The median of `values` (the lower median for an even count); NaN when empty.
// Takes a copy: the order of the caller's values is kept.
[[nodiscard]] double median(std::vector<double> values) noexcept;

// The median absolute deviation from the median, scaled by 1.4826 so it estimates
// the standard deviation of normal data; NaN when empty.
[[nodiscard]] double mad(std::span<const double> values);

}  // namespace dorq::stats
