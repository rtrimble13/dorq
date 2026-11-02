#include "engine/cross_section.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/check.hpp"
#include "checks/coverage.hpp"
#include "checks/coverage_model.hpp"
#include "checks/price.hpp"
#include "core/text.hpp"
#include "dorq/number.hpp"
#include "engine/engine.hpp"
#include "stats/robust.hpp"
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
      healthy_misses_(day_count() + 1, 0.0),
      moved_(day_count() + 1, 0) {
  if (as_of) {
    as_of_ = calendar.session_on_or_before(*as_of);
  }
}

std::size_t CrossSection::slot(Date date) noexcept {
  return static_cast<std::size_t>(std::clamp(date, kFirstDay, kLastDay).days() - kFirstDay.days());
}

void CrossSection::add(CrossSummary summary) {
  for (PeerMove& move : summary.moves) {
    moves_.emplace_back(tails_.size(), std::move(move));
  }
  summary.moves.clear();
  for (const Date date : summary.moved) {
    moved_[slot(date)] += 1;
  }
  for (BandMove& move : summary.band_moves) {
    const auto day = static_cast<std::int32_t>(slot(move.date));
    bands_[{std::move(move.ratio), day}].at(move.in_band ? 0 : 1) += 1;
  }
  summary.moved.clear();
  summary.band_moves.clear();
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

// DQ601: moves by the same clean ratio on the same date. Within a peer group a
// sibling moving with a series is strong evidence that both split: families of
// funds split together, and an unrelated move matching a sibling's ratio to the
// day is rare. kSiblingBf is that evidence, as a Bayes factor for
// unreported_split; it can turn a DQ209 into a DQ203.
constexpr double kSiblingBf = 100.0;

void CrossSection::sibling_evidence(std::size_t i, const std::vector<std::size_t>& members,
                                    std::vector<SeriesResult>& results) const {
  const auto& [index, move] = moves_[i];
  const auto split = static_cast<std::size_t>(PriceHypothesis::kUnreportedSplit);
  const auto siblings = std::count_if(members.begin(), members.end(), [&](std::size_t j) {
    return j != i && !move.peer_group.empty() && moves_[j].second.peer_group == move.peer_group &&
           moves_[j].first != index;
  });
  if (siblings == 0 || !move.considered.at(split) || index >= results.size()) {
    return;
  }
  std::array<double, kPriceHypotheses> posterior{};
  double total = 0.0;
  for (std::size_t h = 0; h < kPriceHypotheses; ++h) {
    posterior.at(h) = move.posterior.at(h) * (h == split ? kSiblingBf : 1.0);
    total += posterior.at(h);
  }
  double p_error = 0.0;
  std::size_t best = split;
  for (std::size_t h = 0; h < kPriceHypotheses; ++h) {
    posterior.at(h) /= total;
    if (is_error(static_cast<PriceHypothesis>(h))) {
      p_error += posterior.at(h);
      best = posterior.at(h) > posterior.at(best) ? h : best;
    }
  }
  const auto severity = thresholds_.for_probability(p_error);
  if (best != split || !severity) {
    return;
  }
  // The series' own report of this bar gives way to the DQ203.
  std::erase_if(results[index].violations, [&move](const Violation& v) {
    return v.date == move.date && v.check->code.starts_with("DQ20") && v.check->code != "DQ206";
  });
  Violation v = move.split;
  v.p_error = std::min(p_error, 1.0);
  v.severity = move.provisional ? std::min(severity.value(), Severity::kWarn) : severity.value();
  v.message += "; P(error) = " + format_probability(v.p_error);
  if (move.provisional) {
    v.message += ", provisional";
  }
  v.hypotheses.clear();
  for (std::size_t h = 0; h < kPriceHypotheses; ++h) {
    if (move.considered.at(h)) {
      v.hypotheses.push_back({to_string(static_cast<PriceHypothesis>(h)), posterior.at(h)});
    }
  }
  v.evidence.push_back(
      {"peer_group", std::int64_t{siblings}, std::log(kSiblingBf),
       std::to_string(siblings) + " in " + move.peer_group + " move by the same ratio that day"});
  results[index].violations.push_back(std::move(v));
}

double CrossSection::base_rate(Date date, const std::string& ratio) const {
  // The input's own dates either side, up to kNearby each way within kSpan days:
  // the band's mean count on them is what chance puts there on a day like these.
  constexpr int kNearby = 10;
  constexpr int kSpan = 30;
  const auto count = [&](std::size_t day, std::size_t which) {
    const auto it = bands_.find({ratio, static_cast<std::int32_t>(day)});
    return it == bands_.end() ? 0 : it->second.at(which);
  };
  const std::size_t at = slot(date);
  double total = 0.0;
  int days = 0;
  for (const int step : {-1, 1}) {
    int found = 0;
    for (int off = 1; off <= kSpan && found < kNearby; ++off) {
      const std::int64_t day = static_cast<std::int64_t>(at) + std::int64_t{step} * off;
      if (day < 0 || day >= static_cast<std::int64_t>(moved_.size()) ||
          moved_[static_cast<std::size_t>(day)] == 0) {
        continue;
      }
      total += count(static_cast<std::size_t>(day), 0);
      ++found;
    }
    days += found;
  }
  const double nearby = days > 0 ? total / days : 0.0;
  // Beside the band on the day itself: two widths of it each side, so half the
  // count is the rate one band's width gets. A day the whole market moves fills
  // these too.
  const double beside = 0.5 * count(at, 1);
  constexpr double kFloor = 1e-3;  // no day's rate is zero
  return std::max({nearby, beside, kFloor});
}

Violation CrossSection::cohort_violation(Date date, const std::string& ratio,
                                         const std::vector<std::size_t>& members) const {
  constexpr std::size_t kNamesShown = 6;
  const PeerMove& first = moves_[members.front()].second;
  const bool family =
      !first.peer_group.empty() && std::all_of(members.begin(), members.end(), [&](std::size_t j) {
        return moves_[j].second.peer_group == first.peer_group;
      });
  std::string names;
  std::string ids;  // every member, for a reader matching the row to its series
  std::vector<double> factors;
  for (std::size_t m = 0; m < members.size(); ++m) {
    factors.push_back(moves_[members[m]].second.factor);
    if (m < kNamesShown) {
      names += (m == 0 ? "" : ", ") + moves_[members[m]].second.series;
    }
    ids += (m == 0 ? "" : ",") + moves_[members[m]].second.id;
  }
  if (members.size() > kNamesShown) {
    names += " and " + std::to_string(members.size() - kNamesShown) + " more";
  }
  const double factor = stats::median(std::move(factors));
  Violation v;
  v.check = &find_check("DQ601")->info();
  v.severity = Severity::kWarn;
  v.classification = family ? Classification::kContextGap : Classification::kDataError;
  v.date = date;
  v.message = std::to_string(members.size()) + " series move ×" +
              format_number(std::round(factor * 1e4) / 1e4) + " (≈ a " + ratio + " split) on " +
              date.to_string() + ": " + names +
              (family ? "; all of peer group " + first.peer_group +
                            ": a family splitting together, with no split on file"
                      : ": a vendor's mass adjustment, or splits the actions file is missing");
  v.detail = {{"series_moving", static_cast<std::int64_t>(members.size())},
              {"ratio", ratio},
              {"factor", factor},
              {"members", ids}};
  // Two cohorts on one date (a 2:1 and a 3:2) are two conditions.
  v.key = {{"ratio", ratio}};
  if (family) {
    v.detail.push_back({"peer_group", first.peer_group});
  }
  return v;
}

std::vector<Violation> CrossSection::cohort_moves(std::vector<SeriesResult>& results) {
  std::vector<Violation> out;
  std::map<std::pair<Date, std::string>, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < moves_.size(); ++i) {
    groups[{moves_[i].second.date, moves_[i].second.ratio}].push_back(i);
  }
  const auto min_series = static_cast<std::size_t>(cohort_.min_series);
  for (const auto& [key, members] : groups) {
    for (const std::size_t i : members) {
      sibling_evidence(i, members, results);
    }
    if (members.size() < min_series) {
      continue;
    }
    const std::string& group = moves_[members.front()].second.peer_group;
    const bool family = !group.empty() && std::all_of(members.begin(), members.end(), [&](auto j) {
      return moves_[j].second.peer_group == group;
    });
    if (family) {
      // A family splitting together, with nothing on file: as before.
      out.push_back(cohort_violation(key.first, key.second, members));
      continue;
    }
    // Unrelated series: each member's move within its tolerance of the ratio, and
    // not a real move by its own evidence; then more of them than chance puts in
    // the ratio's band that day.
    const double ratio = std::log(parse_split_ratio(key.second)->price_factor());
    std::vector<std::size_t> kept;
    std::string dropped;
    for (const std::size_t j : members) {
      const PeerMove& move = moves_[j].second;
      if (std::fabs(std::log(move.factor) - ratio) > move.tolerance) {
        continue;
      }
      if (move.market_like) {
        dropped += (dropped.empty() ? "" : ",") + move.id;
        continue;
      }
      kept.push_back(j);
    }
    if (kept.size() < min_series) {
      continue;
    }
    const double rate = base_rate(key.first, key.second);
    const double p = stats::poisson_upper_tail(static_cast<std::int64_t>(kept.size()), rate);
    if (p > cohort_.move_max_tail) {
      continue;
    }
    Violation v = cohort_violation(key.first, key.second, kept);
    v.message += "; chance puts about " + format_number(std::round(rate * 100.0) / 100.0) +
                 " in that band on a day like it (p = " + significant(p, 2) + ")";
    v.detail.push_back({"base_rate", rate});
    v.detail.push_back({"p_value", p});
    v.detail.push_back({"members_dropped_as_market", dropped});
    out.push_back(std::move(v));
  }
  return out;
}

