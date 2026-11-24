#include "checks/actions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/coverage.hpp"
#include "checks/price.hpp"
#include "dorq/number.hpp"
#include "stats/robust.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

std::string times(double factor) { return "×" + significant(factor, 4); }

// "2 sessions later", "1 session earlier".
std::string sessions_away(int sessions) {
  const int n = std::abs(sessions);
  return std::to_string(n) + (n == 1 ? " session " : " sessions ") +
         (sessions > 0 ? "later" : "earlier");
}

// Describes one split on file the bars contradict.
class SplitBuilder {
 public:
  SplitBuilder(const SeriesContext& context, const PriceAnalysis& analysis,
               const SplitFinding& finding)
      : s_(context.series),
        f_(analysis.features),
        finding_(finding),
        ratio_(finding.split.ratio_text()),
        ex_(finding.split.ex_date.to_string()),
        field_(field_name(Field::kClose, context.series.kind)) {}

  [[nodiscard]] Date date(std::size_t bar) const { return s_.date[f_.row[bar]]; }
  [[nodiscard]] double factor(std::size_t bar) const {
    return s_.close[f_.row[bar]] / s_.close[f_.row[bar - 1]];
  }

  // The violation's common part: what is on file, and how the bars moved there.
  void start(Violation& v) const {
    const std::size_t at = finding_.ex_bar;
    v.date = finding_.split.ex_date;
    v.line = s_.line[f_.row[at]];
    v.message = "split " + ratio_ + " on file ex " + ex_;
    v.detail = {{"ratio", ratio_},
                {"ex_date", ex_},
                {"expected_factor", finding_.split.price_factor()},
                {"factor_at_ex_date", factor(at)},
                {"bar_at_ex_date", date(at).to_string()}};
  }

  void misdated(Violation& v, SuggestedAction& action) const {
    const std::size_t j = finding_.other_bar.value_or(finding_.ex_bar);
    const std::string moved = date(j).to_string();
    v.message += ", but the " + field_ + " moves " + times(factor(j)) + " on " + moved + ", " +
                 sessions_away(finding_.sessions_off) + ", and not on the ex-date (" +
                 times(factor(finding_.ex_bar)) + ")";
    v.detail.push_back({"move_date", moved});
    v.detail.push_back({"factor_at_move", factor(j)});
    v.detail.push_back({"sessions_off", std::int64_t{finding_.sessions_off}});
    action.kind = "redate_split";
    action.fields = {{"ratio", ratio_}, {"ex_date", ex_}, {"new_ex_date", moved}};
    action.text = "move the " + ratio_ + " split from ex " + ex_ + " to ex " + moved;
  }

  void no_jump(Violation& v, SuggestedAction& action) const {
    const std::size_t at = finding_.ex_bar;
    if (finding_.duplicate_of && finding_.other_bar) {
      // Another row on file at the same ratio has the move: this one repeats it.
      const std::string other = finding_.duplicate_of->to_string();
      const std::size_t j = *finding_.other_bar;
      v.message += ", but the " + field_ + " moves " + times(factor(at)) + " on " +
                   date(at).to_string() + ": a duplicate of the " + ratio_ + " split on file ex " +
                   other + ", whose move (" + times(factor(j)) + " on " + date(j).to_string() +
                   ") the bars show once";
      v.detail.push_back({"duplicate_of_ex_date", other});
      action.kind = "delete_split";
      action.fields = {{"ratio", ratio_}, {"ex_date", ex_}};
      action.text =
          "delete the " + ratio_ + " split ex " + ex_ + ", a duplicate of the one ex " + other;
      return;
    }
    v.message += ", but the " + field_ + " does not move by its ratio within 5 sessions (" +
                 times(factor(at)) + " on " + date(at).to_string() +
                 "): the split is not real, or the bars already include it";
    action.kind = "check_split";
    action.fields = {{"ratio", ratio_}, {"ex_date", ex_}};
    action.text = "check the " + ratio_ + " split ex " + ex_ +
                  ": remove it if it is not real, or re-fetch unadjusted bars";
  }

