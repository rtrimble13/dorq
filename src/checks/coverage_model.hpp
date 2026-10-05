#pragma once

#include <tuple>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"

namespace dorq {

// A maximal run of sessions with no bar, inside a series' window.
struct MissingRun {
  Date first;
  Date last;
  int sessions = 0;
  // The density the series shows on the sessions around the run, leaving the run
  // itself (and likely outages nearby) out: what "normal" looks like here.
  double density = 0.0;
  double observed_density = 0.0;     // bars / sessions nearby, as seen
  bool density_from_volume = false;  // volume, not the counts, set the density
  double median_volume = 0.0;        // nearby; 0 when there is none
  double p_outage = 0.0;             // the largest per-session posterior in the run
  std::vector<double> session_p;     // per-session posterior P(outage)
  std::vector<Date> session_dates;
};

// What the coverage model concludes about one daily series. See
// doc/checks/DQ301.md for the model.
struct CoverageAnalysis {
  bool applicable = false;  // a daily series with at least one bar on a session
  Date first;               // first and last bar that fall on a session
  Date last;
  int sessions = 0;  // sessions in [first, last]
  int present = 0;   // of which have a bar
  std::vector<MissingRun> runs;

  // For the cross-sectional checks: the sessions on which this series was expected
  // (its nearby density above CohortSettings::confident_density), as date ranges,
  // and which of those it missed.
  std::vector<std::tuple<Date, Date, double>> confident_ranges;  // first, last, density
  std::vector<Date> confident_missing;
  // The density at the end of the window, for DQ304.
  double tail_density = 0.0;
};

[[nodiscard]] CoverageAnalysis analyze_coverage(const Series& series, const Calendar& calendar,
                                                const CoverageSettings& settings,
                                                double confident_density);

// P(outage) for a run of `k` missing sessions at the end of a series, where the
// density nearby is `density` and nothing has been seen since (DQ304).
[[nodiscard]] double tail_outage_probability(int k, double density,
                                             const CoverageSettings& settings);

}  // namespace dorq
