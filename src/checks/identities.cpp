// DQ903 identity-break and DQ904 period-tie: the statements' identities within a
// row, and the ties across periods (doc/checks/DQ903.md, DQ904.md; plan §4.2).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"
#include "statements/views.hpp"

namespace dorq {
namespace {

// ---------------------------------------------------------------------------
// DQ903: why an identity fails

// The four explanations of a gap beyond tolerance, with their log prior weights,
// and the log Bayes factors of the evidence for them. dorq's defaults, until
// DR-0911 fits them to fafnir's labels.
enum class Gap : std::uint8_t { kStructural, kReclassification, kFieldError, kUnexplained };
constexpr std::array<std::string_view, 4> kGapNames{"structural", "reclassification", "field_error",
                                                    "unexplained"};
// The prior weights are [statements.priors]' identity_* (their defaults exp(-1.4),
// exp(-3), exp(-1.05) and exp(-1.05)), which `dorq calibrate` fits.
constexpr double kPersistent = 4.0;      // the gap is the neighbouring periods' gap
constexpr double kNotPersistent = -2.0;  // ... it is not
// The identity fails in every neighbouring period too, by another amount: "a gap in
// every period is the company's structure" (fafnir's playbook), as a SPAC's
// temporary equity is, or minority interest before 2010.
constexpr double kNeighboursBreak = 3.0;
constexpr double kExplained = 3.0;  // an explained_by item is reported in the period
constexpr double kMatch = 4.0;      // the gap is a term, twice one, or one at a power of ten
constexpr double kNoMatch = -4.0;
constexpr double kOtherHolds = 6.0;  // another version holds, with the same total
constexpr double kNoOtherHolds = -4.0;

// The multiples of a stored term that a gap names it by: the term (dropped or
// counted twice), twice the term (its sign), and the term in thousands or millions
// where units were meant (10^k - 1), or the other way (1 - 10^-k).
constexpr std::array<double, 6> kMultiples{1.0, 2.0, 999.0, 999999.0, 0.999, 0.999999};
// A field error's gap is the term to rounding: it must match within this share of
// the identity's tolerance, so that a gap near some multiple of an unrelated term
// is not taken for it.
constexpr double kMatchShare = 0.2;

// An identity's terms as the series holds them: gap = sum of sign x value.
struct Terms {
  const Identity* identity = nullptr;
  std::vector<const std::vector<double>*> columns;
  std::vector<double> signs;  // +1 on the left, -1 on the right, negated for a "-" term
  std::vector<bool> left;
  std::vector<bool> zero_if_null;  // [statements] null_is_zero: a NULL is 0
  std::vector<const std::string*> names;
  std::vector<const std::vector<double>*> explaining;  // explained_by columns
  std::vector<const std::string*> explaining_names;
};

std::optional<Terms> terms_of(const Series& s, const Identity& identity,
                              const StatementSettings& settings) {
  Terms terms;
  terms.identity = &identity;
  for (const bool left : {true, false}) {
    for (const IdentityTerm& term : left ? identity.left : identity.right) {
      const std::vector<double>* column = s.statements.item(term.item);
      if (column == nullptr) {
        return std::nullopt;  // the series lacks the item: the identity is not judged
      }
      terms.columns.push_back(column);
      terms.signs.push_back((left ? 1.0 : -1.0) * (term.negative ? -1.0 : 1.0));
      terms.left.push_back(left);
      terms.zero_if_null.push_back(std::find(settings.null_is_zero.begin(),
                                             settings.null_is_zero.end(),
                                             term.item) != settings.null_is_zero.end());
      terms.names.push_back(&term.item);
    }
  }
  for (const auto& [name, items] : settings.explained_by) {
    if (name != identity.name) {
      continue;
    }
    for (const std::string& item : items) {
      if (const std::vector<double>* column = s.statements.item(item)) {
        terms.explaining.push_back(column);
        terms.explaining_names.push_back(&item);
      }
    }
  }
  return terms;
}

// An identity on one row.
struct Judged {
  bool complete = false;  // every term is there (none NULL)
  double gap = 0.0;       // left side minus right side
  double tolerance = 0.0;
  double total = 0.0;  // the left side
};

Judged judge_row(const Terms& terms, std::size_t i, const StatementSettings& settings) {
  Judged j;
  double scale = 0.0;
  for (std::size_t k = 0; k < terms.columns.size(); ++k) {
    double x = (*terms.columns[k])[i];
    if (std::isnan(x)) {
      if (!terms.zero_if_null[k]) {
        return Judged{};
      }
      x = 0.0;
    }
    j.gap += terms.signs[k] * x;
    if (terms.left[k]) {
      j.total += terms.signs[k] * x;
    }
    scale = std::max(scale, std::fabs(x));
  }
  j.complete = true;
  j.tolerance = std::max(settings.identity_share * scale, settings.tie_floor);
  return j;
}

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  return values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
}

// The nearest periods in force, two either side, of the row's period type: the
// identity, and its terms, as the company usually files them.
std::vector<std::size_t> neighbours_of(const Series& s, const std::vector<std::size_t>& line,
                                       std::size_t i) {
  const Date date = s.date[i];
  const auto at = std::lower_bound(line.begin(), line.end(), date,
                                   [&s](std::size_t r, Date d) { return s.date[r] < d; });
  std::vector<std::size_t> rows;
  for (auto it = at; it != line.begin() && rows.size() < 2;) {
    --it;
    rows.push_back(*it);
  }
  std::size_t after = 0;
  for (auto it = at; it != line.end() && after < 2; ++it) {
    if (s.date[*it] != date) {
      rows.push_back(*it);
      ++after;
    }
  }
  return rows;
}

// The rows of the period i is in, of any statement: indexes [first, last).
std::pair<std::size_t, std::size_t> same_date(const Series& s, std::size_t i) {
  const auto range = std::equal_range(s.date.begin(), s.date.end(), s.date[i]);
  return {static_cast<std::size_t>(range.first - s.date.begin()),
          static_cast<std::size_t>(range.second - s.date.begin())};
}

struct Match {
  std::size_t term = 0;
  double multiple = 0.0;
  double error = std::numeric_limits<double>::infinity();
};

// The gap a wrong stored value x of a term (signed c in the gap) leaves, by multiple,
// and the value it should have had (NaN where the term is right and the total
// wrong: a term dropped from it, or counted twice).
struct Pattern {
  double gap = 0.0;
  double should_be = std::numeric_limits<double>::quiet_NaN();
};

Pattern pattern(double m, double c, double x) {
  if (m == 2.0) {  // its sign
    return {2.0 * c * x, -x};
  }
  if (m < 1.0) {  // stored 10^k too large
    return {m * c * x, x * (1.0 - m)};
  }
  if (m > 2.0) {  // stored 10^k too small
    return {-m * c * x, x * (m + 1.0)};
  }
  return {c * x};
}

// The term a gap names: one whose stored value, wrong by one of the patterns,
// leaves this gap to rounding, and whose value put right sits with the same item in
// the neighbouring periods better than it does now. For a term dropped from the
// total, or counted twice, the gap is the term itself, of either sign.
std::optional<Match> field_match(const Terms& terms, const Judged& j, std::size_t i, double excess,
                                 const std::vector<std::size_t>& neighbours) {
  std::optional<Match> best;
  for (std::size_t k = 0; k < terms.columns.size(); ++k) {
    const std::vector<double>& column = *terms.columns[k];
    const double x = column[i];
    if (std::isnan(x)) {
      continue;  // a null_is_zero term read as 0: no stored value to be wrong
    }
    std::vector<double> around;
    for (const std::size_t r : neighbours) {
      if (!std::isnan(column[r])) {
        around.push_back(column[r]);
      }
    }
    const double usual =
        around.size() >= 2 ? median(around) : std::numeric_limits<double>::quiet_NaN();
    for (const double m : kMultiples) {
      // The total is no term of itself: the gap equals it when the other side is
      // missing, which names nothing.
      if (m == 1.0 && terms.left[k]) {
        continue;
      }
      // A pattern that leaves no gap beyond tolerance explains none. (A term stored
      // in thousands can itself be below it.)
      const Pattern p = pattern(m, terms.signs[k], x);
      if (std::fabs(p.gap) <= j.tolerance) {
        continue;
      }
      double error = std::fabs(excess - p.gap);
      if (m == 1.0) {
        error = std::min(error, std::fabs(excess + p.gap));
      }
      const bool fits = std::isnan(p.should_be) || std::isnan(usual) ||
                        std::fabs(p.should_be - usual) < std::fabs(x - usual);
      // The total wrong (at a power of ten, or its sign) is preferred to a term
      // dropped from it where both fit: the total is wrong in either reading.
      const auto rank = [&terms](const Match& a) {
        return std::pair{a.multiple == 1.0 && !terms.left[a.term], a.error};
      };
      const Match candidate{k, m, error};
      if (error <= kMatchShare * j.tolerance && fits && (!best || rank(candidate) < rank(*best))) {
        best = candidate;
      }
    }
  }
  return best;
}

// "x1000" or "x0.001": the power of ten a stored value is off by.
std::string power_text(int power) {
  if (power >= 0) {
    return "x1" + std::string(static_cast<std::size_t>(power), '0');
  }
  return "x0." + std::string(static_cast<std::size_t>(-power - 1), '0') + "1";
}

std::string match_text(const std::string& item, double multiple) {
  if (multiple == 1.0) {
    return "the gap equals " + item;
  }
  if (multiple == 2.0) {
    return "the gap is twice " + item + ": its sign";
  }
  // m = 10^k - 1: stored 10^k too small; m = 1 - 10^-k: 10^k too large.
  const int power = multiple > 2.0 ? -static_cast<int>(std::lround(std::log10(multiple + 1.0)))
                                   : static_cast<int>(std::lround(-std::log10(1.0 - multiple)));
  return "the gap is " + item + " stored at " + power_text(power);
}

// The evidence about one gap.
struct GapEvidence {
  bool persistent = false;
  std::string_view side = "around";        // where it persists: around, before or after
  bool neighbours_break = false;           // not persistent, but every neighbour fails too
  const std::string* explained = nullptr;  // the explained_by item reported
  std::optional<Match> match;
  bool other_holds = false;
  double baseline = 0.0;
  std::size_t neighbours = 0;
};

GapEvidence gather(const Series& s, const Terms& terms, const std::vector<Judged>& judged,
                   const std::vector<std::size_t>& line, std::size_t i) {
  const StatementRows& st = s.statements;
  const Judged& j = judged[i];
  GapEvidence e;
  const std::vector<std::size_t> neighbours = neighbours_of(s, line, i);
  std::vector<double> gaps;
  std::size_t breaking = 0;
  for (const std::size_t r : neighbours) {
    gaps.push_back(judged[r].gap);
    if (std::fabs(judged[r].gap) > judged[r].tolerance) {
      ++breaking;
    }
  }
  e.neighbours = gaps.size();
  e.baseline = gaps.size() >= 2 ? median(gaps) : 0.0;
  const double excess = j.gap - e.baseline;
  const double band = std::max(j.tolerance, 0.25 * std::fabs(j.gap));
  e.persistent =
      gaps.size() >= 2 && std::fabs(e.baseline) > j.tolerance && std::fabs(excess) <= band;
  // Or on one side: the gap starts here and carries on (a SPAC's first quarters,
  // an acquisition's), or carries on to here and stops.
  for (const bool before : {true, false}) {
    std::size_t same = 0;
    std::size_t count = 0;
    for (const std::size_t r : neighbours) {
      if ((s.date[r] < s.date[i]) == before) {
        ++count;
        if (std::fabs(judged[r].gap - j.gap) <= band) {
          ++same;
        }
      }
    }
    if (!e.persistent && count == 2 && same == 2) {
      e.persistent = true;
      e.side = before ? "before" : "after";
    }
  }
  e.neighbours_break = !e.persistent && gaps.size() >= 2 && breaking == gaps.size();
  const auto [first, last] = same_date(s, i);
  for (std::size_t r = first; r < last; ++r) {
    if (st.period_type[r] != st.period_type[i]) {
      continue;
    }
    for (std::size_t k = 0; k < terms.explaining.size() && e.explained == nullptr; ++k) {
      const double x = (*terms.explaining[k])[r];
      if (!std::isnan(x) && x != 0.0) {
        e.explained = terms.explaining_names[k];
      }
    }
    if (r != i && st.statement[r] == st.statement[i] && judged[r].complete &&
        std::fabs(judged[r].gap) <= judged[r].tolerance &&
        std::fabs(judged[r].total - j.total) <= j.tolerance) {
      e.other_holds = true;
    }
  }
  if (!e.persistent) {
    e.match = field_match(terms, j, i, excess, neighbours);
  }
  return e;
}

std::array<double, 4> posterior(const GapEvidence& e, const StatementPriors& priors) {
  // The configured prior weights ([statements.priors] identity_*), on the log scale.
  std::array<double, 4> score{
      std::log(priors.identity_structural), std::log(priors.identity_reclassification),
      std::log(priors.identity_field_error), std::log(priors.identity_unexplained)};
  const auto at = [&score](Gap g) -> double& { return score.at(static_cast<std::size_t>(g)); };
  at(Gap::kStructural) += e.persistent ? kPersistent : kNotPersistent;
  if (e.neighbours_break) {
    at(Gap::kStructural) += kNeighboursBreak;
    at(Gap::kUnexplained) -= 1.0;
  }
  if (e.explained != nullptr) {
    at(Gap::kStructural) += kExplained;
    at(Gap::kUnexplained) -= kExplained;
  }
  at(Gap::kReclassification) += e.other_holds ? kOtherHolds : kNoOtherHolds;
  at(Gap::kFieldError) += e.match ? kMatch : kNoMatch;
  if (e.match) {
    at(Gap::kUnexplained) += kNoMatch / 2.0;
  }
  if (e.persistent) {
    at(Gap::kFieldError) -= kPersistent;
    at(Gap::kUnexplained) -= kPersistent;
  }
  const double top = *std::max_element(score.begin(), score.end());
  double sum = 0.0;
  for (double& x : score) {
    x = std::exp(x - top);
    sum += x;
  }
  for (double& x : score) {
    x /= sum;
  }
  return score;
}

Violation report_gap(const CheckInfo& info, const SeriesContext& context, const Terms& terms,
                     const Judged& j, const GapEvidence& e, std::size_t i) {
  const Series& s = context.series;
  const std::array<double, 4> p = posterior(e, context.statement_config().priors);
  const auto best = static_cast<Gap>(std::max_element(p.begin(), p.end()) - p.begin());
  const double p_error = context.statement_config().calibration.apply(
      info.code, p.at(static_cast<std::size_t>(Gap::kFieldError)) +
                     p.at(static_cast<std::size_t>(Gap::kUnexplained)));
  const bool filing_fact = best == Gap::kStructural || best == Gap::kReclassification;
  Violation v = statement_finding(info, s, i, true);
  v.p_error = p_error;
  v.classification = filing_fact ? Classification::kMarketFact : Classification::kDataError;
  v.severity = filing_fact ? Severity::kInfo
                           : context.thresholds.for_probability(p_error).value_or(Severity::kInfo);
  const std::string& name = terms.identity->name;
  std::string message =
      name + " is off by " + amount_text(j.gap) + " (tolerance " + amount_text(j.tolerance) + "): ";
  switch (best) {
    case Gap::kStructural:
      if (e.persistent || e.neighbours_break) {
        message += e.persistent ? "as in the periods " + std::string{e.side} + " it"
                                : "it fails in the periods around it too";
        if (e.explained != nullptr) {
          message += ", with " + *e.explained + " reported";
        }
      } else {
        message += *e.explained + " is reported in the period";  // the only way here
      }
      break;
    case Gap::kReclassification:
      message += "another version holds, with the same " + *terms.names.front();
      break;
    case Gap::kFieldError:  // only with a match: no match weighs it down to the last
      message += e.match ? match_text(*terms.names[e.match->term], e.match->multiple)
                         : std::string{"no term explains it"};
      break;
    case Gap::kUnexplained:
      message += "no term explains it";
      break;
  }
  v.message = std::move(message);
  v.key.push_back({"identity", name});
  v.detail.push_back({"identity", name});
  v.detail.push_back({"equation", terms.identity->to_string()});
  v.detail.push_back({"gap", j.gap});
  v.detail.push_back({"tolerance", j.tolerance});
  if (e.neighbours >= 2) {
    v.detail.push_back({"neighbours_gap", e.baseline});
  }
  if (best == Gap::kFieldError && e.match) {
    const std::string& field = *terms.names[e.match->term];
    v.key.push_back({"item", field});
    v.detail.push_back({"field", field});
    v.suggested_action =
        SuggestedAction{"check_item",
                        {{"item", field}},
                        "check " + field + ": " + match_text(field, e.match->multiple)};
  }
  for (std::size_t h = 0; h < p.size(); ++h) {
    v.hypotheses.push_back({kGapNames.at(h), p.at(h)});
  }
  v.evidence.push_back(
      {"persistent", e.persistent, e.persistent ? kPersistent : kNotPersistent,
       e.neighbours >= 2 ? "the neighbouring periods' gap" : "too few neighbours"});
  if (e.neighbours_break) {
    v.evidence.push_back(
        {"neighbours_break", true, kNeighboursBreak, "every neighbouring period fails"});
  }
  if (e.explained != nullptr) {
    v.evidence.push_back({"explained_by", *e.explained, kExplained, ""});
  }
  v.evidence.push_back({"field_match", e.match ? *terms.names[e.match->term] : std::string{},
                        e.match ? kMatch : kNoMatch, ""});
  v.evidence.push_back(
      {"other_version_holds", e.other_holds, e.other_holds ? kOtherHolds : kNoOtherHolds, ""});
  return v;
}

void judge_identity(const CheckInfo& info, const SeriesContext& context, const Terms& terms,
                    std::vector<Violation>& out) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  const StatementSettings& settings = *context.statement_settings;
  std::vector<Judged> judged(st.size());
  for (std::size_t i = 0; i < st.size(); ++i) {
    judged[i] = judge_row(terms, i, settings);
  }
  // The periods in force, by period type, for the neighbours.
  std::array<std::vector<std::size_t>, 2> lines;
  for (const std::size_t r : context.statements->in_force) {
    if (judged[r].complete) {
      lines.at(st.period_type[r] == PeriodType::kAnnual ? 1 : 0).push_back(r);
    }
  }
  for (std::size_t i = 0; i < st.size(); ++i) {
    const Judged& j = judged[i];
    if (!j.complete || std::fabs(j.gap) <= j.tolerance) {
      continue;
    }
    const auto& line = lines.at(st.period_type[i] == PeriodType::kAnnual ? 1 : 0);
    const GapEvidence e = gather(s, terms, judged, line, i);
    out.push_back(report_gap(info, context, terms, j, e, i));
  }
}