  void mismatch(Violation& v, SuggestedAction& action) const {
    v.message += ", but the " + field_ + " moves " + times(factor(finding_.ex_bar)) + " there (≈ " +
                 finding_.observed + ")";
    v.detail.push_back({"observed_ratio", finding_.observed});
    action.kind = "fix_split_ratio";
    action.fields = {{"ex_date", ex_}, {"ratio", ratio_}, {"new_ratio", finding_.observed}};
    action.text = "correct the split ex " + ex_ + " from " + ratio_ + " to " + finding_.observed;
  }

  void doubled(Violation& v, SuggestedAction& action) const {
    const std::size_t at = finding_.ex_bar;
    const std::size_t j = finding_.other_bar.value_or(at);
    v.message += ", and the " + field_ + " moves by its ratio twice: " + times(factor(at)) +
                 " on " + date(at).to_string() + " and " + times(factor(j)) + " on " +
                 date(j).to_string() + " (" + sessions_away(finding_.sessions_off) +
                 "): the split applied twice";
    v.detail.push_back({"second_move_date", date(j).to_string()});
    v.detail.push_back({"factor_at_second_move", factor(j)});
    // The bars carrying the second application, back to one.
    const std::size_t last = j > at ? f_.size() - 1 : at - 1;
    const double rescale = 1.0 / finding_.split.price_factor();
    v.end_date = date(std::max(j, at));
    action.kind = "rescale";
    action.fields = {
        {"first", date(j).to_string()}, {"last", date(last).to_string()}, {"factor", rescale}};
    action.text = "rescale " + date(j).to_string() + ".." + date(last).to_string() + " by " +
                  times(rescale) + ", undoing the second application";
  }

 private:
  const Series& s_;
  const PriceFeatures& f_;
  const SplitFinding& finding_;
  std::string ratio_;
  std::string ex_;
  std::string field_;
};

Violation split_violation(const SeriesContext& context, const PriceAnalysis& analysis,
                          const SplitFinding& finding, const CheckInfo& info, Severity severity) {
  const SplitBuilder b(context, analysis, finding);
  Violation v;
  v.check = &info;
  v.severity = finding.provisional ? std::min(severity, Severity::kWarn) : severity;
  v.p_error = finding.p_error;
  v.provisional = finding.provisional;
  b.start(v);
  SuggestedAction action;
  switch (finding.verdict) {
    case SplitVerdict::kMisdated:
      b.misdated(v, action);
      break;
    case SplitVerdict::kNoJump:
      b.no_jump(v, action);
      break;
    case SplitVerdict::kRatioMismatch:
      b.mismatch(v, action);
      break;
    case SplitVerdict::kDoubleApplied:
      b.doubled(v, action);
      break;
    case SplitVerdict::kConfirmed:
      break;
  }
  v.message += "; P(error) = " + format_probability(finding.p_error);
  if (finding.provisional) {
    v.message += ", provisional";
  }
  for (std::size_t i = 0; i < kSplitVerdicts; ++i) {
    v.hypotheses.push_back({to_string(static_cast<SplitVerdict>(i)), finding.posterior.at(i)});
  }
  v.suggested_action = std::move(action);
  return v;
}

// The row of the last usable close before a date.
std::optional<std::size_t> close_before(const Series& s, Date day) {
  const auto end = std::lower_bound(s.date.begin(), s.date.end(), day);
  for (auto i = static_cast<std::size_t>(end - s.date.begin()); i-- > 0;) {
    if (std::isfinite(s.close[i]) && s.close[i] > 0.0) {
      return i;
    }
  }
  return std::nullopt;
}

// The row of the first usable close on or after a date.
std::optional<std::size_t> close_on_or_after(const Series& s, Date day) {
  const auto begin = std::lower_bound(s.date.begin(), s.date.end(), day);
  for (auto i = static_cast<std::size_t>(begin - s.date.begin()); i < s.size(); ++i) {
    if (std::isfinite(s.close[i]) && s.close[i] > 0.0) {
      return i;
    }
  }
  return std::nullopt;
}

constexpr std::size_t kMinHistory = 3;  // other positive dividends needed to judge one

