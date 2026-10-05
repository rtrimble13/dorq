#include <array>
#include <sstream>
#include <string>

#include <doctest/doctest.h>

#include "dorq/calendar.hpp"

using dorq::Calendar;
using dorq::CalendarKind;
using dorq::Date;
using dorq::parse_date;

namespace {

Date d(const char* text) { return *parse_date(text); }

dorq::ReferenceCalendar reference(const std::string& text, const std::string& exchange = "") {
  std::istringstream in(text);
  return dorq::read_reference_calendar(in, "ref.csv", exchange);
}

}  // namespace

TEST_CASE("XNYS sessions per year match fafnir's calendar, with MLK Day from 1998") {
  // Generated from fafnir's src/fafnir/db/seed.py, adding back MLK Day before 1998:
  // the NYSE first closed for it in 1998, and fafnir's own playbook treats the
  // 1990-1997 MLK bars as real.
  constexpr std::array<int, 46> kExpected = {
      253, 253, 254, 253, 252, 252, 254, 253, 252, 252, 252, 248, 252, 252, 252, 252,
      251, 251, 253, 252, 252, 252, 250, 252, 252, 252, 252, 251, 251, 252, 253, 252,
      251, 250, 252, 250, 251, 251, 251, 251, 251, 251, 252, 251, 250, 251};
  const Calendar xnys(CalendarKind::kXnys);
  for (int year = 1990; year <= 2035; ++year) {
    CAPTURE(year);
    CHECK(xnys.sessions_between(Date::from_ymd(year, 1, 1), Date::from_ymd(year, 12, 31)) ==
          kExpected.at(static_cast<std::size_t>(year - 1990)));
  }
}

TEST_CASE("XNYS holidays and observance") {
  const Calendar xnys(CalendarKind::kXnys);
  for (const char* closed :
       {"2024-01-01", "2024-01-15", "2024-02-19", "2024-03-29", "2024-05-27",
        "2024-06-19", "2024-07-04", "2024-09-02", "2024-11-28", "2024-12-25",
        "2021-12-24",  // Christmas on a Saturday: the Friday before
        "2020-07-03",  // July 4th on a Saturday
        "2022-06-20",  // Juneteenth on a Sunday: the Monday after
        "2017-01-02",  // New Year's Day on a Sunday
        "1998-01-19",  // the first MLK Day closure
        "2001-09-11", "2012-10-29", "2012-10-30", "2025-01-09", "2024-03-09"}) {  // a Saturday
    CAPTURE(closed);
    CHECK_FALSE(xnys.is_session(d(closed)));
  }
  for (const char* open : {"2021-12-31",  // New Year's Day 2022 on a Saturday: no closure
                           "1997-01-20",  // MLK Day before 1998: open
                           "2001-09-17", "2024-03-28", "2024-07-05", "2021-06-18"}) {
    CAPTURE(open);
    CHECK(xnys.is_session(d(open)));
  }
}

TEST_CASE("weekdays and 24x7") {
  const Calendar weekdays(CalendarKind::kWeekdays);
  const Calendar all(CalendarKind::kAllDays);
  CHECK(weekdays.is_session(d("2024-12-25")));
  CHECK_FALSE(weekdays.is_session(d("2024-03-09")));
  CHECK(all.is_session(d("2024-03-09")));
  CHECK(all.sessions_between(d("2024-01-01"), d("2024-12-31")) == 366);
  CHECK(weekdays.sessions_by_weekday(d("2024-03-04"), d("2024-03-17")) ==
        std::array<int, 7>{2, 2, 2, 2, 2, 0, 0});
}

TEST_CASE("navigation") {
  const Calendar xnys;
  CHECK(xnys.next_session(d("2024-03-28"))->to_string() == "2024-04-01");  // over Good Friday
  CHECK(xnys.session_on_or_before(d("2024-03-31"))->to_string() == "2024-03-28");
  CHECK(xnys.session_on_or_before(d("2024-03-28"))->to_string() == "2024-03-28");
  CHECK(xnys.sessions_between(d("2024-03-29"), d("2024-03-28")) == 0);
  CHECK(dorq::parse_calendar_name("nyse") == CalendarKind::kXnys);
  CHECK_FALSE(dorq::parse_calendar_name("LSE").has_value());
}

TEST_CASE("a reference file overrides the built-in calendar within its span only") {
  // fafnir's shape: open days listed, a closure marked is_open = false.
  const auto ref = reference(
      "exchange_code,trade_date,is_open\n"
      "NASDAQ,2024-01-02,t\n"
      "NASDAQ,2024-01-03,f\n"
      "NASDAQ,2024-01-06,t\n"
      "NASDAQ,2024-01-08,t\n");
  CHECK(ref.first.to_string() == "2024-01-02");
  CHECK(ref.last.to_string() == "2024-01-08");
  Calendar calendar;
  calendar.apply_reference(ref);
  CHECK(calendar.is_session(d("2024-01-02")));
  CHECK_FALSE(calendar.is_session(d("2024-01-03")));  // marked closed
  CHECK_FALSE(calendar.is_session(d("2024-01-04")));  // within the span, not listed
  CHECK(calendar.is_session(d("2024-01-06")));        // a Saturday the file says was open
  CHECK(calendar.is_session(d("2023-12-29")));        // before the span: built-in XNYS
  CHECK(calendar.is_session(d("2024-01-09")));        // after the span: built-in XNYS
  CHECK(calendar.description() == "XNYS, with ref.csv for 2024-01-02..2024-01-08");
}

TEST_CASE("reference files: a plain list, exchanges, and errors") {
  const auto plain = reference("date\n2024-01-03\n2024-01-02\n");
  CHECK(plain.sessions.size() == 2);
  CHECK(plain.sessions.front().to_string() == "2024-01-02");

  const std::string two = "exchange,date\nNYSE,2024-01-02\nLSE,2024-01-03\n";
  CHECK_THROWS_AS(reference(two), dorq::CalendarError);
  CHECK(reference(two, "lse").sessions.front().to_string() == "2024-01-03");
  CHECK_THROWS_AS(reference(two, "XETR"), dorq::CalendarError);
  CHECK_THROWS_AS(reference("date\n2024-01-02\n", "NYSE"), dorq::CalendarError);
  CHECK_THROWS_AS(reference("when\n2024-01-02\n"), dorq::CalendarError);
  CHECK_THROWS_AS(reference("date\nsoon\n"), dorq::CalendarError);
  CHECK_THROWS_AS(reference("date,is_open\n2024-01-02,maybe\n"), dorq::CalendarError);
  CHECK_THROWS_AS(reference(""), dorq::CalendarError);
}
