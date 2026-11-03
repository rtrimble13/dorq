// DQ941 to DQ944: the period sequence, coverage and freshness of a statements
// series (fundamentals plan §4.5; doc/checks/DQ941.md to DQ944.md).

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "context/context.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"

namespace dorq {
namespace {

constexpr double kQuarterDays = 91.3;
constexpr double kYearDays = 365.25;

// One period of a type: its end, its label and the row standing for it (the
// income statement's first version, where it has one).
struct Period {
  Date end;
  FiscalPeriod label = FiscalPeriod::kUnknown;
  std::size_t row = 0;
};

std::vector<Period> periods_of(const Series& s, PeriodType type) {
  const StatementRows& st = s.statements;
  std::vector<Period> out;
  for (std::size_t r = 0; r < st.size(); ++r) {
    if (st.period_type[r] != type) {
      continue;
    }
    const auto it =
        std::find_if(out.begin(), out.end(), [&](const Period& p) { return p.end == s.date[r]; });
    if (it == out.end()) {
      out.push_back({s.date[r], st.fiscal_period[r], r});
    } else if (st.statement[r] == Statement::kIncome &&
               (st.statement[it->row] != Statement::kIncome ||
                st.version[r] < st.version[it->row])) {
      it->row = r;
      it->label = st.fiscal_period[r];
    }
  }
  std::sort(out.begin(), out.end(), [](const Period& a, const Period& b) { return a.end < b.end; });
  return out;
}

// The quarter label `steps` after `label`, Q4 wrapping to Q1; kUnknown for one.
FiscalPeriod advance(FiscalPeriod label, int steps) {
  if (label < FiscalPeriod::kQ1 || label > FiscalPeriod::kQ4) {
    return FiscalPeriod::kUnknown;
  }
  const int q = static_cast<int>(label) - static_cast<int>(FiscalPeriod::kQ1);
  return static_cast<FiscalPeriod>(static_cast<int>(FiscalPeriod::kQ1) + (q + steps) % 4);
}

bool known(FiscalPeriod label) { return label >= FiscalPeriod::kQ1 && label <= FiscalPeriod::kQ4; }

// The year ends a change of fiscal year leaves: a year that is not one apart from
// the year before it (a transition period of a few months, or of more than twelve).
std::vector<Date> transitions(const std::vector<Period>& years, const StatementSettings& settings) {
  std::vector<Date> out;
  for (std::size_t p = 1; p < years.size(); ++p) {
    const int gap = years[p].end.days() - years[p - 1].end.days();
    if ((gap < settings.year_spacing.first && gap > settings.quarter_spacing.first) ||
        (gap > settings.year_spacing.second && gap < 2 * settings.year_spacing.first)) {
      out.push_back(years[p].end);
    }
  }
  return out;
}

bool near_transition(const std::vector<Date>& ends, Date from, Date to) {
  return std::any_of(ends.begin(), ends.end(), [&](Date d) {
    return d.days() >= from.days() - 366 && d.days() <= to.days() + 366;
  });
}

// The span a security's statements are expected in: from its first trade and the
// coverage floor, to its last trade.
struct Span {
  Date from;
  std::optional<Date> to;
};

Span expected_span(const SeriesContext& context) {
  Span span{context.statement_settings != nullptr ? context.statement_settings->coverage_floor
                                                  : Date::from_ymd(1998, 1, 1),
            std::nullopt};
  if (const SeriesMeta* meta = context.meta) {
    if (meta->first_trade) {
      span.from = std::max(span.from, *meta->first_trade);
    }
    span.to = meta->last_trade;
    if (meta->delisted && (!span.to || *meta->delisted < *span.to)) {
      span.to = meta->delisted;
    }
  }
  return span;
}

// The issuer's usual lag from period end to its first filed version, by form (a
// 10-K for a year and its last quarter, a 10-Q otherwise): the median and spread
// of its own, or the SEC's deadlines where it has fewer than four.
struct Lag {
  double median = 0.0;
  double sd = 0.0;
};

Lag usual_lag(const Series& s, bool ten_k) {
  const StatementRows& st = s.statements;
  std::vector<double> lags;
  for (std::size_t r = 0; r < st.size(); ++r) {
    const bool k =
        st.period_type[r] == PeriodType::kAnnual || st.fiscal_period[r] == FiscalPeriod::kQ4;
    if (st.version[r] == 1 && st.knowledge_source[r] == KnowledgeSource::kFiled && k == ten_k &&
        st.statement[r] == Statement::kIncome) {
      lags.push_back(static_cast<double>(st.known_from[r].days() - s.date[r].days()));
    }
  }
  if (lags.size() < 4) {
    return {ten_k ? 75.0 : 42.0, 15.0};
  }
  std::sort(lags.begin(), lags.end());
  const double median = lags[lags.size() / 2];
  for (double& x : lags) {
    x = std::fabs(x - median);
  }
  std::sort(lags.begin(), lags.end());
  return {median, std::max(4.0, 1.4826 * lags[lags.size() / 2])};
}

// The latest knowledge date the series holds: what a period not yet filed is
// judged against.
Date latest_known(const Series& s) {
  Date latest = Date::from_days(0);
  for (std::size_t r = 0; r < s.statements.size(); ++r) {
    latest = std::max(latest, known_of(s, r));
  }
  return latest;
}

enum class Break : std::uint8_t { kMissing, kFiscalYearChange, kMislabel, kNotYetFiled };

Violation break_violation(const CheckInfo& info, const Series& s, const Period& at, Date date,
                          Break kind, const std::string& message, int missing) {
  Violation v = statement_finding(info, s, at.row, false);
  v.date = date;
  v.message = message;
  switch (kind) {
    case Break::kMissing:
      v.severity = Severity::kWarn;
      v.p_error = 0.85;
      break;
    case Break::kMislabel:
      v.severity = Severity::kWarn;
      v.p_error = 0.8;
      break;
    case Break::kFiscalYearChange:
    case Break::kNotYetFiled:
      v.severity = Severity::kInfo;
      v.classification = Classification::kMarketFact;
      v.p_error = kind == Break::kNotYetFiled ? 0.1 : 0.0;
      break;
  }
  constexpr std::array<const char*, 4> kNames{"missing_period", "fiscal_year_change", "mislabel",
                                              "not_yet_filed"};
  v.detail.push_back({"hypothesis", std::string{kNames.at(static_cast<std::size_t>(kind))}});
  if (missing > 0) {
    v.detail.push_back({"periods_missing", static_cast<std::int64_t>(missing)});
  }
  return v;
}

// A gap of `missing` periods between a and b: missing, a change of fiscal year,
// or not yet filed.
void judge_gap(const CheckInfo& info, const SeriesContext& context, const Period& a,
               const Period& b, PeriodType type, int missing, bool transition,
               std::vector<Violation>& out) {
  const Series& s = context.series;
  const Span span = expected_span(context);
  const int gap = b.end.days() - a.end.days();
  // The missing periods' ends, evenly between a and b: on month ends where a and b
  // are on one.
  const auto month_end = [](Date d) { return Date::from_days(d.days() + 1).day() == 1; };
  const bool monthly = month_end(a.end) && month_end(b.end);
  const int step_months = type == PeriodType::kQuarter ? 3 : 12;
  std::vector<Date> ends;
  for (int j = 1; j <= missing; ++j) {
    Date end = Date::from_days(a.end.days() + gap * j / (missing + 1));
    if (monthly) {
      const int months = a.end.year() * 12 + (a.end.month() - 1) + step_months * j + 1;
      end = Date::from_days(Date::from_ymd(months / 12, months % 12 + 1, 1).days() - 1);
    }
    if (end >= span.from && (!span.to || end <= *span.to)) {
      ends.push_back(end);
    }
  }
  if (ends.empty()) {
    return;  // outside the security's trading life, or before the coverage floor
  }
  const bool quarters = type == PeriodType::kQuarter;
  const char* what = quarters ? "quarter" : "year";
  const bool in_cycle =
      !quarters || !known(a.label) || !known(b.label) || advance(a.label, missing + 1) == b.label;
  if (transition && !in_cycle) {
    out.push_back(break_violation(info, s, b, ends.front(), Break::kFiscalYearChange,
                                  "a change of fiscal year: " + std::to_string(gap) +
                                      " days from " + a.end.to_string() + " to " +
                                      b.end.to_string() + ", where the labels rebase",
                                  0));
    return;
  }
  // Filed after the latest knowledge date the series holds: not yet filed.
  const Lag lag = usual_lag(s, !quarters);
  if (ends.front().days() + static_cast<int>(lag.median + 3.0 * lag.sd) > latest_known(s).days()) {
    out.push_back(break_violation(
        info, s, b, ends.front(), Break::kNotYetFiled,
        "the " + std::string{what} + " ending about " + ends.front().to_string() +
            " is not in the input, but would be filed after its latest knowledge date",
        static_cast<int>(ends.size())));
    return;
  }
  const std::string count =
      std::to_string(ends.size()) + " " + what + (ends.size() == 1 ? "" : "s");
  Violation v = break_violation(info, s, b, ends.front(), Break::kMissing,
                                count + " missing between " + a.end.to_string() + " and " +
                                    b.end.to_string() + " (" + std::to_string(gap) +
                                    " days), while the security traded" +
                                    (in_cycle ? "" : "; the labels do not follow either"),
                                static_cast<int>(ends.size()));
  if (ends.size() > 1) {
    v.end_date = ends.back();
  }
  SuggestedAction action{"load_period", {}, {}};
  action.fields.push_back({"fiscal_date", ends.front().to_string()});
  action.text = "load the " + std::string{what} + " ending about " + ends.front().to_string() +
                " from its filing";
  v.suggested_action = std::move(action);
  out.push_back(std::move(v));
}

// One period type's sequence: each break in its spacing or its labels.
void judge_sequence(const CheckInfo& info, const SeriesContext& context, PeriodType type,
                    const std::vector<Date>& year_transitions, std::vector<Violation>& out) {
  const Series& s = context.series;
  const StatementSettings& settings = *context.statement_settings;
  const std::vector<Period> periods = periods_of(s, type);
  const bool quarters = type == PeriodType::kQuarter;
  const auto [low, high] = quarters ? settings.quarter_spacing : settings.year_spacing;
  const double length = quarters ? kQuarterDays : kYearDays;
  // The label each period should have had: a mislabelled one's is the cycle's, so
  // that the quarter after it is judged against that.
  std::vector<FiscalPeriod> labels;
  labels.reserve(periods.size());
  for (const Period& period : periods) {
    labels.push_back(period.label);
  }
  for (std::size_t p = 1; p < periods.size(); ++p) {
    Period a = periods[p - 1];
    a.label = labels[p - 1];
    const Period& b = periods[p];
    const int gap = b.end.days() - a.end.days();
    const bool transition = near_transition(year_transitions, a.end, b.end);
    if (gap > high) {
      const int missing = std::max(1, static_cast<int>(std::lround(gap / length)) - 1);
      judge_gap(info, context, a, b, type, missing, transition, out);
      continue;
    }
    if (!quarters) {
      if (gap < low && gap > settings.quarter_spacing.first) {
        out.push_back(break_violation(info, s, b, b.end, Break::kFiscalYearChange,
                                      "a change of fiscal year: the year ending " +
                                          b.end.to_string() + " is a transition of " +
                                          std::to_string(std::lround(gap / 30.44)) + " months",
                                      0));
      }
      continue;
    }
    if (gap < low || !known(a.label) || !known(b.label) || advance(a.label, 1) == b.label) {
      continue;
    }
    // One quarter apart, but the label skips: a change of fiscal year, where the new
    // labels hold after it, or one quarter mislabelled, where the old ones resume.
    const Period* c = p + 1 < periods.size() ? &periods[p + 1] : nullptr;
    const bool resumes = c != nullptr && advance(a.label, 2) == c->label;
    if (transition && !resumes) {
      out.push_back(break_violation(
          info, s, b, b.end, Break::kFiscalYearChange,
          "a change of fiscal year: the quarter ending " + b.end.to_string() + " is " +
              std::string{to_string(b.label)} + " where " +
              std::string{to_string(advance(a.label, 1))} + " would follow, and the labels rebase",
          0));
      continue;
    }
    labels[p] = advance(a.label, 1);
    Violation v = break_violation(info, s, b, b.end, Break::kMislabel,
                                  "the quarter ending " + b.end.to_string() + " is labelled " +
                                      std::string{to_string(b.label)} + " one quarter after " +
                                      std::string{to_string(a.label)} + ": " +
                                      std::string{to_string(advance(a.label, 1))} +
                                      " was due, and no period is missing" +
                                      (resumes ? " (the labels after it resume the cycle)" : ""),
                                  0);
    SuggestedAction action{"check_labels", {}, {}};
    action.fields.push_back({"fiscal_date", b.end.to_string()});
    action.fields.push_back({"fiscal_period", std::string{to_string(advance(a.label, 1))}});
    action.text = "check the fiscal period of the quarter ending " + b.end.to_string();
    v.suggested_action = std::move(action);
    out.push_back(std::move(v));
  }
}

}  // namespace

const CheckInfo& PeriodGap::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ941",
      .name = "period-gap",
      .summary =
          "a period missing from the quarterly or annual sequence while the security "
          "traded, told apart from a change of fiscal year and a mislabelled quarter",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void PeriodGap::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements || s.size() == 0 || context.statement_settings == nullptr) {
    return;
  }
  const std::vector<Date> years =
      transitions(periods_of(s, PeriodType::kAnnual), *context.statement_settings);
  judge_sequence(info(), context, PeriodType::kQuarter, years, out);
  judge_sequence(info(), context, PeriodType::kAnnual, years, out);
}

