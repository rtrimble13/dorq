// DQ903 identity-break and DQ904 period-tie: the statements' identities within a
// row, and the ties across periods (doc/checks/DQ903.md, DQ904.md; plan §4.2).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
#include "dorq/number.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"
#include "statements/views.hpp"

namespace dorq {
namespace {

// ---------------------------------------------------------------------------
// DQ903: why an identity fails

// The four explanations of a gap beyond tolerance, with their log prior weights,
// and the log Bayes factors of the evidence for them (DE-11). Two are filing facts,
// the series' own process: structural (the gap the series usually has) and
// unexplained (a departure from it that no evidence calls an error). Two are data
// errors, a stored value that differs from its source: version_error (another
// version of the period holds) and field_error (a term named, a cancelling pair, or
// a break where the series has been clean).
enum class Gap : std::uint8_t { kStructural, kVersionError, kFieldError, kUnexplained };
constexpr std::array<std::string_view, 4> kGapNames{"structural", "version_error", "field_error",
                                                    "unexplained"};
// The prior weights are [statements.priors]' identity_* (their defaults exp(-1.4),
// exp(-3), exp(-1.05) and exp(-1.05)), which `dorq calibrate` fits. They hold for
// a series that breaks at [statements] break_rate; the series' own rate scales the
// filing facts (break_odds).
constexpr double kPersistent = 4.0;      // the gap is the neighbouring periods' gap
constexpr double kNotPersistent = -2.0;  // ... it is not
// The identity fails in every neighbouring period too, by another amount: "a gap in
// every period is the company's structure" (fafnir's playbook), as a SPAC's
// temporary equity is, or minority interest before 2010.
constexpr double kNeighboursBreak = 3.0;
constexpr double kExplained = 3.0;  // an explained_by item is reported in the period
constexpr double kMatch = 4.0;      // the gap is a term, twice one, or one at a power of ten
// DE-11: positive evidence against the series' own process. A field error leaves
// a term's pattern about half the time; the process does by chance in a share of
// its breaks the series' others show, shrunk toward kChanceMatch. So a match is
// log(kMatchPower / that share): about kMatch where the series' breaks match
// nothing, and weak where they often match some term.
constexpr double kMatchPower = 0.5;
constexpr double kChanceMatch = 0.02;  // at most log(0.5 / 0.02) = 3.2
// A stored value can be wrong in no pattern of a term: no match weighs a field
// error down only this much (DE-11; before, by 4, when an unexplained gap was an
// error).
constexpr double kNoMatchError = -1.0;
// The next or the last period's gap cancels this one, within this share of it: a
// value in the wrong period, which cancels nearly always. The process's breaks
// cancel by chance in a share its others show, shrunk toward kChanceCancel.
constexpr double kCancelWindow = 0.02;
constexpr double kCancelPower = 0.9;
constexpr double kChanceCancel = 0.01;  // at most log(0.9 / 0.01) = 4.5
constexpr double kCancel = 6.0;
// The gap sits among the series' other gaps, as a share of the scale, or apart.
constexpr double kGapFits = 2.0;
constexpr double kGapApart = -1.0;
constexpr double kGapSpread = 2.5;   // within this many robust spreads
constexpr double kOtherHolds = 6.0;  // another version holds, with the same total
constexpr double kNoOtherHolds = -4.0;

// The multiples of a stored term that a gap names it by: the term (dropped or
// counted twice), twice the term (its sign), and the term in thousands or millions
// where units were meant (10^k - 1), or the other way (1 - 10^-k).
constexpr std::array<double, 6> kMultiples{1.0, 2.0, 999.0, 999999.0, 0.999, 0.999999};
// A field error's gap is the term to rounding: it must match within this share of
// the identity's tolerance, so that a gap near some multiple of an unrelated term
// is not taken for it.
constexpr double kMatchShare = 0.05;

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
  double scale = 0.0;  // the largest term
  [[nodiscard]] bool broken() const noexcept { return complete && std::fabs(gap) > tolerance; }
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
  j.scale = scale;
  j.tolerance = std::max(settings.identity_share * scale, settings.tie_floor);
  return j;
}

// A series' own rate of breaks that are no error, for one identity or tie and
// period type (DE-11): `breaks` of its other `periods` break, shrunk toward the
// cross-section's rate by [statements] break_rate_strength periods of it.
struct BreakRate {
  std::size_t breaks = 0;
  std::size_t periods = 0;
  double rate = 0.0;
  // log(rate / break_rate): what the series' history adds to the filing facts,
  // against the rate the priors are weighed at. Below 0 where the series has been
  // clean, above where it breaks often.
  double log_odds = 0.0;
};

BreakRate break_rate(std::size_t breaks, std::size_t periods, std::string_view name,
                     const StatementSettings& settings) {
  BreakRate b{breaks, periods};
  const double cross = settings.break_rate_for(name);
  const double strength = settings.break_rate_strength;
  const double n = static_cast<double>(periods) + strength;
  b.rate = n > 0.0 ? (static_cast<double>(breaks) + strength * cross) / n : cross;
  b.rate = std::clamp(b.rate, 1e-6, 0.999);
  b.log_odds = std::log(b.rate / settings.break_rate);
  return b;
}

// A share as a percentage, to a tenth: "12.5".
std::string percent_text(double share) { return format_number(std::round(1000.0 * share) / 10.0); }

// "6 of 40 quarters": a break rate's history, for people.
std::string history_text(const BreakRate& b, PeriodType type) {
  return std::to_string(b.breaks) + " of " + std::to_string(b.periods) + " other " +
         (type == PeriodType::kAnnual ? "years" : "quarters");
}

// The median and robust spread of values: the MAD scaled to a standard deviation.
std::pair<double, double> center_spread(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  const double center =
      values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
  for (double& x : values) {
    x = std::fabs(x - center);
  }
  std::sort(values.begin(), values.end());
  const double mad = values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
  return {center, 1.4826 * mad};
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
  // DE-11: the series' own process for the identity and period type.
  BreakRate rate;
  double share = 0.0;            // the gap as a share of the largest term
  std::optional<bool> gap_fits;  // among the series' other gaps; none with fewer than 3
  double usual_share = 0.0;      // their median share
  std::string_view cancels;      // "last" or "next": that period's gap cancels this one
  double match_bf = kMatch;      // the log Bayes factors of a match and a cancelling pair,
  double cancel_bf = kCancel;    // against how often the series' other breaks show them
};

// What positive evidence says against the series' own process (DE-11): seen by
// chance in `seen` of the series' `breaks` other breaks, shrunk toward `chance` by
// kChanceStrength breaks of it; under an error with probability `power`. At most
// `cap`, and never evidence against an error. The shrinkage is strong because the
// other breaks include the series' real errors, whose evidence is no chance: two
// errors in a clean series must not each discount the other much.
constexpr double kChanceStrength = 20.0;

double chance_evidence(std::size_t seen, std::size_t breaks, double chance, double power,
                       double cap) {
  const double strength = kChanceStrength;
  const double n = static_cast<double>(breaks) + strength;
  const double p = n > 0.0 ? (static_cast<double>(seen) + strength * chance) / n : chance;
  return std::clamp(std::log(power / std::max(p, 1e-9)), 0.0, cap);
}

// Whether two gaps cancel: off by as much the other way, within the window.
bool cancel(double a, double b) {
  return a * b < 0.0 && std::fabs(a + b) <= kCancelWindow * std::max(std::fabs(a), std::fabs(b));
}

GapEvidence gather(const Series& s, const Terms& terms, const std::vector<Judged>& judged,
                   const std::vector<std::size_t>& line, std::size_t i,
                   const StatementSettings& settings) {
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
    // Another version holds with the same total, its gap gone and not merely
    // inside a wider tolerance: this version's values differ from it (DE-11).
    if (r != i && st.statement[r] == st.statement[i] && judged[r].complete &&
        std::fabs(judged[r].gap) <= judged[r].tolerance &&
        std::fabs(judged[r].total - j.total) <= j.tolerance &&
        std::fabs(judged[r].gap - j.gap) > j.tolerance) {
      e.other_holds = true;
    }
  }
  // The series' own process (DE-11): how often the identity breaks in its other
  // periods of the type, and by how much as a share of the scale.
  std::size_t breaks = 0;
  std::size_t periods = 0;
  std::vector<double> shares;
  for (const std::size_t r : line) {
    if (s.date[r] == s.date[i]) {
      continue;
    }
    ++periods;
    if (judged[r].broken()) {
      ++breaks;
      shares.push_back(judged[r].gap / judged[r].scale);
    }
  }
  e.rate = break_rate(breaks, periods, terms.identity->name, settings);
  e.share = j.scale > 0.0 ? j.gap / j.scale : 0.0;
  if (shares.size() >= 3) {
    const auto [center, spread] = center_spread(shares);
    const double width = std::max({spread, 0.1 * std::fabs(center), settings.identity_share});
    e.usual_share = center;
    e.gap_fits = std::fabs(e.share - center) <= kGapSpread * width;
  }
  if (!e.persistent) {
    e.match = field_match(terms, j, i, excess, neighbours);
    // A cancelling pair: the period before, or the one after, off by as much the
    // other way. A value booked in the wrong one of two periods.
    const auto cancels = [&](std::size_t r) {
      return judged[r].broken() && cancel(j.gap, judged[r].gap);
    };
    const auto at = std::lower_bound(line.begin(), line.end(), s.date[i],
                                     [&s](std::size_t r, Date d) { return s.date[r] < d; });
    auto after = at;
    while (after != line.end() && s.date[*after] == s.date[i]) {
      ++after;
    }
    if (at != line.begin() && cancels(*std::prev(at))) {
      e.cancels = "last";
    } else if (after != line.end() && cancels(*after)) {
      e.cancels = "next";
    }
  }
  return e;
}

