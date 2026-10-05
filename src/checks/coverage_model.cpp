#include "checks/coverage_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "stats/hmm2.hpp"
#include "stats/special.hpp"

namespace dorq {
namespace {

constexpr int kEmPasses = 2;
constexpr std::size_t kMinVolumeBars = 10;
// The floor on P(no bar | healthy feed) for even the most liquid name: an all-day
// halt is rare, not impossible.
constexpr double kMinNoTrade = 1e-6;
constexpr double kMaxNoTrade = 0.999;
// Volume lifts the density only where the counts already show a bar on nearly
// every session. Below that, trades are lumpy (one 2,000-share print is not two
// trades) and the counts are the better guide.
constexpr double kVolumeFloorFrom = 0.9;

double median_of(std::vector<double> values) {
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

double log_or_floor(double x) { return std::log(std::max(x, 1e-300)); }

// What volume says about a block: the chance a healthy feed has no bar on a
// session because nothing traded.
struct VolumeEvidence {
  bool present = false;
  double no_trade = 1.0;
  double median_volume = 0.0;
};

VolumeEvidence volume_evidence(std::vector<double> volumes, const CoverageSettings& settings) {
  VolumeEvidence out;
  if (volumes.size() < kMinVolumeBars) {
    return out;
  }
  // A median of V a day is about V / trade_size trades. If trades arrive at that
  // rate, a session with none has probability exp(-V / trade_size): nil for a name
  // that trades a million shares, and no claim at all for one that trades two.
  out.median_volume = median_of(std::move(volumes));
  out.no_trade =
      std::clamp(std::exp(-out.median_volume / settings.trade_size), kMinNoTrade, kMaxNoTrade);
  out.present = true;
  return out;
}

// The density a healthy feed shows: the data's, leaving out what is judged to be
// outage, unless volume says the name trades more often than that. Pooling counts
// alone can never say a liquid name misses fewer than about 1 session in n, and a
// gap in such a name is far rarer than that; volume can say it.
double healthy_density(double present, double healthy_missing, const VolumeEvidence& volume,
                       const CoverageSettings& settings) {
  const double a = settings.prior_density * settings.prior_strength;
  const double b = (1.0 - settings.prior_density) * settings.prior_strength;
  const double from_data = (present + a) / (present + healthy_missing + a + b);
  if (!volume.present || from_data < kVolumeFloorFrom) {
    return from_data;
  }
  return std::max(from_data, 1.0 - volume.no_trade);
}

stats::Hmm2 outage_model(const CoverageSettings& settings, bool followed_by_bar) {
  const double alpha = settings.outage_start;
  const double beta = settings.outage_end;
  stats::Hmm2 model;
  // The session before a run always has a bar, so the feed was healthy then.
  model.initial = {std::log1p(-alpha), std::log(alpha)};
  model.transition = {{{std::log1p(-alpha), std::log(alpha)}, {std::log(beta), std::log1p(-beta)}}};
  // A bar after the run means the feed was healthy again by then.
  model.final = followed_by_bar ? std::array<double, 2>{std::log1p(-alpha), std::log(beta)}
                                : std::array<double, 2>{0.0, 0.0};
  return model;
}

// Posterior P(outage) per session of a run of `k` missing sessions: a healthy feed
// misses a session with probability 1 - density; an outage misses every session.
std::vector<double> run_posterior(int k, double density, const CoverageSettings& settings,
                                  bool followed_by_bar) {
  const std::array<double, 2> miss = {log_or_floor(1.0 - density), 0.0};
  const std::vector<std::array<double, 2>> emission(static_cast<std::size_t>(k), miss);
  return stats::posterior_state1(outage_model(settings, followed_by_bar), emission);
}

class Prefix {
 public:
  explicit Prefix(std::size_t n) : sum_(n + 1, 0.0) {}
  void build(std::span<const double> values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      sum_[i + 1] = sum_[i] + values[i];
    }
  }
  [[nodiscard]] double range(std::size_t lo, std::size_t hi) const { return sum_[hi] - sum_[lo]; }

 private:
  std::vector<double> sum_;
};

}  // namespace

CoverageAnalysis analyze_coverage(const Series& series, const Calendar& calendar,
                                  const CoverageSettings& settings, double confident_density) {
  CoverageAnalysis out;
  // The window: the first to the last bar that falls on a session.
  std::size_t first_row = series.size();
  std::size_t last_row = series.size();
  for (std::size_t i = 0; i < series.size(); ++i) {
    if (calendar.is_session(series.date[i])) {
      if (first_row == series.size()) {
        first_row = i;
      }
      last_row = i;
    }
  }
  if (first_row == series.size()) {
    return out;
  }
  out.applicable = true;
  out.first = series.date[first_row];
  out.last = series.date[last_row];

  // One entry per session in the window.
  std::vector<Date> dates;
  std::vector<double> present;  // 1 or 0, as doubles for the prefix sums
  std::vector<double> volume;
  std::size_t row = first_row;
  for (std::int32_t d = out.first.days(); d <= out.last.days(); ++d) {
    const Date day = Date::from_days(d);
    if (!calendar.is_session(day)) {
      continue;
    }
    while (row < series.size() && series.date[row] < day) {
      ++row;
    }
    const bool has_bar = row < series.size() && series.date[row] == day;
    dates.push_back(day);
    present.push_back(has_bar ? 1.0 : 0.0);
    volume.push_back(has_bar && series.has_volume ? series.volume[row]
                                                  : std::numeric_limits<double>::quiet_NaN());
  }
  const std::size_t n = dates.size();
  out.sessions = static_cast<int>(n);
  std::vector<double> missing(n);
  for (std::size_t i = 0; i < n; ++i) {
    missing[i] = 1.0 - present[i];
    out.present += present[i] > 0.0 ? 1 : 0;
  }

  // Blocks, and each block's window: itself and its neighbours.
  const auto block = static_cast<std::size_t>(settings.block_sessions);
  const std::size_t blocks = (n + block - 1) / block;
  const auto window = [&](std::size_t b) {
    const std::size_t lo = b == 0 ? 0 : (b - 1) * block;
    const std::size_t hi = std::min(n, (b + 2) * block);
    return std::pair{lo, hi};
  };
  std::vector<VolumeEvidence> evidence(blocks);
  for (std::size_t b = 0; b < blocks; ++b) {
    const auto [lo, hi] = window(b);
    std::vector<double> traded;
    for (std::size_t i = lo; i < hi; ++i) {
      if (std::isfinite(volume[i]) && volume[i] > 0.0) {
        traded.push_back(volume[i]);
      }
    }
    evidence[b] = volume_evidence(std::move(traded), settings);
  }

  Prefix present_sum(n);
  present_sum.build(present);
  Prefix missing_sum(n);
  missing_sum.build(missing);

  // The missing runs.
  for (std::size_t i = 0; i < n;) {
    if (present[i] > 0.0) {
      ++i;
      continue;
    }
    std::size_t end = i;
    while (end + 1 < n && present[end + 1] == 0.0) {
      ++end;
    }
    MissingRun run;
    run.first = dates[i];
    run.last = dates[end];
    run.sessions = static_cast<int>(end - i + 1);
    run.session_dates.assign(dates.begin() + static_cast<std::ptrdiff_t>(i),
                             dates.begin() + static_cast<std::ptrdiff_t>(end + 1));
    out.runs.push_back(std::move(run));
    i = end + 1;
  }
  std::vector<std::size_t> run_start;
  run_start.reserve(out.runs.size());
  for (const MissingRun& run : out.runs) {
    run_start.push_back(static_cast<std::size_t>(
        std::lower_bound(dates.begin(), dates.end(), run.first) - dates.begin()));
  }

  // Expected outage sessions, refined over a few passes (EM). A run's density
  // leaves out the run itself and what the previous pass judged to be outages, so
  // one outage does not make the next look normal. The first guess judges each run
  // against its block's prior alone: two holidays the calendar does not know about
  // would otherwise each make the other look like ordinary sparsity, and stay that
  // way.
  std::vector<double> outage(n, 0.0);
  for (std::size_t r = 0; r < out.runs.size(); ++r) {
    const std::size_t b = run_start[r] / block;
    const auto [lo, hi] = window(b);
    const double present_w = present_sum.range(lo, hi);
    const double density =
        healthy_density(present_w, static_cast<double>(hi - lo) - present_w, evidence[b], settings);
    const std::vector<double> p = run_posterior(out.runs[r].sessions, density, settings, true);
    std::copy(p.begin(), p.end(), outage.begin() + static_cast<std::ptrdiff_t>(run_start[r]));
  }
  for (int pass = 0; pass < kEmPasses; ++pass) {
    Prefix outage_sum(n);
    outage_sum.build(outage);
    for (std::size_t r = 0; r < out.runs.size(); ++r) {
      MissingRun& run = out.runs[r];
      const std::size_t start = run_start[r];
      const std::size_t stop = start + static_cast<std::size_t>(run.sessions);
      const std::size_t b = start / block;
      const auto [lo, hi] = window(b);
      const std::size_t overlap = std::min(stop, hi) - std::max(start, lo);
      const double own_outage = outage_sum.range(std::max(start, lo), std::min(stop, hi));
      const double other_outage = outage_sum.range(lo, hi) - own_outage;
      const double healthy_missing =
          std::max(0.0, missing_sum.range(lo, hi) - static_cast<double>(overlap) - other_outage);
      const double present_w = present_sum.range(lo, hi);
      const VolumeEvidence& traded = evidence[b];
      run.density = healthy_density(present_w, healthy_missing, traded, settings);
      run.observed_density = present_w / static_cast<double>(hi - lo);
      run.density_from_volume = traded.present && run.density == 1.0 - traded.no_trade;
      run.median_volume = traded.median_volume;
      run.session_p = run_posterior(run.sessions, run.density, settings, true);
      run.p_outage = *std::max_element(run.session_p.begin(), run.session_p.end());
    }
    std::fill(outage.begin(), outage.end(), 0.0);
    for (std::size_t r = 0; r < out.runs.size(); ++r) {
      std::copy(out.runs[r].session_p.begin(), out.runs[r].session_p.end(),
                outage.begin() + static_cast<std::ptrdiff_t>(run_start[r]));
    }
  }

  // Where the series is expected on every session, for the cohort check.
  Prefix outage_sum(n);
  outage_sum.build(outage);
  for (std::size_t b = 0; b < blocks; ++b) {
    const auto [lo, hi] = window(b);
    const double present_w = present_sum.range(lo, hi);
    const double healthy_missing =
        std::max(0.0, missing_sum.range(lo, hi) - outage_sum.range(lo, hi));
    const double density = healthy_density(present_w, healthy_missing, evidence[b], settings);
    if (b + 1 == blocks) {
      out.tail_density = density;
    }
    if (density <= confident_density) {
      continue;
    }
    const std::size_t from = b * block;
    const std::size_t to = std::min(n, (b + 1) * block) - 1;
    out.confident_ranges.emplace_back(dates[from], dates[to], density);
    for (std::size_t i = from; i <= to; ++i) {
      if (present[i] == 0.0) {
        out.confident_missing.push_back(dates[i]);
      }
    }
  }
  return out;
}

double tail_outage_probability(int k, double density, const CoverageSettings& settings) {
  if (k <= 0) {
    return 0.0;
  }
  const std::vector<double> p = run_posterior(k, density, settings, false);
  return *std::max_element(p.begin(), p.end());
}

}  // namespace dorq
