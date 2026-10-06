#pragma once

#include <cstddef>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
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
  // The scale moves are judged on: the log (moves are ratios) or the value itself
  // (moves are changes; a point series that crosses zero). See Transform.
  bool log_scale = true;
  std::vector<std::size_t> row;  // the bar's row in the series
  std::vector<double> y;         // log close, or the value
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
  // The size of a wrong value's jump, on the scale of y: 0.3 on the log scale, and
  // in proportion to the series' typical magnitude on the value scale.
  double error_scale = 0.3;
  double level = 1.0;  // the median magnitude of the values (value scale)
  // With --market (log scale only): the market's log return over each bar's span,
  // and the series' beta to it there. `ret` and `y` are then the residual: the
  // move the market does not explain. Empty without a market.
  std::vector<double> market;
  std::vector<double> beta;

  [[nodiscard]] std::size_t size() const noexcept { return y.size(); }
};

// The class prior on daily volatility, by price and dollar volume: penny stocks
// move more than large caps, and thin names more than liquid ones. A fixed table
// until `dorq calibrate` (M6) fits one; see doc/checks/DQ201.md.
[[nodiscard]] double class_volatility(double median_price, double median_dollar_volume) noexcept;

// The scale a series is judged on: bars always on the log scale; a point series
// as `transform` says, `auto` taking the log when every value is positive.
[[nodiscard]] bool uses_log_scale(const Series& series, Transform transform) noexcept;

// Applicable to every series with at least two usable values: on the log scale a
// finite value above zero, on the value scale any finite value.
// With `market`, returns are taken net of the market's move (see beta above).
[[nodiscard]] PriceFeatures compute_price_features(const Series& series, const Calendar& calendar,
                                                   const PriceSettings& settings,
                                                   const MarketSeries* market = nullptr);

}  // namespace dorq