std::array<double, 4> posterior(const GapEvidence& e, const StatementPriors& priors) {
  // The configured prior weights ([statements.priors] identity_*), on the log scale.
  std::array<double, 4> score{
      std::log(priors.identity_structural), std::log(priors.identity_version_error),
      std::log(priors.identity_field_error), std::log(priors.identity_unexplained)};
  const auto at = [&score](Gap g) -> double& { return score.at(static_cast<std::size_t>(g)); };
  // Not persistent is evidence against the series' structure only where its own
  // gaps do not account for this one (DE-11): an irregular process's gaps are its
  // structure too.
  if (e.persistent) {
    at(Gap::kStructural) += kPersistent;
  } else if (!e.gap_fits.value_or(false)) {
    at(Gap::kStructural) += kNotPersistent;
  }
  if (e.neighbours_break) {
    at(Gap::kStructural) += kNeighboursBreak;
    at(Gap::kUnexplained) -= 1.0;
  }
  if (e.explained != nullptr) {
    at(Gap::kStructural) += kExplained;
    at(Gap::kUnexplained) -= kExplained;
  }
  at(Gap::kVersionError) += e.other_holds ? kOtherHolds : kNoOtherHolds;
  at(Gap::kFieldError) += e.match ? e.match_bf : kNoMatchError;
  if (!e.cancels.empty()) {
    at(Gap::kFieldError) += e.cancel_bf;
  }
  if (e.persistent) {
    at(Gap::kFieldError) -= kPersistent;
    at(Gap::kUnexplained) -= kPersistent;
  }
  if (e.gap_fits) {
    at(Gap::kStructural) += *e.gap_fits ? kGapFits : kGapApart;
  }
  // The filing facts come at the series' own rate: a break where the series has
  // been clean is evidence of an error, one where it often breaks is not.
  at(Gap::kStructural) += e.rate.log_odds;
  at(Gap::kUnexplained) += e.rate.log_odds;
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

// What the winning explanation says, after "name is off by X (tolerance Y): ".
std::string explanation(Gap best, const GapEvidence& e, const Terms& terms, PeriodType type) {
  switch (best) {
    case Gap::kStructural: {
      std::string text;
      if (e.persistent || e.neighbours_break) {
        text = e.persistent ? "as in the periods " + std::string{e.side} + " it"
                            : "it fails in the periods around it too";
        if (e.explained != nullptr) {
          text += ", with " + *e.explained + " reported";
        }
      } else if (e.explained != nullptr) {
        text = *e.explained + " is reported in the period";
      } else {
        text = "the series' own gap: it breaks in " + history_text(e.rate, type);
        if (e.gap_fits) {
          text += ", by a median " + percent_text(e.usual_share) + "% of its largest term";
        }
      }
      return text;
    }
    case Gap::kVersionError:
      return "another version holds, with the same " + *terms.names.front();
    case Gap::kFieldError:
      if (e.match) {
        return match_text(*terms.names[e.match->term], e.match->multiple);
      }
      if (!e.cancels.empty()) {
        return "the " + std::string{e.cancels} +
               " period's gap cancels it: a value in the wrong period";
      }
      return "no term explains it, where the series holds: it breaks in " +
             history_text(e.rate, type);
    case Gap::kUnexplained:
      break;
  }
  return "no term explains it, and the series breaks in " + history_text(e.rate, type);
}

Violation report_gap(const CheckInfo& info, const SeriesContext& context, const Terms& terms,
                     const Judged& j, const GapEvidence& e, std::size_t i) {
  const Series& s = context.series;
  const StatementSettings& settings = context.statement_config();
  const PeriodType type = s.statements.period_type[i];
  const std::array<double, 4> p = posterior(e, settings.priors);
  const auto best = static_cast<Gap>(std::max_element(p.begin(), p.end()) - p.begin());
  const auto prob = [&p](Gap g) { return p.at(static_cast<std::size_t>(g)); };
  // A data error is a stored value that differs from its source: a field error or
  // another version's. A break nothing explains is one only where so configured.
  const double raw = prob(Gap::kFieldError) + prob(Gap::kVersionError) +
                     (settings.unexplained_is_error ? prob(Gap::kUnexplained) : 0.0);
  const double p_error = settings.calibration.apply(info.code, raw);
  Violation v = statement_finding(info, s, i, true);
  v.p_error = p_error;
  v.classification = p_error >= 0.5 ? Classification::kDataError : Classification::kMarketFact;
  v.severity = context.thresholds.for_probability(p_error).value_or(Severity::kInfo);
  const std::string& name = terms.identity->name;
  v.message = name + " is off by " + amount_text(j.gap) + " (tolerance " +
              amount_text(j.tolerance) + "): " + explanation(best, e, terms, type);
  v.key.push_back({"identity", name});
  v.detail.push_back({"identity", name});
  v.detail.push_back({"equation", terms.identity->to_string()});
  v.detail.push_back({"gap", j.gap});
  v.detail.push_back({"tolerance", j.tolerance});
  if (e.neighbours >= 2) {
    v.detail.push_back({"neighbours_gap", e.baseline});
  }
  v.detail.push_back({"share", e.share});
  v.detail.push_back({"breaks", static_cast<std::int64_t>(e.rate.breaks)});
  v.detail.push_back({"periods", static_cast<std::int64_t>(e.rate.periods)});
  v.detail.push_back({"break_rate", e.rate.rate});
  if (e.gap_fits) {
    v.detail.push_back({"usual_share", e.usual_share});
  }
  if (!e.cancels.empty()) {
    v.detail.push_back({"cancels", std::string{e.cancels}});
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
                        e.match ? e.match_bf : kNoMatchError, ""});
  if (!e.cancels.empty()) {
    v.evidence.push_back(
        {"cancelling", std::string{e.cancels}, e.cancel_bf, "that period's gap cancels this one"});
  }
  v.evidence.push_back(
      {"other_version_holds", e.other_holds, e.other_holds ? kOtherHolds : kNoOtherHolds, ""});
  v.evidence.push_back({"break_rate", e.rate.rate, e.rate.log_odds,
                        "breaks in " + history_text(e.rate, type) + ", against the rate " +
                            format_number(settings.break_rate)});
  if (e.gap_fits) {
    v.evidence.push_back({"gap_fits_series", *e.gap_fits, *e.gap_fits ? kGapFits : kGapApart,
                          "the series' other gaps, as a share of the largest term"});
  }
  return v;
}

