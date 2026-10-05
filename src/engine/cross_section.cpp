#include "engine/cross_section.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "checks/check.hpp"
#include "checks/coverage.hpp"
#include "checks/coverage_model.hpp"
#include "core/text.hpp"
#include "dorq/number.hpp"
#include "engine/engine.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

const Date kFirstDay = Date::from_ymd(1800, 1, 1);
const Date kLastDay = Date::from_ymd(2200, 12, 31);
// Days from 1800-01-01 to 2200-12-31, the range dorq's dates live in.
std::size_t day_count() noexcept {
  return static_cast<std::size_t>(std::int64_t{kLastDay.days()} - kFirstDay.days() + 1);
}

bool covers(const Violation& v, Date date) {
  if (!v.date) {
    return false;
  }
  return *v.date <= date && date <= v.end_date.value_or(*v.date);
}

}  // namespace

CrossSection::CrossSection(const Calendar& calendar, const CohortSettings& cohort,
                           const SeverityThresholds& thresholds, const CoverageSettings& coverage,
                           std::optional<Date> as_of)
    : calendar_(calendar),
      cohort_(cohort),
      thresholds_(thresholds),
      coverage_(coverage),
      as_of_given_(as_of.has_value()),
      expected_(day_count() + 1, 0),
      missing_(day_count() + 1, 0),
      healthy_misses_(day_count() + 1, 0.0) {
  if (as_of) {
    as_of_ = calendar.session_on_or_before(*as_of);
  }
}

std::size_t CrossSection::slot(Date date) noexcept {
  return static_cast<std::size_t>(std::clamp(date, kFirstDay, kLastDay).days() - kFirstDay.days());
}

void CrossSection::add(const CrossSummary& summary) {
  CrossSummary tail = summary;
  tail.confident_ranges.clear();
  tail.confident_missing.clear();
  tails_.push_back(std::move(tail));
  if (!summary.daily) {
    return;
  }
  if (!as_of_given_ && (!as_of_ || summary.last > *as_of_)) {
    as_of_ = summary.last;
  }
  if (!summary.cohort) {
    return;
  }
  for (const auto& [first, last, density] : summary.confident_ranges) {
    expected_[slot(first)] += 1;
    expected_[slot(last) + 1] -= 1;
    healthy_misses_[slot(first)] += 1.0 - density;
    healthy_misses_[slot(last) + 1] -= 1.0 - density;
  }
  for (const Date date : summary.confident_missing) {
    missing_[slot(date)] += 1;
  }
}

