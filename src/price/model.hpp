#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "price/features.hpp"

namespace dorq {

// The explanations compared for a suspicious bar (plan section 4.3).
enum class PriceHypothesis : std::uint8_t {
  kMarketMove,       // a real move: ordinary, or a jump
  kTickMove,         // a move of one or two ticks, large only because the price is small
  kBadPrint,         // one to revert_max_bars bars off the series, which then reverts
  kBadClose,         // only the close field is wrong: open, high and low held the level
  kUnreportedSplit,  // the level shifts by a split ratio, and volume shifts inversely
  kScaleError,       // the level shifts by a power of ten (or to an implausible price)
  kHistorySegment,   // after a long gap, a different security's history continues
};
inline constexpr std::size_t kPriceHypotheses = 7;

[[nodiscard]] std::string_view to_string(PriceHypothesis hypothesis) noexcept;
[[nodiscard]] constexpr bool is_error(PriceHypothesis hypothesis) noexcept {
  return hypothesis != PriceHypothesis::kMarketMove && hypothesis != PriceHypothesis::kTickMove;
}

// One evidence term, as the log Bayes factor of the reported hypothesis against
// market_move.
struct PriceEvidence {
  std::string feature;
  DetailValue value;
  double log_bf = 0.0;
  std::string note;
};

// A scored candidate.
struct PriceFinding {
  std::size_t bar = 0;      // feature index of the first bar the hypothesis blames
  std::size_t end_bar = 0;  // its last (a bad print's block, or a scale era)
  PriceHypothesis hypothesis = PriceHypothesis::kMarketMove;  // most probable error
  std::array<double, kPriceHypotheses> posterior{};
  std::array<bool, kPriceHypotheses> considered{};
  double p_error = 0.0;
  double tail = 1.0;  // the return's two-sided tail probability, as ordinary
  bool provisional = false;
  int block = 1;                       // bars in a bad print
  double factor = 1.0;                 // close / previous close
  std::optional<SplitRatio> split;     // the split ratio nearest the move
  int power_of_ten = 0;                // for a scale error: the nearest power of ten
  std::optional<double> volume_ratio;  // median volume after / before
  std::vector<PriceEvidence> evidence;
};

// A run of traded bars with the same close (DQ501): bars first..last repeat the
// close of the bar before them.
struct StaleRun {
  std::size_t first = 0;  // feature index of the first repeat
  std::size_t last = 0;
  int full_bars = 0;      // repeats where open, high and low repeat too
  double log_q = 0.0;     // log P(the closes repeat | a healthy feed)
  double p_error = 0.0;
};

struct PriceAnalysis {
  bool applicable = false;
  PriceFeatures features;
  std::vector<PriceFinding> findings;  // in bar order
  std::vector<StaleRun> stale_runs;    // in bar order; OHLCV with volume only
  // Each bar's return in standard deviations of an ordinary move (0 for the
  // first): what the volume checks call a move.
  std::vector<double> move_z;
};

// `bounds` is a point series' plausible range ([integrity] bounds), if any.
[[nodiscard]] PriceAnalysis analyze_prices(const Series& series, const Calendar& calendar,
                                           const PriceSettings& settings,
                                           std::optional<Bounds> bounds = std::nullopt);

}  // namespace dorq
