// DQ931 filing-date-implausible and DQ932 restatement-implausible: the knowledge
// timeline and revisions (fundamentals plan §4.4; doc/checks/DQ931.md, DQ932.md).

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

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"
#include "statements/views.hpp"

namespace dorq {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double normal_log_pdf(double x, double mean, double sd) {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}

double log_add(double a, double b) {
  if (a == kNegInf) {
    return b;
  }
  if (b == kNegInf) {
    return a;
  }
  const double m = std::max(a, b);
  return m + std::log(std::exp(a - m) + std::exp(b - m));
}

template <std::size_t N>
std::array<double, N> softmax(const std::array<double, N>& log) {
  std::array<double, N> p{};
  const double top = *std::max_element(log.begin(), log.end());
  double sum = 0.0;
  for (std::size_t h = 0; h < N; ++h) {
    p.at(h) = std::exp(log.at(h) - top);
    sum += p.at(h);
  }
  for (double& v : p) {
    v /= sum;
  }
  return p;
}

// The median of a non-empty vector, reordering it.
double median_of(std::vector<double>& v) {
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

// The lower median: of two, the smaller.
double lower_median_of(std::vector<double>& v) {
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>((v.size() - 1) / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

// ---------------------------------------------------------------------------
// DQ931: filing dates.

constexpr double kMinLag = 7.0;        // days: no filing comes sooner after its period ends
constexpr double kFloorPenalty = 2.0;  // nats a day below kMinLag cost an ordinary lag
constexpr double kLagFloorSd = 3.0;    // days: the least spread of an issuer's own lags
constexpr double kLateWindow = 180.0;  // a late filing lags its usual by up to this
constexpr double kAmendmentWindow = 400.0;
constexpr double kLatePrior = 0.01;
constexpr double kEarlyPrior = 0.002;
constexpr double kAmendmentPrior = 0.002;
constexpr std::size_t kOwnLags = 12;  // the issuer's nearest filings of a form, each side
constexpr std::size_t kMinOwnLags = 4;

enum class Form : std::uint8_t { k10Q, k10K };

const char* form_text(Form form) { return form == Form::k10K ? "10-K" : "10-Q"; }

// The SEC's deadlines, as a prior where the issuer has too few filings of its own:
// 40 to 45 days for a 10-Q, 60 to 90 for a 10-K.
constexpr double kDeadline10Q = 42.0;
constexpr double kDeadline10K = 75.0;
constexpr double kDeadlineSd = 15.0;

enum class FilingHypothesis : std::uint8_t { kOrdinary, kLateFiling, kMisdatedEarly, kAmendment };
constexpr std::array<const char*, 4> kFilingNames{"ordinary", "late_filing", "misdated_early",
                                                  "misdated_amendment"};

// One filing: a period's first version, filed, known from one date.
struct Filing {
  std::size_t row = 0;  // the income statement's row where it has one
  Form form = Form::k10Q;
  PeriodType type = PeriodType::kQuarter;
  Date end;
  Date known;
  double lag = 0.0;
};

Form form_of(const StatementRows& st, std::size_t r) {
  if (st.period_type[r] == PeriodType::kAnnual || st.fiscal_period[r] == FiscalPeriod::kQ4 ||
      st.fiscal_period[r] == FiscalPeriod::kFY) {
    return Form::k10K;  // a year's last quarter is known from the 10-K, with the year
  }
  return Form::k10Q;
}

// The series' filings: each period's first version that was filed, one filing per
// period type and knowledge date, in period order.
std::vector<Filing> filings_of(const Series& s) {
  const StatementRows& st = s.statements;
  // The first version of each period: (date, statement, type) -> row.
  std::map<std::tuple<std::int32_t, int, int>, std::size_t> first;
  for (std::size_t r = 0; r < st.size(); ++r) {
    const auto key = std::make_tuple(s.date[r].days(), static_cast<int>(st.statement[r]),
                                     static_cast<int>(st.period_type[r]));
    const auto [it, fresh] = first.emplace(key, r);
    if (!fresh && st.version[r] < st.version[it->second]) {
      it->second = r;
    }
  }
  std::map<std::tuple<std::int32_t, int, std::int32_t>, Filing> by;
  for (const auto& [key, r] : first) {
    if (st.knowledge_source[r] != KnowledgeSource::kFiled) {
      continue;  // a date the loader estimated or observed is no filing date
    }
    const auto filing_key = std::make_tuple(s.date[r].days(), static_cast<int>(st.period_type[r]),
                                            st.known_from[r].days());
    Filing f{r,
             form_of(st, r),
             st.period_type[r],
             s.date[r],
             st.known_from[r],
             static_cast<double>(st.known_from[r].days() - s.date[r].days())};
    const auto [it, fresh] = by.emplace(filing_key, f);
    if (!fresh && st.statement[r] == Statement::kIncome) {
      it->second.row = r;
    }
  }
  std::vector<Filing> out;
  out.reserve(by.size());
  for (auto& [key, f] : by) {
    out.push_back(f);
  }
  return out;
}

// The issuer's usual lag for a filing's form: the median and spread of its nearest
// filings of that form, the filing itself left out.
struct UsualLag {
  double median = 0.0;
  double sd = 0.0;
  bool own = false;  // from the issuer's filings, not the deadlines
};

UsualLag usual_lag(const std::vector<Filing>& filings, std::size_t k) {
  std::vector<std::size_t> same;
  for (std::size_t j = 0; j < filings.size(); ++j) {
    if (j != k && filings[j].form == filings[k].form) {
      same.push_back(j);
    }
  }
  // The nearest kOwnLags on each side, by period end.
  const auto split = std::partition_point(
      same.begin(), same.end(), [&](std::size_t j) { return filings[j].end < filings[k].end; });
  const auto before = static_cast<std::size_t>(split - same.begin());
  const std::size_t from = before > kOwnLags ? before - kOwnLags : 0;
  const std::size_t to = std::min(same.size(), before + kOwnLags);
  std::vector<double> lags;
  for (std::size_t j = from; j < to; ++j) {
    lags.push_back(filings[same[j]].lag);
  }
  UsualLag usual;
  if (lags.size() < kMinOwnLags) {
    usual.median = filings[k].form == Form::k10K ? kDeadline10K : kDeadline10Q;
    usual.sd = kDeadlineSd;
    return usual;
  }
  usual.own = true;
  usual.median = median_of(lags);
  for (double& x : lags) {
    x = std::fabs(x - usual.median);
  }
  usual.sd = std::max(kLagFloorSd, 1.4826 * median_of(lags));
  return usual;
}

// The log density of a lag under each hypothesis.
std::array<double, 4> lag_log_likelihoods(double x, const UsualLag& usual) {
  const double m = usual.median;
  const double s = usual.sd;
  std::array<double, 4> ll{};
  ll[0] = log_add(std::log(0.95) + normal_log_pdf(x, m, s),
                  std::log(0.05) + normal_log_pdf(x, m, 3.0 * s));
  if (x < kMinLag) {
    ll[0] -= kFloorPenalty * (kMinLag - x);  // below any filer's lag
  }
  ll[1] = x > m && x <= m + kLateWindow ? -std::log(kLateWindow) : kNegInf;
  // The period end, a press release's date, or another day well before any filing.
  const double early_top = std::max(10.0, m - 2.0 * s);
  const double uniform = x >= 0.0 && x <= early_top ? -std::log(early_top) : kNegInf;
  ll[2] = log_add(std::log(0.7) + normal_log_pdf(x, 0.0, 2.0), std::log(0.3) + uniform);
  ll[3] = x > m && x <= m + kAmendmentWindow ? -std::log(kAmendmentWindow) : kNegInf;
  return ll;
}

// Known after the next period of its type: likely for an amendment's date, rare for a
// late filing, and next to never otherwise.
std::array<double, 4> successor_log_likelihoods(bool after) {
  constexpr std::array<double, 4> kAfter{0.001, 0.02, 1e-4, 0.9};
  std::array<double, 4> ll{};
  for (std::size_t h = 0; h < 4; ++h) {
    ll.at(h) = std::log(after ? kAfter.at(h) : 1.0 - kAfter.at(h));
  }
  return ll;
}

Violation filing_violation(const CheckInfo& info, const SeriesContext& context, const Filing& f,
                           const UsualLag& usual, const Filing* next,
                           const std::array<double, 4>& p) {
  const Series& s = context.series;
  Violation v = statement_finding(info, s, f.row, true);
  const double p_early = p[2];
  const double p_amendment = p[3];
  v.p_error = p_early + p_amendment;
  const auto lag = static_cast<std::int64_t>(f.lag);
  const std::string usual_text =
      usual.own ? "this issuer's usual " + std::to_string(std::llround(usual.median)) + " days"
                : "the " + std::string(form_text(f.form)) + " deadline";
  const std::string head = "known from " + f.known.to_string() + ", " + std::to_string(lag) +
                           " day" + (lag == 1 ? "" : "s") + " after the period ends, against " +
                           usual_text + " (" + form_text(f.form) + ")";
  if (v.p_error < 0.5 && p[1] >= 0.5) {
    v.classification = Classification::kMarketFact;
    v.message = head + ": a late filing, a filing fact";
  } else if (p_early >= p_amendment) {
    v.message = head +
                ": too early for a filing (the period end or a press release's date), so "
                "every backtest knows the figures too soon";
  } else {
    v.message = head + ": an amendment's date taken for the original's";
    if (next != nullptr) {
      v.message += ", known after the next period (" + next->end.to_string() + ", known from " +
                   next->known.to_string() + ")";
    }
  }
  v.detail.push_back({"lag_days", lag});
  v.detail.push_back({"usual_lag_days", usual.median});
  v.detail.push_back({"usual_lag_sd", usual.sd});
  v.detail.push_back({"form", std::string{form_text(f.form)}});
  v.detail.push_back({"known_from", f.known.to_string()});
  if (next != nullptr) {
    v.detail.push_back({"next_known_from", next->known.to_string()});
  }
  for (std::size_t h = 0; h < 4; ++h) {
    v.hypotheses.push_back({kFilingNames.at(h), p.at(h)});
  }
  SuggestedAction action{"check_filing_date", {}, {}};
  action.fields.push_back({"fiscal_date", f.end.to_string()});
  action.fields.push_back({"known_from", f.known.to_string()});
  action.text = "check the filing date of the " + std::string(form_text(f.form)) + " for " +
                f.end.to_string() + " against the SEC's record";
  v.suggested_action = std::move(action);
  return v;
}

// ---------------------------------------------------------------------------
// DQ932: restatements.

// What a revision's changes make: nothing in particular; a power of ten; a sign; two
// values swapped; a total moved while its terms hold; or an amount moved from one
// item to another, the totals kept (a reclassification).
enum class Pattern : std::uint8_t { kNone, kPower, kSign, kSwap, kTotalOnly, kOffset };

// A pattern's chance under a restatement, and under a version stored wrong.
constexpr std::array<double, 6> kPatternRestated{0.49, 1e-4, 1e-3, 1e-3, 1e-2, 0.5};
constexpr std::array<double, 6> kPatternError{0.3, 0.3, 0.15, 0.1, 0.14, 0.01};
constexpr double kVersionErrorPrior = 0.01;  // each of a new version and an old one wrong
constexpr double kFitFloorSd = 0.02;         // the least spread of an item about its neighbours
constexpr double kOffScale = 30.0;           // how far off a version stored wrong sits, in spreads

constexpr std::array<const char*, 3> kRevisionNames{"restatement", "new_version_error",
                                                    "old_version_error"};

// How well a version's values sit with the periods around them, in spreads: Normal
// with a fat tail for a version that is right, broad for one stored wrong.
double fits(double z) {
  return log_add(std::log(0.9) + normal_log_pdf(z, 0.0, 1.0),
                 std::log(0.1) + normal_log_pdf(z, 0.0, 5.0));
}
double off(double z) { return -std::log(kOffScale) - 2.0 * std::log1p(z / kOffScale); }

// The money items DQ932 compares: all of them without --items, and with it the
// flows and stocks (share counts and per-share values move with splits).
std::vector<bool> money_items(const StatementRows& st, const std::vector<LineItem>* items) {
  std::vector<bool> money(st.item_names.size(), items == nullptr);
  if (items == nullptr) {
    return money;
  }
  for (std::size_t k = 0; k < st.item_names.size(); ++k) {
    for (const LineItem& item : *items) {
      if (item.column == st.item_names[k]) {
        money[k] = item.item_class == ItemClass::kFlow || item.item_class == ItemClass::kStock;
      }
    }
  }
  return money;
}

bool differs(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) != std::isnan(b);
  }
  return std::fabs(a - b) > 0.5 + 1e-9 * std::max(std::fabs(a), std::fabs(b));
}

bool same_value(double a, double b) {
  return !std::isnan(a) && !std::isnan(b) &&
         std::fabs(a - b) <= 0.5 + 1e-6 * std::max(std::fabs(a), std::fabs(b));
}

// What a revision did to a row: its changed items and the pattern they make.
struct Revision {
  std::vector<std::size_t> changed;  // items
  Pattern pattern = Pattern::kNone;
  int power = 0;
  std::vector<std::size_t> pattern_items;  // the items the pattern names
};

std::optional<std::size_t> item_index(const StatementRows& st, const std::string& name) {
  const auto it = std::find(st.item_names.begin(), st.item_names.end(), name);
  if (it == st.item_names.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(it - st.item_names.begin());
}

// A total that moved while every one of its terms held.
std::optional<std::size_t> total_only(const StatementRows& st, const StatementSettings& settings,
                                      std::size_t a, std::size_t b,
                                      const std::vector<bool>& changed) {
  for (const Identity& identity : settings.identities) {
    if (identity.left.size() != 1) {
      continue;  // no one total
    }
    const std::optional<std::size_t> total = item_index(st, identity.left.front().item);
    if (!total || !changed[*total] || std::isnan(st.items[*total][a]) ||
        std::isnan(st.items[*total][b])) {
      continue;
    }
    bool held = !identity.right.empty();
    for (const IdentityTerm& term : identity.right) {
      const std::optional<std::size_t> k = item_index(st, term.item);
      held = held && k && !changed[*k] && !std::isnan(st.items[*k][a]);
    }
    if (held) {
      return total;
    }
  }
  return std::nullopt;
}

Revision revision_of(const Series& s, std::size_t a, std::size_t b, const std::vector<bool>& money,
                     const StatementSettings& settings) {
  const StatementRows& st = s.statements;
  Revision rev;
  std::vector<bool> changed(st.item_names.size(), false);
  for (std::size_t k = 0; k < st.item_names.size(); ++k) {
    if (money[k] && differs(st.items[k][a], st.items[k][b])) {
      changed[k] = true;
      rev.changed.push_back(k);
    }
  }
  if (rev.changed.empty()) {
    return rev;
  }
  // A power of ten that most of the changed items share.
  std::map<int, std::vector<std::size_t>> powers;
  std::vector<std::size_t> signs;
  for (const std::size_t k : rev.changed) {
    const double x = st.items[k][a];
    const double y = st.items[k][b];
    if (std::isnan(x) || std::isnan(y) || x == 0.0 || y == 0.0) {
      continue;
    }
    if (same_value(-x, y)) {
      signs.push_back(k);
      continue;
    }
    const double e = std::log10(std::fabs(y / x));
    const double power = std::round(e);
    if (power != 0.0 && std::fabs(e - power) < 0.01 && (x > 0.0) == (y > 0.0)) {
      powers[static_cast<int>(power)].push_back(k);
    }
  }
  const std::size_t most = std::max<std::size_t>(1, (rev.changed.size() * 4 + 4) / 5);
  for (auto& [power, ks] : powers) {
    if (ks.size() >= most) {
      rev.pattern = Pattern::kPower;
      rev.power = power;
      rev.pattern_items = ks;
      return rev;
    }
  }
  if (!signs.empty() && signs.size() >= most) {
    rev.pattern = Pattern::kSign;
    rev.pattern_items = signs;
    return rev;
  }
  // Two items whose values changed places.
  for (std::size_t i = 0; i < rev.changed.size(); ++i) {
    for (std::size_t j = i + 1; j < rev.changed.size(); ++j) {
      const std::size_t p = rev.changed[i];
      const std::size_t q = rev.changed[j];
      if (same_value(st.items[p][b], st.items[q][a]) &&
          same_value(st.items[q][b], st.items[p][a])) {
        rev.pattern = Pattern::kSwap;
        rev.pattern_items = {p, q};
        return rev;
      }
    }
  }
  if (const std::optional<std::size_t> total = total_only(st, settings, a, b, changed)) {
    rev.pattern = Pattern::kTotalOnly;
    rev.pattern_items = {*total};
    return rev;
  }
  // Two items that moved by the same amount, one up and one down.
  for (std::size_t i = 0; i < rev.changed.size(); ++i) {
    for (std::size_t j = i + 1; j < rev.changed.size(); ++j) {
      const std::size_t p = rev.changed[i];
      const std::size_t q = rev.changed[j];
      const double dp = st.items[p][b] - st.items[p][a];
      const double dq = st.items[q][b] - st.items[q][a];
      if (std::isfinite(dp) && std::isfinite(dq) && dp != 0.0 &&
          std::fabs(dp + dq) <= 1.0 + 1e-3 * (std::fabs(dp) + std::fabs(dq))) {
        rev.pattern = Pattern::kOffset;
        rev.pattern_items = {p, q};
        return rev;
      }
    }
  }
  return rev;
}

// A line of the in-force view: one statement and period type, by fiscal date.
using Lines = std::map<std::pair<int, int>, std::vector<std::size_t>>;

Lines lines_of(const Series& s, const std::vector<std::size_t>& view) {
  Lines lines;
  for (const std::size_t r : view) {
    lines[{static_cast<int>(s.statements.statement[r]),
           static_cast<int>(s.statements.period_type[r])}]
        .push_back(r);
  }
  return lines;
}

// An item's scale on a line: its median magnitude, so that asinh(x / scale) reads
// large values on the log scale and keeps signs and zeros.
double scale_of(const StatementRows& st, const std::vector<std::size_t>& line, std::size_t k) {
  std::vector<double> sizes;
  for (const std::size_t r : line) {
    const double x = st.items[k][r];
    if (std::isfinite(x) && x != 0.0) {
      sizes.push_back(std::fabs(x));
    }
  }
  return sizes.empty() ? 1.0 : std::max(1.0, median_of(sizes));
}

// The value the periods around position p of a line predict for item k: the median
// of the two either side, the period itself left out; NaN with fewer than two.
double predicted(const StatementRows& st, const std::vector<std::size_t>& line, std::size_t p,
                 std::size_t k) {
  std::vector<double> around;
  for (std::size_t q = p >= 2 ? p - 2 : 0; q <= std::min(line.size() - 1, p + 2); ++q) {
    if (q != p && std::isfinite(st.items[k][line[q]])) {
      around.push_back(st.items[k][line[q]]);
    }
  }
  if (around.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return median_of(around);
}

// How far an item's values sit from what their neighbours predict, along the line:
// the spread of the line's own residuals, on the asinh scale.
double fit_sd(const StatementRows& st, const std::vector<std::size_t>& line, std::size_t k,
              double scale) {
  std::vector<double> residuals;
  for (std::size_t p = 0; p < line.size(); ++p) {
    const double x = st.items[k][line[p]];
    const double e = predicted(st, line, p, k);
    if (std::isfinite(x) && std::isfinite(e)) {
      residuals.push_back(std::fabs(std::asinh(x / scale) - std::asinh(e / scale)));
    }
  }
  if (residuals.size() < 4) {
    return 0.1;
  }
  return std::max(kFitFloorSd, 1.4826 * median_of(residuals));
}

// The two versions' distance from the periods around them, in spreads: the old
// version against the periods as first reported, the ones in force beside it, and
// the new one against the periods in force. A restatement that moves a whole year
// (a reclassification) fits both; a version stored wrong fits neither. Over the
// changed items that have a prediction; nullopt when none has.
struct Fit {
  double old_z = 0.0;
  double new_z = 0.0;
};

// The line of row r's statement and period type, and r's period's place in it.
std::optional<std::pair<const std::vector<std::size_t>*, std::size_t>> place(const Series& s,
                                                                             const Lines& lines,
                                                                             std::size_t r) {
  const StatementRows& st = s.statements;
  const auto it =
      lines.find({static_cast<int>(st.statement[r]), static_cast<int>(st.period_type[r])});
  if (it == lines.end()) {
    return std::nullopt;
  }
  const std::vector<std::size_t>& line = it->second;
  const auto at =
      std::find_if(line.begin(), line.end(), [&](std::size_t q) { return s.date[q] == s.date[r]; });
  if (at == line.end()) {
    return std::nullopt;
  }
  return std::pair{&line, static_cast<std::size_t>(at - line.begin())};
}

// A value's distance from what its line's neighbours predict, in the line's spreads.
std::optional<double> off_line(const StatementRows& st, const std::vector<std::size_t>& line,
                               std::size_t p, std::size_t k, double x) {
  const double e = predicted(st, line, p, k);
  if (!std::isfinite(x) || !std::isfinite(e)) {
    return std::nullopt;
  }
  const double scale = scale_of(st, line, k);
  return std::fabs(std::asinh(x / scale) - std::asinh(e / scale)) / fit_sd(st, line, k, scale);
}

std::optional<Fit> fit_of(const Series& s, const Lines& first, const Lines& in_force, std::size_t a,
                          std::size_t b, const std::vector<std::size_t>& changed) {
  const StatementRows& st = s.statements;
  const auto old_place = place(s, first, a);
  const auto new_place = place(s, in_force, b);
  if (!old_place || !new_place) {
    return std::nullopt;
  }
  std::vector<double> old_z;
  std::vector<double> new_z;
  for (const std::size_t k : changed) {
    const std::optional<double> x =
        off_line(st, *old_place->first, old_place->second, k, st.items[k][a]);
    const std::optional<double> y =
        off_line(st, *new_place->first, new_place->second, k, st.items[k][b]);
    if (x && y) {
      old_z.push_back(*x);
      new_z.push_back(*y);
    }
  }
  if (old_z.empty()) {
    return std::nullopt;
  }
  // Most of the changed items: an amount moved to an item that was 0 is off its own
  // history whatever the version, and a row stored wrong is off on nearly all.
  return Fit{lower_median_of(old_z), lower_median_of(new_z)};
}

std::string power_text(int power) {
  if (power >= 0) {
    return "x1" + std::string(static_cast<std::size_t>(power), '0');
  }
  return "x0." + std::string(static_cast<std::size_t>(-power - 1), '0') + "1";
}

std::string pattern_text(const StatementRows& st, const Revision& rev, bool from_new) {
  const std::size_t n = rev.changed.size();
  const std::string items = std::to_string(n) + " item" + (n == 1 ? "" : "s");
  switch (rev.pattern) {
    case Pattern::kPower:
      return "by " + power_text(from_new ? rev.power : -rev.power) + " on " +
             std::to_string(rev.pattern_items.size()) + " of " + items;
    case Pattern::kSign:
      return "by a sign on " + std::to_string(rev.pattern_items.size()) + " of " + items;
    case Pattern::kSwap:
      return "with " + st.item_names[rev.pattern_items[0]] + " and " +
             st.item_names[rev.pattern_items[1]] + " swapped";
    case Pattern::kTotalOnly:
      return "in its total " + st.item_names[rev.pattern_items[0]] + " while its terms hold";
    case Pattern::kOffset:
      return "with an amount moved between " + st.item_names[rev.pattern_items[0]] + " and " +
             st.item_names[rev.pattern_items[1]];
    case Pattern::kNone:
      break;
  }
  return "on " + items;
}

const char* pattern_name(Pattern p) {
  switch (p) {
    case Pattern::kPower:
      return "power_of_ten";
    case Pattern::kSign:
      return "sign";
    case Pattern::kSwap:
      return "swap";
    case Pattern::kTotalOnly:
      return "total_only";
    case Pattern::kOffset:
      return "offset";
    case Pattern::kNone:
      break;
  }
  return "none";
}

std::array<double, 3> revision_posterior(const Revision& rev, const std::optional<Fit>& fit) {
  const auto pattern = static_cast<std::size_t>(rev.pattern);
  std::array<double, 3> log{};
  log[0] = std::log(1.0 - 2.0 * kVersionErrorPrior) + std::log(kPatternRestated.at(pattern));
  log[1] = std::log(kVersionErrorPrior) + std::log(kPatternError.at(pattern));
  log[2] = log[1];
  if (fit) {
    log[0] += fits(fit->old_z) + fits(fit->new_z);
    log[1] += fits(fit->old_z) + off(fit->new_z);
    log[2] += off(fit->old_z) + fits(fit->new_z);
  }
  return softmax(log);
}

Violation revision_violation(const CheckInfo& info, const SeriesContext& context, std::size_t a,
                             std::size_t b, const Revision& rev, const std::optional<Fit>& fit,
                             const std::array<double, 3>& p, Severity severity) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  const bool old_wrong = p[2] > p[1];
  Violation v = statement_finding(info, s, old_wrong ? a : b, true);
  v.severity = severity;
  v.p_error = p[1] + p[2];
  const std::string old_name = version_text(st.version[a]);
  const std::string new_name = version_text(st.version[b]);
  if (old_wrong) {
    const Date to = st.known_to[a].value_or(st.known_from[b]);
    v.message = old_name + ", in force from " + st.known_from[a].to_string() + " to " +
                to.to_string() + ", differs from the restated " + new_name + " " +
                pattern_text(st, rev, false) + "; " + new_name +
                (fit ? " agrees with the periods around it and " + old_name + " does not"
                     : " is likelier the right one") +
                ": the old version was wrong while in force";
    v.detail.push_back({"in_force_from", st.known_from[a].to_string()});
    v.detail.push_back({"in_force_to", to.to_string()});
  } else {
    v.message = new_name + ", known from " + st.known_from[b].to_string() + ", differs from " +
                old_name + " " + pattern_text(st, rev, true) +
                (fit ? ", against the periods around it" : "") + ": the new version is wrong";
  }
  v.detail.push_back({"pattern", std::string{pattern_name(rev.pattern)}});
  v.detail.push_back({"items_changed", static_cast<std::int64_t>(rev.changed.size())});
  if (rev.pattern == Pattern::kPower) {
    v.detail.push_back({"power", static_cast<std::int64_t>(rev.power)});  // new = old x 10^power
  }
  v.detail.push_back({"old_version", static_cast<std::int64_t>(st.version[a])});
  v.detail.push_back({"new_version", static_cast<std::int64_t>(st.version[b])});
  if (fit) {
    v.detail.push_back({"old_off_neighbours", fit->old_z});
    v.detail.push_back({"new_off_neighbours", fit->new_z});
  }
  for (std::size_t h = 0; h < 3; ++h) {
    v.hypotheses.push_back({kRevisionNames.at(h), p.at(h)});
  }
  SuggestedAction action{"check_filing", {}, {}};
  action.fields.push_back({"fiscal_date", s.date[b].to_string()});
  action.fields.push_back({"version_no", static_cast<std::int64_t>(st.version[old_wrong ? a : b])});
  action.text = "check " + (old_wrong ? old_name : new_name) + " of " + s.date[b].to_string() +
                " against its filing";
  v.suggested_action = std::move(action);
  return v;
}

}  // namespace

const CheckInfo& FilingDateImplausible::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ931",
      .name = "filing-date-implausible",
      .summary =
          "a filing date judged against the issuer's own lags and the SEC's deadlines: "
          "too early (a look-ahead), or an amendment's date taken for the original's",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void FilingDateImplausible::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements) {
    return;
  }
  const std::vector<Filing> filings = filings_of(s);
  for (std::size_t k = 0; k < filings.size(); ++k) {
    const Filing& f = filings[k];
    if (f.lag < 0.0) {
      continue;  // known before the period ends: DQ901's
    }
    // The next period of the same type.
    const Filing* next = nullptr;
    for (std::size_t j = k + 1; j < filings.size(); ++j) {
      if (filings[j].type == f.type && filings[j].end > f.end) {
        next = &filings[j];
        break;
      }
    }
    const UsualLag usual = usual_lag(filings, k);
    std::array<double, 4> log = lag_log_likelihoods(f.lag, usual);
    const std::array<double, 4> priors{1.0 - kLatePrior - kEarlyPrior - kAmendmentPrior, kLatePrior,
                                       kEarlyPrior, kAmendmentPrior};
    const std::array<double, 4> after =
        successor_log_likelihoods(next != nullptr && f.known > next->known);
    for (std::size_t h = 0; h < 4; ++h) {
      log.at(h) += std::log(priors.at(h)) + (next != nullptr ? after.at(h) : 0.0);
    }
    const std::array<double, 4> p = softmax(log);
    const double p_error = p[2] + p[3];
    std::optional<Severity> severity = context.thresholds.for_probability(p_error);
    if (severity && p[3] > p[2] && *severity == Severity::kError) {
      severity = Severity::kWarn;  // an amendment's date makes a backtest late, not early
    }
    if (!severity && p[1] >= 0.5) {
      severity = Severity::kInfo;  // a late filing: a filing fact
    }
    if (!severity) {
      continue;
    }
    Violation v = filing_violation(info(), context, f, usual, next, p);
    v.severity = *severity;
    out.push_back(std::move(v));
  }
}