// A chronic break (DE-11): the identity or tie fails in most periods of a type.
// One report for the series, keyed on no period, so that an item mapped wrong in
// every period stays in view while each period's own report is a filing fact. At
// warn when the median gap is at least [statements] chronic_warn_share of the
// scale, else at info.
struct Chronic {
  std::size_t breaks = 0;
  std::size_t periods = 0;
  double median_share = 0.0;  // of the broken periods' |gap| / scale
};

std::optional<Chronic> chronic_of(std::size_t breaks, std::size_t periods,
                                  std::vector<double> shares, const StatementSettings& settings) {
  if (periods < static_cast<std::size_t>(settings.chronic_min_periods) || breaks == 0 ||
      static_cast<double>(breaks) < settings.chronic_share * static_cast<double>(periods)) {
    return std::nullopt;
  }
  for (double& x : shares) {
    x = std::fabs(x);
  }
  return Chronic{breaks, periods, median(std::move(shares))};
}

Violation chronic_finding(const CheckInfo& info, const SeriesContext& context,
                          const std::vector<std::size_t>& rows, const Chronic& c,
                          Statement statement, PeriodType type, const std::string& what) {
  const Series& s = context.series;
  const StatementSettings& settings = context.statement_config();
  Violation v;
  v.check = &info;
  v.date = s.date[rows.front()];
  v.end_date = s.date[rows.back()];
  v.line = s.line[rows.front()];
  // The statement and period type, on no one period or version: its record_key is
  // theirs, with the first period's date.
  v.period = StatementPeriod{.statement = statement,
                             .period_type = type,
                             .fiscal_year = 0,
                             .fiscal_period = FiscalPeriod::kUnknown,
                             .version = std::nullopt};
  Date known = known_of(s, rows.front());
  for (const std::size_t r : rows) {
    known = std::max(known, known_of(s, r));
  }
  v.known = known;
  const bool warn = c.median_share >= settings.chronic_warn_share;
  v.severity = warn ? Severity::kWarn : Severity::kInfo;
  v.p_error =
      warn ? context.thresholds.warn : 0.5 * (context.thresholds.info + context.thresholds.warn);
  v.classification = warn ? Classification::kDataError : Classification::kMarketFact;
  const std::string periods = type == PeriodType::kAnnual ? "years" : "quarters";
  v.message = what + " fails in " + std::to_string(c.breaks) + " of " + std::to_string(c.periods) +
              " " + periods + " in force, by a median " + percent_text(c.median_share) +
              "% of the scale: the company's structure, or an item mapped wrong in every period";
  v.detail.push_back({"breaks", static_cast<std::int64_t>(c.breaks)});
  v.detail.push_back({"periods", static_cast<std::int64_t>(c.periods)});
  v.detail.push_back({"median_share", c.median_share});
  v.detail.push_back({"chronic", true});
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
  std::vector<std::optional<GapEvidence>> evidence(st.size());
  for (std::size_t i = 0; i < st.size(); ++i) {
    if (judged[i].broken()) {
      const auto& line = lines.at(st.period_type[i] == PeriodType::kAnnual ? 1 : 0);
      evidence[i] = gather(s, terms, judged, line, i, settings);
    }
  }
  for (std::size_t i = 0; i < st.size(); ++i) {
    if (!evidence[i]) {
      continue;
    }
    // How often the series' other breaks (of the period type, in force, not
    // persistent) match a term or cancel by chance: what a match or a pair here
    // says (DE-11).
    // Both period types: the vendor's process is the series', whatever the period.
    GapEvidence& e = *evidence[i];
    std::size_t others = 0;
    std::size_t matched = 0;
    std::size_t cancelled = 0;
    for (const auto& line : lines) {
      for (const std::size_t r : line) {
        if ((s.date[r] == s.date[i] && st.period_type[r] == st.period_type[i]) || !evidence[r] ||
            evidence[r]->persistent) {
          continue;
        }
        ++others;
        matched += static_cast<std::size_t>(evidence[r]->match.has_value());
        cancelled += static_cast<std::size_t>(!evidence[r]->cancels.empty());
      }
    }
    e.match_bf = chance_evidence(matched, others, kChanceMatch, kMatchPower, kMatch);
    e.cancel_bf = chance_evidence(cancelled, others, kChanceCancel, kCancelPower, kCancel);
    out.push_back(report_gap(info, context, terms, judged[i], e, i));
  }
  for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
    const auto& line = lines.at(type == PeriodType::kAnnual ? 1 : 0);
    std::size_t breaks = 0;
    std::vector<double> shares;
    for (const std::size_t r : line) {
      if (judged[r].broken()) {
        ++breaks;
        shares.push_back(judged[r].gap / judged[r].scale);
      }
    }
    if (const std::optional<Chronic> c = chronic_of(breaks, line.size(), shares, settings)) {
      const std::string& name = terms.identity->name;
      Violation v =
          chronic_finding(info, context, line, *c, st.statement[line.front()], type, name);
      v.key.insert(v.key.begin(), {"identity", name});
      v.detail.insert(v.detail.begin(), {"identity", name});
      v.detail.push_back({"equation", terms.identity->to_string()});
      out.push_back(std::move(v));
    }
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
  double scale = 0.0;  // the larger balance, or the period's largest flow (DE-11)
  bool broken = false;
};