// ---------------------------------------------------------------------------
// DQ904: the ties across periods

// The rows in force of one statement's item, by period type: (row) sorted by date.
std::array<std::vector<std::size_t>, 2> in_force_with(const SeriesContext& context,
                                                      const std::vector<double>& column) {
  const StatementRows& st = context.series.statements;
  std::array<std::vector<std::size_t>, 2> rows;
  for (const std::size_t r : context.statements->in_force) {
    if (!std::isnan(column[r])) {
      rows.at(st.period_type[r] == PeriodType::kAnnual ? 1 : 0).push_back(r);
    }
  }
  return rows;
}

// One tie of the change in cash: two balance sheets in force and the later
// period's cash flow.
struct CashTie {
  std::size_t before = 0;  // balance sheet rows
  std::size_t after = 0;
  std::size_t flow = 0;  // cash flow row
  double gap = 0.0;
  double tolerance = 0.0;
  bool broken = false;
};

std::vector<CashTie> cash_ties(const SeriesContext& context, const std::vector<std::size_t>& sheets,
                               const std::vector<std::size_t>& flows,
                               const std::vector<double>& cash, const std::vector<double>& change,
                               PeriodType type) {
  const Series& s = context.series;
  const StatementSettings& settings = *context.statement_settings;
  const auto [low, high] =
      type == PeriodType::kQuarter ? settings.quarter_spacing : settings.year_spacing;
  std::map<std::int32_t, std::size_t> flow_on;
  for (const std::size_t f : flows) {
    flow_on.emplace(s.date[f].days(), f);
  }
  std::vector<CashTie> ties;
  for (std::size_t k = 1; k < sheets.size(); ++k) {
    const std::size_t a = sheets[k - 1];
    const std::size_t b = sheets[k];
    const int spacing = s.date[b].days() - s.date[a].days();
    const auto f = flow_on.find(s.date[b].days());
    if (spacing < low || spacing > high || f == flow_on.end()) {
      continue;
    }
    CashTie tie{a, b, f->second};
    tie.gap = (cash[b] - cash[a]) - change[f->second];
    const double share = s.date[b] >= settings.restricted_cash_from
                             ? std::max(settings.restricted_cash_share, settings.cash_tie_share)
                             : settings.cash_tie_share;
    tie.tolerance =
        std::max(share * std::max(std::fabs(cash[a]), std::fabs(cash[b])), settings.tie_floor);
    tie.broken = std::fabs(tie.gap) > tie.tolerance;
    ties.push_back(tie);
  }
  return ties;
}