double normal_log_pdf(double x, double mean, double sd) noexcept {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}
constexpr double kOrder = 10.0;  // ten times off the usual, either way
// Daily accruals: small dividends on ex-dates at most this many calendar days apart
// (a weekend and a holiday), each at most usual / kAccrualShare.
constexpr int kAccrualGapDays = 5;
constexpr double kAccrualShare = 5.0;

// How a series pays (DQ705): which dividends are daily accruals, and which accrual
// begins a run of them after regular payments, the change of frequency.
struct PaymentRegimes {
  std::vector<bool> accrual;  // per dividend
  std::vector<bool> start;    // the first accrual after a regular payment
};

PaymentRegimes payment_regimes(const std::vector<DividendAction>& dividends) {
  const std::size_t n = dividends.size();
  PaymentRegimes out{std::vector<bool>(n, false), std::vector<bool>(n, false)};
  // A positive dividend's neighbours on other ex-dates (two rows on one date, a
  // dividend and a capital gain, are one payment), within kAccrualGapDays.
  const auto near = [&](std::size_t d, std::size_t o) {
    const int gap = std::abs(dividends[o].ex_date.days() - dividends[d].ex_date.days());
    return o != d && dividends[o].amount > 0.0 && gap > 0 && gap <= kAccrualGapDays;
  };
  const auto has_near = [&](std::size_t d, const std::vector<bool>& among) {
    for (std::size_t o = 0; o < n; ++o) {
      if (among[o] && near(d, o)) {
        return true;
      }
    }
    return false;
  };
  // The usual amount: the median of the payments with no other within a few days.
  const std::vector<bool> positive = [&] {
    std::vector<bool> p(n);
    for (std::size_t d = 0; d < n; ++d) {
      p[d] = dividends[d].amount > 0.0;
    }
    return p;
  }();
  std::vector<double> spaced;
  for (std::size_t d = 0; d < n; ++d) {
    if (positive[d] && !has_near(d, positive)) {
      spaced.push_back(dividends[d].amount);
    }
  }
  if (spaced.size() < kMinHistory) {
    return out;  // every payment comes close to another: one regime, judged as such
  }
  const double usual = stats::median(std::move(spaced));
  std::vector<bool> small(n);
  for (std::size_t d = 0; d < n; ++d) {
    small[d] = positive[d] && dividends[d].amount <= usual / kAccrualShare;
  }
  for (std::size_t d = 0; d < n; ++d) {
    out.accrual[d] = small[d] && has_near(d, small);
  }
  // A run of accruals starts where the payment before it on another date is a
  // regular one; at least kMinHistory regular payments must come before it.
  std::size_t regular_before = 0;
  std::optional<std::size_t> previous;  // the last positive dividend on an earlier date
  for (std::size_t d = 0; d < n; ++d) {
    if (!positive[d]) {
      continue;
    }
    const bool earlier_payment = previous && dividends[*previous].ex_date < dividends[d].ex_date;
    if (out.accrual[d] && regular_before >= kMinHistory && earlier_payment &&
        !out.accrual[*previous]) {
      out.start[d] = true;
    }
    regular_before += out.accrual[d] ? 0U : 1U;
    if (!previous || dividends[*previous].ex_date < dividends[d].ex_date) {
      previous = d;
    }
  }
  return out;
}

// What the close did across a dividend's ex-date: the close before, the first on
// or after, and the residual of the second from the first less the amount.
struct ExDateMove {
  double before = 0.0;
  double on = 0.0;
  double residual = 0.0;  // on - (before - amount)
  bool corroborated = false;
};

