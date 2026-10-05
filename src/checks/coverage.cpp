#include "checks/coverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.hpp"
#include "dorq/number.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

constexpr std::array<std::string_view, 7> kWeekdays = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

std::vector<Date> unique_dates(const Series& series) {
  std::vector<Date> dates(series.date.begin(), series.date.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  return dates;
}

Violation make(const CheckInfo& info, Severity severity, Date date) {
  Violation v;
  v.check = &info;
  v.severity = severity;
  v.date = date;
  return v;
}

std::string weekday_counts(const std::array<int, 7>& counts) {
  std::string out;
  for (std::size_t w = 0; w < 7; ++w) {
    out += w == 0 ? "" : " ";
    out += kWeekdays.at(w).substr(0, 3);
    out += ' ';
    out += std::to_string(counts.at(w));
  }
  return out;
}

}  // namespace

std::string format_probability(double p) {
  if (p > 0.995) {
    return ">0.99";
  }
  const double rounded = std::round(p * 100.0) / 100.0;
  std::string text = format_number(rounded);
  if (text.find('.') == std::string::npos) {
    text += ".0";
  }
  return text;
}

// ---------------------------------------------------------------------------
// DQ105 non-session-bar

const CheckInfo& NonSessionBar::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ105",
      .name = "non-session-bar",
      .summary = "a bar dated on a day the calendar has no session",
      .default_severity = Severity::kWarn,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void NonSessionBar::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.frequency != Frequency::kDaily) {
    return;
  }
  const Series& s = context.series;
  std::vector<std::size_t> rows;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!context.calendar.is_session(s.date[i])) {
      rows.push_back(i);
    }
  }
  const std::string calendar = context.calendar.description();
  // A few are reported one by one. Many are one finding -- usually a history
  // dated a day off (DQ206) or a calendar that does not fit the series -- and
  // reporting them one by one would bury it.
  constexpr std::size_t kOneByOne = 10;
  if (rows.size() <= kOneByOne) {
    for (const std::size_t i : rows) {
      Violation v = make(info(), Severity::kWarn, s.date[i]);
      v.line = s.line[i];
      v.message = "a bar on a " +
                  std::string{kWeekdays.at(static_cast<std::size_t>(s.date[i].weekday()))} +
                  ", which is not a session on " + calendar;
      v.detail = {{"calendar", calendar}};
      out.push_back(std::move(v));
    }
    return;
  }
  std::string examples;
  for (std::size_t k = 0; k < std::min<std::size_t>(rows.size(), 5); ++k) {
    examples += (k == 0 ? "" : ", ") + s.date[rows[k]].to_string();
  }
  Violation v = make(info(), Severity::kWarn, s.date[rows.front()]);
  v.end_date = s.date[rows.back()];
  v.message = with_commas(static_cast<long long>(rows.size())) + " of " +
              with_commas(static_cast<long long>(s.size())) + " bars fall on days " + calendar +
              " has no session (" + examples + (rows.size() > 5 ? ", ..." : "") +
              "); see DQ206 for a history dated a day off, or pass a calendar that fits";
  v.detail = {{"bars", static_cast<long long>(rows.size())}, {"calendar", calendar}};
  out.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// DQ206 date-shift

