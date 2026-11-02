// The share terms model: DQ921 share-terms-step and DQ922 share-count-mismatch
// (fundamentals plan section 4.3; doc/checks/DQ921.md, DQ922.md).

#include "statements/shares.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "dorq/statement.hpp"
#include "price/model.hpp"

namespace dorq {

ShareHypothesis ShareFinding::explanation() const noexcept {
  if (posterior[1] >= posterior[2] && posterior[1] >= posterior[3]) {
    return ShareHypothesis::kUnreportedSplit;
  }
  return posterior[2] >= posterior[3] ? ShareHypothesis::kTermsError : ShareHypothesis::kUnitError;
}

namespace {

constexpr double kStepSd = 0.04;  // a clean step's spread, on the log scale (issuance meanwhile)
constexpr double kCapSd = 0.25;   // the market cap's move between two filings, without a line's own
constexpr double kCapFloor = 0.05;   // the least a line's market cap spread is taken to be
constexpr double kCapTail = 0.15;    // the share of market cap moves from the fat tail
constexpr double kRealSd = 0.35;     // a real issuance's step (an offering, a merger)
constexpr double kMinStep = 0.22;    // log 1.25: smaller steps are issuance
constexpr double kMatch = 0.1;       // a step at a split's ratio, to within this on the log scale
constexpr double kMatchMax = 0.25;   // ... widened by a line's own real steps, at most this
constexpr double kSplitRate = 0.04;  // splits missing from the actions a year: 0.01 a quarter
constexpr double kTermsPrior = 0.01;
constexpr double kUnitPrior = 0.002;
constexpr double kMismatchPrior = 0.01;  // DQ922: a period's count stored wrong
constexpr double kLargeIssuance =
    0.05;                         // the share of a count's steps that are offerings or mergers
constexpr double kPairSd = 0.05;  // DQ922: weighted-average against outstanding, without issuance
constexpr std::array<int, 4> kPowers{3, -3, 6, -6};
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double normal_log_pdf(double x, double mean, double sd) {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}

double log_add(double a, double b) {
  if (a == -std::numeric_limits<double>::infinity()) {
    return b;
  }
  if (b == -std::numeric_limits<double>::infinity()) {
    return a;
  }
  const double m = std::max(a, b);
  return m + std::log(std::exp(a - m) + std::exp(b - m));
}

bool usable(double x) { return std::isfinite(x) && x > 0.0; }

// The median of a non-empty vector, reordering it.
double median_of(std::vector<double>& v) {
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

double log_of(const SplitRatio& r) {
  return std::log(static_cast<double>(r.shares_after) / r.shares_before);
}

// The splits on file, in date order.
std::vector<SplitAction> splits_of(const SeriesActions* actions) {
  std::vector<SplitAction> out;
  if (actions != nullptr) {
    out = actions->splits;
  }
  std::sort(out.begin(), out.end(),
            [](const SplitAction& a, const SplitAction& b) { return a.ex_date < b.ex_date; });
  return out;
}

// The share counts --items lists, by item index.
std::vector<std::size_t> share_items(const StatementRows& st, const std::vector<LineItem>* items,
                                     Statement statement) {
  std::vector<std::size_t> out;
  if (items == nullptr) {
    return out;
  }
  for (std::size_t k = 0; k < st.item_names.size(); ++k) {
    const auto it = std::find_if(items->begin(), items->end(), [&](const LineItem& item) {
      return item.column.size() == st.item_names[k].size() &&
             std::equal(item.column.begin(), item.column.end(), st.item_names[k].begin(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    });
    if (it != items->end() && it->item_class == ItemClass::kShareCount &&
        it->statement == statement) {
      out.push_back(k);
    }
  }
  return out;
}

// One view's line of one statement and period type: its rows by fiscal date, and
// which follow within the spacing.
struct Line {
  Statement statement = Statement::kBalanceSheet;
  PeriodType type = PeriodType::kQuarter;
  std::vector<std::size_t> rows;
  std::vector<bool> follows;  // follows[p]: rows[p] is the period after rows[p - 1]
};

std::vector<Line> lines_of(const Series& s, const std::vector<std::size_t>& view,
                           const StatementSettings& settings) {
  std::map<std::pair<int, int>, Line> by;
  for (const std::size_t r : view) {
    Line& line = by[{static_cast<int>(s.statements.statement[r]),
                     static_cast<int>(s.statements.period_type[r])}];
    line.statement = s.statements.statement[r];
    line.type = s.statements.period_type[r];
    line.rows.push_back(r);
  }
  std::vector<Line> lines;
  for (auto& [key, line] : by) {
    const auto [low, high] =
        line.type == PeriodType::kQuarter ? settings.quarter_spacing : settings.year_spacing;
    line.follows.assign(line.rows.size(), false);
    for (std::size_t p = 1; p < line.rows.size(); ++p) {
      const int gap = s.date[line.rows[p]].days() - s.date[line.rows[p - 1]].days();
      line.follows[p] = gap >= low && gap <= high;
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

// What a step is near: a clean split ratio, and a power of ten.
struct Nearest {
  std::optional<SplitRatio> ratio;
  double ratio_ll = -std::numeric_limits<double>::infinity();  // the split mixture's log density
  int power = 0;
  double power_ll = -std::numeric_limits<double>::infinity();
};

// How near a step must be to a ratio: kMatch, or 2.5 times the step's spread (a
// split's ratio plus the line's real change meanwhile), whichever is wider.
double tolerance(double sd) { return std::min(kMatchMax, std::max(kMatch, 2.5 * sd)); }

// A step's spread about a clean ratio on a line whose real steps spread by `noise`.
double ratio_sd(double noise) { return std::hypot(kStepSd, noise); }

Nearest nearest(double d, const PriceSettings& price, double sd) {
  Nearest n;
  double total = 0.0;
  for (const SplitRatio& r : price.split_ratios) {
    total += split_weight(r);
  }
  double best = std::numeric_limits<double>::infinity();
  for (const SplitRatio& r : price.split_ratios) {
    n.ratio_ll =
        log_add(n.ratio_ll, std::log(split_weight(r) / total) + normal_log_pdf(d, log_of(r), sd));
    if (std::fabs(d - log_of(r)) < best) {
      best = std::fabs(d - log_of(r));
      n.ratio = r;
    }
  }
  if (best > tolerance(sd)) {
    n.ratio.reset();
  }
  for (const int k : kPowers) {
    const double ll = normal_log_pdf(d, k * std::numbers::ln10, sd) - std::log(4.0);
    if (ll > n.power_ll) {
      n.power_ll = ll;
      n.power = std::fabs(d - k * std::numbers::ln10) < tolerance(sd) ? k : 0;
    }
  }
  return n;
}

std::array<double, 4> softmax(const std::array<double, 4>& log) {
  std::array<double, 4> p{};
  const double top = *std::max_element(log.begin(), log.end());
  double sum = 0.0;
  for (std::size_t h = 0; h < 4; ++h) {
    p.at(h) = std::exp(log.at(h) - top);
    sum += p.at(h);
  }
  for (double& v : p) {
    v /= sum;
  }
  return p;
}

// The prior of a split missing from the actions with its ex-date in (from, to]: a
// rate a year, so a wide window (a restatement years later) is likelier to hold one.
double missing_split_prior(Date from, Date to) {
  const double years = std::max(1.0, static_cast<double>(to.days() - from.days())) / 365.25;
  return 1.0 - std::exp(-kSplitRate * years);
}

// A missing split's fit to a log ratio `s` (shares after over before), at the price
// before it: its direction by the price level (forward splits bring a high price
// down, reverse splits lift a low one), then its ratio among that direction's.
struct SplitFit {
  std::optional<SplitRatio> ratio;
  double ll = kNegInf;
};

SplitFit split_fit(double s, double price, const PriceSettings& ps, double sd) {
  const double forward = usable(price) ? split_level_weights(price) : 0.5;
  double forward_total = 0.0;
  double reverse_total = 0.0;
  for (const SplitRatio& r : ps.split_ratios) {
    (r.shares_after > r.shares_before ? forward_total : reverse_total) += split_weight(r);
  }
  SplitFit fit;
  double best = std::numeric_limits<double>::infinity();
  for (const SplitRatio& r : ps.split_ratios) {
    const bool up = r.shares_after > r.shares_before;
    const double total = up ? forward_total : reverse_total;
    const double weight = (up ? forward : 1.0 - forward) * split_weight(r) / total;
    fit.ll = log_add(fit.ll, std::log(weight) + normal_log_pdf(s, log_of(r), sd));
    if (std::fabs(s - log_of(r)) < best) {
      best = std::fabs(s - log_of(r));
      fit.ratio = r;
    }
  }
  if (best > tolerance(sd)) {
    fit.ratio.reset();
  }
  return fit;
}

// The context a view's share counts are judged in.
// A view's rows by fiscal date, period type and statement: where a count's partner,
// the other statement's count for the same period, is.
using RowIndex = std::map<std::tuple<std::int32_t, int, int>, std::size_t>;

struct ShareContext {
  const Series& series;
  const StatementSettings& settings;
  const PriceSettings& price;
  const std::vector<SplitAction>& splits;
  Date latest;  // the terms the counts are put into: the latest knowledge date
  const std::vector<std::size_t>& weighted;     // the income statement's counts
  const std::vector<std::size_t>& outstanding;  // the balance sheet's
  const RowIndex& rows;                         // the view's
};

// The splits on file after `known` up to the latest terms: what puts a count
// stored at `known` into the latest terms.
double to_latest(const ShareContext& sc, Date known) {
  double factor = 1.0;
  for (const SplitAction& split : sc.splits) {
    if (split.ex_date > known && split.ex_date <= sc.latest) {
      factor *= split.numerator / split.denominator;
    }
  }
  return factor;
}

// The split on file with its ex-date in (from, to] whose ratio, either way, or
// twice it, the step is; nullptr for none. `multiple` is the matching one.
const SplitAction* split_in(const ShareContext& sc, Date from, Date to, double d, double sd,
                            double* multiple = nullptr) {
  for (const SplitAction& split : sc.splits) {
    if (split.ex_date <= from || split.ex_date > to) {
      continue;
    }
    const double l = std::fabs(std::log(split.numerator / split.denominator));
    for (const double m : {l, 2.0 * l}) {
      if (std::fabs(std::fabs(d) - m) < tolerance(sd)) {
        if (multiple != nullptr) {
          *multiple = m;
        }
        return &split;
      }
    }
  }
  return nullptr;
}

double normal_cdf(double z) { return 0.5 * std::erfc(-z / std::numbers::sqrt2); }

// The log density at x of a value anywhere between lo and hi, uniformly, give or take
// sd.
double uniform_normal_log_pdf(double x, double lo, double hi, double sd) {
  if (hi < lo) {
    std::swap(lo, hi);
  }
  if (hi - lo < 0.1 * sd) {
    return normal_log_pdf(x, 0.5 * (lo + hi), sd);
  }
  const double mass = normal_cdf((hi - x) / sd) - normal_cdf((lo - x) / sd);
  return std::log(std::max(mass, 1e-300) / (hi - lo));
}

// A count's partner at a step: the other statement's count for the same periods
// (an income count's is the balance sheet's outstanding count, a balance-sheet
// count's the weighted average). A period's weighted average is the mean of its
// outstanding counts, so it lies between the counts at its start and end: anywhere
// between for an issuance in the period, at the end for a split (its counts all
// restated) or a filing stored in other terms.
struct PairAt {
  double position = 0.0;    // log(weighted / outstanding) at the step's period, less its median
  double other_step = 0.0;  // the partner's step, a to b, in the latest terms
  double sd = 0.02;         // the position's spread along the line
};

// Two counts' knowledge dates:// Two counts' knowledge dates: the window a missing split would be
// in, and the ratio of the later-known count to the earlier-known, from `d` = log(b / a). Returns
// the earlier-known row.
std::size_t set_window(const StatementRows& st, std::size_t a, std::size_t b, double d,
                       ShareFinding& f) {
  const bool forward = st.known_from[a] <= st.known_from[b];
  f.window_from = forward ? st.known_from[a] : st.known_from[b];
  f.window_to = forward ? st.known_from[b] : st.known_from[a];
  f.same_known = st.known_from[a] == st.known_from[b];
  f.implied = forward ? d : -d;
  return forward ? a : b;
}

// A count's real steps along its line: their median, and their spread about it; and
// the spread of the market cap it implies (the count as stored times the close at
// its knowledge date) from one period to the next.
struct Trend {
  double median = 0.0;
  double noise = 0.0;
  double cap_sd = kCapSd;
  double period_days = 91.0;  // the line's spacing
};

// The market cap's spread between two knowledge dates: the line's own, as a random
// walk over the periods between them.
double cap_sd(const ShareFinding& f, const Trend& trend) {
  const double periods =
      static_cast<double>(f.window_to.days() - f.window_from.days()) / trend.period_days;
  return trend.cap_sd * std::sqrt(std::max(1.0, periods));
}

// The market cap's move: returns have fat tails, so the line's spread most of the
// time and four times it now and then.
double cap_log_pdf(double x, double mean, double sd) {
  return log_add(std::log(1.0 - kCapTail) + normal_log_pdf(x, mean, sd),
                 std::log(kCapTail) + normal_log_pdf(x, mean, 4.0 * sd));
}

// The line's market cap spread from period to period: robust, so the steps a fault
// leaves do not widen it; kCapSd with too few periods to say.
double line_cap_sd(const Series& s, const Line& line, std::size_t item) {
  const StatementRows& st = s.statements;
  std::vector<double> steps;
  for (std::size_t p = 1; p < line.rows.size(); ++p) {
    const std::size_t a = line.rows[p - 1];
    const std::size_t b = line.rows[p];
    if (line.follows[p] && usable(s.close[a]) && usable(s.close[b]) && usable(st.items[item][a]) &&
        usable(st.items[item][b])) {
      steps.push_back(std::log(st.items[item][b] * s.close[b] / (st.items[item][a] * s.close[a])));
    }
  }
  if (steps.size() < 4) {
    return kCapSd;
  }
  const double median = median_of(steps);
  for (double& x : steps) {
    x = std::fabs(x - median);
  }
  return std::max(kCapFloor, 1.4826 * median_of(steps));
}

// DQ921: one step of one item, a to b, put into the latest terms (step = log b / a),
// against the item's own trend; `d` is the factor to judge, the step net of the
// trend, or a run's measured going in and coming out. nullopt for a step no clean
// ratio explains.
std::optional<ShareFinding> judge_step(const ShareContext& sc, std::size_t item, std::size_t a,
                                       std::size_t b, double step, double d, const Trend& trend,
                                       const std::optional<PairAt>& pair) {
  const Series& s = sc.series;
  const StatementRows& st = s.statements;
  const double moved = step - trend.median;  // net of the count's steady issuance or buybacks
  const double noise = trend.noise;
  ShareFinding f;
  const std::size_t earlier = set_window(st, a, b, d, f);
  f.implied_raw = st.known_from[a] <= st.known_from[b] ? step : -step;
  double multiple = 0.0;
  const double sd_ratio = ratio_sd(noise);
  const SplitAction* on_file = split_in(sc, f.window_from, f.window_to, d, sd_ratio, &multiple);
  const Nearest near = nearest(d, sc.price, sd_ratio);
  if (!near.ratio && near.power == 0 && on_file == nullptr) {
    return std::nullopt;  // not a clean ratio: issuance or buybacks
  }
  f.kind = ShareFinding::Kind::kStep;
  f.row = f.last_row = b;
  f.before = a;
  f.item = item;
  f.factor = std::exp(step);
  f.spread = noise;
  f.value = st.items[item][b];
  f.ratio = near.ratio;
  f.power = near.power;
  if (!f.ratio && f.power == 0) {
    // The split on file's ratio, either way: a count left in the terms before it.
    const auto num = static_cast<int>(on_file->numerator);
    const auto den = static_cast<int>(on_file->denominator);
    f.ratio = (d > 0.0) == (num > den) ? SplitRatio{num, den} : SplitRatio{den, num};
  }
  const SplitFit fit = split_fit(f.implied, s.close[earlier], sc.price, sd_ratio);
  f.split = fit.ratio;
  // With close: the implied market cap, the counts as stored times the raw close at
  // their knowledge dates, both in the terms of that date. A missing split leaves it
  // level; a count stored wrong, or a real issuance at the market price, steps it.
  std::optional<double> cap;
  const double raw_a = st.items[item][a];
  if (usable(s.close[a]) && usable(s.close[b]) && usable(raw_a) && usable(f.value)) {
    cap = std::log(f.value * s.close[b] / (raw_a * s.close[a]));
    f.cap_step = cap;
  }
  const double sd = cap_sd(f, trend);
  const double cap_moved = cap ? cap_log_pdf(*cap, step, sd) : 0.0;
  const double cap_level = cap ? cap_log_pdf(*cap, trend.median, sd) : 0.0;
  const double p_split = f.same_known ? 0.0 : missing_split_prior(f.window_from, f.window_to);
  // A real change in the count: mostly the item's own small moves, now and then an
  // offering or a merger.
  const double real = log_add(std::log(1.0 - kLargeIssuance) + normal_log_pdf(moved, 0.0, noise),
                              std::log(kLargeIssuance) + normal_log_pdf(moved, 0.0, kRealSd));
  std::array<double, 4> log{};
  log[0] = std::log(1.0 - p_split - kTermsPrior - kUnitPrior) + real + cap_moved;
  log[1] = f.same_known ? kNegInf : std::log(p_split) + fit.ll + cap_level;
  log[2] = kNegInf;
  if (on_file != nullptr) {
    log[2] = std::log(kTermsPrior) + normal_log_pdf(std::fabs(d), multiple, sd_ratio) + cap_moved;
  }
  log[3] = std::log(kUnitPrior) + near.power_ll + cap_moved;
  // The weighted average's place in its period: at the period's end for a split, a
  // filing in other terms, or (against the other count) a count stored at a power of
  // ten; anywhere between its start and end for an issuance. Against a weighted
  // average's step, a real issuance is the outstanding count's.
  if (pair) {
    const bool weighted_item = st.statement[b] == Statement::kIncome;
    const double outstanding_step = weighted_item ? pair->other_step : step;
    const double issued = uniform_normal_log_pdf(pair->position, -outstanding_step, 0.0, pair->sd);
    const double at_end = normal_log_pdf(pair->position, 0.0, pair->sd);
    if (weighted_item) {
      const double y = pair->other_step - trend.median;
      log[0] = std::log(1.0 - p_split - kTermsPrior - kUnitPrior) +
               log_add(std::log(1.0 - kLargeIssuance) + normal_log_pdf(y, 0.0, noise),
                       std::log(kLargeIssuance) + normal_log_pdf(y, 0.0, kRealSd)) +
               cap_moved;
    }
    log[0] += issued;
    log[1] += at_end;
    log[2] += at_end;
    log[3] += normal_log_pdf(pair->position, weighted_item ? d : -d, pair->sd);
    f.evidence.push_back({"weighted_in_period", pair->position, at_end - issued,
                          "the weighted average against its period's outstanding counts: at "
                          "the end for a split, between for an issuance"});
  }
  f.posterior = softmax(log);
  if (on_file != nullptr) {
    f.split_on_file = on_file->ratio_text() + " " + on_file->ex_date.to_string();
    f.evidence.push_back({"split_on_file", f.split_on_file, 0.0, "in the window"});
  } else {
    f.evidence.push_back({"split_on_file", false, 0.0, "none in the window"});
  }
  if (cap) {
    f.evidence.push_back({"market_cap_step", *cap, cap_level - cap_moved,
                          "level for a split, a step for an issuance or a count stored wrong"});
  }
  if (usable(s.close[earlier])) {
    f.evidence.push_back({"close_before", s.close[earlier], 0.0,
                          "forward splits at high prices, reverse splits at low ones"});
  }
  return f;
}

// A count's partner along its line, in the latest terms, by position on the line:
// NaN where the other statement has no row or no count.
struct Pairing {
  std::vector<double> partner;
  std::vector<double> position;  // log(weighted / outstanding), less its median
  double sd = 0.02;

  [[nodiscard]] std::optional<PairAt> at(std::size_t p) const {
    if (p == 0 || p >= partner.size() || !usable(partner[p]) || !usable(partner[p - 1]) ||
        !std::isfinite(position[p])) {
      return std::nullopt;
    }
    return PairAt{position[p], std::log(partner[p] / partner[p - 1]), sd};
  }
};

Pairing pairing_of(const ShareContext& sc, const Line& line, const std::vector<double>& converted) {
  const Series& s = sc.series;
  const StatementRows& st = s.statements;
  const bool weighted_item = line.statement == Statement::kIncome;
  const std::vector<std::size_t>& others = weighted_item ? sc.outstanding : sc.weighted;
  const Statement other = weighted_item ? Statement::kBalanceSheet : Statement::kIncome;
  const std::size_t n = line.rows.size();
  Pairing out;
  out.partner.assign(n, std::numeric_limits<double>::quiet_NaN());
  out.position.assign(n, std::numeric_limits<double>::quiet_NaN());
  if (others.empty()) {
    return out;
  }
  const std::size_t partner = others.front();
  std::vector<double> offsets;
  for (std::size_t p = 0; p < n; ++p) {
    const std::size_t r = line.rows[p];
    const auto it = sc.rows.find(
        {s.date[r].days(), static_cast<int>(st.period_type[r]), static_cast<int>(other)});
    if (it == sc.rows.end()) {
      continue;
    }
    out.partner[p] = st.items[partner][it->second] * to_latest(sc, st.known_from[it->second]);
    if (usable(out.partner[p]) && usable(converted[p])) {
      const double offset = weighted_item ? std::log(converted[p] / out.partner[p])
                                          : std::log(out.partner[p] / converted[p]);
      out.position[p] = offset;
      offsets.push_back(offset);
    }
  }
  if (offsets.size() < 3) {
    out.position.assign(n, std::numeric_limits<double>::quiet_NaN());
    return out;
  }
  const double median = median_of(offsets);
  for (double& x : offsets) {
    x = std::fabs(x - median);
  }
  out.sd = std::max(0.02, 1.4826 * median_of(offsets));
  for (double& x : out.position) {
    x -= median;  // NaN stays NaN
  }
  return out;
}

// DQ921: one item's steps along a line, put into the latest terms.
void judge_steps(const ShareContext& sc, const Line& line, std::size_t item,
                 std::vector<ShareFinding>& out) {
  const StatementRows& st = sc.series.statements;
  const std::size_t n = line.rows.size();
  std::vector<double> converted(n);
  for (std::size_t p = 0; p < n; ++p) {
    const std::size_t r = line.rows[p];
    converted[p] = st.items[item][r] * to_latest(sc, st.known_from[r]);
  }
  const auto step_at = [&](std::size_t p) {
    return line.follows[p] && usable(converted[p]) && usable(converted[p - 1]);
  };
  std::vector<double> steps;
  for (std::size_t p = 1; p < n; ++p) {
    if (step_at(p)) {
      steps.push_back(std::log(converted[p] / converted[p - 1]));
    }
  }
  if (steps.size() < 3) {
    return;
  }
  Trend trend;
  trend.median = median_of(steps);
  for (double& x : steps) {
    x = std::fabs(x - trend.median);
  }
  trend.noise = std::max(0.02, 1.4826 * median_of(steps));
  trend.cap_sd = line_cap_sd(sc.series, line, item);
  trend.period_days = line.type == PeriodType::kAnnual ? 365.0 : 91.0;
  const double large = std::max(kMinStep, 5.0 * trend.noise);
  const Pairing pairing = pairing_of(sc, line, converted);
  // The step back to the trend's line from before a step at p, within eight
  // periods: the periods between may be one error, seen going in and coming out.
  // Back means one large step against it, to nearer the old line than the step's
  // half way and within the line's own spread of it.
  const auto return_of = [&](std::size_t p) -> std::optional<std::size_t> {
    const double d = std::log(converted[p] / converted[p - 1]) - trend.median;
    for (std::size_t q = p + 1; q < n && q <= p + 8 && step_at(q); ++q) {
      const double step = std::log(converted[q] / converted[q - 1]) - trend.median;
      if (std::fabs(step) < large) {
        continue;
      }
      const auto periods = static_cast<double>(q - p + 1);
      const double off = std::log(converted[q] / converted[p - 1]) - trend.median * periods;
      const bool back = (step > 0.0) != (d > 0.0) &&
                        std::fabs(off) < std::min(0.5 * std::fabs(d),
                                                  kMatch + 2.0 * trend.noise * std::sqrt(periods));
      return back ? std::optional{q} : std::nullopt;
    }
    return std::nullopt;
  };
  for (std::size_t p = 1; p < n; ++p) {
    if (!step_at(p)) {
      continue;
    }
    const double step = std::log(converted[p] / converted[p - 1]);
    if (std::fabs(step - trend.median) < large) {
      continue;
    }
    // A run's factor is seen twice, so its two steps measure it better than one.
    const std::optional<std::size_t> back = return_of(p);
    double d = step - trend.median;
    if (back) {
      d = 0.5 * (d - (std::log(converted[*back] / converted[*back - 1]) - trend.median));
    }
    std::optional<ShareFinding> f =
        judge_step(sc, item, line.rows[p - 1], line.rows[p], step, d, trend, pairing.at(p));
    if (!f) {
      continue;
    }
    // A step back: the periods between are the ones stored wrong (terms or units),
    // one report on the first. A split missing from the actions shifts the level for
    // good; its steps back are restatements in other terms, which fold into it.
    if (back && f->explanation() != ShareHypothesis::kUnreportedSplit) {
      f->last_row = line.rows[*back - 1];
      p = *back;
    }
    f->provisional = p + 1 >= n;
    out.push_back(std::move(*f));
  }
}

// DQ922: one period's weighted-average count (income row i, item w) against its
// outstanding count (balance-sheet row b, item o); nullopt when they agree, or a
// split on file explains them.
// A pair of counts' usual relation over the series: the median log(weighted /
// outstanding), and its spread. Two counts a vendor measures differently, or an
// outstanding count dated weeks after the period, spread it.
struct PairNorm {
  double offset = 0.0;
  double sd = kPairSd;
};

// The log ratio of a period's weighted-average count (income row i, item w) to its
// outstanding count (balance-sheet row b, item o), each put into the latest terms
// from its own version's knowledge date; nullopt without both.
std::optional<double> pair_ratio(const ShareContext& sc, std::size_t i, std::size_t b,
                                 std::size_t w, std::size_t o) {
  const StatementRows& st = sc.series.statements;
  const double wv = st.items[w][i];
  const double ov = st.items[o][b];
  if (!usable(wv) || !usable(ov)) {
    return std::nullopt;
  }
  return std::log(wv * to_latest(sc, st.known_from[i]) / (ov * to_latest(sc, st.known_from[b])));
}

std::optional<ShareFinding> judge_pair(const ShareContext& sc, std::size_t i, std::size_t b,
                                       std::size_t w, std::size_t o, const PairNorm& norm) {
  const Series& s = sc.series;
  const StatementRows& st = s.statements;
  const std::optional<double> ratio = pair_ratio(sc, i, b, w, o);
  if (!ratio) {
    return std::nullopt;
  }
  const double wv = st.items[w][i];
  const double ov = st.items[o][b];
  const double d = *ratio - norm.offset;  // off the pair's usual relation
  if (std::fabs(d) < std::max(kMinStep, 5.0 * norm.sd)) {
    return std::nullopt;
  }
  const double sd_ratio = ratio_sd(norm.sd);
  const Nearest near = nearest(d, sc.price, sd_ratio);
  if (!near.ratio && near.power == 0) {
    return std::nullopt;
  }
  const int length = st.period_type[i] == PeriodType::kAnnual ? 366 : 92;
  const Date start = Date::from_days(s.date[i].days() - length);
  const Date filed = std::max(st.known_from[i], st.known_from[b]);
  if (near.power == 0 && split_in(sc, start, filed, d, sd_ratio) != nullptr) {
    return std::nullopt;  // a split in the period, or before its filing, explains it
  }
  ShareFinding f;
  f.kind = ShareFinding::Kind::kMismatch;
  f.row = f.last_row = i;
  f.before = b;
  f.item = w;
  f.other = o;
  f.factor = std::exp(*ratio);
  f.value = wv;
  f.other_value = ov;
  f.spread = norm.sd;
  f.ratio = near.ratio;
  f.power = near.power;
  // d = log(w / o), so the window's orientation is income row against balance row.
  const std::size_t earlier = set_window(st, b, i, d, f);
  f.implied_raw = f.implied;
  const SplitFit fit = split_fit(f.implied, s.close[earlier], sc.price, sd_ratio);
  f.split = fit.ratio;
  const double p_split = f.same_known ? 0.0 : missing_split_prior(f.window_from, f.window_to);
  // Real: the period's issuance between its average and its end, now and then an
  // offering or a merger.
  const double real = log_add(std::log(1.0 - kLargeIssuance) + normal_log_pdf(d, 0.0, norm.sd),
                              std::log(kLargeIssuance) + normal_log_pdf(d, 0.0, kRealSd));
  std::array<double, 4> log{};
  log[0] = std::log(1.0 - kMismatchPrior - p_split) + real;
  log[1] = std::log(kMismatchPrior) + log_add(near.ratio_ll, near.power_ll);
  log[2] = f.same_known ? kNegInf : std::log(p_split) + fit.ll;
  log[3] = kNegInf;
  f.posterior = softmax(log);
  if (!f.same_known) {
    f.evidence.push_back({"known_from_apart",
                          static_cast<std::int64_t>(f.window_to.days() - f.window_from.days()),
                          log[2] - log[1], "days: a split missing from the actions between them"});
  }
  return f;
}

// DQ922: a period's weighted-average and outstanding shares, by a clean ratio or a
// power of ten apart, which no split on file between the period's start and its
// filing explains.
void judge_mismatch(const ShareContext& sc, const std::vector<std::size_t>& view,
                    const std::vector<std::size_t>& weighted,
                    const std::vector<std::size_t>& outstanding, std::vector<ShareFinding>& out) {
  const Series& s = sc.series;
  const StatementRows& st = s.statements;
  std::map<std::tuple<std::int32_t, int>, std::pair<std::size_t, std::size_t>> periods;
  constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
  for (const std::size_t r : view) {
    auto& [income, balance] =
        periods.try_emplace({s.date[r].days(), static_cast<int>(st.period_type[r])}, kNone, kNone)
            .first->second;
    if (st.statement[r] == Statement::kIncome) {
      income = r;
    } else if (st.statement[r] == Statement::kBalanceSheet) {
      balance = r;
    }
  }
  std::vector<std::pair<std::size_t, std::size_t>> pairs;
  for (const auto& [key, rows] : periods) {
    if (rows.first != kNone && rows.second != kNone) {
      pairs.push_back(rows);
    }
  }
  // Each pair of counts, and each period type: a year's average against its end
  // has its own relation.
  for (const std::size_t w : weighted) {
    for (const std::size_t o : outstanding) {
      for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
        std::vector<double> ratios;
        for (const auto& [i, b] : pairs) {
          if (st.period_type[i] != type) {
            continue;
          }
          if (const std::optional<double> r = pair_ratio(sc, i, b, w, o)) {
            ratios.push_back(*r);
          }
        }
        PairNorm norm;
        if (ratios.size() >= 4) {
          norm.offset = median_of(ratios);
          for (double& x : ratios) {
            x = std::fabs(x - norm.offset);
          }
          norm.sd = std::max(kPairSd, 1.4826 * median_of(ratios));
        }
        for (const auto& [i, b] : pairs) {
          if (st.period_type[i] != type) {
            continue;
          }
          if (std::optional<ShareFinding> f = judge_pair(sc, i, b, w, o, norm)) {
            out.push_back(std::move(*f));
          }
        }
      }
    }
  }
}

// One split missing from the actions: its likeliest step, the narrowest step that
// shows it, and the window every step and mismatch it explains narrows its ex-date
// to.
struct MissingSplit {
  std::size_t anchor = 0;
  std::size_t narrowest = 0;
  double implied = 0.0;
  Date from;
  Date to;
  int explains = 0;
};

// The tolerance on a fold: a split's ratio, widened by the count's own real steps.
constexpr double kFoldMax = 0.2;

bool explained_by(const ShareFinding& f, const MissingSplit& m) {
  const double tolerance = std::min(kFoldMax, kMatch + 2.0 * f.spread);
  const double off =
      std::min(std::fabs(f.implied - m.implied), std::fabs(f.implied_raw - m.implied));
  return !f.same_known && off < tolerance &&
         std::max(f.window_from, m.from) < std::min(f.window_to, m.to);
}

std::int32_t width(const ShareFinding& f) { return f.window_to.days() - f.window_from.days(); }

// A count the evidence says is stored wrong stays its own finding.
bool stored_wrong(const ShareFinding& f) {
  return f.kind == ShareFinding::Kind::kMismatch ? f.posterior[1] >= 0.5
                                                 : f.posterior[2] >= 0.5 || f.posterior[3] >= 0.5;
}

// A missing split leaves a step at every pair of counts known either side of it: at
// the period it came in, and again wherever a version filed after it (a
// restatement) meets one filed before. Each such split is one finding; the others
// it explains, DQ921's and DQ922's, fold into it. A step at the split's ratio inside
// its window is the split's, whatever its own evidence says: it narrows the window,
// and the finding is reported on the narrowest, with the likeliest's posterior.
void fold_missing_splits(std::vector<ShareFinding>& findings) {
  std::vector<std::size_t> anchors;
  for (std::size_t k = 0; k < findings.size(); ++k) {
    const ShareFinding& f = findings[k];
    if (f.kind == ShareFinding::Kind::kStep && !f.same_known && f.posterior[1] >= 0.5) {
      anchors.push_back(k);
    }
  }
  std::stable_sort(anchors.begin(), anchors.end(), [&](std::size_t x, std::size_t y) {
    return width(findings[x]) < width(findings[y]);
  });
  std::vector<MissingSplit> splits;
  std::vector<bool> folded(findings.size(), false);
  std::vector<bool> is_anchor(findings.size(), false);
  const auto join = [&](std::size_t k, MissingSplit& m) {
    const ShareFinding& f = findings[k];
    m.from = std::max(m.from, f.window_from);
    m.to = std::min(m.to, f.window_to);
    if (f.kind == ShareFinding::Kind::kStep && width(f) < width(findings[m.narrowest])) {
      m.narrowest = k;
    }
    ++m.explains;
    folded[k] = true;
  };
  for (const std::size_t k : anchors) {
    is_anchor[k] = true;
    const ShareFinding& f = findings[k];
    const auto it = std::find_if(splits.begin(), splits.end(),
                                 [&](const MissingSplit& m) { return explained_by(f, m); });
    if (it == splits.end()) {
      splits.push_back({k, k, f.implied, f.window_from, f.window_to, 0});
    } else {
      join(k, *it);
    }
  }
  for (std::size_t k = 0; k < findings.size(); ++k) {
    if (is_anchor[k] || stored_wrong(findings[k])) {
      continue;
    }
    const auto it = std::find_if(splits.begin(), splits.end(), [&](const MissingSplit& m) {
      return explained_by(findings[k], m);
    });
    if (it != splits.end()) {
      join(k, *it);
    }
  }
  for (const MissingSplit& m : splits) {
    ShareFinding& f = findings[m.anchor];
    if (m.narrowest != m.anchor) {
      // Reported on the narrowest step, with the likeliest's case for the split.
      ShareFinding at = std::move(findings[m.narrowest]);
      at.posterior = f.posterior;
      at.split = f.split;
      at.implied = f.implied;
      f = std::move(at);
    }
    f.window_from = m.from;
    f.window_to = m.to;
    f.explains = m.explains;
    if (m.explains > 0) {
      f.evidence.push_back({"steps_explained", static_cast<std::int64_t>(m.explains), 0.0,
                            "other steps and mismatches the same missing split explains"});
    }
  }
  std::size_t kept = 0;
  for (std::size_t k = 0; k < findings.size(); ++k) {
    if (!folded[k]) {
      if (kept != k) {
        findings[kept] = std::move(findings[k]);
      }
      ++kept;
    }
  }
  findings.resize(kept);
}

}  // namespace

StatementShareAnalysis analyze_statement_shares(const Series& series, const StatementViews& views,
                                                const StatementSettings& settings,
                                                const PriceSettings& price,
                                                const std::vector<LineItem>* items,
                                                const SeriesActions* actions, bool have_actions) {
  StatementShareAnalysis analysis;
  const StatementRows& st = series.statements;
  if (series.kind != SeriesKind::kStatements || st.size() == 0 || items == nullptr) {
    return analysis;
  }
  const std::vector<SplitAction> splits = splits_of(actions);
  Date latest = st.known_from.front();
  for (const Date d : st.known_from) {
    latest = std::max(latest, d);
  }
  const std::vector<std::size_t> weighted = share_items(st, items, Statement::kIncome);
  const std::vector<std::size_t> outstanding = share_items(st, items, Statement::kBalanceSheet);
  // A finding both views share is one, on the in-force version.
  std::map<std::tuple<std::size_t, std::size_t, int>, std::size_t> seen;
  for (const bool in_force : {true, false}) {
    const std::vector<std::size_t>& view = in_force ? views.in_force : views.first_reported;
    RowIndex rows;
    for (const std::size_t r : view) {
      rows[{series.date[r].days(), static_cast<int>(st.period_type[r]),
            static_cast<int>(st.statement[r])}] = r;
    }
    const ShareContext sc{series, settings, price, splits, latest, weighted, outstanding, rows};
    std::vector<ShareFinding> found;
    if (have_actions) {
      for (const Line& line : lines_of(series, view, settings)) {
        for (const std::size_t item : share_items(st, items, line.statement)) {
          judge_steps(sc, line, item, found);
        }
      }
    }
    judge_mismatch(sc, view, weighted, outstanding, found);
    for (ShareFinding& f : found) {
      (in_force ? f.in_force : f.first_reported) = true;
      const auto key = std::make_tuple(f.row, f.item, static_cast<int>(f.kind));
      const auto [it, fresh] = seen.emplace(key, analysis.findings.size());
      if (fresh) {
        analysis.findings.push_back(std::move(f));
      } else {
        analysis.findings[it->second].first_reported = true;
        analysis.findings[it->second].in_force =
            analysis.findings[it->second].in_force || f.in_force;
      }
    }
  }
  fold_missing_splits(analysis.findings);
  return analysis;
}

}  // namespace dorq
