#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/date.hpp"

namespace dorq {

// The built-in calendars.
enum class CalendarKind : std::uint8_t {
  kXnys,      // the New York Stock Exchange (also US equity markets generally)
  kWeekdays,  // Monday to Friday
  kAllDays,   // every day (24x7)
};

[[nodiscard]] std::optional<CalendarKind> parse_calendar_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(CalendarKind kind) noexcept;

// A reference calendar file that cannot be used. Maps to exit status 2.
class CalendarError : public std::runtime_error {
 public:
  explicit CalendarError(const std::string& message) : std::runtime_error(message) {}
};

// The sessions a reference file lists, and the span it speaks for.
struct ReferenceCalendar {
  std::string source;
  Date first;
  Date last;
  std::vector<Date> sessions;  // sorted, unique
};

// Reads a reference calendar: CSV or TSV with a header and a date column (date,
// trade_date, session_date, day), optionally an is_open column (true/false, t/f,
// 1/0, yes/no) and an exchange column (exchange, exchange_code). Within the span
// from its first to its last date, a date is a session when it is listed and not
// marked closed. That fits fafnir's ref.trading_calendar, which lists open days and
// marks closures is_open = false. `exchange` picks rows when the file holds more
// than one exchange; it is required then. Throws CalendarError.
[[nodiscard]] ReferenceCalendar read_reference_calendar(std::istream& in, const std::string& source,
                                                        const std::string& exchange);

// Which days are sessions. Built in, optionally overridden by a reference file
// within the file's span; outside it the built-in calendar still answers. Lookups
// are O(1): the calendar is a table of every day dorq can parse (1800-2200).
class Calendar {
 public:
  explicit Calendar(CalendarKind kind = CalendarKind::kXnys);

  void apply_reference(const ReferenceCalendar& reference);

  [[nodiscard]] bool is_session(Date date) const noexcept;
  // Sessions in [from, to], inclusive; 0 when from > to.
  [[nodiscard]] int sessions_between(Date from, Date to) const noexcept;
  // The first session strictly after `date`, or nullopt past the table.
  [[nodiscard]] std::optional<Date> next_session(Date date) const noexcept;
  // The last session on or before `date`, or nullopt before the table.
  [[nodiscard]] std::optional<Date> session_on_or_before(Date date) const noexcept;
  // Sessions in [from, to] by weekday (0 = Monday).
  [[nodiscard]] std::array<int, 7> sessions_by_weekday(Date from, Date to) const noexcept;

  // "XNYS", or "XNYS, with sessions.csv for 1990-01-02..2035-12-31".
  [[nodiscard]] const std::string& description() const noexcept { return description_; }
  [[nodiscard]] CalendarKind kind() const noexcept { return kind_; }

 private:
  [[nodiscard]] static std::int32_t index(Date date) noexcept;
  void rebuild_prefix();

  CalendarKind kind_;
  std::vector<std::uint8_t> open_;    // one per day from 1800-01-01
  std::vector<std::int32_t> prefix_;  // sessions before each day
  std::string description_;
};

// NYSE full-day closures in `year`: the holiday rules (with NYSE's observance and
// the years each holiday began) and the unscheduled closures dorq knows of.
[[nodiscard]] std::vector<Date> xnys_holidays(int year);

}  // namespace dorq
