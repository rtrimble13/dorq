#include "price/tick.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dorq {

double tick_size(Date date, double price) noexcept {
  static const Date kSixteenths = Date::from_ymd(1997, 6, 24);
  static const Date kDecimal = Date::from_ymd(2001, 4, 9);
  if (date < kSixteenths) {
    return 0.125;
  }
  if (date < kDecimal) {
    return 0.0625;
  }
  return price < 1.0 ? 0.0001 : 0.01;
}

double written_grid(std::vector<std::uint8_t> decimals) {
  if (decimals.empty()) {
    return 1.0;
  }
  const auto at = decimals.begin() + static_cast<std::ptrdiff_t>(decimals.size() * 9 / 10);
  std::nth_element(decimals.begin(), at, decimals.end());
  return std::pow(10.0, -static_cast<double>(*at));
}

double observed_step(const Series& series, std::size_t from, std::size_t to) {
  std::vector<double> prices;
  const bool bars = series.kind == SeriesKind::kOhlcv;
  for (std::size_t j = from; j < to && j < series.size(); ++j) {
    const double close = series.close[j];
    if (std::isfinite(close) && close > 0.0) {
      prices.push_back(close);
    }
    if (!bars) {
      continue;
    }
    for (const double p : {series.open[j], series.high[j], series.low[j]}) {
      if (std::isfinite(p) && p > 0.0) {
        prices.push_back(p);
      }
    }
  }
  std::sort(prices.begin(), prices.end());
  double step = std::numeric_limits<double>::infinity();
  for (std::size_t k = 1; k < prices.size(); ++k) {
    const double gap = prices[k] - prices[k - 1];
    // Equal as written, or apart by a rounding error.
    if (gap > 1e-9 * prices[k]) {
      step = std::min(step, gap);
    }
  }
  return std::isfinite(step) ? step * (1.0 - 1e-9) : 0.0;
}

}  // namespace dorq