const CheckInfo& DateShift::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ206",
      .name = "date-shift",
      .summary = "weekday counts show the history dated a day early or late",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void DateShift::run(const SeriesContext& context, std::vector<Violation>& out) const {
  constexpr std::size_t kMinBars = 60;
  constexpr double kNoise = 0.01;       // bars that fit no weekday pattern
  constexpr double kPriorShift = 0.01;  // per direction
  if (context.frequency != Frequency::kDaily) {
    return;
  }
  const std::vector<Date> dates = unique_dates(context.series);
  if (dates.size() < kMinBars) {
    return;
  }
  const std::array<int, 7> sessions =
      context.calendar.sessions_by_weekday(dates.front(), dates.back());
  // A calendar with sessions every day of the week says nothing about a shift.
  if (std::all_of(sessions.begin(), sessions.end(), [](int c) { return c > 0; })) {
    return;
  }
  std::array<int, 7> bars{};
  for (const Date d : dates) {
    ++bars.at(static_cast<std::size_t>(d.weekday()));
  }
  double total_sessions = 0.0;
  for (const int c : sessions) {
    total_sessions += c;
  }
  if (total_sessions == 0.0) {
    return;
  }
  // Weekday probabilities if the dates are right (shift 0), a day late (+1: each
  // session's bar is dated the day after), or a day early (-1).
  const auto log_likelihood = [&](int shift) {
    double sum = 0.0;
    for (std::size_t w = 0; w < 7; ++w) {
      const std::size_t source = (w + 7 - static_cast<std::size_t>((shift + 7) % 7)) % 7;
      const double q = (1.0 - kNoise) * sessions.at(source) / total_sessions + kNoise / 7.0;
      sum += bars.at(w) * std::log(q);
    }
    return sum;
  };
  const double aligned = std::log(1.0 - 2.0 * kPriorShift) + log_likelihood(0);
  const double late = std::log(kPriorShift) + log_likelihood(1);
  const double early = std::log(kPriorShift) + log_likelihood(-1);
  const double total = stats::log_add(stats::log_add(aligned, late), early);
  const double p_late = std::exp(late - total);
  const double p_early = std::exp(early - total);
  const double p_shift = p_late + p_early;
  const auto severity = context.thresholds.for_probability(p_shift);
  if (!severity) {
    return;
  }
  const bool is_late = p_late >= p_early;
  Violation v = make(info(), *severity, dates.front());
  v.end_date = dates.back();
  v.p_error = p_shift;
  v.message = std::string{"weekday counts fit dates one day "} + (is_late ? "later" : "earlier") +
              " than the sessions they belong to (bars " + weekday_counts(bars) + "; " +
              context.calendar.description() + " sessions " + weekday_counts(sessions) +
              "); shift the history " + (is_late ? "back" : "forward") +
              " a day; P(shift) = " + format_probability(p_shift);
  v.detail = {{"shift_days", std::int64_t{is_late ? -1 : 1}},
              {"p_late", p_late},
              {"p_early", p_early},
              {"bars", static_cast<std::int64_t>(dates.size())}};
  out.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// DQ301 missing-run

const CheckInfo& MissingRunCheck::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ301",
      .name = "missing-run",
      .summary = "sessions with no bar that look like a feed outage, given how the series trades",
      .default_severity = Severity::kWarn,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void MissingRunCheck::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.analysis == nullptr || !context.analysis->applicable) {
    return;
  }
  for (const MissingRun& run : context.analysis->runs) {
    std::string nearby =
        "nearby the series has a bar on " + percent(run.observed_density) + " of sessions";
    if (run.density_from_volume) {
      nearby += " and trades a median " + with_commas(std::llround(run.median_volume)) + " a day";
    }
    const auto add = [&](Date first, Date last, int sessions, double p) {
      const auto severity = context.thresholds.for_probability(p);
      if (!severity) {
        return;
      }
      Violation v = make(info(), *severity, first);
      if (last != first) {
        v.end_date = last;
      }
      v.p_error = p;
      v.message =
          (sessions == 1 ? std::string{"1 session"} : std::to_string(sessions) + " sessions") +
          " with no bar; " + nearby + "; P(feed outage) = " + format_probability(p);
      v.detail = {{"sessions", std::int64_t{sessions}},
                  {"run_first", run.first.to_string()},
                  {"run_last", run.last.to_string()},
                  {"nearby_density", run.observed_density},
                  {"healthy_density", run.density},
                  {"density_prior", std::string{run.density_from_volume ? "volume" : "default"}},
                  {"p_outage", p}};
      if (run.density_from_volume) {
        v.detail.push_back({"median_volume", run.median_volume});
      }
      out.push_back(std::move(v));
    };
    if (context.gap_report == GapReport::kRun) {
      add(run.first, run.last, run.sessions, run.p_outage);
    } else {
      for (std::size_t i = 0; i < run.session_dates.size(); ++i) {
        add(run.session_dates[i], run.session_dates[i], 1, run.session_p[i]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// DQ302 sparse-series

const CheckInfo& SparseSeries::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ302",
      .name = "sparse-series",
      .summary = "the series has a bar on few of its sessions (info: liquidity, not a defect)",
      .default_severity = Severity::kInfo,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void SparseSeries::run(const SeriesContext& context, std::vector<Violation>& out) const {
  constexpr int kMinSessions = 60;
  const CoverageAnalysis* a = context.analysis;
  if (a == nullptr || !a->applicable || a->sessions < kMinSessions) {
    return;
  }
  const double alpha = a->present + 1.0;
  const double beta = (a->sessions - a->present) + 1.0;
  const double mean = alpha / (alpha + beta);
  if (mean >= context.coverage.sparse_density) {
    return;
  }
  const double low = stats::beta_quantile(alpha, beta, 0.05);
  const double high = stats::beta_quantile(alpha, beta, 0.95);
  Violation v = make(info(), Severity::kInfo, a->first);
  v.end_date = a->last;
  v.p_error = 0.0;
  v.classification = Classification::kMarketFact;
  v.message = "a bar on " + percent(static_cast<double>(a->present) / a->sessions) + " of " +
              with_commas(a->sessions) + " sessions (90% interval " + percent(low) + "-" +
              percent(high) +
              "); a day without a bar is a fact about liquidity here, and DQ301 judges gaps "
              "against it";
  v.detail = {{"bars", std::int64_t{a->present}},
              {"sessions", std::int64_t{a->sessions}},
              {"density", mean},
              {"density_low", low},
              {"density_high", high}};
  out.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// DQ303 cohort-gap, DQ304 stale-feed: identities only; see the engine.

const CheckInfo& CohortGap::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ303",
      .name = "cohort-gap",
      .summary = "many series that trade every session miss the same one: a failed load",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
      .cross_sectional = true,
  };
  return kInfo;
}

const CheckInfo& StaleFeed::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ304",
      .name = "stale-feed",
      .summary = "the series stops before the as-of date by more than its density explains",
      .default_severity = Severity::kWarn,
      .applies = Applies::kAny,
      .cross_sectional = true,
  };
  return kInfo;
}

// ---------------------------------------------------------------------------
// DQ305 frequency-gap

const CheckInfo& FrequencyGap::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ305",
      .name = "frequency-gap",
      .summary = "missing periods in a weekly, monthly, quarterly or annual series",
      .default_severity = Severity::kWarn,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void FrequencyGap::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Frequency f = context.frequency;
  if (f != Frequency::kWeekly && f != Frequency::kMonthly && f != Frequency::kQuarterly &&
      f != Frequency::kAnnual) {
    return;
  }
  const auto period_index = [f](Date d) -> std::int64_t {
    switch (f) {
      case Frequency::kWeekly:
        return (d.days() + 3) / 7;  // weeks, Monday-based
      case Frequency::kMonthly:
        return std::int64_t{d.year()} * 12 + d.month() - 1;
      case Frequency::kQuarterly:
        return std::int64_t{d.year()} * 4 + (d.month() - 1) / 3;
      default:
        return d.year();
    }
  };
  const auto period_start = [f](std::int64_t index) -> Date {
    switch (f) {
      case Frequency::kWeekly:
        return Date::from_days(static_cast<std::int32_t>(index * 7 - 3));
      case Frequency::kMonthly:
        return Date::from_ymd(static_cast<int>(index / 12), static_cast<int>(index % 12) + 1, 1);
      case Frequency::kQuarterly:
        return Date::from_ymd(static_cast<int>(index / 4), static_cast<int>(index % 4) * 3 + 1, 1);
      default:
        return Date::from_ymd(static_cast<int>(index), 1, 1);
    }
  };
  const std::vector<Date> dates = unique_dates(context.series);
  const std::string name{to_string(f)};
  for (std::size_t i = 1; i < dates.size(); ++i) {
    const std::int64_t missing = period_index(dates[i]) - period_index(dates[i - 1]) - 1;
    if (missing <= 0) {
      continue;
    }
    const std::int64_t first = period_index(dates[i - 1]) + 1;
    Violation v = make(info(), Severity::kWarn, period_start(first));
    if (missing > 1) {
      v.end_date = period_start(first + missing - 1);
    }
    v.message = std::to_string(missing) + " " + name + " observation" + (missing == 1 ? "" : "s") +
                " missing between " + dates[i - 1].to_string() + " and " + dates[i].to_string();
    v.detail = {{"missing", missing},
                {"frequency", name},
                {"previous", dates[i - 1].to_string()},
                {"next", dates[i].to_string()}};
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
