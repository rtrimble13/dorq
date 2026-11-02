#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "statements/views.hpp"

namespace dorq {

// The statement value model (DQ911 to DQ913, DQ919; fundamentals plan section 4.1;
// doc/statements.md, "The value model"): each money item of each statement, read
// over time in the in-force and the as-first-reported views, and the explanations
// of each value that does not fit.

enum class ValueHypothesis : std::uint8_t { kRealChange, kBadValue, kUnitError, kSignError };
inline constexpr std::array<const char*, 4> kValueHypothesisNames{"real_change", "bad_value",
                                                                  "unit_error", "sign_error"};

// What a finding is: the value model's four hypotheses (DQ911 to DQ913, DQ919), or
// the quarterly structure's (DR-0907): a row copied from the one before (DQ914), a
// zero where the item is reported either side (DQ915), a year's quarters stored
// year to date, or a quarter holding its year (DQ916).
enum class ValueKind : std::uint8_t {
  kModel,
  kCopied,
  kAbsentZero,
  kCumulative,
  kAnnualInQuarter,
};

// What a finding covers: one value, every money item of one row, or a run of rows
// (or one item's run of values) at a power of ten.
enum class ValueScope : std::uint8_t { kValue, kRow, kEra };

inline constexpr std::size_t kNoItem = std::numeric_limits<std::size_t>::max();

struct ValueFinding {
  ValueKind kind = ValueKind::kModel;
  ValueScope scope = ValueScope::kValue;
  std::size_t row = 0;                // the row (an era's first)
  std::size_t last_row = 0;           // an era's last row; `row` otherwise
  std::size_t item = kNoItem;         // index into StatementRows::items; kNoItem for rows
  std::size_t rows = 1;               // the rows an era spans
  std::size_t items = 1;              // the items that moved together (a row or an era)
  std::array<double, 4> posterior{};  // by ValueHypothesis
  double value = 0.0;                 // the stored value
  double expected = 0.0;              // what the periods around it predict
  double repaired = 0.0;              // the value under the winning error hypothesis
  int power = 0;                      // a unit error: stored = repaired x 10^power
  double score = 0.0;                 // |residual| / scale, on the asinh scale
  bool provisional = false;           // the newest period of its line: judged with nothing after it
  bool open = false;                  // an era with one step only: which side is wrong is a guess
  // A change large for its item: at least double or half the expected value, far
  // out, on an item whose values rarely stray. DQ919 reports these alone.
  bool notable = false;
  // DQ914: the items the row shares with the period before it, of those both
  // report, and the chance of that many repeats from the series' own; DQ916: the
  // flows that fit, of those judged.
  std::size_t present = 0;
  double chance = 0.0;
  std::int32_t fiscal_year = 0;
  bool in_force = false;  // flagged in the in-force view
  bool first_reported = false;
  std::vector<Evidence> evidence;

  // The probability of an error. For the quarterly structure's findings, posterior
  // holds the innocent explanation first and the error second.
  [[nodiscard]] double p_error() const noexcept {
    return posterior[1] + posterior[2] + posterior[3];
  }
  // The most probable error hypothesis.
  [[nodiscard]] ValueHypothesis error() const noexcept;
};

struct StatementValueAnalysis {
  std::vector<ValueFinding> findings;  // ordered by row, then item
};

// `items` is --items' taxonomy, or nullptr: without it every item is judged, by its
// statement's kind (income and cash flow as flows, the balance sheet as stocks),
// and no sign is judged (DQ913 needs each item's sign convention).
[[nodiscard]] StatementValueAnalysis analyze_statement_values(const Series& series,
                                                              const StatementViews& views,
                                                              const StatementSettings& settings,
                                                              const std::vector<LineItem>* items);

}  // namespace dorq
