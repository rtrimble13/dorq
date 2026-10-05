#include "price/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dorq/number.hpp"
#include "price/tick.hpp"
#include "stats/robust.hpp"
#include "stats/special.hpp"
#include "stats/student_t.hpp"

namespace dorq {
namespace {

using H = PriceHypothesis;
constexpr std::size_t kCount = kPriceHypotheses;
using Terms = std::array<double, kCount>;  // a log-likelihood term per hypothesis

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

constexpr std::size_t idx(H h) noexcept { return static_cast<std::size_t>(h); }

// The size of an error's jump, on the log scale: anything from a few percent to
// orders of magnitude (a Cauchy distribution with scale 0.3).
constexpr double kErrorJumpScale = 0.3;
// Log ratio between two unrelated securities' prices, for a history segment.
constexpr double kSegmentSpread = 1.2;
// Fewest bars a window needs to say anything about a volume or precision level.
constexpr std::size_t kMinWindow = 5;
// P(close outside [low, high]) when the bar is genuine.
constexpr double kRangeNoise = 1e-3;
// The weight a scale error puts on ratios that are no power of ten; the
// plausible_level term carries the case for those (AKR's 149,613,176).
constexpr double kScaleBroad = 0.001;
// A bad print's block of k bars has prior weight proportional to this to the k.
constexpr double kBlockDecay = 0.5;
// Volume on the day of a real large move, against the usual: about e times.
constexpr double kSurge = 1.0;
// Volume on the day of a bad print: a little below the usual.
constexpr double kBadPrintVolume = -0.3;
// Round prices a new listing starts at (an ETF at 25, a SPAC at 10).
constexpr std::array<double, 5> kListingPrices = {10.0, 20.0, 25.0, 50.0, 100.0};
// After a real move volatility rises: by about this share of the move's size
// (the square root of a GARCH model's reaction coefficient, about 0.06).
constexpr double kAftershock = 0.25;
// How far a volume level drifts over a few months, on the log scale; after a
// split, share volume follows the ratio more closely than that.
constexpr double kVolumeDrift = 0.5;
constexpr double kSplitVolumeDrift = 0.3;
// A print at the bid or the ask: about 0.7 of the grid step, on the price scale.
constexpr double kBounce = 0.7;

double log_normal_pdf(double x, double mean, double sd) noexcept {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}

double log_cauchy(double x, double scale) noexcept {
  return stats::student_t_log_pdf(x, 1.0, scale);
}

double split_weight(const SplitRatio& ratio) noexcept {
  struct Known {
    SplitRatio ratio;
    double weight;
  };
  // Rough frequencies of US splits: forward 2:1 dominates, and reverse splits of
  // 1:10, 1:5 and 1:20 are the usual penny-stock repair.
  static constexpr std::array<Known, 23> kKnown = {{
      {{2, 1}, 0.30},   {{3, 1}, 0.05},   {{3, 2}, 0.06},   {{4, 1}, 0.03},   {{5, 1}, 0.015},
      {{5, 4}, 0.01},   {{8, 1}, 0.005},  {{10, 1}, 0.01},  {{15, 1}, 0.002}, {{20, 1}, 0.005},
      {{1, 2}, 0.03},   {{1, 3}, 0.02},   {{1, 4}, 0.02},   {{1, 5}, 0.06},   {{1, 8}, 0.01},
      {{1, 10}, 0.12},  {{1, 15}, 0.015}, {{1, 20}, 0.05},  {{1, 25}, 0.02},  {{1, 30}, 0.01},
      {{1, 40}, 0.005}, {{1, 50}, 0.02},  {{1, 100}, 0.01},
  }};
  for (const Known& known : kKnown) {
    if (known.ratio == ratio) {
      return known.weight;
    }
  }
  return 0.005;
}

// How a split's direction depends on the price before it: companies split forward
// to bring a high price down, and reverse-split to lift a low one (often to keep a
// listing). Returns the share of forward splits at `price`, between 0.05 and 0.95:
// log-linear from $5 to $40.
double split_level_weights(double price) noexcept {
  const double share = (std::log(price) - std::log(5.0)) / (std::log(40.0) - std::log(5.0));
  return std::clamp(share, 0.05, 0.95);
}

// Powers of ten a scale error multiplies by, and their weights (each sign half).
constexpr std::array<double, 6> kPowerWeights = {0.5, 0.3, 0.1, 0.05, 0.03, 0.02};

struct Term {
  std::string feature;
  DetailValue value;
  std::string note;
  Terms ll{};
};

struct VolumeWindow {
  std::size_t count = 0;
  double median = 0.0;
  double zero_share = 0.0;
  double log_spread = 0.0;  // robust sd of log(1 + volume)
};

class Scorer {
 public:
  Scorer(const Series& series, const PriceFeatures& f, const PriceSettings& settings)
      : series_(series), f_(f), settings_(settings), n_(f.size()) {
    has_ohlc_ = series.kind == SeriesKind::kOhlcv;
    const double nu = settings.tail_dof;
    t_scale_ = nu > 2.0 ? std::sqrt((nu - 2.0) / nu) : 1.0;
    double total = 0.0;
    for (const SplitRatio& ratio : settings.split_ratios) {
      total += split_weight(ratio);
    }
    for (const SplitRatio& ratio : settings.split_ratios) {
      splits_.emplace_back(ratio, split_weight(ratio) / total);
    }
    std::vector<int> decimals;
    decimals.reserve(n_);
    for (std::size_t i = 0; i < n_; ++i) {
      decimals.push_back(series.close_decimals[f.row[i]]);
    }
    std::sort(decimals.begin(), decimals.end());
    written_grid_ = std::pow(10.0, -decimals[decimals.size() * 9 / 10]);
  }

