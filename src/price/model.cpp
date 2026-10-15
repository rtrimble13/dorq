#include "price/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Log ratio between two unrelated securities' prices, for a history segment.
constexpr double kSegmentSpread = 1.2;
// Fewest bars a window needs to say anything about a volume or precision level.
constexpr std::size_t kMinWindow = 5;
// P(close outside [low, high]) when the bar is genuine.
constexpr double kRangeNoise = 1e-3;
// The weight a scale error puts on ratios that are no power of ten; the
// plausible_level term carries the case for those (AKR's 149,613,176).
constexpr double kScaleBroad = 0.001;
// The log ratio of two windows' typical move sizes, when the units do not change:
// a robust spread, wide enough for a change of volatility regime.
constexpr double kNoiseSpread = 0.7;
double noise_ll(double log_ratio) noexcept {
  return stats::student_t_log_pdf(log_ratio, 4.0, kNoiseSpread);
}
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
// News can draw ten to ten thousand times the usual volume (a squeeze), which then
// decays over days: on the day, about e^4 times the usual give or take e^2.5, and
// the median after about e^2.5 times give or take e^2. Up to this share of real
// moves do, rising with how surprising the move is; kept small, so that a real
// move on ordinary volume loses little by it.
constexpr double kSurgeShare = 0.25;
constexpr double kSurgeDay = 4.0;
constexpr double kSurgeDaySpread = 2.5;
constexpr double kSurgeAfter = 2.5;
constexpr double kSurgeAfterSpread = 2.0;
// The volume level after a move, against before, that counts as a surge (DQ601's
// market-like members): five times, and not a split's inverse shift.
constexpr double kSurgeShift = 1.6094379124341003;  // log 5
// A violent session (a range violent_range times the usual): P(one | hypothesis).
// A real move often trades through its range, more often the more surprising it
// is; a value at the wrong scale is the old bar times a constant, so its range is
// the usual one; a reverse split often has a wild first session.
constexpr double kViolentOrdinary = 0.02;
constexpr double kViolentSurprise = 0.28;
constexpr double kViolentScale = 0.01;
constexpr double kViolentBadPrint = 0.05;
constexpr double kViolentSplit = 0.3;
constexpr double kViolentSegment = 0.1;
// A volume base under this share of [coverage] trade_size is none: a round lot.
constexpr double kRoundLot = 0.1;
// A distribution not yet on file (pending_distribution): a drop of 2% to 35%.
constexpr double kDistributionMin = 0.02;
constexpr double kDistributionMax = 0.35;
// A print at the bid or the ask: about 0.7 of the grid step, on the price scale.
constexpr double kBounce = 0.7;
// With --market, how far a series' beta on the day may be from the estimate: the
// residual of a market move m carries an error of about this times m. On a crash
// day that widens every series' ordinary move (DQ602).
constexpr double kBetaError = 0.5;

double log_normal_pdf(double x, double mean, double sd) noexcept {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}

double log_cauchy(double x, double scale) noexcept {
  return stats::student_t_log_pdf(x, 1.0, scale);
}

// log density at x of a level anywhere between a and b, uniformly, plus a Normal
// error with standard deviation sd: a flat top with Normal shoulders.
double log_flat_top(double x, double a, double b, double sd) noexcept {
  const double lo = std::min(a, b);
  const double hi = std::max(a, b);
  if (hi - lo < 1e-6 * sd) {
    return log_normal_pdf(x, lo, sd);
  }
  // The density is symmetric about the middle: take x at or below it, where both
  // terms are lower tails and lose no precision.
  const double y = std::min(x, lo + hi - x);
  const double z = (y - lo) / sd;
  const double mass = stats::normal_cdf(z) - stats::normal_cdf((y - hi) / sd);
  if (mass > 0.0) {
    return std::log(mass / (hi - lo));
  }
  // Far below: the lower tail at z is about the Normal density over |z|.
  return log_normal_pdf(y, lo, sd) + std::log(sd / (std::fabs(z) * (hi - lo)));
}