// The cash flow row's largest flow: what moves through cash in the period, from
// the statement's flow items in --items; the net change alone without them.
double flow_scale(const SeriesContext& context, std::size_t row, double change) {
  double scale = std::fabs(change);
  if (context.items == nullptr) {
    return scale;
  }
  for (std::size_t k = 0; k < context.items->size(); ++k) {
    const LineItem& item = (*context.items)[k];
    if (item.statement == Statement::kCashFlow && item.item_class == ItemClass::kFlow) {
      if (const std::vector<double>* column = context.series.statements.item(item.column)) {
        const double x = (*column)[row];
        if (!std::isnan(x)) {
          scale = std::max(scale, std::fabs(x));
        }
      }
    }
  }
  return scale;
}

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
    // A reconciling difference (restricted cash, an exchange rate, a held-for-sale
    // reclassification) scales with what flows through cash, not only with what
    // is held (DE-11).
    tie.scale = std::max({std::fabs(cash[a]), std::fabs(cash[b]),
                          flow_scale(context, f->second, change[f->second])});
    tie.tolerance = std::max(share * tie.scale, settings.tie_floor);
    tie.broken = std::fabs(tie.gap) > tie.tolerance;
    ties.push_back(tie);
  }
  return ties;
}

// What a tie's break weighs (DE-11), as DQ903 weighs a gap but with no versions:
// structural (the series' own gap), field_error (a value stored wrong: a cancelling
// pair, a quarter or year out of line on its own, or a break where the series has
// been clean) and unexplained.
struct TieEvidence {
  BreakRate rate;
  std::optional<bool> gap_fits;  // among the series' other breaks; none with fewer than 3
  double usual_share = 0.0;
  bool named = false;     // a cancelling pair, or a clear quarter or year
  double named_bf = 0.0;  // its log Bayes factor
};