  [[nodiscard]] double close(std::size_t bar) const { return series_.close[f_.row[bar]]; }
  [[nodiscard]] Date date(std::size_t bar) const { return series_.date[f_.row[bar]]; }
  [[nodiscard]] double volume(std::size_t bar) const {
    return series_.has_volume ? series_.volume[f_.row[bar]]
                              : std::numeric_limits<double>::quiet_NaN();
  }

  // The per-session volatility at bar t, leaving out returns t..t+k.
  [[nodiscard]] double sigma(std::size_t t, std::size_t k) const {
    const std::size_t after = std::min(t + k, n_ - 1);
    return std::sqrt(stats::combine(f_.forward[t], f_.backward[after]).variance());
  }

  // The grid prices move on between bars a and b: the exchange's tick, or the
  // coarser grid the prices are written on (a sub-dime stock quoted in cents
  // moves a cent at a time).
  [[nodiscard]] double price_grid(std::size_t a, std::size_t b) const {
    return std::max(tick_size(date(b), std::min(close(a), close(b))), written_grid_);
  }

  // The standard deviation of an ordinary move from bar a to bar b: volatility
  // over the sessions between them, plus the bid-ask bounce of a print landing
  // on either side of the spread, which dominates on a coarse grid.
  [[nodiscard]] double move_sd(double sig, std::size_t a, std::size_t b) const {
    int sessions = 0;
    for (std::size_t i = a + 1; i <= b; ++i) {
      sessions += f_.elapsed[i];
    }
    const double bounce = kBounce * price_grid(a, b) / std::min(close(a), close(b));
    return std::hypot(sig * std::sqrt(static_cast<double>(sessions)), bounce);
  }

  // log density of an ordinary move with standard deviation `sd`: Student t, with
  // a small chance of a jump `jump_scale` times larger.
  [[nodiscard]] double ordinary(double x, double sd) const {
    const double scale = sd * t_scale_;
    const double nu = settings_.tail_dof;
    const double q = settings_.jump_prob;
    return stats::log_add(
        std::log1p(-q) + stats::student_t_log_pdf(x, nu, scale),
        std::log(q) + stats::student_t_log_pdf(x, nu, settings_.jump_scale * scale));
  }

