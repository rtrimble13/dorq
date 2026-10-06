#include "price/features.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "stats/robust.hpp"

namespace dorq {
namespace {

// A return more than this many standard deviations from zero updates the
// volatility estimate as if it were exactly this many.
constexpr double kClip = 4.0;
// The class prior counts as this many returns against the series' own.
constexpr double kClassWeight = 10.0;
// The volatility filters start from this many returns' worth of prior.
constexpr double kPriorWeight = 10.0;
// Beta: a series' own regression on the market, shrunk toward 1 (an ordinary
// stock) by this many market-sized returns; locally, a discounted regression
// either side of each bar (about 100 returns each way), shrunk toward the
// series' own beta by as many.
constexpr double kBetaPriorWeight = 20.0;
constexpr double kBetaDiscount = 0.99;
// Returns larger than this many robust standard deviations, or spanning more
// sessions than kBetaMaxGap, say nothing about beta: a split, a bad print.
constexpr double kBetaOutlier = 5.0;
constexpr int kBetaMaxGap = 5;

// Takes the market's move out of each return: ret[i] -= beta[i] * market[i], and y
// follows. The market return of a bar is the market's move between the two bars'
// dates (on or before each).
void remove_market(PriceFeatures& f, const Series& series, const MarketSeries& market,
                   double own_scale) {
  const std::size_t n = f.size();
  f.market.assign(n, 0.0);
  f.beta.assign(n, 0.0);
  for (std::size_t i = 1; i < n; ++i) {
    const auto a = market.log_level_on(series.date[f.row[i - 1]]);
    const auto b = market.log_level_on(series.date[f.row[i]]);
    if (a && b) {
      f.market[i] = *b - *a;
    }
  }
  std::vector<bool> usable(n, false);
  double sxy = 0.0;
  double sxx = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 1; i < n; ++i) {
    const double limit = kBetaOutlier * own_scale * std::sqrt(static_cast<double>(f.elapsed[i]));
    usable[i] = f.market[i] != 0.0 && f.elapsed[i] <= kBetaMaxGap &&
                (!(own_scale > 0.0) || std::fabs(f.ret[i]) <= limit);
    if (usable[i]) {
      sxy += f.ret[i] * f.market[i];
      sxx += f.market[i] * f.market[i];
      ++count;
    }
  }
  if (count == 0 || !(sxx > 0.0)) {
    f.market.clear();
    f.beta.clear();
    return;
  }
  const double prior = kBetaPriorWeight * sxx / static_cast<double>(count);
  const double own_beta = (sxy + prior * 1.0) / (sxx + prior);
  // Discounted sums from the left (returns 1..i-1) and the right (i+1..n-1).
  std::vector<double> left_xy(n, 0.0);
  std::vector<double> left_xx(n, 0.0);
  double xy = 0.0;
  double xx = 0.0;
  for (std::size_t i = 1; i < n; ++i) {
    left_xy[i] = xy;
    left_xx[i] = xx;
    xy *= kBetaDiscount;
    xx *= kBetaDiscount;
    if (usable[i]) {
      xy += f.ret[i] * f.market[i];
      xx += f.market[i] * f.market[i];
    }
  }
  xy = 0.0;
  xx = 0.0;
  for (std::size_t i = n; i-- > 1;) {
    f.beta[i] = (left_xy[i] + xy + prior * own_beta) / (left_xx[i] + xx + prior);
    xy *= kBetaDiscount;
    xx *= kBetaDiscount;
    if (usable[i]) {
      xy += f.ret[i] * f.market[i];
      xx += f.market[i] * f.market[i];
    }
  }
  for (std::size_t i = 1; i < n; ++i) {
    f.ret[i] -= f.beta[i] * f.market[i];
    f.y[i] = f.y[i - 1] + f.ret[i];
  }
}

}  // namespace

bool uses_log_scale(const Series& series, Transform transform) noexcept {
  if (series.kind == SeriesKind::kOhlcv || transform == Transform::kLog) {
    return true;
  }
  if (transform == Transform::kDiff) {
    return false;
  }
  return std::none_of(series.close.begin(), series.close.end(),
                      [](double value) { return std::isfinite(value) && value <= 0.0; });
}

double class_volatility(double median_price, double median_dollar_volume) noexcept {
  double sigma = 0.02;
  if (median_price < 1.0) {
    sigma = 0.08;
  } else if (median_price < 5.0) {
    sigma = 0.045;
  } else if (median_price < 20.0) {
    sigma = 0.03;
  }
  if (std::isfinite(median_dollar_volume) && median_dollar_volume < 1e5) {
    sigma *= 1.5;
  }
  return sigma;
}

