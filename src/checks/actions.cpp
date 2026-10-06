#include "checks/actions.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/coverage.hpp"
#include "checks/price.hpp"
#include "dorq/number.hpp"
#include "stats/robust.hpp"

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

// DQ705 for dividend d of a series, if it is implausible.
std::optional<Violation> judge_dividend(const Series& s,
                                        const std::vector<DividendAction>& dividends, std::size_t d,
                                        const CheckInfo& info) {
  constexpr std::size_t kMinHistory = 3;
  constexpr double kOrder = 10.0;
  const DividendAction& dividend = dividends[d];
  const std::optional<std::size_t> before = close_before(s, dividend.ex_date);
  if (!before) {
    return std::nullopt;
  }
  const double prior_close = s.close[*before];
  const std::string amount = format_number(dividend.amount);
  const std::string ex = dividend.ex_date.to_string();
  Violation v;
  v.check = &info;
  v.date = dividend.ex_date;
  v.detail = {{"amount", dividend.amount}, {"ex_date", ex}};
  if (dividend.amount >= prior_close) {
    v.severity = Severity::kError;
    v.line = s.line[*before];
    v.message = "dividend " + amount + " ex " + ex + " is at or above the close before it, " +
                significant(prior_close, 6) + " on " + s.date[*before].to_string();
    v.detail.push_back({"previous_close", prior_close});
  } else {
    // The others' amounts, and their yields on the close before each: a split
    // changes the one, a price error the other; a slip in the amount changes both.
    std::vector<double> amounts;
    std::vector<double> yields;
    for (std::size_t o = 0; o < dividends.size(); ++o) {
      if (o == d) {
        continue;
      }
      amounts.push_back(dividends[o].amount);
      if (const auto close = close_before(s, dividends[o].ex_date)) {
        yields.push_back(dividends[o].amount / s.close[*close]);
      }
    }
    if (amounts.size() < kMinHistory || yields.size() < kMinHistory) {
      return std::nullopt;
    }
    const std::size_t count = amounts.size();
    const double usual = stats::median(std::move(amounts));
    const double ratio = dividend.amount / usual;
    const double yield_ratio = dividend.amount / prior_close / stats::median(std::move(yields));
    const auto off = [](double x) { return x >= kOrder || x <= 1.0 / kOrder; };
    if (!off(ratio) || !off(yield_ratio) || (ratio > 1.0) != (yield_ratio > 1.0)) {
      return std::nullopt;
    }
    v.severity = Severity::kWarn;
    v.message = "dividend " + amount + " ex " + ex + " is " + times(ratio) + " the series' usual " +
                format_number(usual) + " (the median of " + std::to_string(count) +
                " others), and " + times(yield_ratio) +
                " its usual yield: a decimal slip, or a special dividend";
    v.detail.push_back({"usual_amount", usual});
    v.detail.push_back({"ratio", ratio});
    v.detail.push_back({"yield_ratio", yield_ratio});
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
      .applies = Applies::kAny,
  };
  return kInfo;
}

void DividendImplausible::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.actions == nullptr) {
    return;
  }
  const std::vector<DividendAction>& dividends = context.actions->dividends;
  for (std::size_t d = 0; d < dividends.size(); ++d) {
    if (std::optional<Violation> v = judge_dividend(context.series, dividends, d, info())) {
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