  // The two-sided tail probability of x as an ordinary move.
  [[nodiscard]] double tail(double x, double sd) const {
    return stats::student_t_two_sided_tail(x / (sd * t_scale_), settings_.tail_dof);
  }

  [[nodiscard]] VolumeWindow volume_window(std::size_t from, std::size_t to) const {
    VolumeWindow w;
    if (!series_.has_volume) {
      return w;
    }
    std::vector<double> values;
    std::vector<double> logs;
    std::size_t zeros = 0;
    for (std::size_t i = from; i < to && i < n_; ++i) {
      const double v = volume(i);
      if (std::isfinite(v) && v >= 0.0) {
        values.push_back(v);
        logs.push_back(std::log1p(v));
        zeros += v == 0.0 ? 1 : 0;
      }
    }
    w.count = values.size();
    if (w.count == 0) {
      return w;
    }
    w.median = stats::median(values);
    w.zero_share = static_cast<double>(zeros) / static_cast<double>(w.count);
    w.log_spread = stats::mad(logs);
    return w;
  }

  [[nodiscard]] double median_decimals(std::size_t from, std::size_t to, std::size_t& count) const {
    std::vector<double> values;
    for (std::size_t i = from; i < to && i < n_; ++i) {
      values.push_back(series_.close_decimals[f_.row[i]]);
    }
    count = values.size();
    return values.empty() ? 0.0 : stats::median(values);
  }

  [[nodiscard]] bool plausible(double price) const {
    return price >= settings_.min_price && price <= settings_.max_price;
  }

  PriceFinding score(std::size_t t) const;

  [[nodiscard]] bool screened(std::size_t t) const {
    const double r = f_.ret[t];
    const int elapsed = f_.elapsed[t];
    if (std::fabs(r) > std::log1p(settings_.floor_move) || elapsed >= settings_.segment_gap) {
      return true;
    }
    if (has_ohlc_ && close_outside_range(t)) {
      return true;
    }
    return tail(r, move_sd(sigma(t, 0), t - 1, t)) < settings_.candidate_tail_prob;
  }

  [[nodiscard]] bool close_outside_range(std::size_t t) const {
    const std::size_t row = f_.row[t];
    const double lo = series_.low[row];
    const double hi = series_.high[row];
    const double c = series_.close[row];
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo <= 0.0 || hi <= 0.0) {
      return false;
    }
    return c < lo * (1.0 - 1e-6) || c > hi * (1.0 + 1e-6);
  }