PriceFeatures compute_price_features(const Series& series, const Calendar& calendar,
                                     const PriceSettings& settings, const MarketSeries* market) {
  PriceFeatures f;
  f.log_scale = uses_log_scale(series, settings.transform);
  const auto scaled_value = [&f](double value) { return f.log_scale ? std::log(value) : value; };
  std::vector<double> prices;
  std::vector<double> volumes;
  for (std::size_t i = 0; i < series.size(); ++i) {
    const double close = series.close[i];
    if (!std::isfinite(close) || (f.log_scale && close <= 0.0)) {
      continue;
    }
    if (!f.row.empty() && series.date[f.row.back()] == series.date[i]) {
      continue;
    }
    // A carry bar (no trade, the last close repeated) says nothing about the
    // price: the next trade's move spans the sessions since the last real one.
    if (!f.row.empty() && series.has_volume && series.volume[i] == 0.0 &&
        close == series.close[f.row.back()]) {
      continue;
    }
    if (!f.row.empty()) {
      const Date prev = series.date[f.row.back()];
      const int sessions =
          calendar.sessions_between(Date::from_days(prev.days() + 1), series.date[i]);
      f.elapsed.push_back(std::max(1, sessions));
      f.ret.push_back(scaled_value(close) - f.y.back());
    } else {
      f.elapsed.push_back(1);
      f.ret.push_back(0.0);
    }
    f.row.push_back(i);
    f.y.push_back(scaled_value(close));
    prices.push_back(f.log_scale ? close : std::fabs(close));
    if (series.has_volume && std::isfinite(series.volume[i])) {
      volumes.push_back(series.volume[i]);
    }
  }
  const std::size_t n = f.size();
  if (n < 2) {
    return PriceFeatures{};
  }
  f.median_price = stats::median(prices);
  f.median_volume =
      volumes.empty() ? std::numeric_limits<double>::quiet_NaN() : stats::median(volumes);
  f.class_volatility = class_volatility(f.median_price, f.median_volume * f.median_price);

  // Empirical Bayes within the series: its own robust scale (the MAD of its
  // returns per session), pulled toward the class by the class prior's weight.
  std::vector<double> scaled;
  scaled.reserve(n - 1);
  const auto scale_returns = [&] {
    scaled.clear();
    for (std::size_t i = 1; i < n; ++i) {
      scaled.push_back(f.ret[i] / std::sqrt(static_cast<double>(f.elapsed[i])));
    }
  };
  scale_returns();
  double own = stats::mad(scaled);
  if (market != nullptr && f.log_scale) {
    remove_market(f, series, *market, own);
    scale_returns();
    own = stats::mad(scaled);
  }
  if (!f.log_scale) {
    // A value-scale series has no class to borrow from: its own scale, or, when
    // most of its changes are zero (a policy rate), the root mean square of them.
    f.level = std::max(f.median_price, 1e-12);
    if (!(own > 0.0)) {
      double sum = 0.0;
      for (const double x : scaled) {
        sum += x * x;
      }
      own = std::sqrt(sum / static_cast<double>(scaled.size()));
    }
    f.class_volatility = own > 0.0 ? own : 1e-3 * std::max(f.level, 1.0);
    f.error_scale = 0.3 * std::max(f.level, 20.0 * f.class_volatility);
  }
  double variance = f.class_volatility * f.class_volatility;
  if (std::isfinite(own) && own > 0.0) {
    const auto count = static_cast<double>(scaled.size());
    variance = (count * own * own + kClassWeight * variance) / (count + kClassWeight);
  }
  f.prior_volatility = std::sqrt(variance);

  const stats::DiscountedNig prior = stats::DiscountedNig::prior(variance, kPriorWeight);
  const double discount = settings.volatility_discount;
  f.forward.assign(n, prior);
  f.backward.assign(n, prior);
  stats::DiscountedNig state = prior;
  for (std::size_t i = 1; i < n; ++i) {
    f.forward[i] = state;
    state.update(scaled[i - 1], discount, kClip);
  }
  state = prior;
  for (std::size_t i = n; i-- > 1;) {
    f.backward[i] = state;
    state.update(scaled[i - 1], discount, kClip);
  }
  f.backward[0] = state;
  return f;
}

}  // namespace dorq