// The series' other breaks of a tie, as shares of their scales: (share) by tie.
TieEvidence tie_evidence(const std::vector<bool>& broken, const std::vector<double>& shares,
                         std::size_t k, std::size_t skip, std::string_view name,
                         const StatementSettings& settings) {
  TieEvidence e;
  std::size_t breaks = 0;
  std::size_t periods = 0;
  std::vector<double> others;
  for (std::size_t r = 0; r < broken.size(); ++r) {
    if (r == k || r == skip) {
      continue;
    }
    ++periods;
    if (broken[r]) {
      ++breaks;
      others.push_back(shares[r]);
    }
  }
  e.rate = break_rate(breaks, periods, name, settings);
  if (others.size() >= 3) {
    const auto [center, spread] = center_spread(others);
    const double width = std::max({spread, 0.1 * std::fabs(center), settings.identity_share});
    e.usual_share = center;
    e.gap_fits = std::fabs(shares[k] - center) <= kGapSpread * width;
  }
  return e;
}

// structural, field_error, unexplained.
std::array<double, 3> tie_posterior(const TieEvidence& e, const StatementPriors& priors) {
  std::array<double, 3> score{std::log(priors.identity_structural),
                              std::log(priors.identity_field_error),
                              std::log(priors.identity_unexplained)};
  score[1] += e.named ? e.named_bf : kNoMatchError;
  // The series' structure needs its own gaps to account for this one, as DQ903's
  // needs persistence: with too few breaks to say, nothing does.
  if (!e.gap_fits) {
    score[0] += kNotPersistent;
  } else {
    score[0] += *e.gap_fits ? kGapFits : kGapApart;
  }
  score[0] += e.rate.log_odds;
  score[2] += e.rate.log_odds;
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

// A broken tie's finding: its p_error the model's (field_error, and unexplained
// where so configured), through DQ904's own map when it has one (DE-02), else the
// sub-family's. `detail.score` is the model's, before any map.
Violation tie_finding(const CheckInfo& info, const SeriesContext& context, std::size_t row,
                      const TieEvidence& e, std::string_view error_name, PeriodType type) {
  const Series& s = context.series;
  const StatementSettings& settings = context.statement_config();
  const std::array<double, 3> p = tie_posterior(e, settings.priors);
  const double raw = p[1] + (settings.unexplained_is_error ? p[2] : 0.0);
  Violation v = statement_finding(info, s, row, true);
  const StatementCalibration& calibration = settings.calibration;
  v.p_error = calibration.apply_score(info.code, raw).value_or(calibration.apply(info.code, raw));
  v.classification = v.p_error >= 0.5 ? Classification::kDataError : Classification::kMarketFact;
  v.severity = context.thresholds.for_probability(v.p_error).value_or(Severity::kInfo);
  v.hypotheses = {{"structural", p[0]}, {error_name, p[1]}, {"unexplained", p[2]}};
  v.evidence.push_back({"break_rate", e.rate.rate, e.rate.log_odds,
                        "breaks in " + history_text(e.rate, type) + ", against the rate " +
                            format_number(settings.break_rate)});
  if (e.gap_fits) {
    v.evidence.push_back({"gap_fits_series", *e.gap_fits, *e.gap_fits ? kGapFits : kGapApart,
                          "the series' other breaks, as a share of the scale"});
  }
  v.detail.push_back({"score", raw});
  v.detail.push_back({"breaks", static_cast<std::int64_t>(e.rate.breaks)});
  v.detail.push_back({"periods", static_cast<std::int64_t>(e.rate.periods)});
  v.detail.push_back({"break_rate", e.rate.rate});
  return v;
}

// Whether tie k cancels the next: one balance sheet's cash out of line with the
// cash flows either side, within the tolerance and the window, as a value put
// wrong cancels (DE-11).
bool cash_pair(const std::vector<CashTie>& ties, std::size_t k) {
  return k + 1 < ties.size() && ties[k].broken && ties[k + 1].broken &&
         ties[k + 1].before == ties[k].after &&
         std::fabs(ties[k].gap + ties[k + 1].gap) <=
             std::min(ties[k].tolerance, ties[k + 1].tolerance) &&
         cancel(ties[k].gap, ties[k + 1].gap);
}

// Why a tie broke, for its message: the model's best reading where it is no error.
std::string tie_reading(const TieEvidence& e, double p_error, PeriodType type) {
  if (p_error >= 0.5) {
    return "";
  }
  return e.gap_fits && *e.gap_fits
             ? "; the series' own gap: it breaks in " + history_text(e.rate, type)
             : "; it breaks in " + history_text(e.rate, type);
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
  // Both period types' ties first: how often the series' ties pair by chance is
  // judged across both, as the vendor's cash line is the series' (DE-11).
  std::array<std::vector<CashTie>, 2> all;
  std::array<std::vector<bool>, 2> all_paired;
  for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
    const std::size_t t = type == PeriodType::kAnnual ? 1 : 0;
    all.at(t) = cash_ties(context, sheets.at(t), flows.at(t), *cash, *change, type);
    const std::vector<CashTie>& ties = all.at(t);
    all_paired.at(t).assign(ties.size(), false);
    for (std::size_t k = 0; k + 1 < ties.size(); ++k) {
      if (cash_pair(ties, k)) {
        all_paired.at(t)[k] = true;
        all_paired.at(t)[k + 1] = true;
      }
    }
  }
  for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
    const std::size_t t = type == PeriodType::kAnnual ? 1 : 0;
    const std::vector<CashTie>& ties = all.at(t);
    std::vector<bool> broken;
    std::vector<double> shares;
    for (const CashTie& tie : ties) {
      broken.push_back(tie.broken);
      shares.push_back(tie.gap / tie.scale);
    }
    const auto pairs_next = [&ties](std::size_t k) { return cash_pair(ties, k); };
    for (std::size_t k = 0; k < ties.size(); ++k) {
      const CashTie& tie = ties[k];
      if (!tie.broken) {
        continue;
      }
      const bool pair = pairs_next(k);
      TieEvidence e = tie_evidence(broken, shares, k, pair ? k + 1 : k, "cash", settings);
      if (pair) {
        // Against how often the series' other ties, of either type, pair by
        // chance (DE-11).
        std::size_t others = 0;
        std::size_t seen = 0;
        for (std::size_t u = 0; u < all.size(); ++u) {
          for (std::size_t r = 0; r < all.at(u).size(); ++r) {
            if ((u == t && (r == k || r == k + 1)) || !all.at(u)[r].broken) {
              continue;
            }
            ++others;
            seen += static_cast<std::size_t>(all_paired.at(u)[r]);
          }
        }
        e.named = true;
        e.named_bf = chance_evidence(seen, others, kChanceCancel, kCancelPower, kCancel);
        Violation v = tie_finding(info, context, tie.after, e, "field_error", type);
        v.known = std::max({known_of(s, tie.before), known_of(s, tie.after), known_of(s, tie.flow),
                            known_of(s, ties[k + 1].flow)});
        v.message = settings.cash_tie.at(0) +
                    " is out of line with the cash flows either side: " + "off by " +
                    amount_text(tie.gap) + ", then by " + amount_text(ties[k + 1].gap) +
                    tie_reading(e, v.p_error, type);
        v.key.push_back({"tie", std::string{"cash"}});
        v.key.push_back({"item", settings.cash_tie.at(0)});
        v.detail.push_back({"item", settings.cash_tie.at(0)});
        v.detail.push_back({"gap", tie.gap});
        v.detail.push_back({"next_gap", ties[k + 1].gap});
        v.detail.push_back({"tolerance", tie.tolerance});
        v.evidence.push_back(
            {"cancelling", true, e.named_bf, "the next tie's gap cancels this one"});
        out.push_back(std::move(v));
        ++k;
        continue;
      }
      Violation v = tie_finding(info, context, tie.flow, e, "field_error", type);
      v.known = std::max({known_of(s, tie.before), known_of(s, tie.after), known_of(s, tie.flow)});
      v.message = "the change in " + settings.cash_tie.at(0) + " since " +
                  s.date[tie.before].to_string() + " is not " + settings.cash_tie.at(1) +
                  ": off by " + amount_text(tie.gap) + " (tolerance " + amount_text(tie.tolerance) +
                  ")" + tie_reading(e, v.p_error, type);
      v.key.push_back({"tie", std::string{"cash"}});
      v.detail.push_back({"previous_fiscal_date", s.date[tie.before].to_string()});
      v.detail.push_back({"gap", tie.gap});
      v.detail.push_back({"tolerance", tie.tolerance});
      out.push_back(std::move(v));
    }
    std::size_t breaks = 0;
    std::vector<double> chronic_shares;
    std::vector<std::size_t> rows;
    for (const CashTie& tie : ties) {
      rows.push_back(tie.flow);
      if (tie.broken) {
        ++breaks;
        chronic_shares.push_back(tie.gap / tie.scale);
      }
    }
    if (const std::optional<Chronic> c =
            chronic_of(breaks, ties.size(), chronic_shares, settings)) {
      Violation v = chronic_finding(
          info, context, rows, *c, Statement::kCashFlow, type,
          "the cash tie (" + settings.cash_tie.at(0) + " against " + settings.cash_tie.at(1) + ")");
      v.key.insert(v.key.begin(), {"tie", std::string{"cash"}});
      v.detail.insert(v.detail.begin(), {"tie", std::string{"cash"}});
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
  double scale = 0.0;      // the largest of the five values
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
  g.scale = scale;
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

// A fiscal year judged for its quarter sums: its four quarters and its year in
// force, and the items that break it. Its lead item, the one that breaks most
// against its tolerance, names the row.
struct SumYear {
  std::int32_t fiscal_year = 0;
  std::array<std::size_t, 4> q{};
  std::size_t annual = 0;
  std::vector<SumGap> gaps;
  SumGap lead;
};

using YearRows =
    std::map<std::int32_t, std::pair<std::vector<std::size_t>, std::vector<std::size_t>>>;

// The years with exactly four quarters and one year in force, the quarters within
// the year, each with the flow items that break its sum.
std::vector<SumYear> sum_years(
    const SeriesContext& context,
    const std::vector<std::pair<std::size_t, const std::vector<double>*>>& flows,
    const YearRows& years) {
  const Series& s = context.series;
  const StatementSettings& settings = *context.statement_settings;
  std::vector<SumYear> judged;
  for (const auto& [fiscal_year, rows] : years) {
    const auto& [quarters, annual] = rows;
    if (quarters.size() != 4 || annual.size() != 1 ||
        s.date[quarters.front()].days() <= s.date[annual.front()].days() - 380 ||
        s.date[annual.front()] < s.date[quarters.back()]) {
      continue;
    }
    SumYear y;
    y.fiscal_year = fiscal_year;
    y.q = {quarters[0], quarters[1], quarters[2], quarters[3]};
    y.annual = annual.front();
    for (const auto& [item, column] : flows) {
      if (std::optional<SumGap> g = quarter_gap(*column, y.q, y.annual, settings)) {
        g->item = item;
        y.gaps.push_back(*g);
      }
    }
    if (!y.gaps.empty()) {
      y.lead = *std::max_element(y.gaps.begin(), y.gaps.end(), [](const auto& a, const auto& b) {
        return std::fabs(a.gap) / a.tolerance < std::fabs(b.gap) / b.tolerance;
      });
    }
    judged.push_back(std::move(y));
  }
  return judged;
}

// A statement's quarter sums that break in most judged years: one report for the
// series (DE-11).
void report_chronic_sums(const CheckInfo& info, const SeriesContext& context, Statement statement,
                         const std::vector<SumYear>& judged, std::vector<Violation>& out) {
  std::size_t breaks = 0;
  std::vector<double> shares;
  std::vector<std::size_t> rows;
  for (const SumYear& y : judged) {
    rows.push_back(y.annual);
    if (!y.gaps.empty()) {
      ++breaks;
      shares.push_back(y.lead.gap / y.lead.scale);
    }
  }
  const std::optional<Chronic> c =
      chronic_of(breaks, judged.size(), shares, *context.statement_settings);
  if (!c) {
    return;
  }
  const std::string name{to_string(statement)};
  Violation v = chronic_finding(info, context, rows, *c, statement, PeriodType::kAnnual,
                                "the quarter sums of the " + name + " statement");
  v.key.insert(v.key.begin(), {"tie", std::string{"quarter_sum"}});
  v.detail.insert(v.detail.begin(), {"tie", std::string{"quarter_sum"}});
  out.push_back(std::move(v));
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
    YearRows years;
    for (const std::size_t r : context.statements->in_force) {
      if (st.statement[r] == statement && st.fiscal_year[r] > 0) {
        auto& [quarters, annual] = years[st.fiscal_year[r]];
        (st.period_type[r] == PeriodType::kQuarter ? quarters : annual).push_back(r);
      }
    }
    const std::vector<SumYear> judged = sum_years(context, flows, years);
    std::vector<bool> broken;
    std::vector<double> shares;
    for (const SumYear& y : judged) {
      broken.push_back(!y.gaps.empty());
      shares.push_back(y.gaps.empty() ? 0.0 : y.lead.gap / y.lead.scale);
    }
    for (std::size_t k = 0; k < judged.size(); ++k) {
      const SumYear& y = judged[k];
      if (y.gaps.empty()) {
        continue;
      }
      const SumGap& lead = y.lead;
      const std::size_t row = lead.target == 4 ? y.annual : y.q.at(lead.target);
      std::vector<std::string> items;
      items.reserve(y.gaps.size());
      for (const SumGap& g : y.gaps) {
        items.push_back((*context.items)[g.item].column);
      }
      // A clear row, the quarter or the year out of line on its own, names the
      // value wrong, as a term names a field (DE-11).
      TieEvidence e = tie_evidence(broken, shares, k, k, "quarter_sum", settings);
      e.named = lead.margin >= 0.5;
      // Against how often the series' other broken years have a clear row by
      // chance (DE-11).
      std::size_t others = 0;
      std::size_t seen = 0;
      for (std::size_t r = 0; r < judged.size(); ++r) {
        if (r != k && broken[r]) {
          ++others;
          seen += static_cast<std::size_t>(judged[r].lead.margin >= 0.5);
        }
      }
      e.named_bf = chance_evidence(seen, others, kChanceMatch, kMatchPower, kMatch);
      Violation v =
          tie_finding(info, context, row, e, lead.target == 4 ? "year_error" : "quarter_error",
                      PeriodType::kAnnual);
      Date known = known_of(s, y.annual);
      for (const std::size_t r : y.q) {
        known = std::max(known, known_of(s, r));
      }
      v.known = known;
      const std::string& lead_item = (*context.items)[lead.item].column;
      std::string message = "the quarters of FY" + std::to_string(y.fiscal_year) +
                            " do not sum to the year: " + lead_item + " by " +
                            amount_text(lead.gap);
      if (items.size() > 1) {
        message += ", and " + std::to_string(items.size() - 1) + " more items";
      }
      message += lead.target == 4 ? "; the year is out of line with its quarters"
                                  : "; this quarter is out of line";
      message += tie_reading(e, v.p_error, PeriodType::kAnnual);
      v.message = std::move(message);
      v.key.push_back({"tie", std::string{"quarter_sum"}});
      v.detail.push_back({"fiscal_year", static_cast<std::int64_t>(y.fiscal_year)});
      v.detail.push_back({"items", join(items, ",")});
      v.detail.push_back({"gap", lead.gap});
      v.detail.push_back({"tolerance", lead.tolerance});
      v.detail.push_back({"year_fiscal_date", s.date[y.annual].to_string()});
      v.detail.push_back({"clear", e.named});
      out.push_back(std::move(v));
    }
    report_chronic_sums(info, context, statement, judged, out);
  }
}

}  // namespace

