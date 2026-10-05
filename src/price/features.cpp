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

}  // namespace

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
                                     const PriceSettings& settings) {
  PriceFeatures f;
  if (series.kind == SeriesKind::kPoint) {
    for (const double value : series.close) {
      if (std::isfinite(value) && value <= 0.0) {
        return f;  // needs a transform other than log (M4)
      }
    }
  }
  std::vector<double> prices;
  std::vector<double> volumes;
  for (std::size_t i = 0; i < series.size(); ++i) {
    const double close = series.close[i];
    if (!std::isfinite(close) || close <= 0.0) {
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
      f.ret.push_back(std::log(close) - f.y.back());
    } else {
      f.elapsed.push_back(1);
      f.ret.push_back(0.0);
    }
    f.row.push_back(i);
    f.y.push_back(std::log(close));
    prices.push_back(close);
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
  for (std::size_t i = 1; i < n; ++i) {
    scaled.push_back(f.ret[i] / std::sqrt(static_cast<double>(f.elapsed[i])));
  }
  const double own = stats::mad(scaled);
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