// DE-14: P(a slip) for a dividend `ratio` times its series' usual amount, from the
// ratio and, where the close on the ex-date is known, from it (judge_dividend).
// Sets `moved->corroborated`.
double slip_posterior(double ratio, const std::vector<double>& others, double usual,
                      double prior_close, double amount, const PriceSettings& settings,
                      std::optional<ExDateMove>& moved) {
  constexpr double kSlipBroad = 0.1;
  constexpr double kSlipSpread = 0.05;  // the least spread of a slip's ratio, on the log scale
  constexpr std::array<double, 3> kSlipPowers = {0.6, 0.3, 0.1};
  constexpr double kLogMost = 3.0 * std::numbers::ln10;  // a thousand times off
  constexpr double kSpan = 2.0 * std::numbers::ln10;     // from ten times off to a thousand
  const double log_ratio = std::log(ratio);
  std::vector<double> log_amounts;
  log_amounts.reserve(others.size());
  for (const double other : others) {
    log_amounts.push_back(std::log(other));
  }
  const double spread = std::hypot(kSlipSpread, stats::mad(log_amounts));
  const auto broad = [](double x) {  // anywhere from ten to a thousand times off, either way
    return -std::log(2.0 * kSpan) - std::max(std::fabs(x) - kLogMost, 0.0);
  };
  double slip = std::log(kSlipBroad) + broad(log_ratio);
  for (std::size_t k = 0; k < kSlipPowers.size(); ++k) {
    const double power = static_cast<double>(k + 1) * std::numbers::ln10;
    for (const double sign : {-1.0, 1.0}) {
      slip = stats::log_add(slip, std::log((1.0 - kSlipBroad) * kSlipPowers.at(k) / 2.0) +
                                      normal_log_pdf(log_ratio, sign * power, spread));
    }
  }
  double real = broad(log_ratio);
  if (moved) {
    const double tolerance = std::max(settings.dividend_tolerance * prior_close,
                                      settings.dividend_amount_tolerance * amount);
    const double usual_residual = moved->on - (prior_close - usual);
    moved->corroborated =
        std::fabs(moved->residual) <= tolerance && std::fabs(usual_residual) > tolerance;
    real += normal_log_pdf(moved->residual, 0.0, tolerance);
    slip += normal_log_pdf(usual_residual, 0.0, tolerance);
  }
  const double prior = settings.priors.dividend_slip;
  const double log_slip = std::log(prior) + slip;
  return std::exp(log_slip - stats::log_add(log_slip, std::log1p(-prior) + real));
}