double split_weight(const SplitRatio& ratio) noexcept {
  struct Known {
    SplitRatio ratio;
    double weight = 0.0;
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

// A term's log-likelihood under one hypothesis.
double& at(Term& term, H hypothesis) { return term.ll.at(idx(hypothesis)); }

// A term with the same log-likelihood under every hypothesis, to be overridden for
// the ones it tells apart.
Term uniform_term(std::string feature, DetailValue value, double ll) {
  Term term{std::move(feature), std::move(value), {}, {}};
  term.ll.fill(ll);
  return term;
}

// What scoring one candidate needs to know about it.
struct Candidate {
  std::size_t t = 0;    // the bar
  std::size_t row = 0;  // its row in the series
  double r = 0.0;       // its return
  int elapsed = 1;      // sessions since the bar before
  std::size_t max_block = 1;
  double sig = 0.0;      // per-session volatility, the bar and those after it left out
  double base_sd = 0.0;  // sd of an ordinary move from the bar before
  double prev_close = 0.0;
  double this_close = 0.0;
  double grid = 0.0;                   // the price grid
  bool bar_ohl = false;                // open, high and low are usable
  std::optional<SplitAction> on_file;  // a split on file between the bar and the one before
  // Cash dividends on file between the bar and the one before, when they move the
  // price more than an ordinary move does.
  std::optional<DividendOnFile> dividend;
  double net_r = 0.0;  // the return with those dividends added back
  // The bar's log range, log(high / low), and its ratio to the median of the bars
  // before; unset when too few of those have a range (a NAV fund, a one-print
  // series).
  std::optional<double> log_range;
  std::optional<double> range_vs_typical;
};

// What the volume terms saw, for the DQ203 gate and DQ601's market-like moves.
struct VolumeShift {
  bool judged = false;           // enough bars before with volume
  std::optional<double> on_day;  // log(1 + volume) - log(1 + median before)
  std::optional<double> after;   // log(1 + median after) - log(1 + median before)
  double noise = 0.0;            // the median's own noise in `after`
  double day_spread = 1.0;       // the spread of one day's log volume
};

// The bars after a candidate: the next_bars term, and what describes it.
struct NextBars {
  Term term;
  std::size_t best_block = 1;  // a bad print's most probable length
  std::size_t count = 0;       // returns seen after the bar
  double largest = 1.0;        // the largest of them, as a price factor
};

struct VolumeWindow {
  std::size_t count = 0;
  double median = 0.0;
  double zero_share = 0.0;
  double log_spread = 0.0;  // robust sd of log(1 + volume)
};

class Scorer {
 public:
  Scorer(const Series& series, const PriceFeatures& f, const PriceSettings& settings,
         const PriceContext& context)
      : series_(series),
        f_(f),
        settings_(settings),
        n_(f.size()),
        has_ohlc_(series.kind == SeriesKind::kOhlcv),
        bounds_(context.bounds),
        tick_(context.tick_size),
        actions_(context.actions),
        nav_priced_(context.nav_priced),
        trade_size_(context.trade_size) {
    const double nu = settings.tail_dof;
    t_scale_ = nu > 2.0 ? std::sqrt((nu - 2.0) / nu) : 1.0;
    double total = 0.0;
    for (const SplitRatio& ratio : settings.split_ratios) {
      total += split_weight(ratio);
    }
    for (const SplitRatio& ratio : settings.split_ratios) {
      splits_.emplace_back(ratio, split_weight(ratio) / total);
    }
    std::vector<std::uint8_t> decimals;
    decimals.reserve(n_);
    for (std::size_t i = 0; i < n_; ++i) {
      decimals.push_back(series.close_decimals[f.row[i]]);
    }
    written_grid_ = written_grid(std::move(decimals));
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
    if (tick_) {
      return std::max(*tick_, written_grid_);
    }
    if (!has_ohlc_) {  // the exchange's tick is a fact about equities
      return written_grid_;
    }
    return std::max(tick_size(date(b), std::min(close(a), close(b))), written_grid_);
  }

  // The standard deviation of an ordinary move from bar a to bar b: volatility
  // over the sessions between them, plus the bid-ask bounce of a print landing
  // on either side of the spread, which dominates on a coarse grid.
  [[nodiscard]] double move_sd(double sig, std::size_t a, std::size_t b) const {
    int sessions = 0;
    double market = 0.0;  // the variance of the market term a beta error leaves
    for (std::size_t i = a + 1; i <= b; ++i) {
      sessions += f_.elapsed[i];
      if (!f_.market.empty()) {
        market += std::pow(kBetaError * f_.market[i], 2.0);
      }
    }
    // On the log scale a grid step is a share of the price; on the value scale it
    // is the step itself.
    const double bounce = f_.log_scale ? kBounce * price_grid(a, b) / std::min(close(a), close(b))
                                       : kBounce * price_grid(a, b);
    return std::sqrt(sig * sig * static_cast<double>(sessions) + bounce * bounce + market);
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

  // The typical size of a move over bars [from, to), on a point series' value
  // scale: the median absolute change, else the mean, else half the grid (a run
  // of unchanged values); 0 when fewer than kMinSpread moves.
  [[nodiscard]] double local_spread(std::size_t from, std::size_t to) const {
    constexpr std::size_t kMinSpread = 3;
    const std::size_t first = std::max<std::size_t>(from, 1);
    const std::size_t last = std::min(to, n_);
    if (last < first + kMinSpread) {
      return 0.0;
    }
    std::vector<double> moves(last - first);
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
      moves[i - first] = std::fabs(f_.ret[i]);
      sum += moves[i - first];
    }
    const auto count = static_cast<double>(moves.size());
    double spread = stats::median(std::move(moves));
    if (spread <= 0.0) {
      spread = sum / count;
    }
    return spread > 0.0 ? spread : 0.5 * price_grid(first, last - 1);
  }

  // A price no security trades at, or a point series' value outside its bounds.
  [[nodiscard]] bool plausible(double price) const {
    if (!has_ohlc_) {
      return !bounds_ || (price >= bounds_->low && price <= bounds_->high);
    }
    return price >= settings_.min_price && price <= settings_.max_price;
  }

  // A split on file whose ex-date falls after bar t-1 and on or before bar t: the
  // split the move from one to the other spans.
  [[nodiscard]] std::optional<SplitAction> split_between(std::size_t t) const {
    if (actions_ == nullptr || t == 0 || t >= n_) {
      return std::nullopt;
    }
    const Date before = date(t - 1);
    const Date at = date(t);
    for (const SplitAction& split : actions_->splits) {
      if (split.ex_date > before && split.ex_date <= at) {
        return split;
      }
    }
    return std::nullopt;
  }

  // The cash dividends on file whose ex-dates fall after bar t-1 and on or before
  // bar t, summed: what came off the price between the two. An amount DQ705
  // reports at error (zero or less, or at or above the close before) is left out;
  // nullopt when none is left, or they add up to the whole price.
  [[nodiscard]] std::optional<DividendOnFile> dividend_between(std::size_t t) const {
    if (actions_ == nullptr || t == 0 || t >= n_) {
      return std::nullopt;
    }
    const Date before = date(t - 1);
    const Date at = date(t);
    const double prev = close(t - 1);
    DividendOnFile on_file;
    for (const DividendAction& dividend : actions_->dividends) {
      if (dividend.ex_date > before && dividend.ex_date <= at && dividend.amount > 0.0 &&
          dividend.amount < prev) {
        on_file.amount += dividend.amount;
        on_file.ex_date = dividend.ex_date;  // the list is sorted by ex-date
        ++on_file.count;
      }
    }
    if (on_file.count == 0 || on_file.amount >= prev) {
      return std::nullopt;
    }
    return on_file;
  }

  [[nodiscard]] PriceFinding score(std::size_t t) const;
  [[nodiscard]] std::vector<SplitFinding> split_findings(
      const std::vector<PriceFinding>& findings) const;
  [[nodiscard]] bool era_before(const SplitFinding& finding) const;
  [[nodiscard]] std::vector<StaleRun> stale_runs() const;
  [[nodiscard]] std::vector<double> move_z() const {
    std::vector<double> z(n_, 0.0);
    for (std::size_t t = 1; t < n_; ++t) {
      z[t] = f_.ret[t] / move_sd(sigma(t, 0), t - 1, t);
    }
    return z;
  }

  [[nodiscard]] bool screened(std::size_t t) const {
    const double r = f_.ret[t];
    const int elapsed = f_.elapsed[t];
    // The 50% floor is a rule about prices; a rate's 0.05 to 0.08 is not news.
    const bool floor = f_.log_scale && std::fabs(r) > std::log1p(settings_.floor_move);
    if (floor || elapsed >= settings_.segment_gap || (has_ohlc_ && split_between(t))) {
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
  // DQ701-DQ704 (split_findings).
  struct Elsewhere {
    double log_mean = kNegInf;
    std::size_t best = 0;
    double best_gain = kNegInf;
  };
  [[nodiscard]] int sessions_from(std::size_t from, std::size_t to) const;
  [[nodiscard]] double jump_gain(std::size_t j, double ratio) const {
    return jump_gain(j, ratio, f_.ret[j]);
  }
  [[nodiscard]] double jump_gain(std::size_t j, double ratio, double ret) const;
  // `skip`: bars whose move cannot be this split's (a bad print's edges, another
  // split on file at the same ratio).
  [[nodiscard]] Elsewhere jump_elsewhere(std::size_t ex, double rho, int window,
                                         const std::vector<bool>& skip) const;
  [[nodiscard]] double mismatch_gain(std::size_t ex, const SplitAction& split,
                                     std::string& observed) const;

  [[nodiscard]] Candidate candidate(std::size_t t) const;
  void applicable(const Candidate& c, std::array<bool, kCount>& on) const;
  [[nodiscard]] std::optional<Term> prior_term(const Candidate& c,
                                               std::array<bool, kCount>& on) const;
  // How surprising a move of r is to a candidate: 0 for an ordinary one (2 sd or
  // less), 1 from about six.
  [[nodiscard]] static double surprise(const Candidate& c, double r) {
    return std::clamp((std::fabs(r) / c.base_sd - 2.0) / 4.0, 0.0, 1.0);
  }
  [[nodiscard]] std::optional<Term> range_term(const Candidate& c) const;
  [[nodiscard]] bool split_volume_ok(const PriceFinding& out, const VolumeShift& shift) const;
  [[nodiscard]] Term return_term(const Candidate& c, PriceFinding& out) const;
  [[nodiscard]] double split_ll(const Candidate& c, PriceFinding& out) const;
  [[nodiscard]] double scale_ll(const Candidate& c, PriceFinding& out) const;
  [[nodiscard]] NextBars next_bars(const Candidate& c) const;
  void ohlc_terms(const Candidate& c, std::vector<Term>& terms) const;
  void volume_terms(const Candidate& c, PriceFinding& out, std::vector<Term>& terms,
                    VolumeShift& shift) const;
  [[nodiscard]] std::optional<Term> plausibility_term(const Candidate& c) const;
  [[nodiscard]] std::optional<Term> precision_term(const Candidate& c) const;
  void segment_terms(const Candidate& c, std::vector<Term>& terms) const;

  const Series& series_;
  const PriceFeatures& f_;
  const PriceSettings& settings_;
  std::size_t n_;
  bool has_ohlc_ = false;
  std::optional<Bounds> bounds_;            // a point series' plausible range
  std::optional<double> tick_;              // the exchange tick from --meta
  const SeriesActions* actions_ = nullptr;  // --actions
  bool nav_priced_ = false;
  double trade_size_ = 0.0;
  double t_scale_ = 1.0;
  double written_grid_ = 0.0;  // 10^-d for the decimals most prices are written with
  std::vector<std::pair<SplitRatio, double>> splits_;
};

Candidate Scorer::candidate(std::size_t t) const {
  Candidate c;
  c.t = t;
  c.r = f_.ret[t];
  c.elapsed = f_.elapsed[t];
  c.max_block = static_cast<std::size_t>(settings_.revert_max_bars);
  c.sig = sigma(t, c.max_block);
  c.base_sd = move_sd(c.sig, t - 1, t);
  c.prev_close = close(t - 1);
  c.this_close = close(t);
  c.grid = price_grid(t - 1, t);
  c.row = f_.row[t];
  c.on_file = has_ohlc_ ? split_between(t) : std::nullopt;
  // A dividend comes off the price at its ex-date. One smaller than an ordinary
  // move explains nothing a real move does not, and would only dilute the errors'
  // share of the prior on every ex-date.
  c.dividend = has_ohlc_ ? dividend_between(t) : std::nullopt;
  const double drop = c.dividend ? -std::log1p(-c.dividend->amount / c.prev_close) : 0.0;
  if (drop <= c.base_sd) {
    c.dividend.reset();
  }
  c.net_r = c.dividend ? c.r + drop : c.r;
  const std::size_t row = c.row;
  c.bar_ohl = has_ohlc_ && std::isfinite(series_.open[row]) && std::isfinite(series_.high[row]) &&
              std::isfinite(series_.low[row]) && series_.open[row] > 0.0 &&
              series_.high[row] > 0.0 && series_.low[row] > 0.0;
  // The bar's range against the bars before: on a series where most bars have a
  // range, a violent session (a squeeze, a crash) shows here, and a value at the
  // wrong scale does not.
  if (c.bar_ohl && series_.high[row] >= series_.low[row]) {
    const auto window = static_cast<std::size_t>(settings_.volume_window);
    std::vector<double> ranges;
    std::size_t usable = 0;
    for (std::size_t i = t >= window ? t - window : 0; i < t; ++i) {
      const std::size_t r = f_.row[i];
      const double hi = series_.high[r];
      const double lo = series_.low[r];
      if (!std::isfinite(hi) || !std::isfinite(lo) || lo <= 0.0 || hi < lo) {
        continue;
      }
      ++usable;
      if (hi > lo) {
        ranges.push_back(std::log(hi / lo));
      }
    }
    if (ranges.size() >= kMinWindow && 2 * ranges.size() >= usable) {
      c.log_range = std::log(series_.high[row] / series_.low[row]);
      c.range_vs_typical = *c.log_range / stats::median(std::move(ranges));
    }
  }
  return c;
}

// Which hypotheses apply to a candidate.
void Scorer::applicable(const Candidate& c, std::array<bool, kCount>& on) const {
  on.fill(true);
  const double ticks = std::fabs(c.this_close - c.prev_close) / c.grid;
  on.at(idx(H::kTickMove)) = ticks <= 2.5;
  on.at(idx(H::kBadClose)) = c.bar_ohl;
  on.at(idx(H::kUnreportedSplit)) = has_ohlc_ && !splits_.empty();
  on.at(idx(H::kHistorySegment)) = c.elapsed >= settings_.segment_gap;
  on.at(idx(H::kExplainedSplit)) = c.on_file.has_value();
  on.at(idx(H::kExplainedDividend)) = c.dividend.has_value();
  // A NAV-priced fund falling 2% to 35% with no dividend on file to explain it.
  on.at(idx(H::kPendingDistribution)) = has_ohlc_ && nav_priced_ && !c.dividend &&
                                        c.r <= std::log1p(-kDistributionMin) &&
                                        c.r >= std::log1p(-kDistributionMax);
}

// The prior, normalized over the hypotheses that apply; nullopt when every one of
// them has weight zero.
std::optional<Term> Scorer::prior_term(const Candidate& c, std::array<bool, kCount>& on) const {
  const PricePriors& p = settings_.priors;
  // Funds pay their capital gains in the last months of the year.
  const double pending = p.pending_distribution * (date(c.t).month() >= 9 ? 2.0 : 1.0);
  const Terms weights = {p.market_move,        p.tick_move,   p.bad_print,       p.bad_close,
                         p.unreported_split,   p.scale_error, p.history_segment, p.explained_split,
                         p.explained_dividend, pending};
  double total = 0.0;
  for (std::size_t h = 0; h < kCount; ++h) {
    total += on.at(h) ? weights.at(h) : 0.0;
  }
  if (total <= 0.0) {
    return std::nullopt;
  }
  Term term{"prior", 0.0, {}, {}};
  for (std::size_t h = 0; h < kCount; ++h) {
    on.at(h) = on.at(h) && weights.at(h) > 0.0;
    term.ll.at(h) = on.at(h) ? std::log(weights.at(h) / total) : kNegInf;
  }
  return term;
}

// A split shifts the level by its ratio exactly; the rest of the move is an
// ordinary return. Sets the nearest ratio.
double Scorer::split_ll(const Candidate& c, PriceFinding& out) const {
  const double sd = std::hypot(c.base_sd, settings_.ratio_tolerance);
  const double level = split_level_weights(c.prev_close);
  double total = kNegInf;
  double best = kNegInf;
  for (const auto& [ratio, weight] : splits_) {
    const bool forward = ratio.shares_after > ratio.shares_before;
    const double ll = std::log(weight * (forward ? level : 1.0 - level)) +
                      ordinary(c.r - std::log(ratio.price_factor()), sd);
    total = stats::log_add(total, ll);
    if (ll > best) {
      best = ll;
      out.split = ratio;
    }
  }
  return total;
}

// A scale error shifts the level by a power of ten. Sets the nearest power. On the
// log scale the shift adds; on the value scale it multiplies: a value stored k
// times too large has density f(v / k - previous) / k, and the first value back
// at the right scale after such an era has f(v - previous / k).
double Scorer::scale_ll(const Candidate& c, PriceFinding& out) const {
  const double slack = settings_.ratio_tolerance * (f_.log_scale ? 1.0 : f_.level);
  const double sd = std::hypot(c.base_sd, slack);
  double total = std::log(kScaleBroad) + log_cauchy(c.r, f_.error_scale);
  double best = kNegInf;
  // On the value scale a change of units changes the size of the moves too: the
  // log ratio of the moves' size before the bar to after it. A rate that falls by
  // a quarter point to zero lands near a hundredth of where it was, but its moves
  // are as large as before.
  std::optional<double> noise;
  if (!f_.log_scale) {
    constexpr std::size_t kNoiseBars = 10;
    const double before = local_spread(c.t > kNoiseBars ? c.t - kNoiseBars : 1, c.t);
    const double after = local_spread(c.t + 1, c.t + 1 + kNoiseBars);
    if (before > 0.0 && after > 0.0) {
      noise = std::log(before / after);
    }
  }
  for (std::size_t k = 0; k < kPowerWeights.size(); ++k) {
    for (const int sign : {-1, 1}) {
      const double power = sign * static_cast<double>(k + 1);
      const double weight = std::log((1.0 - kScaleBroad) * kPowerWeights.at(k) / 2.0);
      double ll = weight + ordinary(c.r - power * std::numbers::ln10, sd);
      if (!f_.log_scale && std::fabs(power) < 2.0) {
        continue;  // a point series' units shift by 100, 1000, 10^4 or 10^6, not 10
      }
      if (!f_.log_scale) {
        const double factor = std::pow(10.0, power);
        double starts = ordinary(c.this_close / factor - c.prev_close, sd) - std::log(factor);
        double ends = ordinary(c.this_close - c.prev_close / factor, sd);
        if (noise) {
          // Values stored `factor` times too large move `factor` times as much.
          const double shift = power * std::numbers::ln10;
          starts += noise_ll(*noise + shift) - noise_ll(*noise);
          ends += noise_ll(*noise - shift) - noise_ll(*noise);
        }
        ll = weight + std::log(0.5) + stats::log_add(starts, ends);
      }
      total = stats::log_add(total, ll);
      if (ll > best) {
        best = ll;
        out.power_of_ten = static_cast<int>(power);
      }
    }
  }
  return total;
}

// The move itself.
Term Scorer::return_term(const Candidate& c, PriceFinding& out) const {
  Term term{"return", out.factor, {}, {}};
  at(term, H::kMarketMove) = ordinary(c.r, c.base_sd);
  // A tick's density: uniform over two steps either way, in r's units.
  at(term, H::kTickMove) = (f_.log_scale ? std::log(c.this_close) : 0.0) - std::log(4.0 * c.grid);
  at(term, H::kBadPrint) = log_cauchy(c.r, f_.error_scale);
  at(term, H::kBadClose) = log_cauchy(c.r, f_.error_scale);
  at(term, H::kUnreportedSplit) = split_ll(c, out);
  at(term, H::kScaleError) = scale_ll(c, out);
  const double spread = kSegmentSpread * (f_.log_scale ? 1.0 : f_.level);
  at(term, H::kHistorySegment) = log_normal_pdf(c.r, 0.0, std::hypot(spread, c.base_sd));
  // The split on file: its ratio exactly, and an ordinary move.
  at(term, H::kExplainedSplit) = c.on_file
                                     ? ordinary(c.r - std::log(c.on_file->price_factor()),
                                                std::hypot(c.base_sd, settings_.ratio_tolerance))
                                     : kNegInf;
  // The dividend on file: the price falls by it, and the rest is an ordinary move.
  // When the price did not fall, the move left is the dividend itself, and a wrong
  // row on file invents no move.
  at(term, H::kExplainedDividend) = c.dividend ? ordinary(c.net_r, c.base_sd) : kNegInf;
  // A distribution of unknown size, anywhere from 2% to 35% of the price, and an
  // ordinary move.
  at(term, H::kPendingDistribution) =
      log_flat_top(c.r, std::log1p(-kDistributionMax), std::log1p(-kDistributionMin), c.base_sd);
  if (out.split) {
    term.note = "nearest split " + out.split->to_string();
  }
  return term;
}

// The bars after: a bad print's block of k bars ends with a return that undoes
// it, so the move from before the block to after it is ordinary. A real move
// stirs volatility up (half the time, by kAftershock of its size); a split, a
// scale error or a bad print leaves the bars after it calm.
NextBars Scorer::next_bars(const Candidate& c) const {
  const std::size_t t = c.t;
  NextBars out;
  out.term = Term{"next_bars", false, {}, {}};
  std::vector<double> plain;  // log density of each later return, as ordinary
  double all_plain = 0.0;
  double all_stirred = 0.0;      // and as ordinary after a real move
  double all_stirred_net = 0.0;  // after a real move the size of what a dividend leaves
  double largest = 0.0;
  for (std::size_t j = 1; j <= c.max_block && t + j < n_; ++j) {
    const double sd = move_sd(c.sig, t + j - 1, t + j);
    plain.push_back(ordinary(f_.ret[t + j], sd));
    all_plain += plain.back();
    all_stirred += ordinary(f_.ret[t + j], std::hypot(sd, kAftershock * std::fabs(c.r)));
    all_stirred_net += ordinary(f_.ret[t + j], std::hypot(sd, kAftershock * std::fabs(c.net_r)));
    largest = std::fabs(f_.ret[t + j]) > std::fabs(largest) ? f_.ret[t + j] : largest;
  }
  double norm = 0.0;
  for (std::size_t k = 1; k <= c.max_block; ++k) {
    norm += std::pow(kBlockDecay, static_cast<double>(k));
  }
  double mixture = kNegInf;
  double best = kNegInf;
  double first = all_plain;
  for (std::size_t k = 1; k <= c.max_block; ++k) {
    const double weight = std::log(std::pow(kBlockDecay, static_cast<double>(k)) / norm);
    double ll = all_plain;  // not yet reverted within the data: nothing to score
    if (k <= plain.size()) {
      // The block's own moves cancel: from before it to after it is ordinary.
      const double across = f_.y[t + k] - f_.y[t - 1] - (f_.y[t + k - 1] - f_.y[t]);
      ll = all_plain - plain[k - 1] + ordinary(across, move_sd(c.sig, t - 1, t + k));
    }
    first = k == 1 ? ll : first;
    mixture = stats::log_add(mixture, weight + ll);
    if (weight + ll > best) {
      best = weight + ll;
      out.best_block = k;
    }
  }
  out.term.ll.fill(all_plain);
  const auto real_move = [all_plain](double stirred) {
    return stats::log_add(std::log(0.5) + stirred, std::log(0.5) + all_plain);
  };
  at(out.term, H::kMarketMove) = real_move(all_stirred);
  // A split on file is a real event, often followed by a real move's volatility
  // (a reverse split by a selloff); a distribution is a real move of what is left
  // once the dividend is added back.
  at(out.term, H::kExplainedSplit) = at(out.term, H::kMarketMove);
  at(out.term, H::kExplainedDividend) = real_move(all_stirred_net);
  at(out.term, H::kBadPrint) = mixture;
  at(out.term, H::kBadClose) = first;
  out.count = plain.size();
  out.largest = std::exp(largest);
  if (plain.empty()) {
    out.term.note = "none yet";
  } else if (t + out.best_block < n_) {
    out.term.value = std::exp(f_.ret[t + out.best_block]);
    out.term.note = "the move " + std::to_string(out.best_block) + " bar" +
                    (out.best_block == 1 ? "" : "s") + " later";
  }
  return out;
}

// The bar's own open, high and low.
void Scorer::ohlc_terms(const Candidate& c, std::vector<Term>& terms) const {
  const double o = series_.open[c.row];
  const double hi = series_.high[c.row];
  const double lo = series_.low[c.row];
  // The close outside a range that holds the open blames the close; with the open
  // outside too, the high or low is the bad field (DQ101 reports it).
  const bool outside = lo <= o && o <= hi && close_outside_range(c.t);
  const auto p_out = [outside](double p) { return outside ? std::log(p) : std::log1p(-p); };
  Term range = uniform_term("close_in_range", !outside, p_out(kRangeNoise));
  at(range, H::kBadPrint) = p_out(0.05);
  at(range, H::kBadClose) = p_out(0.9);
  terms.push_back(std::move(range));

  const double mid = std::max(std::min(o, hi), std::min(std::max(o, hi), lo));
  // The open, high and low against the close before, net of the market's move.
  const double market_part = f_.market.empty() ? 0.0 : f_.beta[c.t] * f_.market[c.t];
  const double r_ohl = std::log(mid) - std::log(c.prev_close) - market_part;
  const double sd = 1.5 * c.base_sd;
  Term level = uniform_term("open_high_low", std::exp(r_ohl), log_normal_pdf(r_ohl, c.r, sd));
  // A real move's bar opens at the old level and trades to the new, or gaps.
  const double market =
      stats::log_add(std::log(0.5) + log_normal_pdf(r_ohl, c.r, sd),
                     std::log(0.5) + log_normal_pdf(r_ohl, c.r / 2.0, std::fabs(c.r) / 2.0 + sd));
  at(level, H::kMarketMove) = market;
  at(level, H::kTickMove) = market;
  at(level, H::kBadClose) = log_normal_pdf(r_ohl, 0.0, sd);
  terms.push_back(std::move(level));
}

// Volume: on the day, and the level before and after.
void Scorer::volume_terms(const Candidate& c, PriceFinding& out, std::vector<Term>& terms,
                          VolumeShift& shift) const {
  const std::size_t t = c.t;
  const auto window = static_cast<std::size_t>(settings_.volume_window);
  const VolumeWindow pre = volume_window(t >= window ? t - window : 0, t);
  const VolumeWindow post = volume_window(t + 1, t + 1 + window);
  if (pre.count < kMinWindow) {
    return;
  }
  // No volume to measure against: most bars before have none (a vendor that had
  // none, a fund struck at NAV), or the median is under a round lot, a tenth of
  // [coverage] trade_size. A ratio to it (×51,300 "after") is meaningless, so the
  // volume says nothing here. A thin series that prints a few hundred shares a
  // day still has a level to measure against.
  if (pre.zero_share > 0.5 || pre.median < kRoundLot * trade_size_) {
    out.base_volume_zero = true;
    return;
  }
  shift.judged = true;
  const double spread = std::max(1.0, pre.log_spread);
  const double lpre = std::log1p(pre.median);
  const bool have_post = post.count >= kMinWindow;
  const double lpost = have_post ? std::log1p(post.median) : lpre;
  // The share of real moves this surprising that draw a surge of trading. Not
  // after a long gap, where a new history's volume is its own level.
  const double surge = c.elapsed < settings_.segment_gap ? kSurgeShare * surprise(c, c.r) : 0.0;
  const double v = volume(t);
  if (std::isfinite(v) && v >= 0.0) {
    const double lv = std::log1p(v);
    shift.on_day = lv - lpre;
    Term term{"volume_on_day", pre.median > 0.0 ? v / pre.median : v, {}, {}};
    const auto p0 = [&pre](double zero_floor) {
      return std::min(std::max(pre.zero_share, zero_floor), 0.999);
    };
    const auto ll = [&](double zero_floor, double mean, double sd) {
      if (v == 0.0) {
        return std::log(p0(zero_floor));
      }
      return std::log1p(-p0(zero_floor)) + log_normal_pdf(lv, mean, sd);
    };
    // Anywhere from one level to another (a split on file: see volume_shift).
    const auto ll_between = [&](double zero_floor, double a, double b, double sd) {
      if (v == 0.0) {
        return std::log(p0(zero_floor));
      }
      return std::log1p(-p0(zero_floor)) + log_flat_top(lv, a, b, sd);
    };
    // Volume rises with how surprising a real move is: not at all for an ordinary
    // one (a tick on a coarse grid), by about e from six standard deviations. News
    // can draw far more, ten to ten thousand times the usual.
    const double market = ll(0.002, lpre + kSurge * surprise(c, c.r), spread);
    at(term, H::kMarketMove) =
        surge > 0.0 ? stats::log_add(std::log1p(-surge) + market,
                                     std::log(surge) + ll(0.002, lpre + kSurgeDay, kSurgeDaySpread))
                    : market;
    at(term, H::kTickMove) = ll(0.002, lpre, spread);
    at(term, H::kBadPrint) = ll(0.05, lpre + kBadPrintVolume * surprise(c, c.r), spread);
    at(term, H::kBadClose) = ll(0.05, lpre + kBadPrintVolume * surprise(c, c.r), spread);
    // A split's day trades at the new level: the one after, or, with too few bars
    // after to measure it, the level before shifted inversely by the ratio.
    const double split_level =
        have_post || !out.split ? lpost : lpre - std::log(out.split->price_factor());
    at(term, H::kUnreportedSplit) = ll(0.002, split_level, spread);
    // A value at the wrong scale leaves the volume as it was.
    at(term, H::kScaleError) = ll(0.002, lpre, spread);
    at(term, H::kHistorySegment) = ll(0.002, lpost, 2.0 * spread);
    at(term, H::kExplainedSplit) =
        have_post || !c.on_file
            ? ll(0.002, lpost, spread)
            : ll_between(0.002, lpre, lpre - std::log(c.on_file->price_factor()), spread);
    // A distribution moves no volume: only what is left of the move can.
    at(term, H::kExplainedDividend) = ll(0.002, lpre + kSurge * surprise(c, c.net_r), spread);
    at(term, H::kPendingDistribution) = ll(0.002, lpre, spread);
    term.note = "against the median before";
    terms.push_back(std::move(term));
  }
  if (!have_post) {
    return;
  }
  const double dv = std::log1p(post.median) - std::log1p(pre.median);
  out.volume_ratio = std::exp(dv);
  shift.after = dv;
  // A median of n values is off by about 1.25 sd / sqrt(n); add that to how much
  // each hypothesis lets the level move.
  const auto median_noise = [](const VolumeWindow& w) {
    return 1.25 * w.log_spread / std::sqrt(static_cast<double>(w.count));
  };
  const double noise = std::hypot(median_noise(pre), median_noise(post));
  shift.noise = noise;
  // The volume level drifts by about kVolumeDrift over a few months whatever
  // happens; the hypotheses differ in where they centre it. A real move keeps
  // some dollar volume (shares rise as the price falls), or draws a surge of
  // trading that stays up for days; a split moves share volume by its ratio
  // exactly; an error leaves it alone.
  Term term = uniform_term("volume_shift", std::exp(dv),
                           log_normal_pdf(dv, 0.0, std::hypot(kVolumeDrift, noise)));
  const double market = log_normal_pdf(dv, -0.5 * c.r, std::hypot(kVolumeDrift, noise));
  at(term, H::kMarketMove) =
      surge > 0.0
          ? stats::log_add(std::log1p(-surge) + market,
                           std::log(surge) + log_normal_pdf(dv, kSurgeAfter,
                                                            std::hypot(kSurgeAfterSpread, noise)))
          : market;
  at(term, H::kUnreportedSplit) = log_normal_pdf(dv, -c.r, std::hypot(kSplitVolumeDrift, noise));
  at(term, H::kHistorySegment) = log_normal_pdf(dv, 0.0, std::hypot(2.0, noise));
  if (c.on_file) {
    // A split on file needs no volume to confirm it. A feed's unadjusted volume
    // is sometimes adjusted for the split already, and a reverse split draws a
    // burst of trading: the level after lands anywhere from unchanged to the
    // ratio, with the usual drift either side. Without a split on file, the
    // volume is what tells a split from a scale error.
    at(term, H::kExplainedSplit) = log_flat_top(dv, 0.0, -std::log(c.on_file->price_factor()),
                                                std::hypot(kVolumeDrift, noise));
  }
  at(term, H::kExplainedDividend) =
      log_normal_pdf(dv, -0.5 * c.net_r, std::hypot(kVolumeDrift, noise));
  term.note = "median after / before";
  terms.push_back(std::move(term));
}

// The bar's own range: a violent session (violent_range times the usual or more),
// or not.
std::optional<Term> Scorer::range_term(const Candidate& c) const {
  if (!c.range_vs_typical) {
    return std::nullopt;
  }
  const bool violent = *c.range_vs_typical >= settings_.violent_range;
  const auto ll = [violent](double p) { return violent ? std::log(p) : std::log1p(-p); };
  Term term = uniform_term("bar_range", *c.range_vs_typical, ll(kViolentOrdinary));
  at(term, H::kMarketMove) = ll(kViolentOrdinary + kViolentSurprise * surprise(c, c.r));
  at(term, H::kBadPrint) = ll(kViolentBadPrint);
  at(term, H::kBadClose) = ll(kViolentScale);
  at(term, H::kUnreportedSplit) = ll(kViolentSplit);
  at(term, H::kScaleError) = ll(kViolentScale);
  at(term, H::kHistorySegment) = ll(kViolentSegment);
  at(term, H::kExplainedSplit) = ll(kViolentSplit);
  at(term, H::kExplainedDividend) = ll(kViolentOrdinary + kViolentSurprise * surprise(c, c.net_r));
  term.note = "log range against the median before";
  return term;
}

// DQ203 needs the volume to shift inversely by about the split's ratio: a split
// gives each holder more shares (or fewer), so share volume follows. A series with
// no volume to judge by (a NAV fund, a vendor with none) passes.
bool Scorer::split_volume_ok(const PriceFinding& out, const VolumeShift& shift) const {
  if (!series_.has_volume || out.base_volume_zero || !shift.judged || !out.split) {
    return true;
  }
  const double expected = -std::log(out.split->price_factor());
  if (shift.after) {
    // The level after, within twice the drift any volume level shows over a few
    // months: a 3:2 split's ×1.5 can drift to ×0.8, a 5:1's ×5 cannot stay at ×1.
    return std::fabs(*shift.after - expected) <= 2.0 * std::hypot(kVolumeDrift, shift.noise);
  }
  if (shift.on_day) {
    // Only the day itself so far: its volume must sit nearer the split's level
    // than the old one, more than half the way there.
    return *shift.on_day / expected > 0.5;
  }
  return true;
}

// Plausible price levels either side.
std::optional<Term> Scorer::plausibility_term(const Candidate& c) const {
  const bool pre_ok = plausible(c.prev_close);
  const bool post_ok = plausible(c.this_close);
  if (pre_ok == post_ok) {
    return std::nullopt;
  }
  Term term = uniform_term("plausible_level", pre_ok ? c.this_close : c.prev_close, std::log(1e-3));
  term.note =
      "outside " + format_number(settings_.min_price) + ".." + format_number(settings_.max_price);
  at(term, H::kScaleError) = 0.0;
  if (pre_ok) {  // the new level is the implausible one
    at(term, H::kBadPrint) = 0.0;
    at(term, H::kBadClose) = 0.0;
  } else {  // the old level was: an era ends here
    at(term, H::kHistorySegment) = 0.0;
  }
  return term;
}

// Decimal places as written changing at the boundary (DQ106's signal).
std::optional<Term> Scorer::precision_term(const Candidate& c) const {
  const auto window = static_cast<std::size_t>(settings_.volume_window);
  std::size_t before_count = 0;
  std::size_t after_count = 0;
  const double before = median_decimals(c.t >= window ? c.t - window : 0, c.t, before_count);
  const double after = median_decimals(c.t, c.t + window, after_count);
  if (before_count < kMinWindow || after_count < kMinWindow || std::fabs(after - before) < 3.0) {
    return std::nullopt;
  }
  Term term = uniform_term("precision_shift", after - before, std::log(0.1));
  term.note = "decimal places after - before";
  at(term, H::kScaleError) = 0.0;
  return term;
}

// A history segment: a listing price, and a new volatility regime.
void Scorer::segment_terms(const Candidate& c, std::vector<Term>& terms) const {
  if (has_ohlc_) {  // listing prices are a fact about securities
    const bool round = std::any_of(kListingPrices.begin(), kListingPrices.end(), [&](double p) {
      return std::fabs(c.this_close / p - 1.0) <= 0.0025;
    });
    Term listing =
        uniform_term("listing_price", c.this_close, round ? std::log(0.01) : std::log(0.99));
    at(listing, H::kHistorySegment) = round ? std::log(0.3) : std::log(0.7);
    terms.push_back(std::move(listing));
  }

  const double shift = 0.5 * std::log(f_.backward[c.t].variance() / f_.forward[c.t].variance());
  Term vol = uniform_term("volatility_shift", std::exp(shift), log_normal_pdf(shift, 0.0, 0.35));
  at(vol, H::kHistorySegment) = log_normal_pdf(shift, 0.0, 1.0);
  vol.note = "after / before";
  terms.push_back(std::move(vol));
}

// The posterior over the hypotheses considered, p_error, the most probable error
// and the most probable hypothesis. `excluded` is considered (its evidence stays in
// log_likelihood, for calibrate) but takes no posterior: a rule rules it out here.
// Returns the most probable error's index.
std::size_t conclude(const std::vector<Term>& terms, PriceFinding& out,
                     std::optional<H> excluded = std::nullopt) {
  std::array<bool, kCount> on = out.considered;
  if (excluded) {
    on.at(idx(*excluded)) = false;
  }
  Terms total{};
  for (std::size_t h = 0; h < kCount; ++h) {
    total.at(h) = on.at(h) ? 0.0 : kNegInf;
    for (const Term& term : terms) {
      total.at(h) += on.at(h) ? term.ll.at(h) : 0.0;
    }
  }
  double norm = kNegInf;
  for (const double value : total) {
    norm = stats::log_add(norm, value);
  }
  std::size_t best = idx(H::kBadPrint);
  std::size_t leading = idx(H::kMarketMove);
  out.p_error = 0.0;
  for (std::size_t h = 0; h < kCount; ++h) {
    out.posterior.at(h) = on.at(h) ? std::exp(total.at(h) - norm) : 0.0;
    if (on.at(h) && (total.at(h) > total.at(leading) || !on.at(leading))) {
      leading = h;
    }
    if (!is_error(static_cast<H>(h))) {
      continue;
    }
    out.p_error += out.posterior.at(h);
    if (on.at(h) && (total.at(h) > total.at(best) || !on.at(best))) {
      best = h;
    }
  }
  out.p_error = std::min(out.p_error, 1.0);
  out.hypothesis = static_cast<H>(best);
  out.leading = static_cast<H>(leading);
  return best;
}

PriceFinding Scorer::score(std::size_t t) const {
  const Candidate c = candidate(t);
  PriceFinding out;
  out.bar = t;
  out.end_bar = t;
  out.factor = c.this_close / c.prev_close;
  out.tail = tail(c.r, move_sd(sigma(t, 0), t - 1, t));
  out.provisional = n_ - 1 - t < static_cast<std::size_t>(settings_.provisional_bars);
  applicable(c, out.considered);

  out.move_sd = c.base_sd;
  out.range_vs_typical = c.range_vs_typical;
  if (c.log_range) {
    out.range_ratio = std::exp(*c.log_range);
  }
  out.violent = c.range_vs_typical && *c.range_vs_typical >= settings_.violent_range;
  std::vector<Term> terms;
  std::optional<Term> prior = prior_term(c, out.considered);
  if (!prior) {  // every applicable prior set to zero: nothing to compare
    out.considered.fill(false);
    return out;
  }
  terms.push_back(std::move(*prior));
  terms.push_back(return_term(c, out));
  NextBars next = next_bars(c);
  const std::size_t next_index = terms.size();
  terms.push_back(std::move(next.term));
  if (c.bar_ohl) {
    ohlc_terms(c, terms);
  }
  if (std::optional<Term> range = range_term(c)) {
    terms.push_back(std::move(*range));
  }
  VolumeShift shift;
  volume_terms(c, out, terms, shift);
  if (std::optional<Term> plausible_level = plausibility_term(c)) {
    terms.push_back(std::move(*plausible_level));
  }
  if (std::optional<Term> precision = precision_term(c)) {
    terms.push_back(std::move(*precision));
  }
  if (out.considered.at(idx(H::kHistorySegment))) {
    segment_terms(c, terms);
  }
  // A split on file looks like a split in the bar's own fields and the volume on
  // the day; the bars after it and the volume level are a real event's (next_bars,
  // volume_terms). A dividend on file looks like a real move net of the dividend.
  out.split_on_file = c.on_file;
  out.dividend_on_file = c.dividend;

  std::size_t best = conclude(terms, out);
  // DQ203 only when the volume shifts inversely by about the ratio, or there is
  // no volume to judge by: a traded series whose volume held, or surged, did not
  // split. The next explanation takes the move.
  out.split_volume_ok = split_volume_ok(out, shift);
  if (out.hypothesis == H::kUnreportedSplit && !out.split_volume_ok) {
    best = conclude(terms, out, H::kUnreportedSplit);
  }
  // A surge: the volume level after five times the one before, or more, and not
  // the inverse shift a split would make.
  if (shift.after && *shift.after >= kSurgeShift) {
    const bool split_like = out.split && out.split_volume_ok &&
                            -std::log(out.split->price_factor()) * *shift.after > 0.0 &&
                            std::fabs(*shift.after + std::log(out.split->price_factor())) <=
                                2.0 * std::hypot(kSplitVolumeDrift, shift.noise);
    out.volume_surge = !split_like;
  }
  for (std::size_t h = 0; h < kCount; ++h) {
    double ll = 0.0;
    for (std::size_t i = 1; i < terms.size(); ++i) {
      ll += terms[i].ll.at(h);
    }
    out.log_likelihood.at(h) = ll;
  }
  out.raw_p_error = out.p_error;
  out.p_error = calibrated(settings_.p_error_map, out.p_error);
  if (out.hypothesis == H::kBadPrint) {
    out.block = static_cast<int>(next.best_block);
    out.end_bar = std::min(t + next.best_block, n_) - 1;
  }

  // Evidence for the reported hypothesis, against a market move. The bars after
  // are described for a bad print by the move that undoes it, and otherwise by
  // the largest of them.
  terms.front().value = std::exp(terms.front().ll.at(best));
  if (next.count > 0 && best != idx(H::kBadPrint) && best != idx(H::kBadClose)) {
    terms.at(next_index).value = next.largest;
    terms.at(next_index).note = "the largest of the next " + std::to_string(next.count) + " moves";
  }
  const std::size_t m = idx(H::kMarketMove);
  for (Term& term : terms) {
    const double log_bf = term.ll.at(best) - term.ll.at(m);
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
      const PriceFinding& b = findings[i + 1];
      const double tolerance = std::max(0.1, 3.0 * settings.ratio_tolerance);
      if (b.hypothesis == H::kScaleError && a.p_error >= 0.5 && b.p_error >= 0.5 &&
          std::fabs(std::log(std::fabs(a.factor)) + std::log(std::fabs(b.factor))) < tolerance) {
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

double calibrated(const std::vector<std::pair<double, double>>& map, double p) noexcept {
  if (map.empty()) {
    return p;
  }
  if (p <= map.front().first) {
    return map.front().second;
  }
  for (std::size_t i = 1; i < map.size(); ++i) {
    const auto& [x0, y0] = map[i - 1];
    const auto& [x1, y1] = map[i];
    if (p <= x1) {
      return y0 + (y1 - y0) * (p - x0) / (x1 - x0);
    }
  }
  return map.back().second;
}

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
    case H::kExplainedSplit:
      return "explained_split";
    case H::kExplainedDividend:
      return "explained_dividend";
    case H::kPendingDistribution:
      return "pending_distribution";
  }
  return "market_move";
}

std::string_view to_string(SplitVerdict verdict) noexcept {
  switch (verdict) {
    case SplitVerdict::kConfirmed:
      return "confirmed";
    case SplitVerdict::kMisdated:
      return "misdated";
    case SplitVerdict::kNoJump:
      return "no_jump";
    case SplitVerdict::kRatioMismatch:
      return "ratio_mismatch";
    case SplitVerdict::kDoubleApplied:
      return "double_applied";
  }
  return "confirmed";
}

// Runs of traded bars whose close repeats the bar before (DQ501). Each repeat is a
// coincidence of probability q under a healthy feed: the move from the previous
// close is smaller than half a step of the price grid. q comes from the series'
// own volatility outside the run, with the bid-ask bounce, so on a coarse grid a
// repeat is expected and on a liquid name it is not. A repeat of the whole bar
// (open, high, low and close) counts as two coincidences.
std::vector<StaleRun> Scorer::stale_runs() const {
  std::vector<StaleRun> runs;
  if (!has_ohlc_ || !series_.has_volume) {
    return runs;
  }
  constexpr std::size_t kNearby = 40;    // bars either side that show how sticky prices are
  constexpr double kCoarseSteps = 20.0;  // a price this many grid steps wide, or fewer, is coarse
  const auto traded = [this](std::size_t i) {
    const double v = volume(i);
    return std::isfinite(v) && v > 0.0;
  };
  const auto repeats = [&](std::size_t i) { return close(i) == close(i - 1) && traded(i); };
  const auto whole = [this](std::size_t i) {
    const std::size_t row = f_.row[i];
    const std::size_t prev = f_.row[i - 1];
    return series_.open[row] == series_.open[prev] && series_.high[row] == series_.high[prev] &&
           series_.low[row] == series_.low[prev];
  };
  const double prior = settings_.priors.stale_run;
  for (std::size_t i = 1; i < n_;) {
    if (!repeats(i)) {
      ++i;
      continue;
    }
    StaleRun run;
    run.first = i;
    run.last = i;
    while (run.last + 1 < n_ && repeats(run.last + 1)) {
      ++run.last;
    }
    // How often prices repeat nearby, the run left out: a stock whose moves are
    // smaller than half a tick repeats its close day after day, and repeats come
    // in stretches. Counted as a chain: repeats after a move, and repeats after a
    // repeat.
    const std::size_t from = run.first > kNearby ? run.first - kNearby : 2;
    const std::size_t to = std::min(n_, run.last + 1 + kNearby);
    double after_move = 0.0;
    double repeat_after_move = 0.0;
    double after_repeat = 0.0;
    double repeat_after_repeat = 0.0;
    std::vector<double> close_moves;  // |close - close before|, traded bars nearby
    for (std::size_t j = std::max<std::size_t>(from, 2); j < to; ++j) {
      if ((j >= run.first && j <= run.last + 1) || !traded(j)) {
        continue;
      }
      const bool was = close(j - 1) == close(j - 2);
      const bool is = close(j) == close(j - 1);
      (was ? after_repeat : after_move) += 1.0;
      (was ? repeat_after_repeat : repeat_after_move) += is ? 1.0 : 0.0;
      close_moves.push_back(std::fabs(close(j) - close(j - 1)));
    }
    // The grid, with the lattice the prices nearby sit on. A price only a few steps
    // of it wide (a sub-dime stock in cents, an eighth-dollar stock in sixteenths)
    // seldom moves a whole step: runs are how it trades. The lattice is read on
    // each side of the run, the coarser kept: a stock quoted on a coarse grid for a
    // spell sits next to bars on a fine one.
    constexpr std::size_t kLattice = 20;  // bars either side that show the lattice
    const std::size_t before = run.first > kLattice ? run.first - kLattice : 0;
    const std::size_t after = std::min(n_, run.last + 1 + kLattice);
    const double lattice =
        std::max(observed_step(series_, f_.row[before], f_.row[run.last] + 1),
                 observed_step(series_, f_.row[run.first - 1], f_.row[after - 1] + 1));
    const double grid = std::max(price_grid(run.first - 1, run.first), lattice);
    if (close(run.first) <= kCoarseSteps * grid) {
      i = run.last + 1;
      continue;
    }
    run.grid = grid;
    constexpr std::size_t kMinMoves = 5;
    if (close_moves.size() >= kMinMoves) {
      // In steps, rounded as DQ107 rounds them: 0.05 - 0.03 is not two cents exactly.
      for (double& move : close_moves) {
        move = std::round(move / grid * 1e6) / 1e6;
      }
      run.close_steps = stats::median(std::move(close_moves));
    }
    // Volatility from either side of the run, the run's own zeros left out.
    const double sig =
        std::sqrt(stats::combine(f_.forward[run.first], f_.backward[run.last]).variance());
    constexpr double kWeight = 10.0;  // the model's q counts as this many bars
    for (std::size_t j = run.first; j <= run.last; ++j) {
      // The true price's move, without the bid-ask bounce: a close stays put while
      // the price moves less than half a step, whichever side the last trade hit.
      const double sd = sig * std::sqrt(static_cast<double>(f_.elapsed[j]));
      const double half_step = 0.5 * std::max(price_grid(j - 1, j), lattice);
      const double h = f_.log_scale ? std::log1p(half_step / close(j - 1)) : half_step;
      const double model = std::clamp(1.0 - tail(h, sd), 1e-300, 1.0);
      // The model's chance of a repeat, or the series' own nearby if higher.
      const bool first = j == run.first;
      const double seen = first
                              ? (repeat_after_move + kWeight * model) / (after_move + kWeight)
                              : (repeat_after_repeat + kWeight * model) / (after_repeat + kWeight);
      run.log_q += std::log(std::max(model, seen));
      run.full_bars += whole(j) ? 1 : 0;
    }
    const double stale = std::log(prior);
    const double healthy = std::log1p(-prior) + run.log_q;
    run.p_error = std::exp(stale - stats::log_add(stale, healthy));
    runs.push_back(run);
    i = run.last + 1;
  }
  return runs;
}

// Each split on file against the bars around its ex-date (DQ701-DQ704). Over the
// bars within kDoubleWindow sessions of it, every hypothesis says the moves are
// ordinary but for: a move of the split's ratio at the ex-date's bar (the split
// confirmed); one within kMisdatedWindow sessions of it instead (misdated); none
// (not in the bars: fabricated, or the bars already adjusted for it); a move of
// another clean ratio at the ex-date's bar, or of the ratio inverted (the wrong
// ratio on file); or two moves of the ratio, the ex-date's and one more (applied
// twice). Only the bars that differ enter the comparison.
// Sessions from bar `from` to bar `to`: negative when `to` comes first.
int Scorer::sessions_from(std::size_t from, std::size_t to) const {
  int sessions = 0;
  for (std::size_t i = std::min(from, to) + 1; i <= std::max(from, to); ++i) {
    sessions += f_.elapsed[i];
  }
  return to < from ? -sessions : sessions;
}

// log [p(move | a jump of log ratio `ratio`) / p(move | ordinary)] at bar j: the
// jump is the ratio exactly, plus an ordinary move with ratio_tolerance of slack.
double Scorer::jump_gain(std::size_t j, double ratio, double ret) const {
  const double sd =
      move_sd(sigma(j, static_cast<std::size_t>(settings_.revert_max_bars)), j - 1, j);
  return ordinary(ret - ratio, std::hypot(sd, settings_.ratio_tolerance)) - ordinary(ret, sd);
}

// A jump of log ratio rho at one bar within `window` sessions of bar ex (not ex
// itself), each equally likely: the log mean gain, and the best bar.
Scorer::Elsewhere Scorer::jump_elsewhere(std::size_t ex, double rho, int window,
                                         const std::vector<bool>& skip) const {
  Elsewhere w;
  std::size_t count = 0;
  for (std::size_t j = 1; j < n_; ++j) {
    const int off = sessions_from(ex, j);
    if (j == ex || off < -window || off > window || skip[j]) {
      continue;
    }
    const double gain = jump_gain(j, rho);
    w.log_mean = stats::log_add(w.log_mean, gain);
    ++count;
    if (gain > w.best_gain) {
      w.best_gain = gain;
      w.best = j;
    }
  }
  w.log_mean = count > 0 ? w.log_mean - std::log(static_cast<double>(count)) : kNegInf;
  return w;
}

// Another ratio at bar ex than the split on file's: the clean ratios not near it
// (weighted as in split_ll), or it inverted (2:1 entered as 1:2), which is common.
// Sets the ratio the move fits best.
double Scorer::mismatch_gain(std::size_t ex, const SplitAction& split,
                             std::string& observed) const {
  constexpr double kInverted = 0.3;
  const double rho = std::log(split.price_factor());
  const double near_rho = 3.0 * settings_.ratio_tolerance + 0.05;
  const auto other = [&](double r) {
    return std::fabs(r - rho) > near_rho && std::fabs(r + rho) > near_rho;
  };
  double mismatch = std::log(kInverted) + jump_gain(ex, -rho);
  double best = mismatch;
  observed = format_number(split.denominator) + ":" + format_number(split.numerator);
  double total = 0.0;
  for (const auto& [ratio, weight] : splits_) {
    total += other(std::log(ratio.price_factor())) ? weight : 0.0;
  }
  for (const auto& [ratio, weight] : splits_) {
    const double r = std::log(ratio.price_factor());
    if (total <= 0.0 || !other(r)) {
      continue;
    }
    const double gain = std::log((1.0 - kInverted) * weight / total) + jump_gain(ex, r);
    mismatch = stats::log_add(mismatch, gain);
    if (gain > best) {
      best = gain;
      observed = ratio.to_string();
    }
  }
  return mismatch;
}

std::vector<SplitFinding> Scorer::split_findings(const std::vector<PriceFinding>& findings) const {
  constexpr int kMisdatedWindow = 5;
  constexpr int kDoubleWindow = 20;
  std::vector<SplitFinding> out;
  if (actions_ == nullptr || !has_ohlc_ || n_ < 3) {
    return out;
  }
  // A bad print's edges: the move into the block and the revert out of it. Neither
  // is a split's move (a phantom split's bars at twice the price revert, and the
  // revert is no split), so neither confirms nor misdates one. Where an edge falls
  // on an ex-date, the move across the whole block is what a split there would show.
  std::vector<bool> edge(n_, false);
  std::vector<std::optional<double>> across(n_);
  for (const PriceFinding& f : findings) {
    if (f.hypothesis != H::kBadPrint || f.posterior.at(idx(H::kBadPrint)) < 0.5 ||
        f.end_bar + 1 >= n_) {
      continue;
    }
    const double move = f_.y[f.end_bar + 1] - f_.y[f.bar - 1];
    for (const std::size_t j : {f.bar, f.end_bar + 1}) {
      edge[j] = true;
      across[j] = move;
    }
  }
  // Each split's bar: the first on or after its ex-date; nullopt for a split
  // outside the bars' span, which cannot be judged.
  const std::vector<SplitAction>& splits = actions_->splits;
  std::vector<std::optional<std::size_t>> ex_bars;
  for (const SplitAction& split : splits) {
    std::size_t ex = 1;
    while (ex < n_ && date(ex) < split.ex_date) {
      ++ex;
    }
    ex_bars.push_back(ex >= n_ || date(0) >= split.ex_date ? std::nullopt
                                                           : std::optional<std::size_t>{ex});
  }
  const auto same_ratio = [this](const SplitAction& a, const SplitAction& b) {
    return std::fabs(std::log(a.price_factor() / b.price_factor())) <= settings_.ratio_tolerance;
  };
  const double e = settings_.priors.split_on_file_error;
  for (std::size_t s = 0; s < splits.size(); ++s) {
    if (!ex_bars[s]) {
      continue;
    }
    const SplitAction& split = splits[s];
    const std::size_t ex = *ex_bars[s];
    // Moves this split cannot have made: a bad print's edges, and the ex-date of
    // another split on file at the same ratio (a duplicate row must not be
    // "misdated" onto the date that holds the real one).
    std::vector<bool> skip = edge;
    for (std::size_t o = 0; o < splits.size(); ++o) {
      if (o != s && ex_bars[o] && *ex_bars[o] != ex && same_ratio(split, splits[o])) {
        skip[*ex_bars[o]] = true;
      }
    }
    const double rho = std::log(split.price_factor());
    const double at_ex = jump_gain(ex, rho, across[ex].value_or(f_.ret[ex]));
    const Elsewhere near = jump_elsewhere(ex, rho, kMisdatedWindow, skip);
    const Elsewhere wide = jump_elsewhere(ex, rho, kDoubleWindow, skip);
    std::string observed;
    std::array<double, kSplitVerdicts> ll{};
    ll.at(0) = std::log1p(-e) + at_ex;
    ll.at(1) = std::log(e / 4.0) + near.log_mean;
    ll.at(2) = std::log(e / 4.0);
    ll.at(3) = std::log(e / 4.0) + mismatch_gain(ex, split, observed);
    ll.at(4) = std::log(e / 4.0) + at_ex + wide.log_mean;
    double norm = kNegInf;
    for (const double x : ll) {
      norm = stats::log_add(norm, x);
    }
    SplitFinding finding;
    finding.split = split;
    finding.ex_bar = ex;
    std::size_t best = 1;
    for (std::size_t v = 0; v < kSplitVerdicts; ++v) {
      finding.posterior.at(v) = std::exp(ll.at(v) - norm);
      best = v > 0 && ll.at(v) > ll.at(best) ? v : best;
    }
    finding.p_error = 1.0 - finding.posterior.at(0);
    finding.verdict = static_cast<SplitVerdict>(best);
    if (finding.verdict == SplitVerdict::kMisdated) {
      finding.other_bar = near.best;
    } else if (finding.verdict == SplitVerdict::kDoubleApplied) {
      finding.other_bar = wide.best;
    } else if (finding.verdict == SplitVerdict::kRatioMismatch) {
      finding.observed = observed;
    }
    const std::size_t other = finding.other_bar.value_or(ex);
    finding.sessions_off = sessions_from(ex, other);
    // With too few bars after the moves, they are not yet confirmed.
    finding.provisional =
        n_ - 1 - std::max(ex, other) < static_cast<std::size_t>(settings_.provisional_bars);
    out.push_back(finding);
  }
  // A split the bars do not show, next to another on file at the same ratio that
  // they confirm (its error improbable), is a duplicate of it: the feed applies the
  // ratio once.
  for (SplitFinding& finding : out) {
    if (finding.verdict != SplitVerdict::kNoJump || finding.p_error < 0.5) {
      continue;
    }
    for (const SplitFinding& other : out) {
      const int off = sessions_from(finding.ex_bar, other.ex_bar);
      if (&other == &finding || other.p_error >= 0.5 || other.ex_bar == finding.ex_bar ||
          !same_ratio(finding.split, other.split) || std::abs(off) > kMisdatedWindow) {
        continue;
      }
      finding.duplicate_of = other.split.ex_date;
      finding.other_bar = other.ex_bar;
      finding.sessions_off = off;
      finding.provisional = n_ - 1 - std::max(finding.ex_bar, other.ex_bar) <
                            static_cast<std::size_t>(settings_.provisional_bars);
      break;
    }
  }
  return out;
}

// A misdated split whose move comes before its ex-date may instead be right, with
// the sessions between stored at the post-split scale: a vendor that derives its
// "unadjusted" bars from adjusted ones for dates before a split it knows of. The
// prices cannot tell the two apart (a move of the ratio, then an ordinary move at
// the ex-date); the volume can. Sessions after a split trade at its new level;
// sessions stored at the wrong scale are pre-split sessions, and trade at the
// level before. True when the era's volume is nearer the level before its move
// than the level from the ex-date on.
bool Scorer::era_before(const SplitFinding& finding) const {
  if (finding.verdict != SplitVerdict::kMisdated || !finding.other_bar ||
      *finding.other_bar >= finding.ex_bar || !series_.has_volume) {
    return false;
  }
  const std::size_t start = *finding.other_bar;
  const std::size_t ex = finding.ex_bar;
  // The move out of the era, at the ex-date, is ordinary: the era already sits at
  // the post-split level.
  if (tail(f_.ret[ex], move_sd(sigma(ex, 0), ex - 1, ex)) < settings_.candidate_tail_prob) {
    return false;
  }
  const auto window = static_cast<std::size_t>(settings_.volume_window);
  const auto level = [this](std::size_t from, std::size_t to) -> std::optional<double> {
    const VolumeWindow w = volume_window(from, to);
    if (w.count == 0 || w.zero_share > 0.5) {
      return std::nullopt;
    }
    return std::log1p(w.median);
  };
  const std::optional<double> before = level(start >= window ? start - window : 0, start);
  const std::optional<double> era = level(start, ex);
  // From the ex-date on, or, with no bars yet, the level before shifted by the ratio.
  std::optional<double> after = level(ex, ex + window);
  if (!after && before) {
    after = *before - std::log(finding.split.price_factor());
  }
  if (!before || !era || !after) {
    return false;
  }
  constexpr double kMargin = 0.5;  // the era must be clearly nearer the level before
  return std::fabs(*era - *before) + kMargin < std::fabs(*era - *after);
}

PriceAnalysis analyze_prices(const Series& series, const Calendar& calendar,
                             const PriceSettings& settings, const PriceContext& context) {
  PriceAnalysis out;
  out.features = compute_price_features(series, calendar, settings, context.market);
  const std::size_t n = out.features.size();
  if (n < 2) {
    return out;
  }
  out.applicable = true;
  const Scorer scorer(series, out.features, settings, context);
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
  out.split_findings = scorer.split_findings(out.findings);
  // A misdated split whose bars moved before its ex-date may be right, and the
  // sessions between it and the move stored at its post-split scale: a DQ202 era,
  // the split confirmed (Scorer::era_before).
  for (SplitFinding& split : out.split_findings) {
    if (split.p_error < 0.5 || !scorer.era_before(split)) {
      continue;
    }
    const std::size_t start = *split.other_bar;
    const auto at = std::find_if(out.findings.begin(), out.findings.end(),
                                 [start](const PriceFinding& f) { return f.bar == start; });
    PriceFinding era = at != out.findings.end() ? *at : PriceFinding{};
    era.bar = start;
    era.end_bar = split.ex_bar - 1;
    era.hypothesis = H::kScaleError;
    era.factor = series.close[out.features.row[start]] / series.close[out.features.row[start - 1]];
    era.p_error = split.posterior.at(static_cast<std::size_t>(SplitVerdict::kMisdated));
    era.raw_p_error = era.p_error;
    era.provisional = split.provisional;
    era.claimed = false;
    era.era_split = split.split;
    if (at != out.findings.end()) {
      *at = std::move(era);
    } else {
      out.findings.insert(
          std::upper_bound(out.findings.begin(), out.findings.end(), start,
                           [](std::size_t bar, const PriceFinding& f) { return bar < f.bar; }),
          std::move(era));
    }
    // The move out of the era, at the ex-date, is the split's.
    for (PriceFinding& finding : out.findings) {
      finding.claimed = finding.claimed || finding.bar == split.ex_bar;
    }
    split.era_before = true;
    split.verdict = SplitVerdict::kConfirmed;
    split.other_bar.reset();
    split.sessions_off = 0;
  }
  // A split on file judged wrong accounts for the move it blames: the DQ2xx checks
  // leave that bar to it. A duplicate's move is the confirmed split's.
  for (const SplitFinding& split : out.split_findings) {
    if (split.p_error < 0.5 || split.verdict == SplitVerdict::kConfirmed || split.duplicate_of) {
      continue;
    }
    const std::size_t bar =
        split.verdict == SplitVerdict::kRatioMismatch ? split.ex_bar : split.other_bar.value_or(n);
    for (PriceFinding& finding : out.findings) {
      if (finding.bar == bar) {
        finding.claimed = true;
      }
    }
  }
  out.stale_runs = scorer.stale_runs();
  out.move_z = scorer.move_z();
  out.move_sd.assign(n, 0.0);
  for (std::size_t t = 1; t < n; ++t) {
    out.move_sd[t] = scorer.move_sd(scorer.sigma(t, 0), t - 1, t);
  }
  return out;
}

}  // namespace dorq
