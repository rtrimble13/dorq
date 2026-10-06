#include "dorq/calendar.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.hpp"
#include "io/csv.hpp"
#include "io/input_error.hpp"

namespace dorq {
namespace {

const Date kFirstDay = Date::from_ymd(1800, 1, 1);
const Date kLastDay = Date::from_ymd(2200, 12, 31);

Date easter(int year) noexcept {
  // Anonymous Gregorian algorithm, as fafnir's seed uses.
  const int a = year % 19;
  const int b = year / 100;
  const int c = year % 100;
  const int d = b / 4;
  const int e = b % 4;
  const int f = (b + 8) / 25;
  const int g = (b - f + 1) / 3;
  const int h = (19 * a + b - d - g + 15) % 30;
  const int i = c / 4;
  const int k = c % 4;
  const int l = (32 + 2 * e + 2 * i - h - k) % 7;
  const int m = (a + 11 * h + 22 * l) / 451;
  const int month = (h + l - 7 * m + 114) / 31;
  const int day = ((h + l - 7 * m + 114) % 31) + 1;
  return Date::from_ymd(year, month, day);
}

// The nth `weekday` (0 = Monday) of a month; n = -1 is the last.
Date nth_weekday(int year, int month, int weekday, int n) noexcept {
  if (n > 0) {
    const Date first = Date::from_ymd(year, month, 1);
    const int offset = (weekday - first.weekday() + 7) % 7;
    return Date::from_days(first.days() + offset + 7 * (n - 1));
  }
  const Date next_month =
      month == 12 ? Date::from_ymd(year + 1, 1, 1) : Date::from_ymd(year, month + 1, 1);
  const Date last = Date::from_days(next_month.days() - 1);
  const int offset = (last.weekday() - weekday + 7) % 7;
  return Date::from_days(last.days() - offset);
}

// NYSE observance: a Saturday holiday is taken on the Friday before, a Sunday one
// on the Monday after.
Date observed(Date date) noexcept {
  if (date.weekday() == 5) {
    return Date::from_days(date.days() - 1);
  }
  if (date.weekday() == 6) {
    return Date::from_days(date.days() + 1);
  }
  return date;
}

// Days the market closed that no annual rule predicts (fafnir's AD_HOC_CLOSURES,
// sql/migrations/0022). Earlier ones (the 1977 blackout, Hurricane Gloria in 1985)
// predate the history dorq is used on; add them if that changes.
constexpr std::array<std::array<int, 3>, 11> kUnscheduledClosures = {{
    {1994, 4, 27},   // Richard Nixon, national day of mourning
    {2001, 9, 11},   // September 11 attacks; trading resumed Monday the 17th
    {2001, 9, 12},   //
    {2001, 9, 13},   //
    {2001, 9, 14},   //
    {2004, 6, 11},   // Ronald Reagan, national day of mourning
    {2007, 1, 2},    // Gerald Ford, national day of mourning
    {2012, 10, 29},  // Hurricane Sandy
    {2012, 10, 30},  //
    {2018, 12, 5},   // George H. W. Bush, national day of mourning
    {2025, 1, 9},    // Jimmy Carter, national day of mourning
}};

}  // namespace

std::optional<CalendarKind> parse_calendar_name(std::string_view name) noexcept {
  if (iequals(name, "XNYS") || iequals(name, "NYSE") || iequals(name, "XNAS") ||
      iequals(name, "NASDAQ")) {
    return CalendarKind::kXnys;
  }
  if (iequals(name, "weekdays")) {
    return CalendarKind::kWeekdays;
  }
  if (iequals(name, "24x7") || iequals(name, "all")) {
    return CalendarKind::kAllDays;
  }
  return std::nullopt;
}

std::string_view to_string(CalendarKind kind) noexcept {
  switch (kind) {
    case CalendarKind::kXnys:
      return "XNYS";
    case CalendarKind::kWeekdays:
      return "weekdays";
    case CalendarKind::kAllDays:
      return "24x7";
  }
  return "XNYS";
}

std::vector<Date> xnys_holidays(int year) {
  std::vector<Date> out;
  // New Year's Day. When it falls on a Saturday the NYSE does not close the Friday
  // before, which is the last session of the year (2021-12-31, 2010-12-31).
  const Date new_year = Date::from_ymd(year, 1, 1);
  if (new_year.weekday() != 5) {
    out.push_back(observed(new_year));
  }
  if (year >= 1998) {
    out.push_back(nth_weekday(year, 1, 0, 3));  // Martin Luther King Jr. Day, from 1998
  }
  out.push_back(nth_weekday(year, 2, 0, 3));                // Washington's Birthday
  out.push_back(Date::from_days(easter(year).days() - 2));  // Good Friday
  out.push_back(nth_weekday(year, 5, 0, -1));               // Memorial Day
  if (year >= 2022) {
    out.push_back(observed(Date::from_ymd(year, 6, 19)));  // Juneteenth, from 2022
  }
  out.push_back(observed(Date::from_ymd(year, 7, 4)));    // Independence Day
  out.push_back(nth_weekday(year, 9, 0, 1));              // Labor Day
  out.push_back(nth_weekday(year, 11, 3, 4));             // Thanksgiving
  out.push_back(observed(Date::from_ymd(year, 12, 25)));  // Christmas
  for (const auto& [y, m, d] : kUnscheduledClosures) {
    if (y == year) {
      out.push_back(Date::from_ymd(y, m, d));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Calendar::Calendar(CalendarKind kind)
    : kind_(kind),
      open_(static_cast<std::size_t>(kLastDay.days() - kFirstDay.days() + 1), 0),
      description_(to_string(kind)) {
  for (std::size_t i = 0; i < open_.size(); ++i) {
    const Date day = Date::from_days(kFirstDay.days() + static_cast<std::int32_t>(i));
    open_[i] = kind == CalendarKind::kAllDays || day.weekday() < 5 ? 1 : 0;
  }
  if (kind == CalendarKind::kXnys) {
    for (int year = kFirstDay.year(); year <= kLastDay.year(); ++year) {
      for (const Date holiday : xnys_holidays(year)) {
        if (holiday >= kFirstDay && holiday <= kLastDay) {
          open_[static_cast<std::size_t>(index(holiday))] = 0;
        }
      }
    }
  }
  rebuild_prefix();
}

std::int32_t Calendar::index(Date date) noexcept { return date.days() - kFirstDay.days(); }

void Calendar::rebuild_prefix() {
  prefix_.assign(open_.size() + 1, 0);
  for (std::size_t i = 0; i < open_.size(); ++i) {
    prefix_[i + 1] = prefix_[i] + open_[i];
  }
}

void Calendar::apply_reference(const ReferenceCalendar& reference) {
  const Date from = std::max(reference.first, kFirstDay);
  const Date to = std::min(reference.last, kLastDay);
  for (std::int32_t d = from.days(); d <= to.days(); ++d) {
    open_[static_cast<std::size_t>(index(Date::from_days(d)))] = 0;
  }
  for (const Date session : reference.sessions) {
    if (session >= from && session <= to) {
      open_[static_cast<std::size_t>(index(session))] = 1;
    }
  }
  rebuild_prefix();
  description_ = std::string{to_string(kind_)} + ", with " + reference.source + " for " +
                 reference.first.to_string() + ".." + reference.last.to_string();
}

bool Calendar::is_session(Date date) const noexcept {
  if (date < kFirstDay || date > kLastDay) {
    return false;
  }
  return open_[static_cast<std::size_t>(index(date))] != 0;
}

int Calendar::sessions_between(Date from, Date to) const noexcept {
  from = std::max(from, kFirstDay);
  to = std::min(to, kLastDay);
  if (from > to) {
    return 0;
  }
  return prefix_[static_cast<std::size_t>(index(to)) + 1] -
         prefix_[static_cast<std::size_t>(index(from))];
}

std::optional<Date> Calendar::next_session(Date date) const noexcept {
  for (std::int32_t d = std::max(date.days() + 1, kFirstDay.days()); d <= kLastDay.days(); ++d) {
    if (open_[static_cast<std::size_t>(index(Date::from_days(d)))] != 0) {
      return Date::from_days(d);
    }
  }
  return std::nullopt;
}

std::optional<Date> Calendar::session_on_or_before(Date date) const noexcept {
  for (std::int32_t d = std::min(date.days(), kLastDay.days()); d >= kFirstDay.days(); --d) {
    if (open_[static_cast<std::size_t>(index(Date::from_days(d)))] != 0) {
      return Date::from_days(d);
    }
  }
  return std::nullopt;
}

std::array<int, 7> Calendar::sessions_by_weekday(Date from, Date to) const noexcept {
  std::array<int, 7> out{};
  from = std::max(from, kFirstDay);
  to = std::min(to, kLastDay);
  for (std::int32_t d = from.days(); d <= to.days(); ++d) {
    const Date day = Date::from_days(d);
    if (open_[static_cast<std::size_t>(index(day))] != 0) {
      ++out.at(static_cast<std::size_t>(day.weekday()));
    }
  }
  return out;
}

ReferenceCalendar read_reference_calendar(std::istream& in, const std::string& source,
                                          const std::string& exchange) {
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  const std::string_view first_line = std::string_view{text}.substr(0, text.find('\n'));
  const char delimiter = std::count(first_line.begin(), first_line.end(), '\t') >
                                 std::count(first_line.begin(), first_line.end(), ',')
                             ? '\t'
                             : ',';

  constexpr std::array<std::string_view, 5> kDate = {"date", "tradedate", "sessiondate", "session",
                                                     "day"};
  constexpr std::array<std::string_view, 3> kOpen = {"isopen", "open", "issession"};
  constexpr std::array<std::string_view, 3> kExchange = {"exchange", "exchangecode", "mic"};

  bool have_header = false;
  int date_col = -1;
  int open_col = -1;
  int exchange_col = -1;
  std::set<std::string> exchanges;
  struct Row {
    Date date;
    bool open;
    std::string exchange;
  };
  std::vector<Row> rows;

  const auto fail = [&source](std::uint32_t line, const std::string& what) {
    throw CalendarError(source + (line > 0 ? " line " + std::to_string(line) : "") + ": " + what);
  };

  CsvParser parser(delimiter);
  const CsvParser::RecordFn on_record = [&](std::span<const std::string_view> fields,
                                            std::uint32_t line) {
    if (!have_header) {
      const std::vector<std::string> names(fields.begin(), fields.end());
      date_col = find_column(names, kDate);
      open_col = find_column(names, kOpen);
      exchange_col = find_column(names, kExchange);
      if (date_col < 0) {
        fail(line, "no date column (expected date, trade_date or session_date)");
      }
      have_header = true;
      return;
    }
    const auto cell = [&fields](int col) -> std::string_view {
      return col >= 0 && static_cast<std::size_t>(col) < fields.size()
                 ? trim(fields[static_cast<std::size_t>(col)])
                 : std::string_view{};
    };
    const auto date = parse_date(cell(date_col));
    if (!date) {
      fail(line, "\"" + std::string{cell(date_col)} + "\" is not a date");
    }
    bool open = true;
    if (open_col >= 0 && !parse_bool(cell(open_col), open)) {
      fail(line, "\"" + std::string{cell(open_col)} + "\" is not true or false");
    }
    std::string row_exchange{cell(exchange_col)};
    exchanges.insert(row_exchange);
    rows.push_back({*date, open, std::move(row_exchange)});
  };
  try {
    parser.feed(text, on_record);
    parser.finish(on_record);
  } catch (const InputError& error) {
    throw CalendarError(source + ": " + error.what());
  }
  if (!have_header) {
    throw CalendarError(source + " is empty");
  }

  const std::string& wanted = exchange;
  if (!wanted.empty() && exchange_col < 0) {
    throw CalendarError(source + ": --calendar-exchange " + wanted +
                        " was given, but the file has no exchange column");
  }
  if (wanted.empty() && exchanges.size() > 1) {
    std::string list;
    for (const auto& e : exchanges) {
      list += (list.empty() ? "" : ", ") + e;
    }
    throw CalendarError(source + " holds more than one exchange (" + list +
                        "); choose one with --calendar-exchange");
  }

  ReferenceCalendar reference;
  reference.source = source;
  bool any = false;
  for (const Row& row : rows) {
    if (!wanted.empty() && !iequals(row.exchange, wanted)) {
      continue;
    }
    if (!any || row.date < reference.first) {
      reference.first = row.date;
    }
    if (!any || row.date > reference.last) {
      reference.last = row.date;
    }
    any = true;
    if (row.open) {
      reference.sessions.push_back(row.date);
    }
  }
  if (!any) {
    throw CalendarError(source + ": no rows" +
                        (wanted.empty() ? std::string{} : " for exchange " + wanted));
  }
  std::sort(reference.sessions.begin(), reference.sessions.end());
  reference.sessions.erase(std::unique(reference.sessions.begin(), reference.sessions.end()),
                           reference.sessions.end());
  return reference;
}

}  // namespace dorq
