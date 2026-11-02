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

constexpr double kStepSd = 0.04;   // a clean step's spread, on the log scale (issuance meanwhile)
constexpr double kCapSd = 0.25;    // the market cap's move between two filings, on the log scale
constexpr double kRealSd = 0.35;   // a real issuance's step (an offering, a merger)
constexpr double kMinStep = 0.22;  // log 1.25: smaller steps are issuance
constexpr double kMatch = 0.1;     // a step at a split's ratio, to within this on the log scale
constexpr double kSplitPrior = 0.01;
constexpr double kTermsPrior = 0.01;
constexpr double kUnitPrior = 0.002;
constexpr double kLargeIssuance = 0.05;  // the share of a count's steps that are offerings or mergers
constexpr double kOnFileAgainst = 8.0;  // a split on file in the window would have converted it
constexpr std::array<int, 4> kPowers{3, -3, 6, -6};

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

Nearest nearest(double d, const PriceSettings& price) {
  Nearest n;
  double total = 0.0;
  for (const SplitRatio& r : price.split_ratios) {
    total += split_weight(r);
  }
  double best = std::numeric_limits<double>::infinity();
  for (const SplitRatio& r : price.split_ratios) {
    n.ratio_ll = log_add(n.ratio_ll, std::log(split_weight(r) / total) +
                                         normal_log_pdf(d, log_of(r), kStepSd));
    if (std::fabs(d - log_of(r)) < best) {
      best = std::fabs(d - log_of(r));
      n.ratio = r;
    }
  }
  if (best > kMatch) {
    n.ratio.reset();
  }
  for (const int k : kPowers) {
    const double ll = normal_log_pdf(d, k * std::numbers::ln10, kStepSd) - std::log(4.0);
    if (ll > n.power_ll) {
      n.power_ll = ll;
      n.power = std::fabs(d - k * std::numbers::ln10) < kMatch ? k : 0;
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

// The context a view's share counts are judged in.
struct ShareContext {
  const Series& series;
  const StatementSettings& settings;
  const PriceSettings& price;
  const std::vector<SplitAction>& splits;
  Date latest;  // the terms the counts are put into: the latest knowledge date
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

// The split on file with its ex-date in (from, to], whose ratio, either way,
// the step is; nullptr for none.
const SplitAction* split_in(const ShareContext& sc, Date from, Date to, double d) {
  for (const SplitAction& split : sc.splits) {
    const double l = std::log(split.numerator / split.denominator);
    if (split.ex_date > from && split.ex_date <= to &&
        (std::fabs(std::fabs(d) - std::fabs(l)) < kMatch ||
         std::fabs(std::fabs(d) - 2.0 * std::fabs(l)) < kMatch)) {
      return &split;
    }
  }
  return nullptr;
}

bool any_split_in(const ShareContext& sc, Date from, Date to) {
  return std::any_of(sc.splits.begin(), sc.splits.end(),
                     [&](const SplitAction& s) { return s.ex_date > from && s.ex_date <= to; });
}

// DQ921: one item's steps along a line, put into the latest terms.
void judge_steps(const ShareContext& sc, const Line& line, std::size_t item,
                 std::vector<ShareFinding>& out) {
  const Series& s = sc.series;
  const StatementRows& st = s.statements;
  const std::size_t n = line.rows.size();
  std::vector<double> raw(n);
  std::vector<double> converted(n);
  for (std::size_t p = 0; p < n; ++p) {
    const std::size_t r = line.rows[p];
    raw[p] = st.items[item][r];
    converted[p] = raw[p] * to_latest(sc, st.known_from[r]);
  }
  std::vector<double> steps;
  for (std::size_t p = 1; p < n; ++p) {
    if (line.follows[p] && usable(converted[p]) && usable(converted[p - 1])) {
      steps.push_back(std::fabs(std::log(converted[p] / converted[p - 1])));
    }
  }
  if (steps.size() < 3) {
    return;
  }
  std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2),
                   steps.end());
  const double noise = std::max(0.02, 1.4826 * steps[steps.size() / 2]);
  for (std::size_t p = 1; p < n; ++p) {
    if (!line.follows[p] || !usable(converted[p]) || !usable(converted[p - 1])) {
      continue;
    }
    const double d = std::log(converted[p] / converted[p - 1]);
    if (std::fabs(d) < std::max(kMinStep, 5.0 * noise)) {
      continue;
    }
    const std::size_t a = line.rows[p - 1];
    const std::size_t b = line.rows[p];
    const Nearest near = nearest(d, sc.price);
    if (!near.ratio && near.power == 0) {
      continue;  // not a clean ratio: issuance or buybacks
    }
    ShareFinding f;
    f.kind = ShareFinding::Kind::kStep;
    f.row = f.last_row = b;
    f.before = a;
    f.item = item;
    f.factor = std::exp(d);
    f.value = raw[p];
    f.ratio = near.ratio;
    f.power = near.power;
    f.window_from = st.known_from[a];
    f.window_to = st.known_from[b];
    // With close: the implied market cap, the counts as stored times the raw close at
    // their knowledge dates, both in the terms of that date.
    std::optional<double> cap;
    if (usable(s.close[a]) && usable(s.close[b]) && usable(raw[p]) && usable(raw[p - 1])) {
      cap = std::log(raw[p] * s.close[b] / (raw[p - 1] * s.close[a]));
      f.cap_step = cap;
    }
    const SplitAction* on_file = split_in(sc, f.window_from, f.window_to, d);
    // A real change in the count: mostly the item's own small moves, now and then an
    // offering or a merger.
    const double real = log_add(std::log(1.0 - kLargeIssuance) + normal_log_pdf(d, 0.0, noise),
                                std::log(kLargeIssuance) + normal_log_pdf(d, 0.0, kRealSd));
    std::array<double, 4> log{};
    log[0] = std::log(1.0 - kSplitPrior - kTermsPrior - kUnitPrior) + real +
             (cap ? normal_log_pdf(*cap, d, kCapSd) : 0.0);
    log[1] = std::log(kSplitPrior) + near.ratio_ll + (cap ? normal_log_pdf(*cap, 0.0, kCapSd) : 0.0) -
             (any_split_in(sc, f.window_from, f.window_to) ? kOnFileAgainst : 0.0);
    log[2] = on_file != nullptr ? std::log(kTermsPrior) +
                                      normal_log_pdf(std::fabs(d), std::fabs(d), kStepSd) +
                                      (cap ? normal_log_pdf(*cap, d, kCapSd) : 0.0)
                                : -std::numeric_limits<double>::infinity();
    log[3] = std::log(kUnitPrior) + near.power_ll + (cap ? normal_log_pdf(*cap, d, kCapSd) : 0.0);
    f.posterior = softmax(log);
    if (on_file != nullptr) {
      f.split_on_file = on_file->ratio_text() + " " + on_file->ex_date.to_string();
      f.evidence.push_back({"split_on_file", f.split_on_file, 0.0, "in the window"});
    } else {
      f.evidence.push_back({"split_on_file", false, 0.0, "none in the window"});
    }
    if (cap) {
      f.evidence.push_back({"market_cap_step", *cap, normal_log_pdf(*cap, 0.0, kCapSd) -
                                                          normal_log_pdf(*cap, d, kCapSd),
                            "level for a split, a step for a count stored wrong"});
    }
    // A step back to the level before: the periods between are the ones stored
    // wrong (terms or units), one report on the first.
    for (std::size_t q = p + 1; q < n && q <= p + 8; ++q) {
      if (!line.follows[q] || !usable(converted[q]) || !usable(converted[q - 1])) {
        break;
      }
      const double back = std::log(converted[q] / converted[q - 1]);
      if (std::fabs(back + d) < kMatch && f.explanation() != ShareHypothesis::kUnreportedSplit) {
        f.last_row = line.rows[q - 1];
        p = q;  // the step back is the same error's end
        break;
      }
      if (std::fabs(back) >= std::max(kMinStep, 5.0 * noise)) {
        break;
      }
    }
    f.provisional = p + 1 >= n;
    out.push_back(std::move(f));
  }
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
  for (const auto& [key, rows] : periods) {
    const auto [i, b] = rows;
    if (i == kNone || b == kNone) {
      continue;
    }
    for (const std::size_t w : weighted) {
      for (const std::size_t o : outstanding) {
        const double wv = st.items[w][i];
        const double ov = st.items[o][b];
        if (!usable(wv) || !usable(ov)) {
          continue;
        }
        // Each in the terms of its own version's knowledge date: put into one.
        const double d = std::log(wv * to_latest(sc, st.known_from[i]) /
                                  (ov * to_latest(sc, st.known_from[b])));
        if (std::fabs(d) < kMinStep) {
          continue;
        }
        const Nearest near = nearest(d, sc.price);
        if (!near.ratio && near.power == 0) {
          continue;
        }
        const int length = st.period_type[i] == PeriodType::kAnnual ? 366 : 92;
        const Date start = Date::from_days(s.date[i].days() - length);
        const Date filed = std::max(st.known_from[i], st.known_from[b]);
        if (near.power == 0 && split_in(sc, start, filed, d) != nullptr) {
          continue;  // a split in the period, or before its filing, explains it
        }
        ShareFinding f;
        f.kind = ShareFinding::Kind::kMismatch;
        f.row = f.last_row = i;
        f.before = b;
        f.item = w;
        f.other = o;
        f.factor = std::exp(d);
        f.value = wv;
        f.other_value = ov;
        f.ratio = near.ratio;
        f.power = near.power;
        const double p = near.power != 0 ? 0.95 : 0.9;
        f.posterior = {1.0 - p, p, 0.0, 0.0};
        out.push_back(std::move(f));
      }
    }
  }
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
  const ShareContext sc{series, settings, price, splits, latest};
  const std::vector<std::size_t> weighted = share_items(st, items, Statement::kIncome);
  const std::vector<std::size_t> outstanding = share_items(st, items, Statement::kBalanceSheet);
  // A finding both views share is one, on the in-force version.
  std::map<std::tuple<std::size_t, std::size_t, int>, std::size_t> seen;
  for (const bool in_force : {true, false}) {
    const std::vector<std::size_t>& view = in_force ? views.in_force : views.first_reported;
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
  return analysis;
}

}  // namespace dorq
