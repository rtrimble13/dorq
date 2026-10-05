#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dorq/date.hpp"

namespace dorq {

enum class Severity : std::uint8_t { kInfo = 0, kWarn = 1, kError = 2 };

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;
[[nodiscard]] std::optional<Severity> parse_severity(std::string_view text) noexcept;

// What kind of problem a violation describes (plan section 1.1).
enum class Classification : std::uint8_t {
  kDataError,   // the stored value is wrong
  kMarketFact,  // anomalous but correct
  kContextGap,  // correct, but context needed to explain it is missing
};

[[nodiscard]] std::string_view to_string(Classification classification) noexcept;

// The input series a check is applicable to.
enum class Applies : std::uint8_t { kAny, kOhlcvOnly };

// A check's identity and documentation (doc/adr/0003-check-codes.md).
struct CheckInfo {
  std::string_view code;     // DQ101
  std::string_view name;     // ohlc-bounds
  std::string_view summary;  // one line, for list-checks
  Severity default_severity = Severity::kError;
  Applies applies = Applies::kAny;
};

// One value in a violation's `detail` object. Keys keep the order checks add them
// in, so output is stable.
using DetailValue = std::variant<std::string, double, std::int64_t, bool>;

struct DetailField {
  std::string key;
  DetailValue value;
};

struct Violation {
  const CheckInfo* check = nullptr;
  Severity severity = Severity::kError;
  Classification classification = Classification::kDataError;
  // Probability that this is a data error. Deterministic checks report 1.
  double p_error = 1.0;
  std::optional<Date> date;      // absent only for a row whose date was unusable
  std::optional<Date> end_date;  // for a violation spanning a run of dates
  std::uint32_t line = 0;        // the source line, when one row is at fault
  std::string message;
  std::vector<DetailField> detail;
};

}  // namespace dorq
