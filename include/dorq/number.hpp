#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dorq {

// What a numeric field held, as written.
struct ParsedNumber {
  enum class Status : std::uint8_t {
    kOk,       // a finite number
    kMissing,  // empty, or a missing-value marker: NA, N/A, NaN, null, None, "."
    kInvalid,  // anything else: text, "1,234", inf
  };
  Status status = Status::kMissing;
  double value = 0.0;
  // Decimal places and significant figures as written, ignoring trailing zeros:
  // "10.500000" has 1 decimal place and 3 significant figures. Trailing zeros are
  // ignored because exporters pad to the column's scale (Postgres prints a
  // NUMERIC(20,6) as "10.500000"); the precision that carries information is the
  // precision the value actually needs. Capped at 255.
  std::uint8_t decimals = 0;
  std::uint8_t sig_figs = 0;
};

// Parses one field. Leading and trailing spaces are ignored; a leading '+' is
// accepted. Missing-value markers are matched case-insensitively.
[[nodiscard]] ParsedNumber parse_number(std::string_view text) noexcept;

// The shortest decimal text that reads back as exactly `value`, as std::to_chars
// writes it. Non-finite values come out as "nan", "inf" or "-inf".
[[nodiscard]] std::string format_number(double value);

}  // namespace dorq