std::vector<Violation> CrossSection::finalize(std::vector<SeriesResult>& results) {
  std::vector<Violation> cohort_violations;
  // A series that stops before the as-of date was expected on the session after
  // its last bar. Counting that session lets a load that failed for many series on
  // the latest night show up as a cohort, like any other.
  std::vector<std::optional<Date>> stopped(tails_.size());
  for (std::size_t i = 0; i < tails_.size(); ++i) {
    const CrossSummary& t = tails_[i];
    if (!t.daily || !as_of_) {
      continue;
    }
    const auto next = calendar_.next_session(t.last);
    if (!next || *next > *as_of_ ||
        calendar_.sessions_between(*next, *as_of_) <= t.coverage.publication_lag) {
      continue;
    }
    stopped[i] = next;
    if (t.cohort && t.tail_density > cohort_.confident_density) {
      expected_[slot(*next)] += 1;
      expected_[slot(*next) + 1] -= 1;
      healthy_misses_[slot(*next)] += 1.0 - t.tail_density;
      healthy_misses_[slot(*next) + 1] -= 1.0 - t.tail_density;
      missing_[slot(*next)] += 1;
    }
  }

  // Cohort dates: more missing series than healthy feeds and independent outages
  // explain.
  const double stationary_outage =
      coverage_.outage_start / (coverage_.outage_start + coverage_.outage_end);
  std::vector<Date> cohort_dates;
  std::int32_t expected = 0;
  double healthy = 0.0;
  for (std::size_t d = 0; d < day_count(); ++d) {
    expected += expected_[d];
    healthy += healthy_misses_[d];
    const std::int32_t k = missing_[d];
    if (k < cohort_.min_series) {
      continue;
    }
    const double lambda = std::max(0.0, healthy) + expected * stationary_outage;
    const double tail = stats::poisson_upper_tail(k, lambda);
    if (tail > cohort_.max_tail) {
      continue;
    }
    const Date date = Date::from_days(kFirstDay.days() + static_cast<std::int32_t>(d));
    cohort_dates.push_back(date);
    const CheckInfo& info = find_check("DQ303")->info();
    Violation v;
    v.check = &info;
    v.severity = Severity::kError;
    v.date = date;
    v.p_error = 1.0 - tail;
    const double share = expected > 0 ? static_cast<double>(k) / expected : 1.0;
    v.message = with_commas(k) + " of " + with_commas(expected) +
                " series that have a bar on nearly every session have none on this one, where "
                "healthy feeds would explain about " +
                format_number(std::round(lambda * 10.0) / 10.0) + "; ";
    v.message += share >= 0.9
                     ? "nearly every series is missing: the calendar may be wrong for this "
                       "date (an unlisted closure?), or the whole load failed"
                     : "a failed load, not " + with_commas(k) + " separate gaps";
    v.detail = {{"series_missing", std::int64_t{k}},
                {"series_expected", std::int64_t{expected}},
                {"expected_missing", lambda},
                {"tail_probability", tail},
                {"calendar", calendar_.description()}};
    cohort_violations.push_back(std::move(v));
  }
  const auto in_cohort = [&cohort_dates](Date date) {
    return std::binary_search(cohort_dates.begin(), cohort_dates.end(), date);
  };
  const auto cohort_note = [](Violation& v, Date date) {
    v.severity = Severity::kInfo;
    v.message += "; part of the DQ303 cohort on " + date.to_string();
    v.detail.push_back({"cohort_date", date.to_string()});
  };

  for (std::size_t i = 0; i < results.size() && i < tails_.size(); ++i) {
    SeriesResult& result = results[i];
    const CrossSummary& t = tails_[i];
    // DQ301 runs a cohort explains.
    if (!cohort_dates.empty()) {
      for (Violation& v : result.violations) {
        if (v.check->code != "DQ301") {
          continue;
        }
        const auto it = std::find_if(cohort_dates.begin(), cohort_dates.end(),
                                     [&v](Date date) { return covers(v, date); });
        if (it != cohort_dates.end()) {
          cohort_note(v, *it);
        }
      }
    }
    // DQ304.
    if (!t.stale || !stopped[i]) {
      continue;
    }
    const Date first_missing = *stopped[i];
    const int k = calendar_.sessions_between(first_missing, *as_of_) - t.coverage.publication_lag;
    const double p = tail_outage_probability(k, t.tail_density, t.coverage);
    const auto severity = thresholds_.for_probability(p);
    if (!severity) {
      continue;
    }
    const CheckInfo& info = find_check("DQ304")->info();
    Violation v;
    v.check = &info;
    v.severity = *severity;
    v.date = first_missing;
    v.end_date = *as_of_;
    v.p_error = p;
    v.message =
        "no bar since " + t.last.to_string() + ": " + std::to_string(k) + " session" +
        (k == 1 ? "" : "s") + " missing as of " + as_of_->to_string() +
        (t.coverage.publication_lag > 0
             ? " (allowing " + std::to_string(t.coverage.publication_lag) + " for publication lag)"
             : std::string{}) +
        ", for a series with a bar on " + percent(t.tail_density) +
        " of recent sessions; P(feed outage) = " + format_probability(p);
    v.detail = {{"last_date", t.last.to_string()},
                {"as_of", as_of_->to_string()},
                {"sessions_missing", std::int64_t{k}},
                {"recent_density", t.tail_density},
                {"p_outage", p}};
    if (in_cohort(first_missing)) {
      cohort_note(v, first_missing);
    }
    result.violations.push_back(std::move(v));
  }
  return cohort_violations;
}

}  // namespace dorq