const CheckInfo& RestatementImplausible::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ932",
      .name = "restatement-implausible",
      .summary =
          "a new version that differs from the one before it by a power of ten, a sign, "
          "two fields swapped or a total alone: the new version wrong, or the old one",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void RestatementImplausible::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements || context.statements == nullptr ||
      context.statement_settings == nullptr) {
    return;
  }
  const StatementRows& st = s.statements;
  const std::vector<bool> money = money_items(st, context.items);
  // Each period's versions, in the order they became known.
  std::map<std::tuple<std::int32_t, int, int>, std::vector<std::size_t>> periods;
  for (std::size_t r = 0; r < st.size(); ++r) {
    periods[{s.date[r].days(), static_cast<int>(st.statement[r]),
             static_cast<int>(st.period_type[r])}]
        .push_back(r);
  }
  std::optional<Lines> first;
  std::optional<Lines> in_force;
  for (auto& [key, versions] : periods) {
    if (versions.size() < 2) {
      continue;
    }
    std::stable_sort(versions.begin(), versions.end(), [&](std::size_t a, std::size_t b) {
      return std::tuple{st.known_from[a], st.version[a]} <
             std::tuple{st.known_from[b], st.version[b]};
    });
    if (!in_force) {
      first = lines_of(s, context.statements->first_reported);
      in_force = lines_of(s, context.statements->in_force);
    }
    for (std::size_t i = 1; i < versions.size(); ++i) {
      const std::size_t a = versions[i - 1];
      const std::size_t b = versions[i];
      const Revision rev = revision_of(s, a, b, money, *context.statement_settings);
      if (rev.changed.empty()) {
        continue;
      }
      const std::optional<Fit> fit = fit_of(s, *first, *in_force, a, b, rev.changed);
      const std::array<double, 3> p = revision_posterior(rev, fit);
      const std::optional<Severity> severity = context.thresholds.for_probability(p[1] + p[2]);
      if (!severity) {
        continue;
      }
      out.push_back(revision_violation(info(), context, a, b, rev, fit, p, *severity));
    }
  }
}

}  // namespace dorq
