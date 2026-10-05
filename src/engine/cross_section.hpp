#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/violation.hpp"

namespace dorq {

struct SeriesResult;

// What one series contributes to the cross-sectional checks.
struct CrossSummary {
  bool daily = false;   // only daily series take part
  bool cohort = false;  // DQ303 is enabled for this series
  bool stale = false;   // DQ304 is enabled for this series
  Date last;            // the last bar on a session
  double tail_density = 0.0;
  CoverageSettings coverage;
  // Sessions on which the series was expected: (first, last, nearby density).
  std::vector<std::tuple<Date, Date, double>> confident_ranges;
  std::vector<Date> confident_missing;
};

// DQ303 cohort-gap and DQ304 stale-feed. Fed every series' summary in input
// order; judged once all series are in, when the cohort dates and the as-of date
// are known. See doc/checks/DQ303.md and DQ304.md.
class CrossSection {
 public:
  CrossSection(const Calendar& calendar, const CohortSettings& cohort,
               const SeverityThresholds& thresholds, const CoverageSettings& coverage,
               std::optional<Date> as_of);

  void add(const CrossSummary& summary);

  // Adds DQ304 violations to `results` (one per series, in the same order as the
  // summaries were added), downgrades the DQ301/DQ304 violations a cohort
  // explains, and returns the DQ303 violations.
  [[nodiscard]] std::vector<Violation> finalize(std::vector<SeriesResult>& results);

  // The as-of date used: the one given, or the latest session with a bar.
  [[nodiscard]] std::optional<Date> as_of() const noexcept { return as_of_; }

 private:
  [[nodiscard]] static std::size_t slot(Date date) noexcept;

  const Calendar& calendar_;
  CohortSettings cohort_;
  SeverityThresholds thresholds_;
  CoverageSettings coverage_;
  std::optional<Date> as_of_;
  bool as_of_given_ = false;

  std::vector<CrossSummary> tails_;  // per series, in order (ranges dropped)
  // Per day, as difference arrays: series expected, series missing, and the
  // misses a healthy feed would explain.
  std::vector<std::int32_t> expected_;
  std::vector<std::int32_t> missing_;
  std::vector<double> healthy_misses_;
};

}  // namespace dorq