Violation tie_finding(const CheckInfo& info, const SeriesContext& context, std::size_t row,
                      double p_error) {
  const Series& s = context.series;
  Violation v = statement_finding(info, s, row, true);
  v.p_error = context.statement_config().calibration.apply(info.code, p_error);
  v.severity = context.thresholds.for_probability(v.p_error).value_or(Severity::kInfo);
  return v;
}

// The change in cash against the cash flow's net change. Two broken ties that
// cancel are one balance sheet's cash out of line with both cash flows around it:
// one report, on that balance sheet. A run of them pairs from its start.
void judge_cash(const CheckInfo& info, const SeriesContext& context, std::vector<Violation>& out) {
  const Series& s = context.series;
  const StatementSettings& settings = *context.statement_settings;
  const std::vector<double>* cash = s.statements.item(settings.cash_tie.at(0));
  const std::vector<double>* change = s.statements.item(settings.cash_tie.at(1));
  if (cash == nullptr || change == nullptr) {
    return;
  }
  const auto sheets = in_force_with(context, *cash);
  const auto flows = in_force_with(context, *change);
  for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
    const std::size_t t = type == PeriodType::kAnnual ? 1 : 0;
    const std::vector<CashTie> ties =
        cash_ties(context, sheets.at(t), flows.at(t), *cash, *change, type);
    for (std::size_t k = 0; k < ties.size(); ++k) {
      const CashTie& tie = ties[k];
      if (!tie.broken) {
        continue;
      }
      const bool pair =
          k + 1 < ties.size() && ties[k + 1].broken && ties[k + 1].before == tie.after &&
          std::fabs(tie.gap + ties[k + 1].gap) <= std::min(tie.tolerance, ties[k + 1].tolerance);
      if (pair) {
        Violation v = tie_finding(info, context, tie.after, 0.95);
        v.known = std::max({known_of(s, tie.before), known_of(s, tie.after), known_of(s, tie.flow),
                            known_of(s, ties[k + 1].flow)});
        v.message = settings.cash_tie.at(0) +
                    " is out of line with the cash flows either side: " + "off by " +
                    amount_text(tie.gap) + ", then by " + amount_text(ties[k + 1].gap);
        v.key.push_back({"tie", std::string{"cash"}});
        v.key.push_back({"item", settings.cash_tie.at(0)});
        v.detail.push_back({"item", settings.cash_tie.at(0)});
        v.detail.push_back({"gap", tie.gap});
        v.detail.push_back({"next_gap", ties[k + 1].gap});
        v.detail.push_back({"tolerance", tie.tolerance});
        v.hypotheses = {{"field_error", 0.95}, {"unexplained", 0.05}};
        out.push_back(std::move(v));
        ++k;
        continue;
      }
      Violation v = tie_finding(info, context, tie.flow, 0.85);
      v.known = std::max({known_of(s, tie.before), known_of(s, tie.after), known_of(s, tie.flow)});
      v.message = "the change in " + settings.cash_tie.at(0) + " since " +
                  s.date[tie.before].to_string() + " is not " + settings.cash_tie.at(1) +
                  ": off by " + amount_text(tie.gap) + " (tolerance " + amount_text(tie.tolerance) +
                  ")";
      v.key.push_back({"tie", std::string{"cash"}});
      v.detail.push_back({"previous_fiscal_date", s.date[tie.before].to_string()});
      v.detail.push_back({"gap", tie.gap});
      v.detail.push_back({"tolerance", tie.tolerance});
      v.hypotheses = {{"unexplained", 0.85}, {"structural", 0.15}};
      out.push_back(std::move(v));
    }
  }
}