// DQ705 for dividend d of a series, if it is implausible.
std::optional<Violation> judge_dividend(const Series& s,
                                        const std::vector<DividendAction>& dividends, std::size_t d,
                                        const PaymentRegimes& regimes,
                                        const PriceSettings& settings,
                                        const SeverityThresholds& thresholds,
                                        const CheckInfo& info) {
  const DividendAction& dividend = dividends[d];
  const std::string amount = format_number(dividend.amount);
  const std::string ex = dividend.ex_date.to_string();
  Violation v;
  v.check = &info;
  v.date = dividend.ex_date;
  v.detail = {{"amount", dividend.amount}, {"ex_date", ex}};
  // An amount of zero or less needs no close to judge; the others do.
  const std::optional<std::size_t> before = close_before(s, dividend.ex_date);
  if (dividend.amount > 0.0 && !before) {
    return std::nullopt;
  }
  const double prior_close = before ? s.close[*before] : 0.0;
  // The close on the ex-date (or the first bar after it), against the close before
  // less the amount.
  std::optional<ExDateMove> moved;
  if (before && dividend.amount > 0.0) {
    if (const auto on = close_on_or_after(s, dividend.ex_date)) {
      moved = ExDateMove{prior_close, s.close[*on], s.close[*on] - (prior_close - dividend.amount),
                         false};
    }
  }
  const auto add_residual = [&v, &moved] {
    if (moved) {
      v.detail.push_back({"ex_date_close", moved->on});
      v.detail.push_back({"residual", moved->residual});
      v.detail.push_back({"corroborated", moved->corroborated});
    }
  };
  // Every other positive dividend of the same regime: accruals are judged against
  // accruals, payments against payments.
  const auto others = [&](std::vector<double>& amounts, std::vector<double>& yields) {
    for (std::size_t o = 0; o < dividends.size(); ++o) {
      if (o == d || !(dividends[o].amount > 0.0) || regimes.accrual[o] != regimes.accrual[d]) {
        continue;
      }
      amounts.push_back(dividends[o].amount);
      if (const auto close = close_before(s, dividends[o].ex_date)) {
        yields.push_back(dividends[o].amount / s.close[*close]);
      }
    }
  };
  if (dividend.amount < 0.0) {
    v.severity = Severity::kError;
    v.message = "dividend " + amount + " ex " + ex + " is negative";
    v.detail.push_back({"kind", std::string{"negative"}});
  } else if (dividend.amount == 0.0) {
    // Harmless to an adjustment (its factor is 1), but no cash dividend pays
    // nothing: the feed lost the amount, or the event is not cash.
    v.severity = Severity::kWarn;
    v.message = "dividend 0 ex " + ex +
                " pays nothing: an amount missing from the feed, or a distribution that is not "
                "cash";
    v.detail.push_back({"kind", std::string{"zero"}});
  } else if (dividend.amount >= prior_close) {
    v.severity = Severity::kError;
    v.line = s.line[*before];
    v.message = "dividend " + amount + " ex " + ex + " is at or above the close before it, " +
                significant(prior_close, 6) + " on " + s.date[*before].to_string();
    v.detail.push_back({"previous_close", prior_close});
    v.detail.push_back({"kind", std::string{"exceeds_price"}});
    add_residual();
  } else if (regimes.start[d]) {
    // Daily accruals after regular payments: one report, at the first of them.
    std::vector<double> regular;
    std::size_t accruals = 0;
    std::vector<double> accrued;
    for (std::size_t o = 0; o < dividends.size(); ++o) {
      if (!(dividends[o].amount > 0.0)) {
        continue;
      }
      if (o < d && !regimes.accrual[o]) {
        regular.push_back(dividends[o].amount);
      } else if (o >= d && regimes.accrual[o] && (o == d || !regimes.start[o])) {
        ++accruals;
        accrued.push_back(dividends[o].amount);
      } else if (o > d && regimes.start[o]) {
        break;
      }
    }
    const double usual = stats::median(std::move(regular));
    const double each = stats::median(std::move(accrued));
    v.severity = Severity::kWarn;
    v.message = "dividends from " + ex + " come every few days (" + std::to_string(accruals) +
                " so far, a median " + format_number(each) + ", " + times(each / usual) +
                " the usual " + format_number(usual) +
                "): the fund now pays daily accruals. If the period's total also arrives as "
                "one dividend, the period is counted twice";
    v.detail.push_back({"usual_amount", usual});
    v.detail.push_back({"accrual_amount", each});
    v.detail.push_back({"ratio", each / usual});
    v.detail.push_back({"accruals", static_cast<std::int64_t>(accruals)});
    v.detail.push_back({"kind", std::string{"frequency_change"}});
    SuggestedAction action;
    action.kind = "check_dividend_frequency";
    action.fields = {{"from", ex}};
    action.text = "check the dividends from " + ex +
                  ": keep the daily accruals or the period's total, not both";
    v.suggested_action = std::move(action);
    return v;
  } else {
    // The others' amounts, and their yields on the close before each: a split
    // changes the one, a price error the other; a slip in the amount changes both.
    // A zero or negative other is reported on its own and is no measure of these.
    std::vector<double> amounts;
    std::vector<double> yields;
    others(amounts, yields);
    if (amounts.size() < kMinHistory || yields.size() < kMinHistory) {
      return std::nullopt;
    }
    const std::size_t count = amounts.size();
    const std::vector<double> spread_amounts = amounts;
    const double usual = stats::median(std::move(amounts));
    const double ratio = dividend.amount / usual;
    const double yield_ratio = dividend.amount / prior_close / stats::median(std::move(yields));
    const auto off = [](double x) { return x >= kOrder || x <= 1.0 / kOrder; };
    if (!off(ratio) || !off(yield_ratio) || (ratio > 1.0) != (yield_ratio > 1.0)) {
      return std::nullopt;
    }
    // DE-14: a slip (the amount stored at the wrong scale) against a real special
    // dividend, a distribution or a cut, by [priors] dividend_slip.
    // - The ratio. A slip's is a power of ten, ten to a thousand times either way
    //   (0.6, 0.3 and 0.1 of them), give or take the spread of the series' own
    //   amounts (a slip of a dividend twice the usual is twenty times it), or now
    //   and then anything else (kSlipBroad). A real one's is anywhere from ten to a
    //   thousand times off.
    // - The ex-date close. A real dividend comes off the price; after a slip, the
    //   usual amount does. Each a normal about its level, the larger of
    //   dividend_tolerance of the close and dividend_amount_tolerance of the amount
    //   wide.
    const double p =
        slip_posterior(ratio, spread_amounts, usual, prior_close, dividend.amount, settings, moved);
    const bool fell_by_it = p < 0.5;  // a real dividend, more than a slip
    v.p_error = p;
    // A real one is still reported, at info: the caller may want to know of it.
    v.severity = thresholds.for_probability(p).value_or(Severity::kInfo);
    v.classification = fell_by_it ? Classification::kMarketFact : Classification::kDataError;
    v.message = "dividend " + amount + " ex " + ex + " is " + times(ratio) + " the series' usual " +
                format_number(usual) + " (the median of " + std::to_string(count) +
                " others), and " + times(yield_ratio) + " its usual yield";
    if (moved) {
      v.message += "; the close went " + significant(prior_close, 6) + "→" +
                   significant(moved->on, 6) + " on the ex-date, where less the dividend is " +
                   significant(prior_close - dividend.amount, 6) + " and less the usual " +
                   significant(prior_close - usual, 6);
    }
    v.message += fell_by_it ? ": a special dividend or distribution (or a cut) more than a slip"
                            : ": an amount stored wrong (a slip) more than a special dividend";
    v.message += "; P(error) = " + format_probability(p);
    v.detail.push_back({"usual_amount", usual});
    v.detail.push_back({"ratio", ratio});
    v.detail.push_back({"yield_ratio", yield_ratio});
    v.detail.push_back({"kind", std::string{fell_by_it ? "special" : "slip"}});
    add_residual();
  }
  SuggestedAction action;
  action.kind = "fix_dividend";
  action.fields = {{"ex_date", ex}, {"amount", dividend.amount}};
  action.text = "check the dividend ex " + ex;
  v.suggested_action = std::move(action);
  return v;
}

}  // namespace

