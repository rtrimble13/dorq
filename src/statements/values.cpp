// The statement value model: DQ911 bad-value, DQ912 unit-error, DQ913 sign-error
// and DQ919 large-change (fundamentals plan section 4.1; doc/statements.md, "The
// value model"). Closed form and O(n) a series, as the bars' models are.

#include "statements/values.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/text.hpp"
#include "dorq/date.hpp"
#include "dorq/statement.hpp"
#include "stats/student_t.hpp"

namespace dorq {

ValueHypothesis ValueFinding::error() const noexcept {
  std::size_t best = 1;
  for (std::size_t h = 2; h < posterior.size(); ++h) {
    if (posterior.at(h) > posterior.at(best)) {
      best = h;
    }
  }
  return static_cast<ValueHypothesis>(best);
}

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kNu = 5.0;    // the predictive's degrees of freedom: tails heavier than normal
constexpr double kTail = 1e-3;  // screening: a predictive tail probability below this
constexpr double kRatio = 5.0;  // screening: this far from both neighbours
// A step between two periods at a power of ten, to within this in log10 (a factor of
// two either way), is a unit step.
constexpr double kPowerTolerance = 0.3;
constexpr std::array<int, 4> kPowers{3, -3, 6, -6};
constexpr double kBadScale = 1.0;  // a bad value's spread around the prediction (asinh scale)
constexpr double kUnusual = 4.0;   // |residual| / scale: another item that moved too
constexpr std::ptrdiff_t kWindow = 12;  // periods either side of the local scale
constexpr std::size_t kMinValues = 4;   // an item judged in a line has this many values
constexpr double kOutlier = 5.0;  // |residual| / scale: left out of the next pass's predictions
constexpr int kPasses = 4;
constexpr std::size_t kNear = 5;  // positions either side an outlier must be the worst of
// The evidence's log Bayes factors. dorq's defaults, as DQ2xx's are, until DR-0911
// fits them.
constexpr double kMovesWithOthers = 3.0;  // other items of the row moved too: a real event
constexpr double kMovesAlone = 1.0;
constexpr double kRowMoved = 8.0;  // against a single wrong value
constexpr double kSubtotalMoves = 6.0;  // a subtotal of the row moved by the same amount
constexpr double kHolds = 4.0;    // the level holds in the next period: a real step
constexpr double kReverts = 1.0;  // the next period goes back
constexpr double kIdentityRepair = 4.0;  // an identity fails, and holds with the repair
constexpr double kIdentityHolds = 6.0;   // an identity holds with the stored value
constexpr double kQuarterRepair = 3.0;   // the year's quarters sum to it once repaired
constexpr double kQuarterHolds = 2.0;
constexpr double kSignHistory = 2.0;  // the item has the other sign in nearly every period
constexpr double kPairedEra = 0.98;   // p(unit_error) of a run between two unit steps
constexpr double kOpenEra = 0.85;     // ... of one step with no step back

constexpr std::ptrdiff_t kNone = -1;

bool usable(double x) { return std::isfinite(x) && x != 0.0; }

// The middle of `values`: the median, the mean of the middle two for an even count.
double middle(std::vector<double> values) {
  if (values.empty()) {
    return kNaN;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  return values.size() % 2 == 1 ? values[mid] : 0.5 * (values[mid - 1] + values[mid]);
}

// One statement's rows of one period type in a view, by fiscal date, and each
// position's neighbours: the periods before and after within the spacing, and for
// quarters the same quarter a year either side.
struct Line {
  Statement statement = Statement::kIncome;
  PeriodType type = PeriodType::kQuarter;
  std::vector<std::size_t> rows;
  std::vector<std::ptrdiff_t> prev, next, year_before, year_after;

  [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
  [[nodiscard]] bool flows() const noexcept { return statement != Statement::kBalanceSheet; }
  [[nodiscard]] bool seasonal() const noexcept { return flows() && type == PeriodType::kQuarter; }
};

std::vector<Line> lines_of(const Series& s, const std::vector<std::size_t>& view,
                           const StatementSettings& settings) {
  const StatementRows& st = s.statements;
  std::array<Line, 6> by{};
  for (const std::size_t r : view) {
    const auto at = static_cast<std::size_t>(st.statement[r]) * 2 +
                    (st.period_type[r] == PeriodType::kAnnual ? 1 : 0);
    Line& line = by.at(at);
    line.statement = st.statement[r];
    line.type = st.period_type[r];
    line.rows.push_back(r);
  }
  std::vector<Line> lines;
  for (Line& line : by) {
    const std::size_t n = line.size();
    if (n == 0) {
      continue;
    }
    line.prev.assign(n, kNone);
    line.next.assign(n, kNone);
    line.year_before.assign(n, kNone);
    line.year_after.assign(n, kNone);
    const auto [low, high] =
        line.type == PeriodType::kQuarter ? settings.quarter_spacing : settings.year_spacing;
    for (std::size_t p = 1; p < n; ++p) {
      const int gap = s.date[line.rows[p]].days() - s.date[line.rows[p - 1]].days();
      if (gap >= low && gap <= high) {
        line.prev[p] = static_cast<std::ptrdiff_t>(p - 1);
        line.next[p - 1] = static_cast<std::ptrdiff_t>(p);
      }
    }
    if (line.type == PeriodType::kQuarter) {
      for (std::size_t p = 1; p < n; ++p) {
        for (std::size_t q = p; q-- > 0 && q + 7 > p;) {
          const int gap = s.date[line.rows[p]].days() - s.date[line.rows[q]].days();
          if (gap > 380) {
            break;
          }
          if (gap >= 350) {
            line.year_before[p] = static_cast<std::ptrdiff_t>(q);
            line.year_after[q] = static_cast<std::ptrdiff_t>(p);
            break;
          }
        }
      }
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

// What the model knows of each item from --items.
struct ItemInfo {
  bool judged = true;     // a money item: a flow or a stock
  bool signs = false;     // its sign is judged: a convention other than signed
  bool known = false;     // listed in --items
  bool flow = false;      // listed as a flow
  bool subtotal = false;  // listed as a subtotal
  Statement statement = Statement::kIncome;
};

std::vector<ItemInfo> item_info(const StatementRows& st, const std::vector<LineItem>* items) {
  std::vector<ItemInfo> info(st.item_names.size());
  if (items == nullptr) {
    return info;
  }
  for (std::size_t k = 0; k < st.item_names.size(); ++k) {
    ItemInfo& i = info[k];
    const auto it = std::find_if(items->begin(), items->end(), [&](const LineItem& item) {
      return iequals(item.column, st.item_names[k]);
    });
    if (it == items->end()) {
      i.judged = false;
      continue;
    }
    i.known = true;
    i.judged = it->item_class == ItemClass::kFlow || it->item_class == ItemClass::kStock;
    i.flow = it->item_class == ItemClass::kFlow;
    i.subtotal = it->is_subtotal;
    // A subtotal's sign follows its terms', which may change it.
    i.signs = it->sign != SignConvention::kSigned && !it->is_subtotal;
    i.statement = it->statement;
  }
  return info;
}

// An identity's terms as item indexes, or nullopt when the series lacks one.
struct IdentityTerms {
  const Identity* identity = nullptr;
  std::vector<std::size_t> items;
  std::vector<double> signs;
  std::vector<bool> zero_if_null;
};

std::vector<IdentityTerms> identity_terms(const StatementRows& st,
                                          const StatementSettings& settings) {
  std::vector<IdentityTerms> out;
  for (const Identity& identity : settings.identities) {
    IdentityTerms terms;
    terms.identity = &identity;
    bool complete = true;
    for (const bool left : {true, false}) {
      for (const IdentityTerm& term : left ? identity.left : identity.right) {
        const auto it = std::find_if(st.item_names.begin(), st.item_names.end(),
                                     [&](const std::string& n) { return iequals(n, term.item); });
        if (it == st.item_names.end()) {
          complete = false;
          break;
        }
        terms.items.push_back(static_cast<std::size_t>(it - st.item_names.begin()));
        terms.signs.push_back((left ? 1.0 : -1.0) * (term.negative ? -1.0 : 1.0));
        terms.zero_if_null.push_back(std::find(settings.null_is_zero.begin(),
                                               settings.null_is_zero.end(),
                                               term.item) != settings.null_is_zero.end());
      }
    }
    if (complete) {
      out.push_back(std::move(terms));
    }
  }
  return out;
}

struct Gap {
  double gap = 0.0;
  double tolerance = 0.0;
  [[nodiscard]] bool holds() const noexcept { return std::fabs(gap) <= tolerance; }
};

// An identity on row i with item k's value replaced by v; nullopt when a term is NULL.
std::optional<Gap> identity_gap(const StatementRows& st, const IdentityTerms& terms,
                                const StatementSettings& settings, std::size_t i, std::size_t k,
                                double v) {
  Gap g;
  double scale = 0.0;
  for (std::size_t t = 0; t < terms.items.size(); ++t) {
    double x = terms.items[t] == k ? v : st.items[terms.items[t]][i];
    if (std::isnan(x)) {
      if (!terms.zero_if_null[t]) {
        return std::nullopt;
      }
      x = 0.0;
    }
    g.gap += terms.signs[t] * x;
    scale = std::max(scale, std::fabs(x));
  }
  g.tolerance = std::max(settings.identity_share * scale, settings.tie_floor);
  return g;
}

// A fiscal year's quarters and years of one statement in a view: series rows.
using Years = std::map<std::int32_t, std::pair<std::vector<std::size_t>, std::vector<std::size_t>>>;

std::array<Years, 3> years_of(const StatementRows& st, const std::vector<std::size_t>& view) {
  std::array<Years, 3> years;
  for (const std::size_t r : view) {
    if (st.fiscal_year[r] > 0) {
      auto& [quarters, annual] =
          years.at(static_cast<std::size_t>(st.statement[r]))[st.fiscal_year[r]];
      (st.period_type[r] == PeriodType::kQuarter ? quarters : annual).push_back(r);
    }
  }
  return years;
}

// The year's four quarters against its year, with row i's value of item k replaced
// by v; nullopt where the year has another count of either, or a NULL.
std::optional<Gap> quarter_gap(const StatementRows& st, const Years& years,
                               const StatementSettings& settings, std::size_t i, std::size_t k,
                               double v) {
  const auto it = years.find(st.fiscal_year[i]);
  if (it == years.end() || it->second.first.size() != 4 || it->second.second.size() != 1) {
    return std::nullopt;
  }
  const std::vector<double>& column = st.items[k];
  const auto value = [&](std::size_t r) { return r == i ? v : column[r]; };
  Gap g;
  double scale = std::fabs(value(it->second.second.front()));
  for (const std::size_t q : it->second.first) {
    g.gap += value(q);
    scale = std::max(scale, std::fabs(value(q)));
  }
  g.gap -= value(it->second.second.front());
  if (!std::isfinite(g.gap)) {
    return std::nullopt;
  }
  g.tolerance = std::max(settings.identity_share * scale, settings.tie_floor);
  return g;
}

// One item along a line: its values, the working values (with unit errors found in
// rows and eras put right), and the model's view of each position.
struct Track {
  std::size_t item = 0;
  std::vector<double> x;  // stored
  std::vector<double> w;  // working
  std::vector<bool> in_era;
  double s = 1.0;  // the median magnitude: z = asinh(w / s)
  std::vector<double> z, pred, before, after, sigma;
  std::size_t outliers = 0;  // positions far from their prediction, in the last pass
  std::size_t judged = 0;    // positions with a prediction
  bool ready = false;        // enough values to judge
};

int step_power(double from, double to) {
  if (!usable(from) || !usable(to) || std::signbit(from) != std::signbit(to)) {
    return 0;
  }
  const double l = std::log10(std::fabs(to / from));
  for (const int k : {3, 6}) {
    if (std::fabs(l - k) < kPowerTolerance) {
      return k;
    }
    if (std::fabs(l + k) < kPowerTolerance) {
      return -k;
    }
  }
  return 0;
}

double pow10(int k) { return std::pow(10.0, k); }

// A run of positions [first, last) at 10^power, found from the steps into and out
// of it.
struct Era {
  std::size_t first = 0;
  std::size_t last = 0;
  int power = 0;
  std::size_t items = 0;  // the items that stepped (row and era) or 1
  std::size_t present = 0;
  bool open = false;
  std::size_t item = kNoItem;  // kNoItem: every money item of the rows
};

struct Step {
  std::size_t at = 0;  // the first position after the step
  int power = 0;
  std::size_t items = 0;
  std::size_t present = 0;
};

// Pairs each step with the next one back. A step with none is an era to the
// line's end or from its start: the side `before_wrong` names, or the shorter.
std::vector<Era> pair_steps(const std::vector<Step>& steps, std::size_t n, std::size_t item,
                            const std::function<std::optional<bool>(std::size_t)>& before_wrong =
                                {}) {
  std::vector<Era> eras;
  for (std::size_t i = 0; i < steps.size();) {
    const Step& a = steps[i];
    if (i + 1 < steps.size() && steps[i + 1].power == -a.power) {
      eras.push_back({a.at, steps[i + 1].at, a.power, std::max(a.items, steps[i + 1].items),
                      std::max(a.present, steps[i + 1].present), false, item});
      i += 2;
      continue;
    }
    const std::optional<bool> wrong = before_wrong ? before_wrong(a.at) : std::nullopt;
    if (wrong ? !*wrong : n - a.at <= a.at) {
      eras.push_back({a.at, n, a.power, a.items, a.present, true, item});
    } else {
      eras.push_back({0, a.at, -a.power, a.items, a.present, true, item});
    }
    ++i;
  }
  return eras;
}

// The median log10 magnitude of a track's values in [from, to).
double level_of(const Track& t, std::size_t from, std::size_t to) {
  std::vector<double> l;
  for (std::size_t p = from; p < to; ++p) {
    if (usable(t.x[p])) {
      l.push_back(std::log10(std::fabs(t.x[p])));
    }
  }
  return middle(std::move(l));
}

// Whether a run is at its power of ten against the periods either side as a
// whole, not only at its two edges: its median level, put right, is theirs. A
// volatile item steps by a thousand now and then, and steps back elsewhere.
bool era_holds(const Track& t, const Era& e) {
  const std::size_t n = t.x.size();
  if (e.item != kNoItem) {
    // One item's run: the item keeps one sign through it and the periods either side,
    // as a volatile one crossing zero does not.
    std::optional<bool> negative;
    for (std::size_t p = e.first >= 2 ? e.first - 2 : 0; p < std::min(n, e.last + 2); ++p) {
      if (usable(t.x[p])) {
        if (negative && *negative != std::signbit(t.x[p])) {
          return false;
        }
        negative = std::signbit(t.x[p]);
      }
    }
  }
  const double inside = level_of(t, e.first, e.last);
  const double before = level_of(t, e.first >= 4 ? e.first - 4 : 0, e.first);
  const double after = level_of(t, e.last, std::min(n, e.last + 4));
  std::vector<double> outside;
  for (const double v : {before, after}) {
    if (std::isfinite(v)) {
      outside.push_back(v);
    }
  }
  if (!std::isfinite(inside) || outside.empty() ||
      std::fabs(inside - e.power - middle(std::move(outside))) >= kPowerTolerance) {
    return false;
  }
  // An item that sits at the run's level in periods outside it too is one whose
  // values range that widely: no step to put right.
  for (std::size_t p = e.first >= 8 ? e.first - 8 : 0; p < std::min(n, e.last + 8); ++p) {
    if ((p < e.first || p >= e.last) && usable(t.x[p]) &&
        std::fabs(std::log10(std::fabs(t.x[p])) - inside) < 0.5) {
      return false;
    }
  }
  return true;
}

// Rows at a power of ten: a step at one boundary taken by most money items of the
// row (at least three), and a step back.
// The median log10 magnitude of the tracks' values in [from, to).
double tracks_level(const std::vector<Track>& tracks, std::size_t from, std::size_t to) {
  std::vector<double> l;
  for (const Track& t : tracks) {
    for (std::size_t p = from; p < std::min(to, t.x.size()); ++p) {
      if (usable(t.x[p])) {
        l.push_back(std::log10(std::fabs(t.x[p])));
      }
    }
  }
  return middle(std::move(l));
}

// `other_level` is the company's other statements' median log10 magnitude: with one
// step and no step back, the side further from it is the wrong one.
std::vector<Era> row_eras(const Line& line, const std::vector<Track>& tracks,
                          double other_level) {
  std::vector<Step> steps;
  std::vector<Step> weak;
  for (std::size_t p = 1; p < line.size(); ++p) {
    if (line.prev[p] == kNone) {
      continue;
    }
    std::map<int, std::size_t> count;
    std::size_t present = 0;
    for (const Track& t : tracks) {
      if (usable(t.x[p - 1]) && usable(t.x[p])) {
        ++present;
        if (const int k = step_power(t.x[p - 1], t.x[p]); k != 0) {
          ++count[k];
        }
      }
    }
    for (const auto& [k, c] : count) {
      if (c >= 3 && 2 * c >= present) {
        steps.push_back({p, k, c, present});
      } else if (c >= 2 && 3 * c >= present) {
        weak.push_back({p, k, c, present});
      }
    }
  }
  // A step with no step back may have one that fewer items took (items near zero
  // stepping less cleanly): the next weaker step the other way closes it.
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const bool paired = (i + 1 < steps.size() && steps[i + 1].power == -steps[i].power) ||
                        (i > 0 && steps[i - 1].power == -steps[i].power);
    if (paired) {
      continue;
    }
    const auto back = std::find_if(weak.begin(), weak.end(), [&](const Step& w) {
      return w.at > steps[i].at && w.power == -steps[i].power &&
             (i + 1 == steps.size() || w.at < steps[i + 1].at);
    });
    if (back != weak.end()) {
      steps.insert(steps.begin() + static_cast<std::ptrdiff_t>(i) + 1, *back);
      ++i;
    }
  }
  const auto before_wrong = [&](std::size_t at) -> std::optional<bool> {
    const double before = tracks_level(tracks, 0, at);
    const double after = tracks_level(tracks, at, line.size());
    if (!std::isfinite(other_level) || !std::isfinite(before) || !std::isfinite(after)) {
      return std::nullopt;
    }
    return std::fabs(before - other_level) > std::fabs(after - other_level);
  };
  std::vector<Era> eras;
  for (const Era& e : pair_steps(steps, line.size(), kNoItem, before_wrong)) {
    std::size_t held = 0;
    std::size_t judged = 0;
    for (const Track& t : tracks) {
      if (std::isfinite(level_of(t, e.first, e.last))) {
        ++judged;
        if (era_holds(t, e)) {
          ++held;
        }
      }
    }
    if (2 * held > judged) {
      eras.push_back(e);
    }
  }
  return eras;
}

// One item's run of two or more values at a power of ten, outside the row eras. A
// single value is the value model's to judge; an unpaired step, a real change's.
std::vector<Era> item_eras(const Line& line, const Track& t) {
  std::vector<Step> steps;
  for (std::size_t p = 1; p < line.size(); ++p) {
    if (line.prev[p] != kNone && !t.in_era[p] && !t.in_era[p - 1]) {
      if (const int k = step_power(t.x[p - 1], t.x[p]); k != 0) {
        steps.push_back({p, k, 1, 1});
      }
    }
  }
  std::vector<Era> eras;
  for (const Era& e : pair_steps(steps, line.size(), t.item)) {
    if (!e.open && e.last - e.first >= 2 && era_holds(t, e)) {
      eras.push_back(e);
    }
  }
  return eras;
}

void repair(Track& t, const Era& e) {
  for (std::size_t p = e.first; p < e.last; ++p) {
    if (usable(t.x[p])) {
      t.w[p] = t.x[p] / pow10(e.power);
      t.in_era[p] = true;
    }
  }
}

double at(const std::vector<double>& v, std::ptrdiff_t p) {
  return p == kNone ? kNaN : v[static_cast<std::size_t>(p)];
}

std::ptrdiff_t step(const std::vector<std::ptrdiff_t>& link, std::ptrdiff_t p) {
  return p == kNone ? kNone : link[static_cast<std::size_t>(p)];
}

// The prediction at p from the periods around it, and from those before and after
// it alone. A quarterly flow is predicted from the same quarter a year either side,
// moved by the change since in the adjacent quarters (seasonality); any other line
// from the two periods either side.
// Up to four predictions of one value, and their middle.
struct Candidates {
  std::array<double, 4> values{};
  std::size_t size = 0;
  [[nodiscard]] double middle() {
    if (size == 0) {
      return kNaN;
    }
    for (std::size_t i = 1; i < size; ++i) {  // insertion sort: four at most
      for (std::size_t j = i; j > 0 && values.at(j - 1) > values.at(j); --j) {
        std::swap(values.at(j - 1), values.at(j));
      }
    }
    const std::size_t mid = size / 2;
    return size % 2 == 1 ? values.at(mid) : 0.5 * (values.at(mid - 1) + values.at(mid));
  }
};

void predict(const Line& line, Track& t, std::size_t p, const std::vector<double>& z) {
  const std::vector<double>& full = t.z;  // before and after read the values as stored
  const std::ptrdiff_t pv = line.prev[p];
  const std::ptrdiff_t nx = line.next[p];
  Candidates c;
  const auto add = [&c](double v) {
    if (std::isfinite(v)) {
      c.values.at(c.size++) = v;
    }
  };
  if (line.seasonal()) {
    const std::ptrdiff_t yb = line.year_before[p];
    const std::ptrdiff_t ya = line.year_after[p];
    add(at(z, yb) + at(z, pv) - at(z, step(line.prev, yb)));
    add(at(z, ya) + at(z, nx) - at(z, step(line.next, ya)));
    add(at(z, yb) + at(z, nx) - at(z, step(line.next, yb)));
    add(at(z, ya) + at(z, pv) - at(z, step(line.prev, ya)));
    if (c.size == 0) {
      add(at(z, yb));
      add(at(z, ya));
    }
    if (c.size == 0) {
      add(at(z, pv));
      add(at(z, nx));
    }
    const double b1 = at(full, yb) + at(full, pv) - at(full, step(line.prev, yb));
    const double a1 = at(full, ya) + at(full, nx) - at(full, step(line.next, ya));
    t.before[p] = std::isfinite(b1) ? b1 : std::isfinite(at(full, yb)) ? at(full, yb) : at(full, pv);
    t.after[p] = std::isfinite(a1) ? a1 : std::isfinite(at(full, ya)) ? at(full, ya) : at(full, nx);
  } else {
    add(at(z, step(line.prev, pv)));
    add(at(z, pv));
    add(at(z, nx));
    add(at(z, step(line.next, nx)));
    t.before[p] = at(full, pv);
    t.after[p] = at(full, nx);
  }
  t.pred[p] = c.middle();
}

// The middle of `values`, by selection: reorders them.
double select_middle(std::vector<double>& values) {
  const std::size_t n = values.size();
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(n / 2);
  std::nth_element(values.begin(), mid, values.end());
  if (n % 2 == 1) {
    return *mid;
  }
  return 0.5 * (*mid + *std::max_element(values.begin(), mid));
}

// The local scale of the residuals: their spread over the periods either side,
// shrunk to the class prior by its weight.
void scale(Track& t, const std::vector<double>& r, double prior, double weight) {
  const std::size_t n = r.size();
  std::vector<double> near;
  for (std::size_t p = 0; p < n; ++p) {
    near.clear();
    const std::size_t from = p > static_cast<std::size_t>(kWindow) ? p - kWindow : 0;
    const std::size_t to = std::min(n, p + kWindow + 1);
    for (std::size_t q = from; q < to; ++q) {
      if (std::isfinite(r[q])) {
        near.push_back(r[q]);
      }
    }
    double mad = 0.0;
    if (!near.empty()) {
      const double m = select_middle(near);
      for (double& v : near) {
        v = std::fabs(v - m);
      }
      mad = 1.4826 * select_middle(near);
    }
    const auto m = static_cast<double>(near.size());
    t.sigma[p] = std::max(prior, std::sqrt((m * mad * mad + weight * prior * prior) / (m + weight)));
  }
}

Track track_of(const Line& line, const StatementRows& st, std::size_t k) {
  Track t;
  t.item = k;
  const std::size_t n = line.size();
  t.x.resize(n);
  for (std::size_t p = 0; p < n; ++p) {
    t.x[p] = st.items[k][line.rows[p]];
  }
  t.w = t.x;
  t.in_era.assign(n, false);
  return t;
}

// Predictions and scales, made robust to the outliers themselves: each pass leaves
// out of every prediction the values the last pass found far from theirs, so that
// one bad value does not make its neighbours look bad too.
void prepare(const Line& line, Track& t, const StatementPriors& priors) {
  const std::size_t n = line.size();
  std::vector<double> magnitudes;
  std::size_t zeros = 0;
  std::size_t finite = 0;
  for (const double v : t.w) {
    if (std::isfinite(v)) {
      ++finite;
      if (v == 0.0) {
        ++zeros;
      } else {
        magnitudes.push_back(std::fabs(v));
      }
    }
  }
  // An item often at 0 is reported now and then: its zeros are DQ915's to judge.
  if (magnitudes.size() < kMinValues || 4 * zeros > finite) {
    return;
  }
  t.s = middle(std::move(magnitudes));
  t.z.assign(n, kNaN);
  for (std::size_t p = 0; p < n; ++p) {
    if (std::isfinite(t.w[p])) {
      t.z[p] = std::asinh(t.w[p] / t.s);
    }
  }
  t.pred.assign(n, kNaN);
  t.before.assign(n, kNaN);
  t.after.assign(n, kNaN);
  t.sigma.assign(n, kNaN);
  // Years move more than quarters, and a line of them has few points.
  const double prior = (line.flows() ? priors.flow_scale : priors.stock_scale) *
                       (line.type == PeriodType::kAnnual ? 1.5 : 1.0);
  std::vector<bool> outlier(n, false);
  std::vector<double> masked = t.z;
  std::vector<double> r(n, kNaN);
  for (int pass = 0; pass < kPasses; ++pass) {
    for (std::size_t p = 0; p < n; ++p) {
      masked[p] = outlier[p] ? kNaN : t.z[p];
    }
    for (std::size_t p = 0; p < n; ++p) {
      predict(line, t, p, masked);
      r[p] = t.z[p] - t.pred[p];
    }
    scale(t, r, prior, priors.prior_weight);
    // Each pass adds the values furthest from their predictions, each the worst of
    // the periods around it: a bad value's neighbours, whose predictions it pulled,
    // are less far out than it is, and are left in.
    bool changed = false;
    for (std::size_t p = 0; p < n; ++p) {
      const double score = std::fabs(r[p]) / t.sigma[p];
      if (outlier[p] || !std::isfinite(score) || score <= kOutlier) {
        continue;
      }
      bool worst = true;
      for (std::size_t q = p > kNear ? p - kNear : 0; q < std::min(n, p + kNear + 1); ++q) {
        worst = worst && (q == p || outlier[q] || !(std::fabs(r[q]) / t.sigma[q] > score));
      }
      if (worst) {
        outlier[p] = true;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  t.outliers = static_cast<std::size_t>(std::count(outlier.begin(), outlier.end(), true));
  t.judged = static_cast<std::size_t>(
      std::count_if(r.begin(), r.end(), [](double v) { return std::isfinite(v); }));
  t.ready = true;
}

// What the analysis of one view needs besides the line.
struct ViewContext {
  const Series& series;
  const StatementSettings& settings;
  const std::vector<ItemInfo>& info;
  const std::vector<IdentityTerms>& identities;
  const std::array<Years, 3>& years;
  bool have_items = false;
};

struct Scores {
  std::array<double, 4> log{};
  std::vector<Evidence> evidence;
  double repaired_bad = kNaN;
  int power = 0;
};

void add_evidence(Scores& sc, ValueHypothesis h, double bf, std::string feature, DetailValue value,
                  std::string note = {}) {
  sc.log.at(static_cast<std::size_t>(h)) += bf;
  sc.evidence.push_back({std::move(feature), std::move(value), bf, std::move(note)});
}

double expected_of(const Track& t, std::size_t p) { return t.s * std::sinh(t.pred[p]); }

// The other items of the row: how many moved as much as this one, or by the same
// factor (a row stored as a year, or year to date, moves every flow by one factor,
// the volatile ones too); and whether a subtotal moved by the same amount.
void co_movement(const ViewContext& vc, const std::vector<Track>& tracks, std::size_t self,
                 std::size_t p, Scores& sc) {
  const Track& t = tracks[self];
  const double change = t.x[p] - expected_of(t, p);
  const double factor = t.x[p] / expected_of(t, p);
  std::size_t judged = 0;
  std::size_t moved = 0;
  bool subtotal = false;
  for (std::size_t j = 0; j < tracks.size(); ++j) {
    const Track& o = tracks[j];
    if (j == self || !o.ready || !std::isfinite(o.z[p]) || !std::isfinite(o.pred[p])) {
      continue;
    }
    ++judged;
    const double other = o.x[p] / expected_of(o, p);
    const bool same_factor = std::fabs(std::log(factor)) > std::log(1.4) && other > 0.0 &&
                             factor > 0.0 && std::fabs(std::log(other / factor)) < std::log(1.3);
    if (std::fabs(o.z[p] - o.pred[p]) > kUnusual * o.sigma[p] || same_factor) {
      ++moved;
    }
    const double moved_by = o.x[p] - expected_of(o, p);
    if (vc.info[o.item].subtotal && std::fabs(moved_by - change) <= 0.35 * std::fabs(change)) {
      subtotal = true;
    }
  }
  if (subtotal) {
    add_evidence(sc, ValueHypothesis::kRealChange, kSubtotalMoves, "subtotal_moves", true);
  }
  const auto value = static_cast<std::int64_t>(moved);
  if (judged >= 3 && moved >= 2 && 5 * moved >= judged) {
    // Several items moving at once is the row's doing, not one value's: a real event,
    // or a row stored wrong (a copy, a year to date), which other checks judge. The
    // faults this model names leave the rest of the row as it was.
    const double against = kRowMoved;
    const std::string note = std::to_string(moved) + " of " + std::to_string(judged) + " items";
    add_evidence(sc, ValueHypothesis::kRealChange, kMovesWithOthers, "moves_with_others", value,
                 note);
    for (std::size_t h = 1; h < 4; ++h) {
      sc.log.at(h) -= against;
    }
  } else if (judged >= 2 && moved == 0) {
    add_evidence(sc, ValueHypothesis::kRealChange, -kMovesAlone, "moves_alone", value);
  }
}

// The value against the periods before it and after it alone: a step that holds,
// or one value that the next period takes back.
void level(const Track& t, std::size_t p, Scores& sc) {
  const double db = t.z[p] - t.before[p];
  const double da = t.z[p] - t.after[p];
  const double s = t.sigma[p];
  if (!std::isfinite(db) || !std::isfinite(da)) {
    return;
  }
  if (std::fabs(db) > kUnusual * s && std::fabs(da) < std::max(3.0 * s, 0.3 * std::fabs(db))) {
    add_evidence(sc, ValueHypothesis::kRealChange, kHolds, "level_holds", true);
    for (std::size_t h = 1; h < 4; ++h) {
      sc.log.at(h) -= kHolds - 1.0;
    }
  } else if (std::fabs(db) > kUnusual * s && std::fabs(da) > kUnusual * s &&
             std::signbit(db) == std::signbit(da)) {
    add_evidence(sc, ValueHypothesis::kRealChange, -kReverts, "reverts", true);
  }
}

// The row's identities, with the stored value and each repair: one that holds as
// stored is evidence the value is real, and one that fails and holds once repaired
// is evidence for that repair.
void identities(const ViewContext& vc, const Track& t, std::size_t i, double x, double pred,
                double sigma, Scores& sc, bool signs) {
  const StatementRows& st = vc.series.statements;
  bool held = false;
  std::array<bool, 4> repaired{};
  for (const IdentityTerms& terms : vc.identities) {
    if (std::find(terms.items.begin(), terms.items.end(), t.item) == terms.items.end()) {
      continue;
    }
    const std::optional<Gap> stored = identity_gap(st, terms, vc.settings, i, t.item, x);
    if (!stored) {
      continue;
    }
    if (stored->holds()) {
      if (!held) {
        // A single wrong value breaks every identity it is a term of.
        add_evidence(sc, ValueHypothesis::kRealChange, kIdentityHolds, "identity_holds",
                     terms.identity->name);
      }
      held = true;
      continue;
    }
    // The value the identity needs: the stored one less the gap, as its term counts.
    const auto term = static_cast<std::size_t>(
        std::find(terms.items.begin(), terms.items.end(), t.item) - terms.items.begin());
    const double needed = x - stored->gap / terms.signs[term];
    const double z_needed = std::asinh(needed / t.s);
    const std::array<std::pair<ValueHypothesis, double>, 3> repairs{
        {{ValueHypothesis::kBadValue, needed},
         {ValueHypothesis::kUnitError, x / pow10(sc.power)},
         {ValueHypothesis::kSignError, -x}}};
    for (const auto& [h, v] : repairs) {
      const auto hi = static_cast<std::size_t>(h);
      if (repaired.at(hi) || (h == ValueHypothesis::kSignError && !signs) ||
          (h == ValueHypothesis::kUnitError && sc.power == 0)) {
        continue;
      }
      if (h == ValueHypothesis::kBadValue &&
          std::fabs(z_needed - pred) > std::max(kUnusual * sigma, 0.3)) {
        continue;  // the identity's value does not fit the periods around it either
      }
      const std::optional<Gap> after = identity_gap(st, terms, vc.settings, i, t.item, v);
      if (after && after->holds()) {
        repaired.at(hi) = true;
        if (h == ValueHypothesis::kBadValue) {
          sc.repaired_bad = needed;
        }
        add_evidence(sc, h, kIdentityRepair, "identity_repair", terms.identity->name);
      }
    }
  }
}

// The values the row's failing identities need of the item: each the stored value
// less the identity's gap, as the item's term counts in it.
std::vector<double> implied_values(const ViewContext& vc, const Track& t, std::size_t i, double x) {
  std::vector<double> out;
  for (const IdentityTerms& terms : vc.identities) {
    const auto it = std::find(terms.items.begin(), terms.items.end(), t.item);
    if (it == terms.items.end()) {
      continue;
    }
    const std::optional<Gap> g = identity_gap(vc.series.statements, terms, vc.settings, i, t.item, x);
    if (g && !g->holds()) {
      out.push_back(x - g->gap / terms.signs[static_cast<std::size_t>(it - terms.items.begin())]);
    }
  }
  return out;
}

// The fiscal year's quarters against its year (a flow, with --items).
void quarter_sum(const ViewContext& vc, const Track& t, std::size_t i, double x, double expected,
                 Scores& sc, bool signs) {
  const StatementRows& st = vc.series.statements;
  const Years& years = vc.years.at(static_cast<std::size_t>(st.statement[i]));
  const std::optional<Gap> stored = quarter_gap(st, years, vc.settings, i, t.item, x);
  if (!stored) {
    return;
  }
  if (stored->holds()) {
    add_evidence(sc, ValueHypothesis::kRealChange, kQuarterHolds, "quarter_sum_holds", true);
    return;
  }
  const std::array<std::pair<ValueHypothesis, double>, 3> repairs{
      {{ValueHypothesis::kBadValue, x - stored->gap * (st.period_type[i] == PeriodType::kAnnual
                                                           ? -1.0
                                                           : 1.0)},
       {ValueHypothesis::kUnitError, x / pow10(sc.power)},
       {ValueHypothesis::kSignError, -x}}};
  for (const auto& [h, v] : repairs) {
    if ((h == ValueHypothesis::kSignError && !signs) ||
        (h == ValueHypothesis::kUnitError && sc.power == 0)) {
      continue;
    }
    if (h == ValueHypothesis::kBadValue &&
        std::fabs(std::asinh(v / t.s) - std::asinh(expected / t.s)) > 0.3) {
      continue;
    }
    const std::optional<Gap> after = quarter_gap(st, years, vc.settings, i, t.item, v);
    if (after && after->holds()) {
      add_evidence(sc, h, kQuarterRepair, "quarter_sum_repair", true);
    }
  }
}

// The share of the item's other values in the line with the value's sign.
double same_sign_share(const Track& t, std::size_t p) {
  std::size_t same = 0;
  std::size_t total = 0;
  for (std::size_t q = 0; q < t.x.size(); ++q) {
    if (q != p && usable(t.x[q])) {
      ++total;
      if (std::signbit(t.x[q]) == std::signbit(t.x[p])) {
        ++same;
      }
    }
  }
  return total == 0 ? 1.0 : static_cast<double>(same) / static_cast<double>(total);
}

std::array<double, 4> softmax(const std::array<double, 4>& log) {
  std::array<double, 4> p{};
  const double top = *std::max_element(log.begin(), log.end());
  double sum = 0.0;
  for (std::size_t h = 0; h < 4; ++h) {
    p.at(h) = std::isfinite(log.at(h)) ? std::exp(log.at(h) - top) : 0.0;
    sum += p.at(h);
  }
  for (double& v : p) {
    v /= sum;
  }
  return p;
}

// Whether position p of the track is worth judging: screening (plan section 4.1).
bool screened(const Line& line, const Track& t, std::size_t p, bool signs) {
  const double x = t.x[p];
  const double r = t.z[p] - t.pred[p];
  if (stats::student_t_two_sided_tail(r / t.sigma[p], kNu) < kTail) {
    return true;
  }
  const double before = at(t.x, line.prev[p]);
  const double after = at(t.x, line.next[p]);
  if (usable(before) && usable(after)) {
    const double a = x / before;
    const double b = x / after;
    if ((a >= kRatio && b >= kRatio) || (a > 0.0 && a <= 1.0 / kRatio && b > 0.0 &&
                                          b <= 1.0 / kRatio)) {
      return true;
    }
    if (signs && std::signbit(before) == std::signbit(after) &&
        std::signbit(x) != std::signbit(before)) {
      return true;  // a sign change on an item with a sign
    }
  }
  return false;
}

std::optional<ValueFinding> judge(const ViewContext& vc, const Line& line,
                                  const std::vector<Track>& tracks, std::size_t self,
                                  std::size_t p) {
  const Track& t = tracks[self];
  const double x = t.x[p];
  const double pred = t.pred[p];
  const double sigma = t.sigma[p];
  if (!usable(x) || t.in_era[p] || !std::isfinite(pred) || !std::isfinite(t.z[p]) ||
      at(t.x, line.prev[p]) == 0.0 || at(t.x, line.next[p]) == 0.0) {
    return std::nullopt;  // next to a zero: DQ915's (absent-as-zero) to judge
  }
  const ItemInfo& info = vc.info[t.item];
  // A sign is judged on an item that keeps one: the other sign in at least 19 of
  // every 20 of its other periods.
  const bool signs = vc.have_items && info.signs && same_sign_share(t, p) <= 0.05;
  if (!screened(line, t, p, signs)) {
    return std::nullopt;
  }
  const StatementPriors& priors = vc.settings.priors;
  const double r = t.z[p] - pred;
  Scores sc;
  // A real change is mostly small, and sometimes as large as an error: as often as
  // the item's other values are far from their predictions.
  const double wide = std::max(kBadScale, 4 * sigma);
  const double large = (static_cast<double>(t.outliers > 0 ? t.outliers - 1 : 0) + 0.1) /
                       (static_cast<double>(t.judged) + 10.0);
  // The log likelihood of a true residual under the real-change model: what the
  // value is under real_change, and what its repair is under an error.
  const auto real_like = [&](double residual) {
    return std::log((1.0 - large) * std::exp(stats::student_t_log_pdf(residual, kNu, sigma)) +
                    large * std::exp(stats::student_t_log_pdf(residual, kNu, wide)));
  };
  sc.log.at(0) =
      std::log(1.0 - priors.bad_value - priors.unit_error - priors.sign_error) + real_like(r);
  sc.log.at(1) = std::log(priors.bad_value) + stats::student_t_log_pdf(r, kNu, wide);
  // A unit error is the stored value at a power of ten of the expected one, and a
  // sign error its negative, each to within a factor of two: an expected value near
  // zero names neither.
  const double expected = t.s * std::sinh(pred);
  const double ratio = usable(expected) ? x / expected : 0.0;
  const double magnitude = ratio != 0.0 ? std::log10(std::fabs(ratio)) : kNaN;
  sc.log.at(2) = -std::numeric_limits<double>::infinity();
  // Or the value is at a power of ten of the item's usual magnitude, where what is
  // expected is near zero (an item that crosses it).
  // Or an identity of the row needs the value at a power of ten.
  const double usual = std::log10(std::fabs(x) / t.s);
  const std::vector<double> implied = implied_values(vc, t, line.rows[p], x);
  for (const int k : kPowers) {
    const bool near_expected = ratio > 0.0 && std::fabs(magnitude - k) < kPowerTolerance;
    const bool near_usual = (ratio <= 0.0 || std::fabs(expected) < t.s) &&
                            std::fabs(usual - k) < kPowerTolerance;
    const bool near_identity = std::any_of(implied.begin(), implied.end(), [&](double v) {
      return usable(v) && std::fabs(v - x / pow10(k)) <= 0.05 * std::fabs(v);
    });
    if (!near_expected && !near_usual && !near_identity) {
      continue;
    }
    const double rk = std::asinh(x / pow10(k) / t.s) - pred;
    sc.log.at(2) = std::log(priors.unit_error) + real_like(rk);
    sc.power = k;
  }
  sc.log.at(3) = signs && ratio < 0.0 && std::fabs(magnitude) < kPowerTolerance
                     ? std::log(priors.sign_error) + real_like(std::asinh(-x / t.s) - pred)
                     : -std::numeric_limits<double>::infinity();
  sc.evidence.push_back({"predictive", r / sigma, 0.0, "residual / scale, asinh scale"});

  const std::size_t i = line.rows[p];
  co_movement(vc, tracks, self, p, sc);
  level(t, p, sc);
  identities(vc, t, i, x, pred, sigma, sc, signs);
  if (vc.have_items && info.flow) {
    quarter_sum(vc, t, i, x, expected, sc, signs);
  }
  if (signs) {
    add_evidence(sc, ValueHypothesis::kSignError, kSignHistory, "sign_history",
                 same_sign_share(t, p));
  }

  ValueFinding f;
  f.scope = ValueScope::kValue;
  f.row = f.last_row = i;
  f.item = t.item;
  f.posterior = softmax(sc.log);
  f.value = x;
  f.expected = expected;
  f.power = sc.power;
  f.score = std::fabs(r) / sigma;
  f.provisional = line.next[p] == kNone;
  f.notable = std::fabs(std::log(std::fabs(ratio))) >= std::log(2.0) && ratio > 0.0 &&
              f.score >= 8.0 && large < 0.05;
  switch (f.error()) {
    case ValueHypothesis::kUnitError:
      f.repaired = x / pow10(sc.power);
      break;
    case ValueHypothesis::kSignError:
      f.repaired = -x;
      break;
    default:
      f.repaired = std::isfinite(sc.repaired_bad) ? sc.repaired_bad : expected;
      break;
  }
  f.evidence = std::move(sc.evidence);
  return f;
}

ValueFinding era_finding(const Line& line, const Era& e) {
  ValueFinding f;
  f.scope = e.item == kNoItem && e.last - e.first == 1 ? ValueScope::kRow : ValueScope::kEra;
  f.row = line.rows[e.first];
  f.last_row = line.rows[e.last - 1];
  f.item = e.item;
  f.rows = e.last - e.first;
  f.items = e.items;
  f.power = e.power;
  f.open = e.open;
  const double p = e.open ? kOpenEra : kPairedEra;
  f.posterior = {1.0 - p, 0.0, p, 0.0};
  f.provisional = e.last == line.size();
  const std::string note = e.item == kNoItem ? std::to_string(e.items) + " of " +
                                                   std::to_string(e.present) + " items"
                                             : std::string{};
  f.evidence.push_back({e.open ? "unit_step" : "unit_steps", static_cast<std::int64_t>(e.power),
                        std::log(p / (1.0 - p)), note});
  return f;
}

// Judges a view's lines; a line `judged` already holds the same rows of is left
// out (each line of the as-first-reported view that no restatement changed).
std::vector<Line> analyze_view(const ViewContext& vc, const std::vector<std::size_t>& view,
                               const std::vector<Line>& judged, std::vector<ValueFinding>& out) {
  const Series& s = vc.series;
  const StatementRows& st = s.statements;
  std::vector<Line> lines = lines_of(s, view, vc.settings);
  std::erase_if(lines, [&judged](const Line& line) {
    return std::any_of(judged.begin(), judged.end(),
                       [&line](const Line& j) { return j.rows == line.rows; });
  });
  std::vector<double> levels;  // each line's median log10 magnitude
  for (const Line& line : lines) {
    std::vector<double> l;
    for (std::size_t k = 0; k < st.items.size(); ++k) {
      if (vc.info[k].judged) {
        for (const std::size_t r : line.rows) {
          if (usable(st.items[k][r])) {
            l.push_back(std::log10(std::fabs(st.items[k][r])));
          }
        }
      }
    }
    levels.push_back(middle(std::move(l)));
  }
  for (std::size_t li = 0; li < lines.size(); ++li) {
    const Line& line = lines[li];
    std::vector<double> others;
    for (std::size_t o = 0; o < lines.size(); ++o) {
      if (o != li && lines[o].type == line.type && std::isfinite(levels[o])) {
        others.push_back(levels[o]);
      }
    }
    const double other_level = middle(std::move(others));
    std::vector<Track> tracks;
    for (std::size_t k = 0; k < st.items.size(); ++k) {
      const ItemInfo& info = vc.info[k];
      if (!info.judged || (info.known && info.statement != line.statement)) {
        continue;
      }
      Track t = track_of(line, st, k);
      if (static_cast<std::size_t>(std::count_if(t.x.begin(), t.x.end(), usable)) >= kMinValues) {
        tracks.push_back(std::move(t));
      }
    }
    if (tracks.empty()) {
      continue;
    }
    for (const Era& e : row_eras(line, tracks, other_level)) {
      for (Track& t : tracks) {
        repair(t, e);
      }
      out.push_back(era_finding(line, e));
    }
    for (Track& t : tracks) {
      for (const Era& e : item_eras(line, t)) {
        repair(t, e);
        out.push_back(era_finding(line, e));
      }
    }
    for (Track& t : tracks) {
      prepare(line, t, vc.settings.priors);
    }
    for (std::size_t j = 0; j < tracks.size(); ++j) {
      if (!tracks[j].ready) {
        continue;
      }
      for (std::size_t p = 0; p < line.size(); ++p) {
        if (std::optional<ValueFinding> f = judge(vc, line, tracks, j, p)) {
          out.push_back(std::move(*f));
        }
      }
    }
  }
  return lines;
}

}  // namespace

StatementValueAnalysis analyze_statement_values(const Series& series, const StatementViews& views,
                                                const StatementSettings& settings,
                                                const std::vector<LineItem>* items) {
  StatementValueAnalysis analysis;
  const StatementRows& st = series.statements;
  if (series.kind != SeriesKind::kStatements || st.size() == 0) {
    return analysis;
  }
  const std::vector<ItemInfo> info = item_info(st, items);
  const std::vector<IdentityTerms> identities = identity_terms(st, settings);
  // A value both views share is one finding, kept with the higher p_error.
  // Keyed by period, item and scope, and kept apart where the value differs: a
  // value a restatement left as it was is one finding, on the in-force version.
  std::map<std::tuple<std::int32_t, int, int, std::size_t, int>, std::vector<std::size_t>> seen;
  std::vector<Line> judged;  // the in-force view's lines
  std::vector<bool> shared(st.size(), false);  // rows of lines both views hold alike
  for (const bool in_force : {true, false}) {
    const std::vector<std::size_t>& view = in_force ? views.in_force : views.first_reported;
    const std::array<Years, 3> years = years_of(st, view);
    const ViewContext vc{series, settings, info, identities, years, items != nullptr};
    std::vector<ValueFinding> found;
    if (in_force) {
      judged = analyze_view(vc, view, {}, found);
    } else {
      const std::vector<Line> own = analyze_view(vc, view, judged, found);
      for (const Line& line : judged) {
        if (std::none_of(own.begin(), own.end(),
                         [&line](const Line& o) { return o.statement == line.statement &&
                                                         o.type == line.type; })) {
          for (const std::size_t r : line.rows) {
            shared[r] = true;
          }
        }
      }
      for (ValueFinding& f : analysis.findings) {
        f.first_reported = f.first_reported || shared[f.row];
      }
    }
    for (ValueFinding& f : found) {
      (in_force ? f.in_force : f.first_reported) = true;
      const auto key = std::make_tuple(series.date[f.row].days(),
                                       static_cast<int>(st.statement[f.row]),
                                       static_cast<int>(st.period_type[f.row]), f.item,
                                       static_cast<int>(f.scope));
      std::vector<std::size_t>& same = seen[key];
      const auto match = std::find_if(same.begin(), same.end(), [&](std::size_t k) {
        const ValueFinding& o = analysis.findings[k];
        return o.row == f.row || (o.value == f.value && o.rows == f.rows && o.power == f.power);
      });
      if (match == same.end()) {
        same.push_back(analysis.findings.size());
        analysis.findings.push_back(std::move(f));
        continue;
      }
      ValueFinding& kept = analysis.findings[*match];
      const bool both_in_force = kept.in_force || f.in_force;
      if (f.p_error() > kept.p_error() && kept.row == f.row) {
        kept = std::move(f);
      }
      kept.in_force = both_in_force;
      kept.first_reported = true;
    }
  }
  std::sort(analysis.findings.begin(), analysis.findings.end(),
            [](const ValueFinding& a, const ValueFinding& b) {
              return std::tie(a.row, a.item, a.scope) < std::tie(b.row, b.item, b.scope);
            });
  return analysis;
}

}  // namespace dorq