namespace {

constexpr double kRevisionMaxTail = 1e-4;  // DQ951, as DQ601's
constexpr std::size_t kMembersShown = 6;

// A code that many series carry in rows known the same day.
struct FindingCohort {
  std::string code;
  std::int64_t rows = 0;
  double expected = 0.0;
  double p = 1.0;
  std::vector<std::size_t> members;
};

// One day's DQ951: the series revised beyond the base rate, and the findings.
struct DayCohort {
  std::vector<std::size_t> revised;
  double revision_rate = 0.0;
  double revision_p = 1.0;
  std::vector<FindingCohort> findings;
};

std::string member_names(const std::vector<SeriesResult>& results,
                         const std::vector<std::size_t>& members) {
  std::string names;
  for (std::size_t m = 0; m < members.size() && m < kMembersShown; ++m) {
    names += (m == 0 ? "" : ", ") + results[members[m]].id;
  }
  if (members.size() > kMembersShown) {
    names += " and " + std::to_string(members.size() - kMembersShown) + " more";
  }
  return names;
}

Violation cohort_report(Date day, const DayCohort& cohort,
                        const std::vector<SeriesResult>& results) {
  Violation v;
  v.check = &find_check("DQ951")->info();
  v.date = day;
  std::vector<std::size_t> members = cohort.revised;
  for (const FindingCohort& f : cohort.findings) {
    members.insert(members.end(), f.members.begin(), f.members.end());
  }
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  std::string ids;
  for (const std::size_t i : members) {
    ids += (ids.empty() ? "" : ",") + results[i].id;
  }
  std::string revised;
  if (!cohort.revised.empty()) {
    revised = std::to_string(cohort.revised.size()) + " series revised on " + day.to_string() +
              ", against about " + format_number(std::round(cohort.revision_rate * 10.0) / 10.0) +
              " on a day with revisions (p = " + significant(cohort.revision_p, 2) + ")";
    v.detail.push_back({"series_revised", static_cast<std::int64_t>(cohort.revised.size())});
    v.detail.push_back({"base_rate", cohort.revision_rate});
    v.detail.push_back({"p_value", cohort.revision_p});
  }
  if (cohort.findings.empty()) {
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    v.p_error = 0.0;
    v.message = revised + ": a vendor batch or a backfill, not " +
                std::to_string(cohort.revised.size()) +
                " separate events: " + member_names(results, cohort.revised);
  } else {
    v.severity = Severity::kWarn;
    v.classification = Classification::kDataError;
    std::string codes;
    std::string found;
    double p = 1.0;
    for (const FindingCohort& f : cohort.findings) {
      codes += (codes.empty() ? "" : ",") + f.code;
      found += (found.empty() ? "" : "; ") + std::to_string(f.members.size()) + " carry " + f.code +
               " (" + std::to_string(f.rows) + " rows, against about " +
               significant(f.expected, 2) + ", p = " + significant(f.p, 2) + ")";
      p = std::min(p, f.p);
    }
    v.p_error = 1.0 - p;
    v.message = std::to_string(members.size()) + " series in rows known on " + day.to_string() +
                ": " + found + (revised.empty() ? "" : "; " + revised) +
                ": a load fault, not separate findings: " + member_names(results, members);
    v.detail.push_back({"codes", codes});
  }
  v.detail.push_back({"members", ids});
  return v;
}

// A member's reports a cohort explains: on rows known that day, of its codes (a
// revision cohort's: DQ932 and DQ903). Each is linked to the cohort; a finding
// cohort's DQ932 and DQ903 reports, one condition reported once, go to info, as a
// DQ303 cohort's members do.
void link_members(Date day, const DayCohort& cohort, std::vector<SeriesResult>& results) {
  const auto link = [&](std::size_t i, const std::string& code, bool downgrade) {
    for (Violation& v : results[i].violations) {
      if (v.check->code != code || v.known != day) {
        continue;
      }
      v.detail.push_back({"cohort_date", day.to_string()});
      if (downgrade && (code == "DQ932" || code == "DQ903")) {
        v.severity = Severity::kInfo;
        v.message += "; part of the DQ951 cohort on " + day.to_string();
      }
    }
  };
  for (const std::size_t i : cohort.revised) {
    link(i, "DQ932", false);
    link(i, "DQ903", false);
  }
  for (const FindingCohort& f : cohort.findings) {
    for (const std::size_t i : f.members) {
      link(i, f.code, true);
    }
  }
}

}  // namespace

