#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "calibrate/labels.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/series.hpp"
#include "price/model.hpp"

namespace dorq {

// `dorq calibrate` (plan section 6; doc/calibration.md): fits the price model's
// hypothesis priors and a few hyperparameters to labelled history by maximising
// the labels' marginal likelihood, then fits an isotonic map from p_error to the
// rate at which labelled moves were errors.

struct CalibrateOptions {
  // Scored moves that match no label count as market facts (a synthetic universe
  // whose every fault is labelled). Off: they are left out, as unknown.
  bool clean_unlabelled = false;
  bool grid = true;      // search jump_prob, jump_scale and ratio_tolerance
  bool isotonic = true;  // fit the p_error map
  double holdout = 0.0;  // the share of series held out for the report, by id hash
  unsigned threads = 1;
  std::string version;  // names the calibration ([calibration] version)
};

// One scored move with a label: the evidence under each hypothesis, and which
// hypotheses the label allows.
struct Observation {
  std::array<double, kPriceHypotheses> log_likelihood{};
  std::array<bool, kPriceHypotheses> considered{};
  std::array<bool, kPriceHypotheses> allowed{};
  bool error = false;  // a data error or a context gap
  bool test = false;   // held out
  // The label's weight (DE-01): the observation's log-likelihood and its point in
  // the p_error map count this many times.
  double weight = 1.0;
};

// A point of a p_error map's fit: p_error, the outcome (1 an error, 0 not) and its
// weight.
struct WeightedPoint {
  double x = 0.0;
  double y = 0.0;
  double weight = 1.0;
  [[nodiscard]] bool operator<(const WeightedPoint& other) const noexcept {
    return std::tie(x, y, weight) < std::tie(other.x, other.y, other.weight);
  }
};

// DE-13: a bad print's block length, from the labels that record one (a DQ201
// label's bars in the series). The model gives a block of k bars prior weight
// proportional to block_decay^k, up to revert_max_bars: a geometric distribution,
// whose parameter is fitted here with kBlockPseudoLabels pseudo-labels at the
// configured one. The longest block is then the smallest that leaves less than
// 1 / (labels + 1) of the fitted distribution beyond it, and no shorter than any
// labelled block: a longer one would have been expected among the labels. With
// fewer than kMinBlockLabels labels both stay as configured.
inline constexpr std::size_t kMinBlockLabels = 20;
inline constexpr double kBlockPseudoLabels = 5.0;
struct BlockFit {
  std::size_t labels = 0;  // DQ201 labels whose span holds 1 to 20 bars of their series
  int longest = 0;         // bars, among them
  double block_decay = 0.5;
  int revert_max_bars = 5;
  bool fitted = false;  // enough labels to fit
};

// The fit of `blocks` (each a labelled block's bars, and its label's weight) from
// the configured settings.
[[nodiscard]] BlockFit fit_blocks(const std::vector<std::pair<int, double>>& blocks,
                                  double block_decay, int revert_max_bars);

// How well one setting explains the labels.
struct Fit {
  PricePriors priors;
  double jump_prob = 0.0;
  double jump_scale = 0.0;
  double ratio_tolerance = 0.0;
  double block_decay = 0.5;
  int revert_max_bars = 5;
  double objective = 0.0;     // penalized log-likelihood, training observations
  double mean_log_lik = 0.0;  // per training observation
  double test_mean_log_lik = 0.0;
};

struct CalibrationReport {
  std::size_t labels = 0;          // in the file
  std::size_t unverified = 0;      // of those, not verified ([calibration] unverified)
  std::size_t excluded = 0;        // of those, left out of the fit
  std::string unverified_rule;     // "exclude", "include" or "weight:x"
  std::size_t price_labels = 0;    // about price moves (DQ2xx, DQ601), or market facts
  std::size_t matched_labels = 0;  // with at least one scored move
  std::size_t observations = 0;
  std::size_t errors = 0;  // observations labelled an error or a context gap
  std::size_t test_observations = 0;
  std::size_t settings_tried = 0;
  BlockFit blocks;  // DE-13
  Fit before;       // the configuration as given
  Fit after;
  std::vector<std::pair<double, double>> p_error_map;
  double ece_before = 0.0;  // on the held-out observations, or all when none
  double ece_after = 0.0;
  std::string version;
};

[[nodiscard]] CalibrationReport calibrate(const std::vector<Series>& series,
                                          const std::vector<Label>& labels, const Config& config,
                                          const Calendar& calendar, const Context& context,
                                          const CalibrateOptions& options);

// The fitted settings as a TOML file for `include`, with the fit summarised in
// comments.
[[nodiscard]] std::string priors_toml(const CalibrationReport& report);

// The fit summarised for people.
[[nodiscard]] std::string calibration_summary(const CalibrationReport& report);

// Isotonic regression (pool adjacent violators) of y on x, over at most `bins`
// equal-count bins of x: rising (x, y) knots. A bin's x and y are its points'
// weighted means, and it pools with the weight of its points.
[[nodiscard]] std::vector<std::pair<double, double>> isotonic_map(std::vector<WeightedPoint> points,
                                                                  std::size_t bins = 20);

// Expected calibration error over `bins` equal-width bins of p: the mean
// |p - observed rate|, weighted by each bin's share of the points' weight.
[[nodiscard]] double expected_calibration_error(const std::vector<WeightedPoint>& points,
                                                std::size_t bins = 10);

}  // namespace dorq
