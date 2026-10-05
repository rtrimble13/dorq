#pragma once

#include <cstddef>
#include <vector>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/series.hpp"
#include "stats/nig.hpp"

namespace dorq {

// The per-series features every price check reads (plan section 4.1), computed once.
//
// Bars are the series' rows with a usable price: a finite close above zero, and the
// first row of each date (DQ102, DQ104 and DQ103 report the others). Carry bars,
// with no volume and the last close repeated, are left out: they record that
// nothing traded, not a price. Index i below
// is the i-th such bar; return i is the move from bar i-1 to bar i.
struct PriceFeatures {
  std::vector<std::size_t> row;  // the bar's row in the series
  std::vector<double> y;         // log close
  std::vector<double> ret;       // y[i] - y[i-1]; ret[0] is 0
  std::vector<int> elapsed;      // sessions from bar i-1 to bar i (at least 1)
  // The volatility of one session's return, before and after each return:
  // forward[i] has seen returns 1..i-1, backward[i] returns i+1..n-1. Returns are
  // scaled by sqrt(elapsed) and clipped (see stats::DiscountedNig).
  std::vector<stats::DiscountedNig> forward;
  std::vector<stats::DiscountedNig> backward;
  double class_volatility = 0.0;  // the class prior's daily volatility
  double prior_volatility = 0.0;  // the class prior combined with the series' own
  double median_price = 0.0;
  double median_volume = 0.0;  // NaN without volume

  [[nodiscard]] std::size_t size() const noexcept { return y.size(); }
};

// The class prior on daily volatility, by price and dollar volume: penny stocks
// move more than large caps, and thin names more than liquid ones. A fixed table
// until `dorq calibrate` (M6) fits one; see doc/checks/DQ201.md.
[[nodiscard]] double class_volatility(double median_price, double median_dollar_volume) noexcept;

// Applicable to OHLCV series and to point series whose values are all positive
// (the log transform). Returns empty features otherwise.
[[nodiscard]] PriceFeatures compute_price_features(const Series& series, const Calendar& calendar,
                                                   const PriceSettings& settings);

}  // namespace dorq
