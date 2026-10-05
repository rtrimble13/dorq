#include "io/columns.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.hpp"
#include "io/input_error.hpp"

namespace dorq {
namespace {

// Lower case, without spaces, underscores, hyphens or dots: "Adj Close" and
// "adj_close" are both "adjclose".
std::string normalize(std::string_view name) {
  std::string out;
  for (const char ch : trim(name)) {
    if (ch != ' ' && ch != '_' && ch != '-' && ch != '.') {
      out.push_back(ascii_lower(ch));
    }
  }
  return out;
}

constexpr std::array<std::string_view, 10> kDateAliases = {
    "date", "tradedate", "timestamp",       "datetime", "time",
    "day",  "period",    "observationdate", "asof",     "asofdate"};
constexpr std::array<std::string_view, 7> kSeriesAliases = {
    "series", "seriesid", "securityid", "sid", "id", "symbol", "ticker"};
constexpr std::array<std::string_view, 3> kOpenAliases = {"open", "o", "openprice"};
constexpr std::array<std::string_view, 3> kHighAliases = {"high", "h", "highprice"};
constexpr std::array<std::string_view, 3> kLowAliases = {"low", "l", "lowprice"};
constexpr std::array<std::string_view, 10> kCloseAliases = {
    "close",         "c",     "closeprice", "last",       "price", "adjclose",
    "adjustedclose", "value", "val",        "observation"};
constexpr std::array<std::string_view, 4> kVolumeAliases = {"volume", "vol", "v",
                                                            "unadjustedvolume"};
constexpr std::array<std::string_view, 1> kVwapAliases = {"vwap"};
constexpr std::array<std::string_view, 4> kLabelAliases = {"label", "symbol", "ticker", "name"};

struct Aliases {
  Field field;
  std::span<const std::string_view> names;  // normalized, most preferred first
};

// Resolved in this order. `series` before `label`, so a lone `symbol` column
// names the series and a `symbol` beside `security_id` labels it. `close` before
// `value`: a file with both is OHLC and its value column is someone else's.
constexpr std::array<Aliases, 9> kAliases = {{
    {Field::kDate, kDateAliases},
    {Field::kSeries, kSeriesAliases},
    {Field::kOpen, kOpenAliases},
    {Field::kHigh, kHighAliases},
    {Field::kLow, kLowAliases},
    {Field::kClose, kCloseAliases},
    {Field::kVolume, kVolumeAliases},
    {Field::kVwap, kVwapAliases},
    {Field::kLabel, kLabelAliases},
}};

}  // namespace

std::optional<KindOption> parse_kind_option(std::string_view text) noexcept {
  if (iequals(text, "auto")) {
    return KindOption::kAuto;
  }
  if (iequals(text, "ohlcv")) {
    return KindOption::kOhlcv;
  }
  if (iequals(text, "point")) {
    return KindOption::kPoint;
  }
  return std::nullopt;
}

std::optional<Field> parse_field_name(std::string_view name) {
  const std::string n = normalize(name);
  if (n == "series") {
    return Field::kSeries;
  }
  if (n == "label") {
    return Field::kLabel;
  }
  if (n == "date") {
    return Field::kDate;
  }
  if (n == "open") {
    return Field::kOpen;
  }
  if (n == "high") {
    return Field::kHigh;
  }
  if (n == "low") {
    return Field::kLow;
  }
  if (n == "close" || n == "value") {
    return Field::kClose;
  }
  if (n == "volume") {
    return Field::kVolume;
  }
  if (n == "vwap") {
    return Field::kVwap;
  }
  return std::nullopt;
}

namespace {

std::string join_names(std::span<const std::string> names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += '"' + name + '"';
  }
  return out.empty() ? "(none)" : out;
}

}  // namespace

std::optional<std::string> parse_column_overrides(const std::string& text, ColumnOverrides& out) {
  std::string_view rest = text;
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const std::string_view entry = trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    if (entry.empty()) {
      continue;
    }
    const auto eq = entry.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == entry.size()) {
      return "expected FIELD=COLUMN, got \"" + std::string{entry} + "\"";
    }
    const auto field = parse_field_name(entry.substr(0, eq));
    if (!field) {
      return "unknown field \"" + std::string{entry.substr(0, eq)} +
             "\" (fields: series, label, date, open, high, low, close, value, volume, vwap)";
    }
    out.emplace_back(*field, std::string{trim(entry.substr(eq + 1))});
  }
  return std::nullopt;
}

