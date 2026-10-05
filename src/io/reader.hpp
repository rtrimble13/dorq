#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dorq/series.hpp"
#include "io/columns.hpp"

namespace dorq {

enum class InputFormat : std::uint8_t { kAuto, kCsv, kTsv, kJsonl, kJson };

[[nodiscard]] std::optional<InputFormat> parse_input_format(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(InputFormat format) noexcept;

// How rows become series.
enum class Grouping : std::uint8_t {
  // Hold every series until all input is read. Rows of one series may be spread
  // anywhere in the input.
  kBuffer,
  // Hand each series on as soon as the next one starts, so memory holds one series
  // at a time. The input must be grouped by series (every row of a series
  // together, as `ORDER BY series, date` gives); a series that reappears is an
  // InputError.
  kStream,
};

struct ReadOptions {
  InputFormat format = InputFormat::kAuto;
  KindOption kind = KindOption::kAuto;
  ColumnOverrides columns;
};

// Turns rows into Series, grouping by the series column.
class SeriesAssembler {
 public:
  using SeriesFn = std::function<void(Series&&)>;

  SeriesAssembler(Grouping grouping, SeriesFn on_series);

  // Readers call these.
  void begin_source(const std::string& source, const std::string& default_id,
                    const ColumnMapping& mapping);
  // `cells` holds one entry per input column; a short row's missing cells read as
  // empty.
  void add_row(std::span<const std::string_view> cells, std::uint32_t line);

  // Hands on every series not yet handed on. Call once, after the last source.
  void finish();

  [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
  [[nodiscard]] std::size_t series_count() const noexcept { return series_count_; }

 private:
  Series& series_for(std::string_view id, std::uint32_t line);
  void emit(Series& series);

  Grouping grouping_;
  SeriesFn on_series_;
  std::string source_;
  std::string default_id_;
  ColumnMapping mapping_;

  // kStream
  std::optional<Series> current_;
  std::unordered_set<std::string> emitted_;
  // kBuffer
  std::vector<Series> buffered_;
  std::unordered_map<std::string, std::size_t> index_;

  std::size_t rows_ = 0;
  std::size_t series_count_ = 0;
};

// Reads one input (a file, or stdin) into `out`. `extension` (".csv", ...) helps
// detect the format when options.format is kAuto; otherwise the content is
// sniffed. `default_id` names the series when there is no series column.
// Throws InputError when the input as a whole cannot be read.
void read_input(std::istream& in, const std::string& source, const std::string& default_id,
                std::string_view extension, const ReadOptions& options, SeriesAssembler& out);

}  // namespace dorq
