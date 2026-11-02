// DQ911 bad-value, DQ912 unit-error, DQ913 sign-error and DQ919 large-change: the
// statement value model's findings (statements/values.cpp), one code each, as the
// price model's are (doc/checks/DQ911.md to DQ919.md).

#include "statements/values.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "config/config.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"

namespace dorq {
namespace {

// "x1000" or "x0.001": the power of ten a stored value is off by.
std::string power_text(int power) {
  if (power >= 0) {
    return "x1" + std::string(static_cast<std::size_t>(power), '0');
  }
  return "x0." + std::string(static_cast<std::size_t>(-power - 1), '0') + "1";
}

std::string ratio_text(double ratio) {
  const double r = std::round(ratio * 10.0) / 10.0;
  return amount_text(r);
}

// The code a finding is reported under: its most probable error, or DQ919 where a
// real change is likelier than any error.
std::string_view code_of(const ValueFinding& f) {
  switch (f.kind) {
    case ValueKind::kCopied:
      return "DQ914";
    case ValueKind::kAbsentZero:
      return "DQ915";
    case ValueKind::kCumulative:
    case ValueKind::kAnnualInQuarter:
      return "DQ916";
    case ValueKind::kModel:
      break;
  }
  if (f.scope != ValueScope::kValue) {
    return "DQ912";
  }
  const auto real = static_cast<std::size_t>(ValueHypothesis::kRealChange);
  if (f.posterior.at(real) >= *std::max_element(f.posterior.begin(), f.posterior.end())) {
    return "DQ919";
  }
  switch (f.error()) {
    case ValueHypothesis::kUnitError:
      return "DQ912";
    case ValueHypothesis::kSignError:
      return "DQ913";
    default:
      return "DQ911";
  }
}

std::string message_of(std::string_view code, const Series& s, const ValueFinding& f) {
  const StatementRows& st = s.statements;
  const std::string item = f.item == kNoItem ? std::string{} : st.item_names[f.item];
  const std::string value = amount_text(f.value);
  const std::string expected = amount_text(std::round(f.expected));
  if (f.scope == ValueScope::kRow) {
    return "the row's money items are at " + power_text(f.power) +
           " against the periods around it (" + std::to_string(f.items) + " items)";
  }
  if (f.scope == ValueScope::kEra) {
    const char* periods = st.period_type[f.row] == PeriodType::kAnnual ? " years" : " quarters";
    std::string text = (f.item == kNoItem ? std::string{"the money items of "} : item + " for ") +
                       std::to_string(f.rows) + periods + ", " + s.date[f.row].to_string() +
                       " to " + s.date[f.last_row].to_string() + ", are at " + power_text(f.power) +
                       " against the periods either side";
    if (f.item == kNoItem) {
      text += " (" + std::to_string(f.items) + " items)";
    }
    if (f.open) {
      text += "; one step only, so the shorter side is taken for the wrong one";
    }
    return text;
  }
  const double ratio = f.expected != 0.0 ? f.value / f.expected : 0.0;
  if (code == "DQ912") {
    return item + " is " + value + ", expected about " + expected + ": stored at " +
           power_text(f.power);
  }
  if (code == "DQ913") {
    return item + " is " + value + ", expected about " + expected + ": its sign is flipped";
  }
  if (code == "DQ911") {
    return item + " is " + value + ", expected about " + expected + " (" + ratio_text(ratio) +
           " times): one value off the periods around it";
  }
  return item + " is " + value + ", expected about " + expected + " (" + ratio_text(ratio) +
         " times): a change the evidence calls real";
}

// DQ914 to DQ916: the quarterly structure's findings.
Violation structure_violation(const CheckInfo& info, const SeriesContext& context,
                              const ValueFinding& f, Severity severity) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  Violation v = statement_finding(info, s, f.row, true);
  v.severity = severity;
  v.p_error = f.p_error();
  v.provisional = f.provisional;
  const std::string fy = "FY" + std::to_string(f.fiscal_year);
  const std::string of = " (" + std::to_string(f.items) + " of " + std::to_string(f.present);
  std::string_view innocent = "real_change";
  std::string_view error;
  switch (f.kind) {
    case ValueKind::kCopied:
      error = "copied_period";
      v.message = "the row repeats the period before it, to the unit, on " +
                  std::to_string(f.items) + " of " + std::to_string(f.present) +
                  " items: a copy, against this series' own repeats";
      v.detail.push_back({"items", static_cast<std::int64_t>(f.items)});
      v.detail.push_back({"present", static_cast<std::int64_t>(f.present)});
      v.detail.push_back({"chance", f.chance});
      break;
    case ValueKind::kAbsentZero: {
      innocent = "real_zero";
      error = "absent_as_zero";
      const std::string& item = st.item_names[f.item];
      v.message = item + " is 0, reported either side (expected about " +
                  amount_text(std::round(f.expected)) + "): a value the filing did not carry";
      v.key.push_back({"item", item});
      v.detail.push_back({"item", item});
      v.detail.push_back({"expected", f.expected});
      v.detail.push_back({"repaired", f.repaired});
      break;
    }
    case ValueKind::kCumulative:
      error = "cumulative_quarter";
      v.end_date = s.date[f.last_row];
      v.known = std::max(known_of(s, f.row), known_of(s, f.last_row));
      v.message = "the quarters of " + fy +
                  " are stored year to date: Q2 to Q4 hold the sums from Q1, and Q4 the year" +
                  of + " flows)";
      v.detail.push_back({"fiscal_year", static_cast<std::int64_t>(f.fiscal_year)});
      v.detail.push_back({"items", static_cast<std::int64_t>(f.items)});
      v.detail.push_back({"present", static_cast<std::int64_t>(f.present)});
      break;
    case ValueKind::kAnnualInQuarter:
      error = "annual_in_quarter";
      v.message = "the quarter holds " + fy + "'s flows" + of + " equal the year's)";
      v.detail.push_back({"fiscal_year", static_cast<std::int64_t>(f.fiscal_year)});
      v.detail.push_back({"items", static_cast<std::int64_t>(f.items)});
      v.detail.push_back({"present", static_cast<std::int64_t>(f.present)});
      break;
    case ValueKind::kModel:
      break;
  }
  v.hypotheses = {{innocent, f.posterior[0]}, {error, f.posterior[1]}};
  v.evidence = f.evidence;
  SuggestedAction action{"check_filing", {}, {}};
  if (f.item != kNoItem) {
    action.fields.push_back({"item", st.item_names[f.item]});
  }
  action.fields.push_back({"fiscal_date", s.date[f.row].to_string()});
  if (const std::optional<Date>& filed = st.filing_date[f.row]) {
    action.fields.push_back({"filing_date", filed->to_string()});
  }
  action.text = "check " + std::string{f.item == kNoItem ? "the row" : st.item_names[f.item]} +
                " for " + s.date[f.row].to_string() +
                " against its filing, and report a vendor error to the vendor";
  v.suggested_action = std::move(action);
  return v;
}

std::string views_text(const ValueFinding& f) {
  if (f.in_force && f.first_reported) {
    return "both";
  }
  return f.in_force ? "in_force" : "first_reported";
}

void report(const CheckInfo& info, const SeriesContext& context, std::vector<Violation>& out) {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements || context.values == nullptr) {
    return;
  }
  const StatementRows& st = s.statements;
  for (const ValueFinding& f : context.values->findings) {
    const std::string_view code = code_of(f);
    if (code != info.code) {
      continue;
    }
    if (f.kind != ValueKind::kModel) {
      std::optional<Severity> severity = context.thresholds.for_probability(f.p_error());
      if (severity && f.provisional && *severity == Severity::kError) {
        severity = Severity::kWarn;
      }
      if (severity) {
        Violation v = structure_violation(info, context, f, *severity);
        v.detail.push_back({"views", views_text(f)});
        out.push_back(std::move(v));
      }
      continue;
    }
    const bool large_change = code == "DQ919";
    if (large_change && !f.notable) {
      continue;  // a real change of the size the item often makes: nothing to review
    }
    const double p_error = f.p_error();
    std::optional<Severity> severity =
        large_change ? Severity::kInfo : context.thresholds.for_probability(p_error);
    if (!severity) {
      continue;
    }
    if (f.provisional && *severity == Severity::kError) {
      severity = Severity::kWarn;  // the newest period: nothing after it yet
    }
    Violation v = statement_finding(info, s, f.row, true);
    v.severity = *severity;
    v.p_error = p_error;
    v.classification = large_change ? Classification::kMarketFact : Classification::kDataError;
    v.provisional = f.provisional;
    v.message = message_of(code, s, f);
    if (f.scope == ValueScope::kEra) {
      v.end_date = s.date[f.last_row];
      v.known = std::max(known_of(s, f.row), known_of(s, f.last_row));
    }
    const std::string item = f.item == kNoItem ? std::string{} : st.item_names[f.item];
    if (!item.empty()) {
      v.key.push_back({"item", item});
      v.detail.push_back({"item", item});
    }
    if (f.scope != ValueScope::kValue) {
      const std::string scope = f.scope == ValueScope::kRow ? "row" : "era";
      v.key.push_back({"scope", scope});
      v.detail.push_back({"scope", scope});
      v.detail.push_back({"power", static_cast<std::int64_t>(f.power)});
      v.detail.push_back({"rows", static_cast<std::int64_t>(f.rows)});
      v.detail.push_back({"items", static_cast<std::int64_t>(f.items)});
      if (f.scope == ValueScope::kEra) {
        v.detail.push_back({"last_fiscal_date", s.date[f.last_row].to_string()});
      }
    } else {
      v.detail.push_back({"value", f.value});
      v.detail.push_back({"expected", f.expected});
      v.detail.push_back({"score", f.score});
      if (!large_change) {
        v.detail.push_back({"repaired", f.repaired});
      }
      if (code == "DQ912") {
        v.detail.push_back({"power", static_cast<std::int64_t>(f.power)});
      }
    }
    v.detail.push_back({"views", views_text(f)});
    for (std::size_t h = 0; h < f.posterior.size(); ++h) {
      v.hypotheses.push_back({kValueHypothesisNames.at(h), f.posterior.at(h)});
    }
    v.evidence = f.evidence;
    if (!large_change) {
      // Repairs are the vendor's: dorq names the value and the filing to check.
      SuggestedAction action{"check_filing", {}, {}};
      if (!item.empty()) {
        action.fields.push_back({"item", item});
      }
      action.fields.push_back({"fiscal_date", s.date[f.row].to_string()});
      if (const std::optional<Date>& filed = st.filing_date[f.row]) {
        action.fields.push_back({"filing_date", filed->to_string()});
      }
      std::string text = "check " + (item.empty() ? std::string{"the row"} : item) + " for " +
                         s.date[f.row].to_string() + " against its filing";
      if (f.scope == ValueScope::kValue) {
        action.fields.push_back({"expected", f.repaired});
        text += " (expected about " + amount_text(std::round(f.repaired)) + ")";
      }
      action.text = text + ", and report a vendor error to the vendor";
      v.suggested_action = std::move(action);
    }
    out.push_back(std::move(v));
  }
}

}  // namespace

