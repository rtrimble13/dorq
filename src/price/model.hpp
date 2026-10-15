#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "price/features.hpp"

namespace dorq {

// The explanations compared for a suspicious bar (plan section 4.3).
enum class PriceHypothesis : std::uint8_t {
  kMarketMove,         // a real move: ordinary, or a jump
  kTickMove,           // a move of one or two ticks, large only because the price is small
  kBadPrint,           // one to revert_max_bars bars off the series, which then reverts
  kBadClose,           // only the close field is wrong: open, high and low held the level
  kUnreportedSplit,    // the level shifts by a split ratio, and volume shifts inversely
  kScaleError,         // the level shifts by a power of ten (or to an implausible price)
  kHistorySegment,     // after a long gap, a different security's history continues
  kExplainedSplit,     // with --actions: a split on file between the bars explains the move
  kExplainedDividend,  // with --actions: a cash dividend on file between the bars comes off
  // A NAV-priced fund (--meta nav_priced) falls by a distribution not yet on file.
  kPendingDistribution,
};
inline constexpr std::size_t kPriceHypotheses = 10;

[[nodiscard]] std::string_view to_string(PriceHypothesis hypothesis) noexcept;
[[nodiscard]] constexpr bool is_error(PriceHypothesis hypothesis) noexcept {
  return hypothesis != PriceHypothesis::kMarketMove && hypothesis != PriceHypothesis::kTickMove &&
         hypothesis != PriceHypothesis::kExplainedSplit &&
         hypothesis != PriceHypothesis::kExplainedDividend &&
         hypothesis != PriceHypothesis::kPendingDistribution;
}

// The cash dividends on file between a bar and the one before, together.
struct DividendOnFile {
  double amount = 0.0;  // their sum, per share
  Date ex_date;         // the latest ex-date
  int count = 0;
};

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
  // Each hypothesis's log-likelihood of the evidence, every term but the prior:
  // what `dorq calibrate` fits the priors to.
  std::array<double, kPriceHypotheses> log_likelihood{};
  double p_error = 0.0;      // calibrated by [calibration] p_error_map, when given
  double raw_p_error = 0.0;  // as the model computes it
  double tail = 1.0;         // the return's two-sided tail probability, as ordinary
  bool provisional = false;
  int block = 1;                             // bars in a bad print
  double factor = 1.0;                       // close / previous close
  std::optional<SplitRatio> split;           // the split ratio nearest the move
  int power_of_ten = 0;                      // for a scale error: the nearest power of ten
  std::optional<double> volume_ratio;        // median volume after / before
  std::optional<SplitAction> split_on_file;  // a split on file between this bar and the last
  // Dividends on file between this bar and the last, when explained_dividend was
  // considered.
  std::optional<DividendOnFile> dividend_on_file;
  // A DQ7xx finding about a split on file accounts for this move (a misdated or
  // doubled split, the wrong ratio): the DQ2xx checks leave it to that one.
  bool claimed = false;
  // The most probable hypothesis of all, errors or not.
  PriceHypothesis leading = PriceHypothesis::kMarketMove;
  // The bar's range: high / low, and its log against the median log range of the
  // bars before (when enough of them have one). A violent session's is
  // violent_range times the usual or more.
  std::optional<double> range_ratio;
  std::optional<double> range_vs_typical;
  bool violent = false;
  // Most bars before have no volume, or the median is under one trade: the volume
  // terms say nothing and are left out.
  bool base_volume_zero = false;
  // Volume surged and stayed up after the move, more than a split's inverse shift.
  bool volume_surge = false;
  // A real move by its own evidence (a violent session or a volume surge): DQ601
  // leaves such a move out of a cohort.
  [[nodiscard]] bool market_like() const noexcept { return violent || volume_surge; }
  // DQ203 needs the volume to shift inversely by about the ratio, or no volume to
  // judge by. false: unreported_split was left out of the posterior for that.
  bool split_volume_ok = true;
  double move_sd = 0.0;  // the sd of an ordinary move to this bar, on the scale of y
  // A scale era the bars show before a split on file whose ratio they already
  // carry (DQ202): the split.
  std::optional<SplitAction> era_split;
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
  // kNoJump: another split on file at the same ratio has its ex-date where the
  // bars move, and the bars confirm it: this row is a duplicate of that one.
  std::optional<Date> duplicate_of;
  // The bars moved by the ratio before the ex-date and not on it, and the volume
  // between stayed at the level before: sessions before the split stored at the
  // post-split scale. Reported as a DQ202 era; the split itself is confirmed.
  bool era_before = false;
};

// A run of traded bars with the same close (DQ501): bars first..last repeat the
// close of the bar before them.
struct StaleRun {
  std::size_t first = 0;  // feature index of the first repeat
  std::size_t last = 0;
  int full_bars = 0;   // repeats where open, high and low repeat too
  double log_q = 0.0;  // log P(the closes repeat | a healthy feed)
  double p_error = 0.0;
  double grid = 0.0;  // the price grid the run is judged on
  // The median move of the traded closes nearby, the run left out, in steps of
  // the grid; NaN when too few. With a fine grid and closes that barely move, the
  // price is pinned (a SPAC at its trust value) and a run is how it trades.
  double close_steps = std::numeric_limits<double>::quiet_NaN();
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
  // The standard deviation of each bar's ordinary move (0 for the first).
  std::vector<double> move_sd;
};

// What the model knows about a series besides its bars (plan section 2.2).
struct PriceContext {
  std::optional<Bounds> bounds = std::nullopt;     // a point series' range ([integrity] bounds)
  bool have_actions = false;                       // --actions was given
  const SeriesActions* actions = nullptr;          // this series' actions on file, if any
  std::optional<double> tick_size = std::nullopt;  // --meta: the tick, instead of inferring it
  const MarketSeries* market = nullptr;            // --market
  bool nav_priced = false;                         // --meta: struck once a day at NAV
  double trade_size = 0.0;  // [coverage] trade_size: a volume median below it is no base
};

// p mapped through a calibration map (PriceSettings::p_error_map): linear between
// knots, the end knots' values beyond them; p itself when the map is empty.
[[nodiscard]] double calibrated(const std::vector<std::pair<double, double>>& map,
                                double p) noexcept;

[[nodiscard]] PriceAnalysis analyze_prices(const Series& series, const Calendar& calendar,
                                           const PriceSettings& settings,
                                           const PriceContext& context = {});

}  // namespace dorq