// One flow item's gap between a year's quarters and the year, and the row that
// explains it best: the quarter whose own value, put right, sits with the other
// three, or the year when the quarters sit together.
struct SumGap {
  std::size_t item = 0;
  double gap = 0.0;
  double tolerance = 0.0;
  std::size_t target = 0;  // 0-3: a quarter; 4: the year
  double margin = 0.0;     // the runner-up's score over the winner's
};

std::optional<SumGap> quarter_gap(const std::vector<double>& column,
                                  const std::array<std::size_t, 4>& quarters, std::size_t year,
                                  const StatementSettings& settings) {
  std::array<double, 4> q{};
  double scale = std::fabs(column[year]);
  for (std::size_t k = 0; k < 4; ++k) {
    q.at(k) = column[quarters.at(k)];
    if (std::isnan(q.at(k))) {
      return std::nullopt;
    }
    scale = std::max(scale, std::fabs(q.at(k)));
  }
  if (std::isnan(column[year])) {
    return std::nullopt;
  }
  SumGap g;
  g.gap = q.at(0) + q.at(1) + q.at(2) + q.at(3) - column[year];
  g.tolerance = std::max(settings.identity_share * scale, settings.tie_floor);
  if (std::fabs(g.gap) <= g.tolerance) {
    return std::nullopt;
  }
  std::array<double, 5> score{};
  for (std::size_t k = 0; k < 4; ++k) {
    std::vector<double> others;
    for (std::size_t o = 0; o < 4; ++o) {
      if (o != k) {
        others.push_back(q.at(o));
      }
    }
    const double m = median(others);
    const double unit = std::max(std::fabs(m), g.tolerance);
    score.at(k) = std::fabs(q.at(k) - g.gap - m) / unit;                 // quarter k put right
    score.at(4) = std::max(score.at(4), std::fabs(q.at(k) - m) / unit);  // the year wrong
  }
  g.target = static_cast<std::size_t>(std::min_element(score.begin(), score.end()) - score.begin());
  const double best = score.at(g.target);
  double runner = std::numeric_limits<double>::infinity();
  for (std::size_t k = 0; k < score.size(); ++k) {
    if (k != g.target) {
      runner = std::min(runner, score.at(k));
    }
  }
  g.margin = runner - best;
  return g;
}

