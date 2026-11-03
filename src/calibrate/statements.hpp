#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "calibrate/labels.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"

namespace dorq {

// `dorq calibrate` on statements (DR-0911; doc/calibration.md, "Statements"): the
// statement checks are run as `dorq check` runs them, with every candidate
// reported, and each candidate a statements label covers is an observation. The
// priors of the value model (DQ911 to DQ913), the identity model (DQ903) and the
// filing-date model (DQ931) are fitted to the labels' marginal likelihood, and an
// isotonic p_error map to each sub-family (DQ90x to DQ94x).

struct StatementCalibrateOptions {
  // Candidates no label covers count as innocent (a synthetic universe whose
  // every fault is labelled). Off: they are left out.
  bool clean_unlabelled = false;
  bool isotonic = true;  // fit the p_error maps
  double holdout = 0.0;  // the share of series held out for the report, by id hash
  unsigned threads = 1;
  std::optional<Date> as_of;  // as for check: DQ942's date
  std::string version;        // [statements.calibration] version
};

// One model whose priors are fitted.
struct StatementModelFit {
  std::string name;                     // values, identities, filings
  std::vector<std::string> hypotheses;  // in the model's order
  std::vector<double> before;           // prior probabilities, as configured
  std::vector<double> after;            // as fitted
  std::size_t observations = 0;         // training and held out
  std::size_t errors = 0;
  std::size_t test_observations = 0;
  // How far the priors may move from the configured ones, in e-folds: chosen by the
  // held-out observations' log-likelihood when there are some.
  double spread = 1.5;
  double mean_log_lik_before = 0.0;  // log P(label | evidence), per training observation
  double mean_log_lik_after = 0.0;
  double test_mean_log_lik_before = 0.0;
  double test_mean_log_lik_after = 0.0;
};

// One sub-family's p_error map.
struct StatementFamilyFit {
  std::size_t observations = 0;
  std::size_t errors = 0;
  std::vector<std::pair<double, double>> map;
  double ece_before = 0.0;  // on the held-out observations, or all when none
  double ece_after = 0.0;
};

struct StatementCalibrationReport {
  std::size_t labels = 0;            // in the file
  std::size_t statement_labels = 0;  // about statements
  std::size_t matched_labels = 0;    // covering at least one candidate
  std::size_t candidates = 0;        // reported with every candidate shown
  std::array<StatementModelFit, 3> models;
  std::array<StatementFamilyFit, StatementCalibration::kFamilies> families;
  double ece_before = 0.0;  // over every family's observations
  double ece_after = 0.0;
  StatementPriors priors;  // the fitted priors, the rest as configured
  std::string version;
};

[[nodiscard]] StatementCalibrationReport calibrate_statements(
    const std::vector<Series>& series, const std::vector<Label>& labels, const Config& config,
    const Calendar& calendar, const Context& context, const StatementCalibrateOptions& options);

// The fitted settings as a TOML file for `include`, with the fit in comments.
[[nodiscard]] std::string statements_toml(const StatementCalibrationReport& report);

// The fit summarised for people.
[[nodiscard]] std::string statements_summary(const StatementCalibrationReport& report);

}  // namespace dorq
