#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dorq/series.hpp"

namespace dorq {

enum class KindOption : std::uint8_t { kAuto, kOhlcv, kPoint };

inline constexpr std::size_t kFieldCount = 9;  // the number of Field values

// --columns: explicit column names for fields, e.g. {Field::kDate, "trade_date"}.
// A point series' value column is given as `value` and maps to Field::kClose.
using ColumnOverrides = std::vector<std::pair<Field, std::string>>;

// "auto", "ohlcv" or "point".
[[nodiscard]] std::optional<KindOption> parse_kind_option(std::string_view text) noexcept;

// A field by the name --columns and the config file use for it: series, label,
// date, open, high, low, close, value (the same as close), volume, vwap.
[[nodiscard]] std::optional<Field> parse_field_name(std::string_view name);

// Parses "date=trade_date,series=security_id,value=DGS10". Returns an error
// message for an unknown field name or a malformed entry.
[[nodiscard]] std::optional<std::string> parse_column_overrides(const std::string& text,
                                                                ColumnOverrides& out);

// Which input column feeds each field (-1 when none), and the series kind.
struct ColumnMapping {
  SeriesKind kind = SeriesKind::kOhlcv;
  std::array<int, kFieldCount> column{};  // indexed by Field

  [[nodiscard]] int index(Field field) const noexcept {
    return column.at(static_cast<std::size_t>(field));
  }
  [[nodiscard]] bool has(Field field) const noexcept { return index(field) >= 0; }
};

// Maps column names to fields: explicit overrides first, then case-insensitive
// aliases ("Date", "trade_date", "Adj Close", "vol", ...), then the series kind.
// Throws InputError when there is no date column, or no price or value column, or
// when an explicit kind's columns are missing. `source` names the input in errors.
[[nodiscard]] ColumnMapping map_columns(std::span<const std::string> names,
                                        const ColumnOverrides& overrides, KindOption kind,
                                        const std::string& source);

}  // namespace dorq
