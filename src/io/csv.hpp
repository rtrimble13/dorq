#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dorq {

// An incremental RFC 4180 parser: feed it chunks of any size, in order, and it
// calls back once per record. Quoted fields may contain delimiters, doubled
// quotes and newlines. CRLF and LF line ends are both accepted. Blank lines are
// skipped. An unterminated quoted field is an InputError at finish().
//
// The parser is lenient where leniency loses nothing: a quote in the middle of an
// unquoted field, or text after a closing quote, is kept as written.
class CsvParser {
 public:
  // `fields` are valid only for the duration of the call. `line` is the 1-based
  // physical line the record starts on.
  using RecordFn =
      std::function<void(std::span<const std::string_view> fields, std::uint32_t line)>;

  explicit CsvParser(char delimiter) noexcept : delimiter_(delimiter) {}

  void feed(std::string_view chunk, const RecordFn& on_record);
  void finish(const RecordFn& on_record);

 private:
  enum class State : std::uint8_t { kFieldStart, kUnquoted, kQuoted, kQuoteInQuoted };

  void end_field();
  void end_record(const RecordFn& on_record);

  char delimiter_;
  State state_ = State::kFieldStart;
  std::string buffer_;             // all field text of the current record
  std::vector<std::size_t> ends_;  // end offset of each completed field
  std::vector<std::string_view> views_;
  std::uint32_t line_ = 1;         // current physical line
  std::uint32_t record_line_ = 1;  // line the current record started on
  bool field_was_quoted_ = false;
  bool record_has_content_ = false;
};

}  // namespace dorq
