#include "checks/volume.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/coverage.hpp"
#include "checks/price.hpp"
#include "core/text.hpp"
#include "dorq/number.hpp"
#include "price/model.hpp"
#include "price/tick.hpp"
#include "stats/robust.hpp"
#include "stats/special.hpp"
#include "stats/student_t.hpp"

namespace dorq {
namespace {

using H = PriceHypothesis;

double log_normal_pdf(double x, double mean, double sd) noexcept {
  const double z = (x - mean) / sd;
  return -0.5 * z * z - std::log(sd) - 0.5 * std::log(2.0 * std::numbers::pi);
}

// Ratios a volume error multiplies by, with prior weights: a change of units
// (shares to hundreds or thousands), a decimal slip, and volume adjusted for a
// split once too often (by the split ratio, or its square: ADR 0004).
struct CleanRatio {
  double ratio = 1.0;
  double weight = 0.0;
};
constexpr std::array<CleanRatio, 11> kVolumeRatios = {{{100.0, 0.30},
                                                       {1000.0, 0.20},
                                                       {10.0, 0.12},
                                                       {4.0, 0.10},
                                                       {1e6, 0.05},
                                                       {9.0, 0.05},
                                                       {2.0, 0.05},
                                                       {16.0, 0.03},
                                                       {25.0, 0.03},
                                                       {3.0, 0.03},
                                                       {5.0, 0.04}}};

// log density of a log-ratio under an error at a clean ratio, either way.
double clean_ratio_ll(double log_ratio, double sd) {
  double total = -std::numeric_limits<double>::infinity();
  for (const CleanRatio& c : kVolumeRatios) {
    for (const double sign : {-1.0, 1.0}) {
      total = stats::log_add(total, std::log(c.weight / 2.0) +
                                        log_normal_pdf(log_ratio, sign * std::log(c.ratio), sd));
    }
  }
  return total;
}

// The nearest clean ratio, as "×100" or "×1/100".
std::string nearest_ratio(double log_ratio) {
  double best = 1.0;
  for (const CleanRatio& c : kVolumeRatios) {
    if (std::fabs(std::fabs(log_ratio) - std::log(c.ratio)) <
        std::fabs(std::fabs(log_ratio) - std::log(best))) {
      best = c.ratio;
    }
  }
  return log_ratio >= 0.0 ? "×" + format_number(best) : "×1/" + format_number(best);
}

// The traded bars of a series' price features: feature index and log volume.
struct Traded {
  std::vector<std::size_t> bar;
  std::vector<double> log_volume;
};

Traded traded_bars(const Series& s, const PriceFeatures& f) {
  Traded t;
  for (std::size_t i = 0; i < f.size(); ++i) {
    const double v = s.volume[f.row[i]];
    if (std::isfinite(v) && v > 0.0) {
      t.bar.push_back(i);
      t.log_volume.push_back(std::log(v));
    }
  }
  return t;
}

double median_of(const std::vector<double>& values, std::size_t from, std::size_t to) {
  return stats::median({values.begin() + static_cast<std::ptrdiff_t>(from),
                        values.begin() + static_cast<std::ptrdiff_t>(to)});
}

double mad_of(const std::vector<double>& values, std::size_t from, std::size_t to) {
  return stats::mad(std::span<const double>(values).subspan(from, to - from));
}

// The share of a window's values that count as independent: (1 - rho) / (1 + rho)
// with rho the lag-1 autocorrelation of [from, mid) and [mid, to), each about its
// own median.
double effective_share(const std::vector<double>& values, std::size_t from, std::size_t mid,
                       std::size_t to) {
  double cross = 0.0;
  double square = 0.0;
  for (const auto& [a, b] : {std::pair{from, mid}, std::pair{mid, to}}) {
    const double centre = median_of(values, a, b);
    for (std::size_t i = a; i < b; ++i) {
      const double x = values[i] - centre;
      square += x * x;
      if (i > a) {
        cross += x * (values[i - 1] - centre);
      }
    }
  }
  const double rho = square > 0.0 ? std::clamp(cross / square, 0.0, 0.9) : 0.0;
  return (1.0 - rho) / (1.0 + rho);
}

// A step in the volume level, at traded bar k.
struct Shift {
  std::size_t k = 0;       // the first traded bar after the step
  std::size_t end = 0;     // the last traded bar of an era (two opposite steps)
  double log_ratio = 0.0;  // median after / before
  double p_error = 0.0;
  bool era = false;
};

constexpr double kShiftErrorPrior = 0.05;  // a volume step being an error, per step found
constexpr double kNaturalScale = 0.5;      // natural shifts: Student t (4 dof), this scale
constexpr double kScreen = 0.5878;         // log 1.8: steps smaller than this are not judged
constexpr double kRatioTolerance = 0.05;   // an error's step about its clean ratio

std::vector<Shift> find_shifts(const Series& s, const PriceAnalysis& analysis, std::size_t window) {
  std::vector<Shift> shifts;
  const Traded t = traded_bars(s, analysis.features);
  const std::size_t m = t.log_volume.size();
  if (m < 2 * window + 1) {
    return shifts;
  }
  std::vector<double> prefix(m + 1, 0.0);
  for (std::size_t i = 0; i < m; ++i) {
    prefix[i + 1] = prefix[i] + t.log_volume[i];
  }
  const auto mean = [&](std::size_t from, std::size_t to) {
    return (prefix[to] - prefix[from]) / static_cast<double>(to - from);
  };
  const auto w = static_cast<double>(window);
  for (std::size_t k = window; k + window <= m;) {
    if (std::fabs(mean(k, k + window) - mean(k - window, k)) < kScreen) {
      ++k;
      continue;
    }
    // The strongest step in this stretch of candidates.
    std::size_t best = k;
    double best_d = 0.0;
    std::size_t j = k;
    for (; j + window <= m; ++j) {
      const double d = mean(j, j + window) - mean(j - window, j);
      if (std::fabs(d) < kScreen) {
        break;
      }
      if (std::fabs(d) > std::fabs(best_d)) {
        best_d = d;
        best = j;
      }
    }
    k = j + 1;
    const double after = median_of(t.log_volume, best, best + window);
    const double before = median_of(t.log_volume, best - window, best);
    const double dv = after - before;
    // A median of n values is off by about 1.25 sd / sqrt(n), with n the effective
    // count: volume runs in spells, so neighbouring days tell less than two.
    const double n_eff = w * effective_share(t.log_volume, best - window, best, best + window);
    const double noise =
        std::hypot(1.25 * mad_of(t.log_volume, best - window, best) / std::sqrt(n_eff),
                   1.25 * mad_of(t.log_volume, best, best + window) / std::sqrt(n_eff));
    // A step the price explains is a split's (DQ203), not volume's.
    std::vector<double> price_before;
    std::vector<double> price_after;
    for (std::size_t i = best - window; i < best + window; ++i) {
      (i < best ? price_before : price_after).push_back(analysis.features.y[t.bar[i]]);
    }
    const double dp = stats::median(price_after) - stats::median(price_before);
    const std::size_t bar = t.bar[best];
    const bool price_moved =
        std::any_of(analysis.findings.begin(), analysis.findings.end(), [&](const PriceFinding& f) {
          return f.bar + 2 >= bar && f.bar <= bar + 2 && f.p_error >= 0.5 &&
                 (f.hypothesis == H::kUnreportedSplit || f.hypothesis == H::kScaleError ||
                  f.hypothesis == H::kHistorySegment);
        });
    if (price_moved || (std::fabs(dp) > 0.3 && std::fabs(dv + dp) < 0.3)) {
      continue;
    }
    const double natural = std::log1p(-kShiftErrorPrior) +
                           stats::student_t_log_pdf(dv, 4.0, std::hypot(kNaturalScale, noise));
    const double error =
        std::log(kShiftErrorPrior) + clean_ratio_ll(dv, std::hypot(kRatioTolerance, noise));
    Shift shift;
    shift.k = best;
    shift.end = best;
    shift.log_ratio = dv;
    shift.p_error = std::exp(error - stats::log_add(error, natural));
    shifts.push_back(shift);
  }
  // Two opposite steps bound an era at the wrong scale: report it once.
  std::vector<Shift> out;
  for (std::size_t i = 0; i < shifts.size(); ++i) {
    Shift a = shifts[i];
    const Shift& b = i + 1 < shifts.size() ? shifts[i + 1] : a;
    if (i + 1 < shifts.size() && a.p_error >= 0.5 && b.p_error >= 0.5 &&
        (a.log_ratio > 0.0) != (b.log_ratio > 0.0) &&
        nearest_ratio(a.log_ratio) == nearest_ratio(-b.log_ratio)) {
      a.end = shifts[i + 1].k - 1;
      a.era = true;
      a.p_error = std::max(a.p_error, shifts[i + 1].p_error);
      ++i;
    }
    out.push_back(a);
  }
  // Back to rows' feature indices.
  for (Shift& shift : out) {
    shift.k = t.bar[shift.k];
    shift.end = t.bar[shift.end];
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// DQ401 volume-scale-shift

const CheckInfo& VolumeScaleShift::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ401",
      .name = "volume-scale-shift",
      .summary = "the volume level steps by a clean ratio with no price change to explain it",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void VolumeScaleShift::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (!s.has_volume || context.price_analysis == nullptr || !context.price_analysis->applicable) {
    return;
  }
  const PriceAnalysis& analysis = *context.price_analysis;
  const auto window = static_cast<std::size_t>(context.price.volume_window);
  for (const Shift& shift : find_shifts(s, analysis, window)) {
    const auto severity = context.thresholds.for_probability(shift.p_error);
    if (!severity) {
      continue;
    }
    const std::size_t row = analysis.features.row[shift.k];
    const std::size_t end_row = analysis.features.row[shift.end];
    const double factor = std::exp(shift.log_ratio);
    Violation v;
    v.check = &info();
    v.severity = *severity;
    v.p_error = shift.p_error;
    v.date = s.date[row];
    v.line = s.line[row];
    if (shift.era) {
      v.end_date = s.date[end_row];
    }
    v.message = std::string{shift.era ? "volume is " : "volume level steps to "} + "×" +
                significant(factor, 4) + " (≈ " + nearest_ratio(shift.log_ratio) + ")" +
                (shift.era ? " the level either side," : std::string{}) +
                " with no price change to explain it: a change of units, or volume adjusted "
                "for a split; P(error) = " +
                format_probability(shift.p_error);
    v.detail = {{"volume_ratio", factor}, {"nearest_ratio", nearest_ratio(shift.log_ratio)}};
    SuggestedAction action;
    action.kind = "rescale_volume";
    const std::string last =
        shift.era ? s.date[end_row].to_string() : s.date[s.size() - 1].to_string();
    action.fields = {{"first", s.date[row].to_string()}, {"last", last}, {"factor", 1.0 / factor}};
    action.text = "rescale the volume of " + s.date[row].to_string() + ".." + last + " by ×" +
                  significant(1.0 / factor, 4) + ", or re-fetch it";
    v.suggested_action = std::move(action);
    out.push_back(std::move(v));
  }
}

// ---------------------------------------------------------------------------
// DQ402 volume-spike-no-move

const CheckInfo& VolumeSpikeNoMove::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ402",
      .name = "volume-spike-no-move",
      .summary = "info: volume ten times or more its usual level on a day the price did not move",
      .default_severity = Severity::kInfo,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void VolumeSpikeNoMove::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (!s.has_volume || context.price_analysis == nullptr || !context.price_analysis->applicable) {
    return;
  }
  const PriceAnalysis& analysis = *context.price_analysis;
  const auto window = static_cast<std::size_t>(context.price.volume_window);
  const Traded t = traded_bars(s, analysis.features);
  const std::size_t m = t.log_volume.size();
  if (m <= window) {
    return;
  }
  // Bars a volume step (DQ401) explains: its era, and the window after it, while
  // the trailing median still holds the other level.
  std::vector<std::pair<std::size_t, std::size_t>> explained;
  for (const Shift& shift : find_shifts(s, analysis, window)) {
    if (shift.p_error >= 0.5) {
      explained.emplace_back(shift.k, shift.end + window);
    }
  }
  constexpr double kSpike = std::numbers::ln10;  // ×10
  constexpr double kQuiet = 1.5;                 // |move| below this many sd is no move
  constexpr double kSpikePrior = 0.1;            // a volume spike without a move being an error
  constexpr double kSpikeScale = 1.0;            // real spikes: log ratio above 10x, exponential
  double sum = 0.0;
  for (std::size_t i = 0; i < window; ++i) {
    sum += t.log_volume[i];
  }
  for (std::size_t k = window; k < m; sum += t.log_volume[k] - t.log_volume[k - window], ++k) {
    // A cheap screen on the trailing mean, then the trailing median.
    if (t.log_volume[k] - sum / static_cast<double>(window) < kSpike - 0.5) {
      continue;
    }
    const double usual = median_of(t.log_volume, k - window, k);
    const double spread = std::max(0.3, mad_of(t.log_volume, k - window, k));
    const double lr = t.log_volume[k] - usual;
    const std::size_t bar = t.bar[k];
    if (lr < kSpike || lr / spread < 4.0 || std::fabs(analysis.move_z[bar]) >= kQuiet ||
        std::any_of(explained.begin(), explained.end(), [bar](const auto& range) {
          return bar >= range.first && bar <= range.second;
        })) {
      continue;
    }
    // A recording error lands on a clean ratio; a real spike anywhere above 10x.
    const double real =
        std::log1p(-kSpikePrior) - std::log(kSpikeScale) - (lr - kSpike) / kSpikeScale;
    const double error = std::log(kSpikePrior) + clean_ratio_ll(lr, 0.1);
    const double p = std::exp(error - stats::log_add(error, real));
    const std::size_t row = analysis.features.row[bar];
    Violation v;
    v.check = &info();
    v.severity = Severity::kInfo;
    v.classification = p >= 0.5 ? Classification::kDataError : Classification::kMarketFact;
    v.p_error = p;
    v.date = s.date[row];
    v.line = s.line[row];
    v.message = "volume " + whole_number(s.volume[row]) + " is ×" + significant(std::exp(lr), 3) +
                " the usual " + whole_number(std::exp(usual)) + ", on a day the " +
                std::string{field_name(Field::kClose, s.kind)} + " moved " +
                significant(std::fabs(analysis.move_z[bar]), 2) +
                " standard deviations; P(error) = " + format_probability(p);
    v.detail = {{"volume", s.volume[row]},
                {"median_volume", std::exp(usual)},
                {"move_sd", analysis.move_z[bar]}};
    out.push_back(std::move(v));
  }
}

// ---------------------------------------------------------------------------
// DQ403 move-on-zero-volume

const CheckInfo& MoveOnZeroVolume::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ403",
      .name = "move-on-zero-volume",
      .summary = "a price change on a bar with no volume, where the series rarely has one",
      .default_severity = Severity::kWarn,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void MoveOnZeroVolume::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (!s.has_volume) {
    return;
  }
  std::vector<std::uint8_t> decimals;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (std::isfinite(s.close[i])) {
      decimals.push_back(s.close_decimals[i]);
    }
  }
  const double written = written_grid(std::move(decimals));
  // Bars with no volume whose close moved at least half a grid step from the
  // close before; and how many bars carry a volume at all.
  std::vector<std::pair<std::size_t, std::size_t>> moves;  // row, previous row
  std::size_t with_volume = 0;
  std::size_t previous = s.size();
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double c = s.close[i];
    if (!std::isfinite(c)) {
      continue;
    }
    if (std::isfinite(s.volume[i])) {
      ++with_volume;
    }
    if (previous < s.size() && s.volume[i] == 0.0 && s.date[i] != s.date[previous]) {
      const double tick = context.meta_tick().value_or(tick_size(s.date[i], std::fabs(c)));
      const double grid = std::max(tick, written);
      if (std::fabs(c - s.close[previous]) >= 0.5 * grid) {
        moves.emplace_back(i, previous);
      }
    }
    previous = i;
  }
  // A healthy feed's rate of such bars, from the others; against an error's prior.
  constexpr double kErrorPrior = 0.01;
  const double others = static_cast<double>(moves.size()) - 1.0;
  const double rate = (others + 0.5) / (static_cast<double>(with_volume) + 1.0);
  const double p = kErrorPrior / (kErrorPrior + (1.0 - kErrorPrior) * rate);
  const auto severity = context.thresholds.for_probability(p);
  if (!severity) {
    return;
  }
  const Severity level = *severity;
  for (const auto& [row, prev] : moves) {
    Violation v;
    v.check = &info();
    v.severity = level;
    v.p_error = p;
    v.date = s.date[row];
    v.line = s.line[row];
    v.message = "close " + significant(s.close[prev], 6) + "→" + significant(s.close[row], 6) +
                " on zero volume, where " + with_commas(static_cast<long long>(others)) +
                " other bars of " + with_commas(static_cast<long long>(with_volume)) +
                " move with no volume: an indicative quote, or a lost volume; P(error) = " +
                format_probability(p);
    v.detail = {{"close", s.close[row]},
                {"previous_close", s.close[prev]},
                {"other_zero_volume_moves", static_cast<std::int64_t>(others)}};
    SuggestedAction action;
    action.kind = "refetch_bar";
    action.fields = {{"date", s.date[row].to_string()}, {"field", std::string{"volume"}}};
    action.text = "re-fetch the bar of " + s.date[row].to_string();
    v.suggested_action = std::move(action);
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