const CheckInfo& StatementsStale::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ942",
      .name = "statements-stale",
      .summary =
          "no newer period by the as-of date, judged by the issuer's own cadence and "
          "filing lag (--as-of; --meta's trading life)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void StatementsStale::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements || s.size() == 0 || !context.as_of) {
    return;
  }
  const Date as_of = *context.as_of;
  // The latest period, by quarters where the series has them.
  std::vector<Period> periods = periods_of(s, PeriodType::kQuarter);
  const bool quarters = !periods.empty();
  if (!quarters) {
    periods = periods_of(s, PeriodType::kAnnual);
  }
  if (periods.empty()) {
    return;
  }
  // Periods known by the as-of date only.
  while (!periods.empty() && s.statements.known_from[periods.back().row] > as_of) {
    periods.pop_back();
  }
  if (periods.empty()) {
    return;
  }
  const Period& latest = periods.back();
  const auto next_end = Date::from_days(
      latest.end.days() + static_cast<int>(std::lround(quarters ? kQuarterDays : kYearDays)));
  // The next quarter is a year's last when the series' latest year ends a year before it.
  bool ten_k = !quarters;
  if (quarters) {
    std::vector<Period> years = periods_of(s, PeriodType::kAnnual);
    while (!years.empty() && s.statements.known_from[years.back().row] > as_of) {
      years.pop_back();
    }
    ten_k = years.empty() ? latest.label == FiscalPeriod::kQ3
                          : std::abs(next_end.days() - years.back().end.days() - 365) <= 20;
  }
  const Lag lag = usual_lag(s, ten_k);
  const auto due = Date::from_days(next_end.days() + static_cast<int>(std::lround(lag.median)));
  // A security no longer trading at the as-of date is not stale: it files no more.
  constexpr int kTradingSlack = 30;  // days its last trade may lag the as-of date
  const Span span = expected_span(context);
  if (span.to && span.to->days() < as_of.days() - kTradingSlack) {
    return;
  }
  if (context.meta != nullptr && context.meta->active == false) {
    return;
  }
  const auto overdue = static_cast<double>(as_of.days() - due.days());
  if (overdue <= 0.0) {
    return;
  }
  // The chance a filing on time is still to come this late: the issuer's spread,
  // with a fat tail for the late filings every issuer has now and then.
  const auto upper = [](double z) { return 0.5 * std::erfc(z / std::numbers::sqrt2); };
  const double tail = 0.9 * upper(overdue / lag.sd) + 0.1 * upper(overdue / (4.0 * lag.sd));
  constexpr double kStalePrior = 0.02;
  const double p = context.statement_settings->calibration.apply(
      info().code, kStalePrior / (kStalePrior + (1.0 - kStalePrior) * tail));
  const std::optional<Severity> severity = context.thresholds.for_probability(p);
  if (!severity) {
    return;
  }
  Violation v = statement_finding(info(), s, latest.row, false);
  v.severity = *severity;
  v.p_error = p;
  v.end_date = as_of;
  const auto days = static_cast<std::int64_t>(overdue);
  v.message = "no period after " + latest.end.to_string() + " by " + as_of.to_string() +
              ": the next, ending " + next_end.to_string() + " (a " + (ten_k ? "10-K" : "10-Q") +
              "), was due about " + due.to_string() + " at this issuer's usual " +
              std::to_string(std::lround(lag.median)) + " days, " + std::to_string(days) +
              " days ago";
  v.detail.push_back({"latest_fiscal_date", latest.end.to_string()});
  v.detail.push_back({"next_period_end", next_end.to_string()});
  v.detail.push_back({"due", due.to_string()});
  v.detail.push_back({"as_of", as_of.to_string()});
  v.detail.push_back({"days_overdue", days});
  SuggestedAction action{"check_feed", {}, {}};
  action.text = "check whether the issuer filed for " + next_end.to_string() +
                " and the vendor has it, or the issuer is delinquent";
  v.suggested_action = std::move(action);
  out.push_back(std::move(v));
}

