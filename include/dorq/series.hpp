#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/date.hpp"
#include "dorq/statement.hpp"

namespace dorq {

enum class SeriesKind : std::uint8_t {
  kOhlcv,       // bars: open, high, low, close, and usually volume
  kPoint,       // one value per date: a rate, a NAV, an index level
  kStatements,  // financial statements: versions of periods (doc/statements.md)
};

[[nodiscard]] std::string_view to_string(SeriesKind kind) noexcept;

// The fields a row can carry. A point series stores its value in `close`, and
// names it `value` in everything it reports. A statements row's date is its
// fiscal date; the fields after kVwap are statements' only, and kItem stands for
// any line item (FieldIssue::column names which).
enum class Field : std::uint8_t {
  kSeries,
  kLabel,
  kDate,
  kOpen,
  kHigh,
  kLow,
  kClose,
  kVolume,
  kVwap,
  kStatement,
  kPeriodType,
  kFiscalYear,
  kFiscalPeriod,
  kVersion,
  kKnownFrom,
  kKnownTo,
  kKnowledgeSource,
  kFilingDate,
  kLoadedOn,
  kCurrency,
  kItem,
};

[[nodiscard]] std::string_view field_name(Field field, SeriesKind kind) noexcept;

// A field the reader could not turn into a value. Kept, not dropped: these are
// what DQ104 reports.
struct FieldIssue {
  enum class Kind : std::uint8_t { kMissing, kInvalid };
  std::uint32_t line = 0;  // 1-based line (CSV) or record (JSON) in the source
  Field field = Field::kDate;
  Kind kind = Kind::kMissing;
  std::string text;  // the raw text, abbreviated
  // The row's date when it had one; a row whose date is unusable is not in the
  // series at all, and is known only by its line.
  bool has_date = false;
  Date date;
  std::string column;      // the line item's column, when field is kItem
  bool skips_row = false;  // the row could not be used, and is not in the series
  // Statements: the row's period and version, as far as they could be read (the
  // statement and period type at least).
  std::optional<StatementPeriod> period;
  // Statements: the newest of the row's known_from and loaded_on, when it has
  // them (what --since judges a statement finding by).
  std::optional<Date> known;
};

// One series, as columns. Rows are sorted by date (ties keep input order) by the
// time a check sees them. Missing numbers are NaN.
struct Series {
  std::string id;      // the series column, or the file's stem when there is none
  std::string label;   // the label column (e.g. a ticker), or empty
  std::string source;  // the file it came from
  SeriesKind kind = SeriesKind::kOhlcv;
  bool has_volume = false;
  bool has_vwap = false;

  std::vector<Date> date;
  std::vector<double> open;    // OHLCV only
  std::vector<double> high;    // OHLCV only
  std::vector<double> low;     // OHLCV only
  std::vector<double> close;   // the value of a point series
  std::vector<double> volume;  // when has_volume
  std::vector<double> vwap;    // when has_vwap
  // Precision of `close` as written (see ParsedNumber).
  std::vector<std::uint8_t> close_decimals;
  std::vector<std::uint8_t> close_sig_figs;
  std::vector<std::uint32_t> line;
  // A statements series' own columns; empty for the other kinds.
  StatementRows statements;

  std::vector<FieldIssue> issues;
  std::size_t skipped_rows = 0;  // rows read but not used: each has an issue that skips it

  [[nodiscard]] std::size_t size() const noexcept { return date.size(); }
  // The name to show people: the label when there is one.
  [[nodiscard]] const std::string& display_name() const noexcept {
    return label.empty() ? id : label;
  }
};

// Sorts rows by date, keeping input order among equal dates. A statements series
// is sorted by fiscal date, then statement, period type and version.
void sort_by_date(Series& series);

}  // namespace dorq