std::vector<BreakCount> count_breaks(const SeriesContext& context) {
  std::vector<BreakCount> counts;
  const Series& s = context.series;
  const StatementSettings* settings = context.statement_settings;
  if (s.kind != SeriesKind::kStatements || settings == nullptr || context.statements == nullptr) {
    return counts;
  }
  const StatementRows& st = s.statements;
  for (const Identity& identity : settings->identities) {
    const std::optional<Terms> terms = terms_of(s, identity, *settings);
    if (!terms) {
      continue;
    }
    BreakCount count{identity.name};
    for (const std::size_t r : context.statements->in_force) {
      const Judged j = judge_row(*terms, r, *settings);
      if (j.complete) {
        ++count.periods;
        count.breaks += static_cast<std::size_t>(j.broken());
      }
    }
    counts.push_back(std::move(count));
  }
  if (settings->cash_tie.size() == 2) {
    const std::vector<double>* cash = st.item(settings->cash_tie.at(0));
    const std::vector<double>* change = st.item(settings->cash_tie.at(1));
    if (cash != nullptr && change != nullptr) {
      const auto sheets = in_force_with(context, *cash);
      const auto flows = in_force_with(context, *change);
      BreakCount count{"cash"};
      for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
        const std::size_t t = type == PeriodType::kAnnual ? 1 : 0;
        for (const CashTie& tie :
             cash_ties(context, sheets.at(t), flows.at(t), *cash, *change, type)) {
          ++count.periods;
          count.breaks += static_cast<std::size_t>(tie.broken);
        }
      }
      counts.push_back(std::move(count));
    }
  }
  if (context.items != nullptr) {
    // A fiscal year judged as DQ904 judges it: four quarters and one year in force.
    BreakCount count{"quarter_sum"};
    for (const Statement statement : {Statement::kIncome, Statement::kCashFlow}) {
      const auto flows = flows_of(context, statement);
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
        bool broken = false;
        for (const auto& [item, column] : flows) {
          broken = broken || quarter_gap(*column, q, annual.front(), *settings).has_value();
        }
        ++count.periods;
        count.breaks += static_cast<std::size_t>(broken);
      }
    }
    counts.push_back(std::move(count));
  }
  return counts;
}

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