const CheckInfo& StatementsMissing::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ943",
      .name = "statements-missing",
      .summary =
          "a security the metadata expects statements for, with none in the input "
          "(--meta's statements_expected)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void StatementsMissing::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const SeriesMeta* meta = context.meta;
  if (s.kind != SeriesKind::kStatements || s.size() != 0 || meta == nullptr ||
      meta->statements_expected != true) {
    return;
  }
  // From its first trade, or the coverage floor, whichever is later, to its last.
  Date from = context.statement_settings != nullptr ? context.statement_settings->coverage_floor
                                                    : Date::from_ymd(1998, 1, 1);
  if (meta->first_trade) {
    from = std::max(from, *meta->first_trade);
  }
  if (meta->last_trade && *meta->last_trade < from) {
    return;  // it stopped trading before statements were expected
  }
  Violation v;
  v.check = &info();
  v.severity = Severity::kWarn;
  v.classification = Classification::kDataError;
  v.p_error = 1.0;
  v.date = from;
  v.end_date = meta->last_trade;
  v.message = "the metadata expects statements for this security, and the input has none";
  if (meta->first_trade || meta->last_trade) {
    v.message += ": it traded from " + from.to_string() +
                 (meta->last_trade ? " to " + meta->last_trade->to_string() : "");
  }
  v.detail.push_back({"from", from.to_string()});
  if (meta->last_trade) {
    v.detail.push_back({"to", meta->last_trade->to_string()});
  }
  SuggestedAction action{"load_statements", {}, {}};
  action.text = "load the vendor's statements for this security, or mark it as not expected";
  v.suggested_action = std::move(action);
  out.push_back(std::move(v));
}