const CheckInfo& BadValue::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ911",
      .name = "bad-value",
      .summary =
          "one period's value off an item's series, which the periods around it and the "
          "row's identities do not bear out",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void BadValue::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& UnitError::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ912",
      .name = "unit-error",
      .summary =
          "a value, a whole row or a run of periods stored at a power of ten (x1000, "
          "x0.001, x10^6)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void UnitError::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& SignError::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ913",
      .name = "sign-error",
      .summary =
          "a value whose sign is flipped against the item's convention and its own "
          "history (--items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void SignError::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& CopiedPeriod::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ914",
      .name = "copied-period",
      .summary = "a row that repeats the period before it, to the unit, on most of its items, "
                 "against the series' own repeats ([statements] copy_min_items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void CopiedPeriod::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& AbsentAsZero::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ915",
      .name = "absent-as-zero",
      .summary = "a 0 where the item is reported in the periods either side: a value the "
                 "filing did not carry ([statements] never_absent)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void AbsentAsZero::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& CumulativeQuarters::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ916",
      .name = "cumulative-quarters",
      .summary = "a fiscal year's quarters stored year to date, or a quarter holding its year's "
                 "flows (--items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void CumulativeQuarters::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

const CheckInfo& LargeChange::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ919",
      .name = "large-change",
      .summary =
          "a large change the evidence calls real (an acquisition, a write-down), kept "
          "for review at info",
      .default_severity = Severity::kInfo,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void LargeChange::run(const SeriesContext& context, std::vector<Violation>& out) const {
  report(info(), context, out);
}

}  // namespace dorq