void SplitOnFileCheck::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.price_analysis == nullptr || !context.price_analysis->applicable) {
    return;
  }
  for (const SplitFinding& finding : context.price_analysis->split_findings) {
    if (finding.verdict != verdict_) {
      continue;
    }
    if (const auto severity = context.thresholds.for_probability(finding.p_error)) {
      out.push_back(
          split_violation(context, *context.price_analysis, finding, info_, severity.value()));
    }
  }
}

const CheckInfo& DividendImplausible::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ705",
      .name = "dividend-implausible",
      .summary = "a dividend on file at or above the price, or ten times off the series' others",
      .default_severity = Severity::kError,
      .applies = Applies::kTimeSeries,
  };
  return kInfo;
}

void DividendImplausible::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.actions == nullptr) {
    return;
  }
  const std::vector<DividendAction>& dividends = context.actions->dividends;
  const PaymentRegimes regimes = payment_regimes(dividends);
  for (std::size_t d = 0; d < dividends.size(); ++d) {
    if (std::optional<Violation> v = judge_dividend(context.series, dividends, d, regimes,
                                                    context.price, context.thresholds, info())) {
      out.push_back(std::move(*v));
    }
  }
}

const CheckInfo& split_misdated_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ701",
      .name = "split-misdated",
      .summary = "the bars move by a split's ratio a few sessions from its ex-date, not on it",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

const CheckInfo& split_without_jump_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ702",
      .name = "split-without-jump",
      .summary = "a split on file the bars do not move by: not real, or already applied",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

const CheckInfo& split_ratio_mismatch_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ703",
      .name = "split-ratio-mismatch",
      .summary = "the bars move on a split's ex-date, but by another ratio",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

const CheckInfo& split_double_applied_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ704",
      .name = "split-double-applied",
      .summary = "the bars move by a split's ratio twice, close together",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

}  // namespace dorq