 private:
  const Series& series_;
  const PriceFeatures& f_;
  const PriceSettings& settings_;
  std::size_t n_;
  bool has_ohlc_ = false;
  double t_scale_ = 1.0;
  double written_grid_ = 0.0;  // 10^-d for the decimals most prices are written with
  std::vector<std::pair<SplitRatio, double>> splits_;
};

PriceFinding Scorer::score(std::size_t t) const {
  PriceFinding out;
  out.bar = t;
  out.end_bar = t;
  const double r = f_.ret[t];
  const int elapsed = f_.elapsed[t];
  const auto max_block = static_cast<std::size_t>(settings_.revert_max_bars);
  const double sig = sigma(t, max_block);
  const double base_sd = move_sd(sig, t - 1, t);
  const double prev_close = close(t - 1);
  const double this_close = close(t);
  out.factor = this_close / prev_close;
  out.tail = tail(r, move_sd(sigma(t, 0), t - 1, t));
  const std::size_t after = n_ - 1 - t;  // bars after t
  out.provisional = after < static_cast<std::size_t>(settings_.provisional_bars);

  // Which hypotheses apply.
  std::array<bool, kCount>& on = out.considered;
  on.fill(true);
  const double tick = price_grid(t - 1, t);
  const double ticks = std::fabs(this_close - prev_close) / tick;
  on[idx(H::kTickMove)] = ticks <= 2.5;
  const std::size_t row = f_.row[t];
  const bool bar_ohl = has_ohlc_ && std::isfinite(series_.open[row]) &&
                       std::isfinite(series_.high[row]) && std::isfinite(series_.low[row]) &&
                       series_.open[row] > 0.0 && series_.high[row] > 0.0 && series_.low[row] > 0.0;
  on[idx(H::kBadClose)] = bar_ohl;
  on[idx(H::kUnreportedSplit)] = has_ohlc_ && !splits_.empty();
  on[idx(H::kHistorySegment)] = elapsed >= settings_.segment_gap;

  std::vector<Term> terms;

  // The prior.
  {
    const PricePriors& p = settings_.priors;
    const Terms weights = {p.market_move,      p.tick_move,   p.bad_print,      p.bad_close,
                           p.unreported_split, p.scale_error, p.history_segment};
    double total = 0.0;
    for (std::size_t h = 0; h < kCount; ++h) {
      total += on[h] ? weights[h] : 0.0;
    }
    if (total <= 0.0) {  // every applicable prior set to zero: nothing to compare
      on.fill(false);
      return out;
    }
    Term term{"prior", 0.0, {}, {}};
    for (std::size_t h = 0; h < kCount; ++h) {
      term.ll[h] = on[h] && weights[h] > 0.0 ? std::log(weights[h] / total) : kNegInf;
      on[h] = on[h] && weights[h] > 0.0;
    }
    terms.push_back(std::move(term));
  }

  // The return itself.
  {
    Term term{"return", out.factor, {}, {}};
    term.ll[idx(H::kMarketMove)] = ordinary(r, base_sd);
    term.ll[idx(H::kTickMove)] = std::log(this_close) - std::log(4.0 * tick);
    term.ll[idx(H::kBadPrint)] = log_cauchy(r, kErrorJumpScale);
    term.ll[idx(H::kBadClose)] = log_cauchy(r, kErrorJumpScale);
    // A split or a scale error shifts the level exactly; the rest of the move is
    // an ordinary return.
    const double slack = settings_.ratio_tolerance;
    double split = kNegInf;
    double best = kNegInf;
    const double level = split_level_weights(prev_close);
    for (const auto& [ratio, weight] : splits_) {
      const bool forward = ratio.shares_after > ratio.shares_before;
      const double at_level = forward ? level : 1.0 - level;
      const double ll = std::log(weight * at_level) +
                        ordinary(r - std::log(ratio.price_factor()), std::hypot(base_sd, slack));
      split = stats::log_add(split, ll);
      if (ll > best) {
        best = ll;
        out.split = ratio;
      }
    }
    term.ll[idx(H::kUnreportedSplit)] = split;
    double scale = std::log(kScaleBroad) + log_cauchy(r, kErrorJumpScale);
    double best_power = kNegInf;
    for (std::size_t k = 0; k < kPowerWeights.size(); ++k) {
      for (const int sign : {-1, 1}) {
        const double power = sign * static_cast<double>(k + 1);
        const double ll = std::log((1.0 - kScaleBroad) * kPowerWeights[k] / 2.0) +
                          ordinary(r - power * std::numbers::ln10, std::hypot(base_sd, slack));
        scale = stats::log_add(scale, ll);
        if (ll > best_power) {
          best_power = ll;
          out.power_of_ten = static_cast<int>(power);
        }
      }
    }
    term.ll[idx(H::kScaleError)] = scale;
    term.ll[idx(H::kHistorySegment)] = log_normal_pdf(r, 0.0, std::hypot(kSegmentSpread, base_sd));
    if (out.split) {
      term.note = "nearest split " + out.split->to_string();
    }
    terms.push_back(std::move(term));
  }

  std::size_t next_term = 0;
  double next_largest = 1.0;
  std::size_t next_count = 0;
  // The bars after: a bad print's block of k bars ends with a return that undoes
  // it, so the move from before the block to after it is ordinary. A real move
  // stirs volatility up (half the time, by kAftershock of its size); a split, a
  // scale error or a bad print leaves the bars after it calm.
  std::size_t best_block = 1;
  {
    Term term{"next_bars", false, {}, {}};
    std::vector<double> plain;    // log density of each later return, as ordinary
    std::vector<double> stirred;  // and as ordinary after a real move
    for (std::size_t j = 1; j <= max_block && t + j < n_; ++j) {
      const double sd = move_sd(sig, t + j - 1, t + j);
      plain.push_back(ordinary(f_.ret[t + j], sd));
      stirred.push_back(ordinary(f_.ret[t + j], std::hypot(sd, kAftershock * std::fabs(r))));
    }
    double all_plain = 0.0;
    double all_stirred = 0.0;
    for (std::size_t j = 0; j < plain.size(); ++j) {
      all_plain += plain[j];
      all_stirred += stirred[j];
    }
    double norm = 0.0;
    for (std::size_t k = 1; k <= max_block; ++k) {
      norm += std::pow(kBlockDecay, static_cast<double>(k));
    }
    double mixture = kNegInf;
    double best = kNegInf;
    double first = all_plain;
    for (std::size_t k = 1; k <= max_block; ++k) {
      const double weight = std::pow(kBlockDecay, static_cast<double>(k)) / norm;
      double ll = all_plain;  // not yet reverted within the data: nothing to score
      if (k <= plain.size()) {
        // The block's own moves cancel: from before it to after it is ordinary.
        const double across = f_.y[t + k] - f_.y[t - 1] - (f_.y[t + k - 1] - f_.y[t]);
        ll = all_plain - plain[k - 1] + ordinary(across, move_sd(sig, t - 1, t + k));
      }
      if (k == 1) {
        first = ll;
      }
      mixture = stats::log_add(mixture, std::log(weight) + ll);
      if (std::log(weight) + ll > best) {
        best = std::log(weight) + ll;
        best_block = k;
      }
    }
    for (std::size_t h = 0; h < kCount; ++h) {
      term.ll[h] = all_plain;
    }
    term.ll[idx(H::kMarketMove)] =
        stats::log_add(std::log(0.5) + all_stirred, std::log(0.5) + all_plain);
    term.ll[idx(H::kBadPrint)] = mixture;
    term.ll[idx(H::kBadClose)] = first;
    if (plain.empty()) {
      term.note = "none yet";
    } else {
      // Described for a bad print by the move that undoes it, and otherwise by the
      // largest of the moves that follow (see the evidence step below).
      double largest = 0.0;
      for (std::size_t j = 1; j <= plain.size(); ++j) {
        largest = std::fabs(f_.ret[t + j]) > std::fabs(largest) ? f_.ret[t + j] : largest;
      }
      next_largest = std::exp(largest);
      next_count = plain.size();
      if (t + best_block < n_) {
        term.value = std::exp(f_.ret[t + best_block]);
        term.note = "the move " + std::to_string(best_block) + " bar" +
                    (best_block == 1 ? "" : "s") + " later";
      }
    }
    next_term = terms.size();
    terms.push_back(std::move(term));
  }

  // The bar's own open, high and low.
  if (bar_ohl) {
    const double o = series_.open[row];
    const double hi = series_.high[row];
    const double lo = series_.low[row];
    // The close outside a range that holds the open blames the close; with the open
    // outside too, the high or low is the bad field (DQ101 reports it).
    const bool range_holds_open = lo <= o && o <= hi;
    const bool outside = range_holds_open && close_outside_range(t);
    Term range{"close_in_range", !outside, {}, {}};
    const auto p_out = [outside](double p) { return outside ? std::log(p) : std::log1p(-p); };
    for (std::size_t h = 0; h < kCount; ++h) {
      range.ll[h] = p_out(kRangeNoise);
    }
    range.ll[idx(H::kBadPrint)] = p_out(0.05);
    range.ll[idx(H::kBadClose)] = p_out(0.9);
    terms.push_back(std::move(range));

    const double mid = std::max(std::min(o, hi), std::min(std::max(o, hi), lo));
    const double r_ohl = std::log(mid) - f_.y[t - 1];
    const double sd = 1.5 * base_sd;
    Term level{"open_high_low", std::exp(r_ohl), {}, {}};
    for (std::size_t h = 0; h < kCount; ++h) {
      level.ll[h] = log_normal_pdf(r_ohl, r, sd);
    }
    // A real move's bar opens at the old level and trades to the new, or gaps.
    const double market =
        stats::log_add(std::log(0.5) + log_normal_pdf(r_ohl, r, sd),
                       std::log(0.5) + log_normal_pdf(r_ohl, r / 2.0, std::fabs(r) / 2.0 + sd));
    level.ll[idx(H::kMarketMove)] = market;
    level.ll[idx(H::kTickMove)] = market;
    level.ll[idx(H::kBadClose)] = log_normal_pdf(r_ohl, 0.0, sd);
    terms.push_back(std::move(level));
  }

  // Volume: on the day, and the level before and after.
  const auto window = static_cast<std::size_t>(settings_.volume_window);
  const VolumeWindow pre = volume_window(t >= window ? t - window : 0, t);
  const VolumeWindow post = volume_window(t + 1, t + 1 + window);
  const double v = volume(t);
  if (pre.count >= kMinWindow && std::isfinite(v) && v >= 0.0) {
    const double spread = std::max(1.0, pre.log_spread);
    const double lv = std::log1p(v);
    const double lpre = std::log1p(pre.median);
    const double lpost = post.count >= kMinWindow ? std::log1p(post.median) : lpre;
    Term term{"volume_on_day", pre.median > 0.0 ? v / pre.median : v, {}, {}};
    const auto ll = [&](double zero_floor, double mean, double sd) {
      const double p0 = std::max(pre.zero_share, zero_floor);
      if (v == 0.0) {
        return std::log(p0);
      }
      return std::log1p(-std::min(p0, 0.999)) + log_normal_pdf(lv, mean, sd);
    };
    // Volume rises with how surprising a real move is: not at all for an ordinary
    // one (a tick on a coarse grid), fully from about six standard deviations.
    const double surprise = std::clamp((std::fabs(r) / base_sd - 2.0) / 4.0, 0.0, 1.0);
    term.ll[idx(H::kMarketMove)] = ll(0.002, lpre + kSurge * surprise, spread);
    term.ll[idx(H::kTickMove)] = ll(0.002, lpre, spread);
    term.ll[idx(H::kBadPrint)] = ll(0.05, lpre + kBadPrintVolume * surprise, spread);
    term.ll[idx(H::kBadClose)] = ll(0.05, lpre + kBadPrintVolume * surprise, spread);
    term.ll[idx(H::kUnreportedSplit)] = ll(0.002, lpost, spread);
    term.ll[idx(H::kScaleError)] = ll(0.002, lpost, spread);
    term.ll[idx(H::kHistorySegment)] = ll(0.002, lpost, 2.0 * spread);
    term.note = "against the median before";
    terms.push_back(std::move(term));
  }
  if (pre.count >= kMinWindow && post.count >= kMinWindow) {
    const double dv = std::log1p(post.median) - std::log1p(pre.median);
    out.volume_ratio = std::exp(dv);
    // A median of n values is off by about 1.25 sd / sqrt(n); add that to how much
    // each hypothesis lets the level move.
    const auto median_noise = [](const VolumeWindow& w) {
      return 1.25 * w.log_spread / std::sqrt(static_cast<double>(w.count));
    };
    const double noise = std::hypot(median_noise(pre), median_noise(post));
    const auto sd = [noise](double base) { return std::hypot(base, noise); };
    Term term{"volume_shift", std::exp(dv), {}, {}};
    // The volume level drifts by about kVolumeDrift over a few months whatever
    // happens; the hypotheses differ in where they centre it. A real move keeps
    // some dollar volume (shares rise as the price falls); a split moves share
    // volume by its ratio exactly; an error leaves it alone.
    term.ll[idx(H::kMarketMove)] = log_normal_pdf(dv, -0.5 * r, sd(kVolumeDrift));
    term.ll[idx(H::kTickMove)] = log_normal_pdf(dv, 0.0, sd(kVolumeDrift));
    term.ll[idx(H::kBadPrint)] = log_normal_pdf(dv, 0.0, sd(kVolumeDrift));
    term.ll[idx(H::kBadClose)] = log_normal_pdf(dv, 0.0, sd(kVolumeDrift));
    term.ll[idx(H::kUnreportedSplit)] = log_normal_pdf(dv, -r, sd(kSplitVolumeDrift));
    term.ll[idx(H::kScaleError)] = log_normal_pdf(dv, 0.0, sd(kVolumeDrift));
    term.ll[idx(H::kHistorySegment)] = log_normal_pdf(dv, 0.0, sd(2.0));
    term.note = "median after / before";
    terms.push_back(std::move(term));
  }

  // Plausible price levels either side.
  {
    const bool pre_ok = plausible(prev_close);
    const bool post_ok = plausible(this_close);
    if (pre_ok != post_ok) {
      Term term{"plausible_level", pre_ok ? this_close : prev_close, {}, {}};
      term.note = "outside " + format_number(settings_.min_price) + ".." +
                  format_number(settings_.max_price);
      const double unlikely = std::log(1e-3);
      for (std::size_t h = 0; h < kCount; ++h) {
        term.ll[h] = unlikely;
      }
      term.ll[idx(H::kScaleError)] = 0.0;
      if (pre_ok) {  // the new level is the implausible one
        term.ll[idx(H::kBadPrint)] = 0.0;
        term.ll[idx(H::kBadClose)] = 0.0;
      } else {  // the old level was: an era ends here
        term.ll[idx(H::kHistorySegment)] = 0.0;
      }
      terms.push_back(std::move(term));
    }
  }

  // Decimal places as written changing at the boundary (DQ106's signal).
  {
    std::size_t before_count = 0;
    std::size_t after_count = 0;
    const double before = median_decimals(t >= window ? t - window : 0, t, before_count);
    const double after_dp = median_decimals(t, t + window, after_count);
    if (before_count >= kMinWindow && after_count >= kMinWindow &&
        std::fabs(after_dp - before) >= 3.0) {
      Term term{"precision_shift", after_dp - before, {}, {}};
      term.note = "decimal places after - before";
      for (std::size_t h = 0; h < kCount; ++h) {
        term.ll[h] = std::log(0.1);
      }
      term.ll[idx(H::kScaleError)] = 0.0;
      terms.push_back(std::move(term));
    }
  }

  // A history segment: a listing price, and a new volatility regime.
  if (on[idx(H::kHistorySegment)]) {
    const bool round = std::any_of(kListingPrices.begin(), kListingPrices.end(), [&](double p) {
      return std::fabs(this_close / p - 1.0) <= 0.0025;
    });
    Term listing{"listing_price", this_close, {}, {}};
    for (std::size_t h = 0; h < kCount; ++h) {
      listing.ll[h] = round ? std::log(0.01) : std::log(0.99);
    }
    listing.ll[idx(H::kHistorySegment)] = round ? std::log(0.3) : std::log(0.7);
    terms.push_back(std::move(listing));

    const double shift = 0.5 * std::log(f_.backward[t].variance() / f_.forward[t].variance());
    Term vol{"volatility_shift", std::exp(shift), {}, {}};
    for (std::size_t h = 0; h < kCount; ++h) {
      vol.ll[h] = log_normal_pdf(shift, 0.0, 0.35);
    }
    vol.ll[idx(H::kHistorySegment)] = log_normal_pdf(shift, 0.0, 1.0);
    vol.note = "after / before";
    terms.push_back(std::move(vol));
  }

  // The posterior.
  Terms total{};
  for (std::size_t h = 0; h < kCount; ++h) {
    total[h] = on[h] ? 0.0 : kNegInf;
  }
  for (const Term& term : terms) {
    for (std::size_t h = 0; h < kCount; ++h) {
      if (on[h]) {
        total[h] += term.ll[h];
      }
    }
  }
  double norm = kNegInf;
  for (std::size_t h = 0; h < kCount; ++h) {
    norm = stats::log_add(norm, total[h]);
  }
  std::size_t best = idx(H::kBadPrint);
  for (std::size_t h = 0; h < kCount; ++h) {
    out.posterior[h] = on[h] ? std::exp(total[h] - norm) : 0.0;
    if (is_error(static_cast<H>(h))) {
      out.p_error += out.posterior[h];
      if (on[h] && (total[h] > total[best] || !on[best])) {
        best = h;
      }
    }
  }
  out.p_error = std::min(out.p_error, 1.0);
  out.hypothesis = static_cast<H>(best);
  if (out.hypothesis == H::kBadPrint) {
    out.block = static_cast<int>(best_block);
    out.end_bar = std::min(t + best_block, n_) - 1;
  }

  // Evidence for the reported hypothesis, against a market move.
  const std::size_t m = idx(H::kMarketMove);
  terms.front().value = std::exp(terms.front().ll[best]);
  if (next_count > 0 && best != idx(H::kBadPrint) && best != idx(H::kBadClose)) {
    terms[next_term].value = next_largest;
    terms[next_term].note = "the largest of the next " + std::to_string(next_count) + " moves";
  }
  for (Term& term : terms) {
    const double log_bf = term.ll[best] - term.ll[m];
    if (!std::isfinite(log_bf) || std::fabs(log_bf) < 0.05) {
      continue;
    }
    out.evidence.push_back(
        {std::move(term.feature), std::move(term.value), log_bf, std::move(term.note)});
  }
  return out;
}

// Two scale errors in opposite directions bound an era stored at the wrong scale:
// report it once.
void pair_scale_eras(std::vector<PriceFinding>& findings, const PriceSettings& settings) {
  std::vector<PriceFinding> out;
  for (std::size_t i = 0; i < findings.size(); ++i) {
    PriceFinding& a = findings[i];
    if (a.hypothesis == H::kScaleError && i + 1 < findings.size()) {
      PriceFinding& b = findings[i + 1];
      const double tolerance = std::max(0.1, 3.0 * settings.ratio_tolerance);
      if (b.hypothesis == H::kScaleError && a.p_error >= 0.5 && b.p_error >= 0.5 &&
          std::fabs(std::log(a.factor) + std::log(b.factor)) < tolerance) {
        a.end_bar = b.bar - 1;
        a.p_error = std::max(a.p_error, b.p_error);
        a.provisional = false;
        out.push_back(std::move(a));
        ++i;
        continue;
      }
    }
    out.push_back(std::move(a));
  }
  findings = std::move(out);
}

}  // namespace

