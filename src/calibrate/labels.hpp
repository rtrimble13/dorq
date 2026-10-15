#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dorq/date.hpp"
#include "dorq/series.hpp"

namespace dorq {

// Labels: what a person decided about a stretch of a series (plan section 6;
// doc/labels.md). `dorq calibrate` fits the price model to them, and dorq-eval
// scores dorq's output against them.

enum class LabelClass : std::uint8_t {
  kDataError,   // the stored value was wrong (a repair was made)
  kContextGap,  // the data was right, but context was missing (a split added)
  kMarketFact,  // anomalous but correct: nothing should be reported
};

[[nodiscard]] std::string_view to_string(LabelClass label) noexcept;

struct Label {
  std::string series;
  Date first;
  Date last;
  LabelClass label_class = LabelClass::kDataError;
  // The code dorq should report (data errors and context gaps); empty when any
  // code will do.
  std::string expect;
  // Market facts: the code prefix a report would be wrong under ("DQ2" by
  // default). Hard negatives in dorq-synth.
  std::string codes;
  std::string kind;    // e.g. "bad_print", "hn_earnings"; free text
  std::string source;  // e.g. "operator_override"
  std::string note;
  // Dates a repair added that were not in the series as it stood (a shifted bar's
  // new date): dropped before checking, with --restore.
  std::vector<Date> remove;
  // When the label was decided, and until when it applies, in seconds since
  // 1970-01-01 UTC. A label with applies_until applies only to a run whose input
  // predates it: a repair's label describes data the repair replaced.
  std::optional<std::int64_t> decided_at;
  std::optional<std::int64_t> applies_until;
  // Checked bar by bar (true), or accepted in bulk (false); unset when not said.
  std::optional<bool> verified;
  // A cross-sectional label (series "*") about a cohort: its members' ids. It
  // matches a DQ601 row whose members overlap them.
  std::vector<std::string> members;
  std::uint32_t line = 0;  // in the labels file
};

// An ISO 8601 date or timestamp ("2026-10-14", "2026-10-14T17:15:47Z",
// "2026-10-14 17:15:47.25+02:00") as seconds since 1970-01-01 UTC; a time without
// an offset is UTC. nullopt when it is not one.
[[nodiscard]] std::optional<std::int64_t> parse_timestamp(std::string_view text);

// One JSON object per line:
//   {"series":"AKR","first":"2020-03-02","last":"2020-03-02","class":"data_error",
//    "expect":"DQ202","kind":"scale_era","source":"operator_override","note":"..."}
// `date` stands for first and last; `last` defaults to `first`. `remove` is a
// comma-separated list of dates. Blank lines are skipped. Throws InputError
// naming the line.
[[nodiscard]] std::vector<Label> read_labels(std::istream& in, const std::string& source);

// Rebuilds series as they stood before repairs: each restore row replaces the
// series' row on its date (or is added), and the labels' `remove` dates are
// dropped. Rows keep their date order.
class Restorer {
 public:
  Restorer() = default;
  Restorer(const std::vector<Label>& labels, std::vector<Series> restored);

  [[nodiscard]] bool empty() const noexcept { return rows_.empty() && removed_.empty(); }
  // Applies the restore to one series (matched by id, else by label).
  void apply(Series& series) const;

 private:
  std::unordered_map<std::string, Series> rows_;
  std::unordered_map<std::string, std::vector<Date>> removed_;
};

}  // namespace dorq