ColumnMapping map_columns(std::span<const std::string> names, const ColumnOverrides& overrides,
                          KindOption kind, const std::string& source) {
  ColumnMapping mapping;
  mapping.column.fill(-1);
  std::vector<bool> used(names.size(), false);

  const auto set = [&](Field field, std::size_t column) {
    mapping.column.at(static_cast<std::size_t>(field)) = static_cast<int>(column);
    used[column] = true;
  };

  for (const auto& [field, wanted] : overrides) {
    bool found = false;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (iequals(trim(names[i]), trim(wanted))) {
        set(field, i);
        found = true;
        break;
      }
    }
    if (!found) {
      std::string message = source;
      message += ": --columns names \"";
      message += wanted;
      message += "\" for ";
      message += field_name(field, SeriesKind::kOhlcv);
      message += ", but there is no such column; columns are ";
      message += join_names(names);
      throw InputError(message);
    }
  }

  std::vector<std::string> normalized;
  normalized.reserve(names.size());
  for (const auto& name : names) {
    normalized.push_back(normalize(name));
  }
  const auto pick = [&](Field field, std::span<const std::string_view> aliases) {
    if (mapping.has(field)) {
      return;
    }
    for (const std::string_view alias : aliases) {
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (!used[i] && normalized[i] == alias) {
          set(field, i);
          return;
        }
      }
    }
  };
  for (const auto& entry : kAliases) {
    pick(entry.field, entry.names);
  }

  if (!mapping.has(Field::kDate)) {
    throw InputError(source + ": no date column; columns are " + join_names(names) +
                     " (name one with --columns date=COLUMN)");
  }

  const bool any_ohl =
      mapping.has(Field::kOpen) || mapping.has(Field::kHigh) || mapping.has(Field::kLow);
  const bool all_ohlc = any_ohl && mapping.has(Field::kOpen) && mapping.has(Field::kHigh) &&
                        mapping.has(Field::kLow) && mapping.has(Field::kClose);

  // A point series with no recognised value column: a FRED download is
  // "observation_date,DGS10", so the one column left over is the value.
  if (!mapping.has(Field::kClose) && !any_ohl && kind != KindOption::kOhlcv) {
    int leftover = -1;
    int count = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (!used[i]) {
        leftover = static_cast<int>(i);
        ++count;
      }
    }
    if (count == 1) {
      set(Field::kClose, static_cast<std::size_t>(leftover));
    }
  }

  const auto missing_ohlc = [&] {
    std::string out;
    for (const Field f : {Field::kOpen, Field::kHigh, Field::kLow, Field::kClose}) {
      if (!mapping.has(f)) {
        out += out.empty() ? "" : ", ";
        out += field_name(f, SeriesKind::kOhlcv);
      }
    }
    return out;
  };

  switch (kind) {
    case KindOption::kOhlcv:
      if (!all_ohlc) {
        throw InputError(source + ": --kind ohlcv needs open, high, low and close; missing " +
                         missing_ohlc() + "; columns are " + join_names(names));
      }
      mapping.kind = SeriesKind::kOhlcv;
      break;
    case KindOption::kPoint:
      if (!mapping.has(Field::kClose)) {
        throw InputError(source + ": --kind point needs a value column; columns are " +
                         join_names(names) + " (name one with --columns value=COLUMN)");
      }
      mapping.kind = SeriesKind::kPoint;
      mapping.column.at(static_cast<std::size_t>(Field::kOpen)) = -1;
      mapping.column.at(static_cast<std::size_t>(Field::kHigh)) = -1;
      mapping.column.at(static_cast<std::size_t>(Field::kLow)) = -1;
      break;
    case KindOption::kAuto:
      if (any_ohl) {
        if (!all_ohlc) {
          throw InputError(source + ": bars need open, high, low and close; missing " +
                           missing_ohlc() + "; columns are " + join_names(names) +
                           " (use --kind point for a single-value series)");
        }
        mapping.kind = SeriesKind::kOhlcv;
      } else if (mapping.has(Field::kClose)) {
        mapping.kind = SeriesKind::kPoint;
      } else {
        throw InputError(source + ": no price or value column; columns are " + join_names(names) +
                         " (name one with --columns close=COLUMN or value=COLUMN)");
      }
      break;
  }
  return mapping;
}

}  // namespace dorq