// The statement's flow items the series has, by their index in --items.
std::vector<std::pair<std::size_t, const std::vector<double>*>> flows_of(
    const SeriesContext& context, Statement statement) {
  std::vector<std::pair<std::size_t, const std::vector<double>*>> flows;
  for (std::size_t k = 0; k < context.items->size(); ++k) {
    const LineItem& item = (*context.items)[k];
    if (item.statement == statement && item.item_class == ItemClass::kFlow) {
      if (const std::vector<double>* column = context.series.statements.item(item.column)) {
        flows.emplace_back(k, column);
      }
    }
  }
  return flows;
}

// A fiscal year's four quarters in force against its one year in force, for each
// flow item of a statement. A year with another count of either is not judged:
// a change of fiscal year can label a transition period with the year it shares.
void judge_quarter_sums(const CheckInfo& info, const SeriesContext& context,
                        std::vector<Violation>& out) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  const StatementSettings& settings = *context.statement_settings;
  for (const Statement statement : {Statement::kIncome, Statement::kCashFlow}) {
    const auto flows = flows_of(context, statement);
    // By fiscal year: its quarters and years in force.
    std::map<std::int32_t, std::pair<std::vector<std::size_t>, std::vector<std::size_t>>> years;
    for (const std::size_t r : context.statements->in_force) {
      if (st.statement[r] == statement && st.fiscal_year[r] > 0) {
        auto& [quarters, annual] = years[st.fiscal_year[r]];
        (st.period_type[r] == PeriodType::kQuarter ? quarters : annual).push_back(r);
      }
    }
    for (const auto& [fiscal_year, rows] : years) {
      const auto& [quarters, annual] = rows;
      if (quarters.size() != 4 || annual.size() != 1 ||
          s.date[quarters.front()].days() <= s.date[annual.front()].days() - 380 ||
          s.date[annual.front()] < s.date[quarters.back()]) {
        continue;
      }
      const std::array<std::size_t, 4> q{quarters[0], quarters[1], quarters[2], quarters[3]};
      std::vector<SumGap> gaps;
      for (const auto& [item, column] : flows) {
        if (std::optional<SumGap> g = quarter_gap(*column, q, annual.front(), settings)) {
          g->item = item;
          gaps.push_back(*g);
        }
      }
      if (gaps.empty()) {
        continue;
      }
      // The item that breaks the most, relative to its tolerance, names the row.
      const SumGap& lead =
          *std::max_element(gaps.begin(), gaps.end(), [](const auto& a, const auto& b) {
            return std::fabs(a.gap) / a.tolerance < std::fabs(b.gap) / b.tolerance;
          });
      const std::size_t row = lead.target == 4 ? annual.front() : q.at(lead.target);
      std::vector<std::string> items;
      items.reserve(gaps.size());
      for (const SumGap& g : gaps) {
        items.push_back((*context.items)[g.item].column);
      }
      // A clear winner, or not. (The rest is written as it is, not as 1 - p.)
      const bool clear = lead.margin >= 0.5;
      const double p_error = clear ? 0.9 : 0.7;
      const double rest = clear ? 0.1 : 0.3;
      Violation v = tie_finding(info, context, row, p_error);
      Date known = known_of(s, annual.front());
      for (const std::size_t r : q) {
        known = std::max(known, known_of(s, r));
      }
      v.known = known;
      const std::string& lead_item = (*context.items)[lead.item].column;
      std::string message = "the quarters of FY" + std::to_string(fiscal_year) +
                            " do not sum to the year: " + lead_item + " by " +
                            amount_text(lead.gap);
      if (items.size() > 1) {
        message += ", and " + std::to_string(items.size() - 1) + " more items";
      }
      message += lead.target == 4 ? "; the year is out of line with its quarters"
                                  : "; this quarter is out of line";
      v.message = std::move(message);
      v.key.push_back({"tie", std::string{"quarter_sum"}});
      v.detail.push_back({"fiscal_year", static_cast<std::int64_t>(fiscal_year)});
      v.detail.push_back({"items", join(items, ",")});
      v.detail.push_back({"gap", lead.gap});
      v.detail.push_back({"tolerance", lead.tolerance});
      v.detail.push_back({"year_fiscal_date", s.date[annual.front()].to_string()});
      v.hypotheses = {{lead.target == 4 ? "year_error" : "quarter_error", p_error},
                      {"unexplained", rest}};
      out.push_back(std::move(v));
    }
  }
}

}  // namespace

const CheckInfo& IdentityBreak::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ903",
      .name = "identity-break",
      .summary =
          "a configured identity that fails beyond tolerance, weighed as a structural gap, a "
          "reclassification, a field error (named) or unexplained",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void IdentityBreak::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const StatementSettings* settings = context.statement_settings;
  if (s.kind != SeriesKind::kStatements || settings == nullptr || context.statements == nullptr) {
    return;
  }
  for (const Identity& identity : settings->identities) {
    if (const std::optional<Terms> terms = terms_of(s, identity, *settings)) {
      judge_identity(info(), context, *terms, out);
    }
  }
}

const CheckInfo& PeriodTie::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ904",
      .name = "period-tie",
      .summary =
          "a tie across periods that fails: the change in cash against the cash flow "
          "([statements] cash_tie), or a year's four quarters against the year (--items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void PeriodTie::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const StatementSettings* settings = context.statement_settings;
  if (s.kind != SeriesKind::kStatements || settings == nullptr || context.statements == nullptr) {
    return;
  }
  if (settings->cash_tie.size() == 2) {
    judge_cash(info(), context, out);
  }
  if (context.items != nullptr) {
    judge_quarter_sums(info(), context, out);
  }
}

}  // namespace dorq