// DQ951: revisions by the day their versions became known, tested against the
// count on the input's other days with revisions; and findings by the day their
// rows became known, against the rate per row on the other days, so that a first
// load holding all of history is no cohort of its findings.
std::vector<Violation> CrossSection::revision_cohorts(std::vector<SeriesResult>& results) const {
  std::map<Date, std::vector<std::size_t>> revised;
  std::map<Date, std::int64_t> rows;
  std::map<std::pair<std::string, Date>, FindingCohort> found;
  std::map<std::string, std::int64_t> found_total;
  std::int64_t rows_total = 0;
  std::size_t revisions = 0;
  for (std::size_t i = 0; i < tails_.size() && i < results.size(); ++i) {
    const CrossSummary& t = tails_[i];
    if (!t.revision_cohort) {
      continue;
    }
    for (const Date day : t.revised) {
      revised[day].push_back(i);
      ++revisions;
    }
    for (const auto& [day, n] : t.known_rows) {
      rows[day] += n;
      rows_total += n;
    }
    for (const auto& [day, code, n] : t.found) {
      FindingCohort& f = found[{code, day}];
      f.code = code;
      f.rows += n;
      f.members.push_back(i);
      found_total[code] += n;
    }
  }
  const auto min_series = static_cast<std::size_t>(cohort_.min_series);
  std::map<Date, DayCohort> days;
  for (const auto& [day, members] : revised) {
    if (members.size() < min_series || revised.size() < 2) {
      continue;
    }
    const double rate = std::max(1e-3, static_cast<double>(revisions - members.size()) /
                                           static_cast<double>(revised.size() - 1));
    const double p = stats::poisson_upper_tail(static_cast<std::int64_t>(members.size()), rate);
    if (p <= kRevisionMaxTail) {
      DayCohort& d = days[day];
      d.revised = members;
      d.revision_rate = rate;
      d.revision_p = p;
    }
  }
  for (auto& [key, f] : found) {
    const Date day = key.second;
    const std::int64_t others = rows_total - rows[day];
    if (f.members.size() < min_series || others <= 0) {
      continue;
    }
    // The code's rate per row on the other days, half a finding added so that a
    // clean rest of the input is no certainty.
    const double rate =
        (static_cast<double>(found_total[f.code] - f.rows) + 0.5) / static_cast<double>(others);
    f.expected = rate * static_cast<double>(rows[day]);
    f.p = stats::poisson_upper_tail(f.rows, f.expected);
    if (f.p <= kRevisionMaxTail) {
      days[day].findings.push_back(std::move(f));
    }
  }
  std::vector<Violation> out;
  for (const auto& [day, cohort] : days) {
    out.push_back(cohort_report(day, cohort, results));
    link_members(day, cohort, results);
  }
  return out;
}