const CheckInfo& ReportingCurrency::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ944",
      .name = "reporting-currency",
      .summary =
          "info: statements reported in a currency other than the one the security trades "
          "in (--meta's currency)",
      .default_severity = Severity::kInfo,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void ReportingCurrency::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const SeriesMeta* meta = context.meta;
  if (s.kind != SeriesKind::kStatements || meta == nullptr || meta->currency.empty()) {
    return;
  }
  const StatementRows& st = s.statements;
  // Each reporting currency other than the trading one: its first and last periods.
  struct Span {
    std::string currency;
    std::size_t first = 0;
    std::size_t last = 0;
  };
  std::vector<Span> spans;
  for (std::size_t r = 0; r < st.size(); ++r) {
    std::string currency = st.currency[r];
    std::transform(currency.begin(), currency.end(), currency.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (currency.empty() || currency == meta->currency) {
      continue;
    }
    const auto it = std::find_if(spans.begin(), spans.end(),
                                 [&](const Span& span) { return span.currency == currency; });
    if (it == spans.end()) {
      spans.push_back({currency, r, r});
    } else {
      if (s.date[r] < s.date[it->first]) {
        it->first = r;
      }
      if (s.date[r] > s.date[it->last]) {
        it->last = r;
      }
    }
  }
  for (const Span& span : spans) {
    Violation v = statement_finding(info(), s, span.first, false);
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    v.p_error = 0.0;
    if (s.date[span.last] != s.date[span.first]) {
      v.end_date = s.date[span.last];
    }
    v.message = "the statements are reported in " + span.currency +
                ", and the security trades in " + meta->currency +
                ": a per-share figure or a market cap needs the exchange rate";
    v.key.push_back({"currency", span.currency});
    v.detail.push_back({"reported_currency", span.currency});
    v.detail.push_back({"trading_currency", meta->currency});
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
