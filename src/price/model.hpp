#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
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
  kExplainedSplit,   // with --actions: a split on file between the bars explains the move
};
inline constexpr std::size_t kPriceHypotheses = 8;

[[nodiscard]] std::string_view to_string(PriceHypothesis hypothesis) noexcept;
[[nodiscard]] constexpr bool is_error(PriceHypothesis hypothesis) noexcept {
  return hypothesis != PriceHypothesis::kMarketMove && hypothesis != PriceHypothesis::kTickMove &&
         hypothesis != PriceHypothesis::kExplainedSplit;
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
  int block = 1;                             // bars in a bad print
  double factor = 1.0;                       // close / previous close
  std::optional<SplitRatio> split;           // the split ratio nearest the move
  int power_of_ten = 0;                      // for a scale error: the nearest power of ten
  std::optional<double> volume_ratio;        // median volume after / before
  std::optional<SplitAction> split_on_file;  // a split on file between this bar and the last
  // A DQ7xx finding about a split on file accounts for this move (a misdated or
  // doubled split, the wrong ratio): the DQ2xx checks leave it to that one.
  bool claimed = false;
  std::vector<PriceEvidence> evidence;
};

// What the bars say about a split on file (DQ701-DQ704).
enum class SplitVerdict : std::uint8_t {
  kConfirmed,      // the bars move by its ratio between the bars around its ex-date
  kMisdated,       // they move by its ratio, but a few sessions away (DQ701)
  kNoJump,         // they do not move by it nearby (DQ702)
  kRatioMismatch,  // they move on the ex-date, by another ratio (DQ703)
  kDoubleApplied,  // they move by its ratio twice, close together (DQ704)
};
inline constexpr std::size_t kSplitVerdicts = 5;

[[nodiscard]] std::string_view to_string(SplitVerdict verdict) noexcept;

struct SplitFinding {
  SplitAction split;
  SplitVerdict verdict = SplitVerdict::kConfirmed;  // the most probable error
  std::array<double, kSplitVerdicts> posterior{};
  double p_error = 0.0;    // P(any verdict but kConfirmed)
  std::size_t ex_bar = 0;  // feature index of the first bar on or after the ex-date
  // kMisdated: the bar the bars move at; kDoubleApplied: the second move.
  std::optional<std::size_t> other_bar;
  int sessions_off = 0;  // from the ex-date's bar to other_bar, in sessions
  // kRatioMismatch: the ratio the move at the ex-date fits best ("10:1").
  std::string observed;
  bool provisional = false;
};

// A run of traded bars with the same close (DQ501): bars first..last repeat the
// close of the bar before them.
struct StaleRun {
  std::size_t first = 0;  // feature index of the first repeat
  std::size_t last = 0;
  int full_bars = 0;   // repeats where open, high and low repeat too
  double log_q = 0.0;  // log P(the closes repeat | a healthy feed)
  double p_error = 0.0;
};

struct PriceAnalysis {
  bool applicable = false;
  PriceFeatures features;
  std::vector<PriceFinding> findings;        // in bar order
  std::vector<StaleRun> stale_runs;          // in bar order; OHLCV with volume only
  std::vector<SplitFinding> split_findings;  // with --actions: each split on file judged
  // Each bar's return in standard deviations of an ordinary move (0 for the
  // first): what the volume checks call a move.
  std::vector<double> move_z;
};

// What the model knows about a series besides its bars (plan section 2.2).
struct PriceContext {
  std::optional<Bounds> bounds = std::nullopt;     // a point series' range ([integrity] bounds)
  bool have_actions = false;                       // --actions was given
  const SeriesActions* actions = nullptr;          // this series' actions on file, if any
  std::optional<double> tick_size = std::nullopt;  // --meta: the tick, instead of inferring it
  const MarketSeries* market = nullptr;            // --market
};

[[nodiscard]] PriceAnalysis analyze_prices(const Series& series, const Calendar& calendar,
                                           const PriceSettings& settings,
                                           const PriceContext& context = {});

}  // namespace dorq
