#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "statements/values.hpp"
#include "statements/views.hpp"

namespace dorq {

// The share terms model (DQ921, DQ922; fundamentals plan section 4.3;
// doc/statements.md, "Share counts"). A share count is stored in the terms of its
// version's known_from (fafnir's ADR 0018); put into one terms with the splits on
// file, a count moves only by issuance and buybacks.

enum class ShareHypothesis : std::uint8_t { kRealIssuance, kUnreportedSplit, kTermsError, kUnitError };
inline constexpr std::array<const char*, 4> kShareHypothesisNames{
    "real_issuance", "unreported_split", "terms_error", "unit_error"};

struct ShareFinding {
  enum class Kind : std::uint8_t {
    kStep,      // DQ921: a share count's step between two periods
    kMismatch,  // DQ922: a period's weighted-average and outstanding shares
  };
  Kind kind = Kind::kStep;
  std::size_t row = 0;       // the period the step lands on, or the period (DQ922)
  std::size_t last_row = 0;  // a run in the wrong terms: its last period
  std::size_t before = 0;    // DQ921: the period before the step
  std::size_t item = kNoItem;
  std::size_t other = kNoItem;  // DQ922: the count compared with
  std::array<double, 4> posterior{};  // by ShareHypothesis (DQ922: [0] innocent, [1] error)
  double factor = 1.0;  // DQ921: the step, after / before; DQ922: item / other
  double value = 0.0;   // the stored count
  double other_value = 0.0;
  std::optional<SplitRatio> ratio;  // the clean ratio the factor is (shares after:before)
  int power = 0;                    // a power of ten the factor is
  Date window_from;                 // DQ921: the window a missing split's ex-date is in
  Date window_to;
  std::optional<double> cap_step;  // the log step of the implied market cap, with close
  std::string split_on_file;       // the split on file in the window, if any: "2:1 2018-12-06"
  bool provisional = false;
  bool in_force = false;
  bool first_reported = false;
  std::vector<Evidence> evidence;

  // DQ921: the probability that the step is not the company's (a split missing
  // from the actions, or a count stored wrong); DQ922: of the mismatch being wrong.
  [[nodiscard]] double p_not_real() const noexcept { return 1.0 - posterior[0]; }
  // DQ921: the likeliest explanation other than a real issuance.
  [[nodiscard]] ShareHypothesis explanation() const noexcept;
};

struct StatementShareAnalysis {
  std::vector<ShareFinding> findings;
};

// `actions` is the series' splits on file (nullptr: none); `have_actions` whether
// --actions was given at all, without which DQ921 judges nothing. `items` is
// --items' taxonomy, which names the share counts.
[[nodiscard]] StatementShareAnalysis analyze_statement_shares(
    const Series& series, const StatementViews& views, const StatementSettings& settings,
    const PriceSettings& price, const std::vector<LineItem>* items, const SeriesActions* actions,
    bool have_actions);

}  // namespace dorq