std::string_view to_string(PriceHypothesis hypothesis) noexcept {
  switch (hypothesis) {
    case H::kMarketMove:
      return "market_move";
    case H::kTickMove:
      return "tick_move";
    case H::kBadPrint:
      return "bad_print";
    case H::kBadClose:
      return "bad_close";
    case H::kUnreportedSplit:
      return "unreported_split";
    case H::kScaleError:
      return "scale_error";
    case H::kHistorySegment:
      return "history_segment";
  }
  return "market_move";
}

PriceAnalysis analyze_prices(const Series& series, const Calendar& calendar,
                             const PriceSettings& settings) {
  PriceAnalysis out;
  out.features = compute_price_features(series, calendar, settings);
  const std::size_t n = out.features.size();
  if (n < 2) {
    return out;
  }
  out.applicable = true;
  const Scorer scorer(series, out.features, settings);
  std::size_t skip_through = 0;  // returns explained by an earlier bad print
  for (std::size_t t = 1; t < n; ++t) {
    if (t <= skip_through || !scorer.screened(t)) {
      continue;
    }
    PriceFinding finding = scorer.score(t);
    const double bad_print = finding.posterior[idx(H::kBadPrint)];
    const double bad_close = finding.posterior[idx(H::kBadClose)];
    if (bad_print >= 0.5 && finding.hypothesis == H::kBadPrint) {
      skip_through = t + static_cast<std::size_t>(finding.block);
    } else if (bad_close >= 0.5) {
      skip_through = t + 1;
    }
    out.findings.push_back(std::move(finding));
  }
  pair_scale_eras(out.findings, settings);
  return out;
}

}  // namespace dorq