std::vector<Violation> CrossSection::finalize(std::vector<SeriesResult>& results) {
  std::vector<Violation> cohort_violations = cohort_moves(results);
  std::vector<Violation> revisions = revision_cohorts(results);
  cohort_violations.insert(cohort_violations.end(), std::make_move_iterator(revisions.begin()),
                           std::make_move_iterator(revisions.end()));
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
    // DQ301 runs a cohort explains: every session missing is a cohort date. A long
    // run that merely contains one is still its own outage.
    if (!cohort_dates.empty()) {
      for (Violation& v : result.violations) {
        if (v.check->code != "DQ301" || !v.date) {
          continue;
        }
        const auto covered = std::count_if(cohort_dates.begin(), cohort_dates.end(),
                                           [&v](Date date) { return covers(v, date); });
        const int sessions = calendar_.sessions_between(*v.date, v.end_date.value_or(*v.date));
        if (covered > 0 && covered >= sessions) {
          const auto it = std::find_if(cohort_dates.begin(), cohort_dates.end(),
                                       [&v](Date date) { return covers(v, date); });
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
    // The date is the first missing session, whatever the as-of date: the key a
    // caller deduplicates by stays the same while the feed stays stopped.
    v.detail = {{"last_date", t.last.to_string()},
                {"as_of", as_of_->to_string()},
                {"sessions_missing", std::int64_t{k}},
                {"recent_density", t.tail_density},
                {"p_outage", p},
                {"first_missing", first_missing.to_string()},
                {"active_through", as_of_->to_string()}};
    if (in_cohort(first_missing)) {
      cohort_note(v, first_missing);
    }
    result.violations.push_back(std::move(v));
  }
  return cohort_violations;
}

}  // namespace dorq
