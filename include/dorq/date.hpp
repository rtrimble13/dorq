#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dorq {

// A calendar date, stored as days since 1970-01-01 in the proleptic Gregorian
// calendar. Dates are what daily series are keyed on; dorq has no time of day.
class Date {
 public:
  constexpr Date() = default;
  [[nodiscard]] static constexpr Date from_days(std::int32_t days) noexcept {
    Date date;
    date.days_ = days;
    return date;
  }
  // `month` is 1-12 and `day` 1-31; the caller has checked the date is valid.
  [[nodiscard]] static Date from_ymd(int year, int month, int day) noexcept;

  [[nodiscard]] constexpr std::int32_t days() const noexcept { return days_; }
  [[nodiscard]] int year() const noexcept;
  // 0 = Monday ... 6 = Sunday.
  [[nodiscard]] int weekday() const noexcept;
  // YYYY-MM-DD.
  [[nodiscard]] std::string to_string() const;

  constexpr auto operator<=>(const Date&) const noexcept = default;

 private:
  std::int32_t days_ = 0;
};

// Parses YYYY-MM-DD, YYYYMMDD or YYYY/MM/DD, optionally followed by a time of
// exactly midnight ("T00:00", "T00:00:00", " 00:00:00.000", with an optional "Z"
// or "+00:00" style offset). Any other time of day is not a daily date, and is
// rejected rather than truncated: truncating would quietly turn intraday bars
// into duplicate dates. Years 1800-2200 only.
[[nodiscard]] std::optional<Date> parse_date(std::string_view text) noexcept;

}  // namespace dorq
