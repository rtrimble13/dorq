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

}  // namespace

void SplitOnFileCheck::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.price_analysis == nullptr || !context.price_analysis->applicable) {
    return;
  }
  const PriceAnalysis& analysis = *context.price_analysis;
  const Series& s = context.series;
  const PriceFeatures& f = analysis.features;
  const auto date = [&](std::size_t bar) { return s.date[f.row[bar]]; };
  const auto close = [&](std::size_t bar) { return s.close[f.row[bar]]; };
  const auto factor = [&](std::size_t bar) { return close(bar) / close(bar - 1); };
  for (const SplitFinding& finding : analysis.split_findings) {
    if (finding.verdict != verdict_) {
      continue;
    }
    std::optional<Severity> severity = context.thresholds.for_probability(finding.p_error);
    if (!severity) {
      continue;
    }
    const SplitAction& split = finding.split;
    const std::string ratio = split.ratio_text();
    const std::string ex = split.ex_date.to_string();
    const std::size_t at = finding.ex_bar;
    const std::string field{field_name(Field::kClose, s.kind)};
    Violation v;
    v.check = &info_;
    v.severity = finding.provisional ? std::min(*severity, Severity::kWarn) : *severity;
    v.p_error = finding.p_error;
    v.provisional = finding.provisional;
    v.date = split.ex_date;
    v.line = s.line[f.row[at]];
    v.message = "split " + ratio + " on file ex " + ex;
    v.detail = {{"ratio", ratio},
                {"ex_date", ex},
                {"expected_factor", split.price_factor()},
                {"factor_at_ex_date", factor(at)},
                {"bar_at_ex_date", date(at).to_string()}};
    SuggestedAction action;
    switch (verdict_) {
      case SplitVerdict::kMisdated: {
        const std::size_t j = *finding.other_bar;
        v.message += ", but the " + field + " moves " + times(factor(j)) + " on " +
                     date(j).to_string() + ", " + sessions_away(finding.sessions_off) +
                     ", and not on the ex-date (" + times(factor(at)) + ")";
        v.detail.push_back({"move_date", date(j).to_string()});
        v.detail.push_back({"factor_at_move", factor(j)});
        v.detail.push_back({"sessions_off", std::int64_t{finding.sessions_off}});
        action.kind = "redate_split";
        action.fields = {{"ratio", ratio}, {"ex_date", ex}, {"new_ex_date", date(j).to_string()}};
        action.text = "move the " + ratio + " split from ex " + ex + " to ex " + date(j).to_string();
        break;
      }
      case SplitVerdict::kNoJump:
        v.message += ", but the " + field + " does not move by its ratio within 5 sessions (" +
                     times(factor(at)) + " on " + date(at).to_string() +
                     "): the split is not real, or the bars already include it";
        action.kind = "check_split";
        action.fields = {{"ratio", ratio}, {"ex_date", ex}};
        action.text = "check the " + ratio + " split ex " + ex +
                      ": remove it if it is not real, or re-fetch unadjusted bars";
        break;
      case SplitVerdict::kRatioMismatch:
        v.message += ", but the " + field + " moves " + times(factor(at)) + " there (≈ " +
                     finding.observed + ")";
        v.detail.push_back({"observed_ratio", finding.observed});
        action.kind = "fix_split_ratio";
        action.fields = {{"ex_date", ex}, {"ratio", ratio}, {"new_ratio", finding.observed}};
        action.text = "correct the split ex " + ex + " from " + ratio + " to " + finding.observed;
        break;
      case SplitVerdict::kDoubleApplied: {
        const std::size_t j = *finding.other_bar;
        v.message += ", and the " + field + " moves by its ratio twice: " + times(factor(at)) +
                     " on " + date(at).to_string() + " and " + times(factor(j)) + " on " +
                     date(j).to_string() + " (" + sessions_away(finding.sessions_off) +
                     "): the split applied twice";
        v.detail.push_back({"second_move_date", date(j).to_string()});
        v.detail.push_back({"factor_at_second_move", factor(j)});
        // The bars carrying the second application, back to one.
        const std::size_t first = j;
        const std::size_t last = j > at ? f.size() - 1 : at - 1;
        const double rescale = 1.0 / split.price_factor();
        v.end_date = date(std::max(j, at));
        action.kind = "rescale";
        action.fields = {{"first", date(first).to_string()},
                         {"last", date(last).to_string()},
                         {"factor", rescale}};
        action.text = "rescale " + date(first).to_string() + ".." + date(last).to_string() +
                      " by " + times(rescale) + ", undoing the second application";
        break;
      }
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
    out.push_back(std::move(v));
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
  if (context.actions == nullptr || context.actions->dividends.empty()) {
    return;
  }
  const Series& s = context.series;
  const std::vector<DividendAction>& dividends = context.actions->dividends;
  constexpr std::size_t kMinHistory = 3;
  constexpr double kOrder = 10.0;
  // The row of the last usable close before a date.
  const auto close_before = [&s](Date day) -> std::optional<std::size_t> {
    const auto end = std::lower_bound(s.date.begin(), s.date.end(), day);
    for (auto i = static_cast<std::size_t>(end - s.date.begin()); i-- > 0;) {
      if (std::isfinite(s.close[i]) && s.close[i] > 0.0) {
        return i;
      }
    }
    return std::nullopt;
  };
  for (std::size_t d = 0; d < dividends.size(); ++d) {
    const DividendAction& dividend = dividends[d];
    const std::optional<std::size_t> before = close_before(dividend.ex_date);
    const std::string amount = format_number(dividend.amount);
    const std::string ex = dividend.ex_date.to_string();
    Violation v;
    v.check = &info();
    v.date = dividend.ex_date;
    v.detail = {{"amount", dividend.amount}, {"ex_date", ex}};
    if (before && dividend.amount >= s.close[*before]) {
      v.severity = Severity::kError;
      v.line = s.line[*before];
      v.message = "dividend " + amount + " ex " + ex + " is at or above the close before it, " +
                  significant(s.close[*before], 6) + " on " + s.date[*before].to_string();
      v.detail.push_back({"previous_close", s.close[*before]});
    } else {
      // The others' amounts, and their yields on the close before each: a split
      // changes the one, a price error the other; a slip in the amount changes
      // both.
      std::vector<double> amounts;
      std::vector<double> yields;
      for (std::size_t o = 0; o < dividends.size(); ++o) {
        if (o == d) {
          continue;
        }
        amounts.push_back(dividends[o].amount);
        if (const auto close = close_before(dividends[o].ex_date)) {
          yields.push_back(dividends[o].amount / s.close[*close]);
        }
      }
      if (amounts.size() < kMinHistory || yields.size() < kMinHistory || !before) {
        continue;
      }
      const std::size_t count = amounts.size();
      const double usual = stats::median(std::move(amounts));
      const double ratio = dividend.amount / usual;
      const double yield_ratio =
          dividend.amount / s.close[*before] / stats::median(std::move(yields));
      const auto off = [](double x) { return x >= kOrder || x <= 1.0 / kOrder; };
      if (!off(ratio) || !off(yield_ratio) || (ratio > 1.0) != (yield_ratio > 1.0)) {
        continue;
      }
      v.severity = Severity::kWarn;
      v.message = "dividend " + amount + " ex " + ex + " is " + times(ratio) +
                  " the series' usual " + format_number(usual) + " (the median of " +
                  std::to_string(count) + " others), and " + times(yield_ratio) +
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
    out.push_back(std::move(v));
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
