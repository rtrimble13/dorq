#include "engine/cross_section.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/check.hpp"
#include "checks/coverage.hpp"
#include "checks/coverage_model.hpp"
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
      healthy_misses_(day_count() + 1, 0.0) {
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
std::vector<Violation> CrossSection::cohort_moves(std::vector<SeriesResult>& results) {
  constexpr double kSiblingBf = 100.0;
  constexpr std::size_t kNamesShown = 6;
  std::vector<Violation> out;
  std::map<std::pair<Date, std::string>, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < moves_.size(); ++i) {
    groups[{moves_[i].second.date, moves_[i].second.ratio}].push_back(i);
  }
  const auto split = static_cast<std::size_t>(PriceHypothesis::kUnreportedSplit);
  for (const auto& [key, members] : groups) {
    for (const std::size_t i : members) {
      auto& [index, move] = moves_[i];
      const auto siblings = std::count_if(members.begin(), members.end(), [&](std::size_t j) {
        return j != i && !move.peer_group.empty() && moves_[j].second.peer_group == move.peer_group &&
               moves_[j].first != index;
      });
      if (siblings == 0 || !move.considered.at(split) || index >= results.size()) {
        continue;
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
        continue;
      }
      // The series' own report of this bar gives way to the DQ203.
      std::erase_if(results[index].violations, [&move](const Violation& v) {
        return v.date == move.date && v.check->code.starts_with("DQ20") && v.check->code != "DQ206";
      });
      Violation v = move.split;
      v.p_error = std::min(p_error, 1.0);
      v.severity = move.provisional ? std::min(*severity, Severity::kWarn) : *severity;
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
      v.evidence.push_back({"peer_group", std::int64_t{siblings}, std::log(kSiblingBf),
                            std::to_string(siblings) + " in " + move.peer_group +
                                " move by the same ratio that day"});
      results[index].violations.push_back(std::move(v));
    }
    if (members.size() < static_cast<std::size_t>(cohort_.min_series)) {
      continue;
    }
    const PeerMove& first = moves_[members.front()].second;
    const bool family =
        !first.peer_group.empty() &&
        std::all_of(members.begin(), members.end(),
                    [&](std::size_t j) { return moves_[j].second.peer_group == first.peer_group; });
    std::string names;
    for (std::size_t m = 0; m < members.size() && m < kNamesShown; ++m) {
      names += (m == 0 ? "" : ", ") + moves_[members[m]].second.series;
    }
    if (members.size() > kNamesShown) {
      names += " and " + std::to_string(members.size() - kNamesShown) + " more";
    }
    std::vector<double> factors;
    for (const std::size_t j : members) {
      factors.push_back(moves_[j].second.factor);
    }
    const double factor = stats::median(std::move(factors));
    const CheckInfo& info = find_check("DQ601")->info();
    Violation v;
    v.check = &info;
    v.severity = Severity::kWarn;
    v.classification = family ? Classification::kContextGap : Classification::kDataError;
    v.date = key.first;
    v.message = std::to_string(members.size()) + " series move ×" +
                format_number(std::round(factor * 1e4) / 1e4) + " (≈ a " + key.second +
                " split) on " + key.first.to_string() + ": " + names +
                (family ? "; all of peer group " + first.peer_group +
                              ": a family splitting together, with no split on file"
                        : ": a vendor's mass adjustment, or splits the actions file is missing");
    v.detail = {{"series_moving", static_cast<std::int64_t>(members.size())},
                {"ratio", key.second},
                {"factor", factor}};
    if (family) {
      v.detail.push_back({"peer_group", first.peer_group});
    }
    out.push_back(std::move(v));
  }
  return out;
}

std::vector<Violation> CrossSection::finalize(std::vector<SeriesResult>& results) {
  std::vector<Violation> cohort_violations = cohort_moves(results);
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
