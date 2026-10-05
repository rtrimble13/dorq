#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/text.hpp"
#include "dorq/date.hpp"
#include "dorq/number.hpp"

using dorq::Date;
using dorq::parse_date;
using dorq::parse_number;
using dorq::ParsedNumber;

TEST_CASE("dates round-trip through text") {
  for (const char* text : {"1970-01-01", "2000-02-29", "2020-08-31", "1899-12-31", "2199-12-31"}) {
    const auto date = parse_date(text);
    REQUIRE(date.has_value());
    CHECK(date->to_string() == text);
  }
  CHECK(parse_date("1970-01-01")->days() == 0);
  CHECK(parse_date("1970-01-02")->days() == 1);
  CHECK(parse_date("1969-12-31")->days() == -1);
}

TEST_CASE("date formats accepted") {
  const Date expected = Date::from_ymd(2024, 3, 5);
  CHECK(parse_date("2024-03-05") == expected);
  CHECK(parse_date("20240305") == expected);
  CHECK(parse_date("2024/03/05") == expected);
  CHECK(parse_date("2024-03-05T00:00:00") == expected);
  CHECK(parse_date("2024-03-05 00:00:00.000") == expected);
  CHECK(parse_date("2024-03-05T00:00Z") == expected);
  CHECK(parse_date("2024-03-05T00:00:00-05:00") == expected);
}

TEST_CASE("dates rejected") {
  for (const char* text : {"", "2024-3-5", "2024-02-30", "2023-02-29", "2024-13-01", "05/03/2024",
                           "2024-03-05T09:30:00", "2024-03-05 16:00", "1799-12-31", "abc",
                           "2024-03-05x", "2024-03/05"}) {
    CAPTURE(text);
    CHECK_FALSE(parse_date(text).has_value());
  }
}

TEST_CASE("weekday") {
  CHECK(parse_date("1970-01-01")->weekday() == 3);  // Thursday
  CHECK(parse_date("2024-03-04")->weekday() == 0);  // Monday
  CHECK(parse_date("2024-03-10")->weekday() == 6);  // Sunday
  CHECK(parse_date("1969-12-28")->weekday() == 6);  // Sunday, before the epoch
}

TEST_CASE("numbers parse with their written precision") {
  const ParsedNumber a = parse_number("10.500000");
  CHECK(a.status == ParsedNumber::Status::kOk);
  CHECK(a.value == 10.5);
  CHECK(a.decimals == 1);
  CHECK(a.sig_figs == 3);

  const ParsedNumber b = parse_number(" 12.345678 ");
  CHECK(b.decimals == 6);
  CHECK(b.sig_figs == 8);

  const ParsedNumber c = parse_number("0.000123");
  CHECK(c.decimals == 6);
  CHECK(c.sig_figs == 3);

  const ParsedNumber d = parse_number("1.5e-3");
  CHECK(d.value == doctest::Approx(0.0015));
  CHECK(d.decimals == 4);
  CHECK(d.sig_figs == 2);

  const ParsedNumber e = parse_number("+42");
  CHECK(e.status == ParsedNumber::Status::kOk);
  CHECK(e.value == 42.0);
  CHECK(e.decimals == 0);

  CHECK(parse_number("-3.25").value == -3.25);
  CHECK(parse_number("0").sig_figs == 0);
}

TEST_CASE("missing markers and invalid numbers") {
  for (const char* text : {"", "  ", "NA", "n/a", "NaN", "null", "None", ".", "#N/A"}) {
    CAPTURE(text);
    CHECK(parse_number(text).status == ParsedNumber::Status::kMissing);
  }
  for (const char* text :
       {"abc", "1,234", "inf", "-inf", "1e999", "12.3.4", "--1", "+-1", "1 2", "0x10"}) {
    CAPTURE(text);
    CHECK(parse_number(text).status == ParsedNumber::Status::kInvalid);
  }
}

TEST_CASE("format_number is the shortest round-trip text") {
  CHECK(dorq::format_number(10.5) == "10.5");
  CHECK(dorq::format_number(100.0) == "100");
  CHECK(dorq::format_number(0.1) == "0.1");
  CHECK(dorq::format_number(-2.25) == "-2.25");
  CHECK(dorq::format_number(std::nan("")) == "nan");
}

#include "dorq/frequency.hpp"

namespace {

std::vector<dorq::Date> every(int step_days, int count, const char* from = "2020-01-06") {
  std::vector<dorq::Date> out;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    out.push_back(dorq::Date::from_days(parse_date(from)->days() + i * step_days));
  }
  return out;
}

}  // namespace

TEST_CASE("frequency inference") {
  using dorq::Frequency;
  using dorq::infer_frequency;
  // Weekdays: gaps of 1 and 3.
  std::vector<dorq::Date> weekdays;
  for (int i = 0; i < 100; ++i) {
    const auto d = dorq::Date::from_days(parse_date("2020-01-06")->days() + i);
    if (d.weekday() < 5) {
      weekdays.push_back(d);
    }
  }
  CHECK(infer_frequency(weekdays) == Frequency::kDaily);
  // A thin daily name: every third weekday. Median gap 4-5 days; lower quartile
  // still short.
  std::vector<dorq::Date> thin;
  for (std::size_t i = 0; i < weekdays.size(); i += (i % 2 == 0 ? 1U : 4U)) {
    thin.push_back(weekdays[i]);
  }
  CHECK(infer_frequency(thin) == Frequency::kDaily);
  CHECK(infer_frequency(every(7, 50)) == Frequency::kWeekly);
  std::vector<dorq::Date> monthly;
  std::vector<dorq::Date> quarterly;
  std::vector<dorq::Date> annual;
  for (int i = 0; i < 24; ++i) {
    monthly.push_back(dorq::Date::from_ymd(2020 + i / 12, i % 12 + 1, 1));
    quarterly.push_back(dorq::Date::from_ymd(2010 + i / 4, (i % 4) * 3 + 1, 15));
    annual.push_back(dorq::Date::from_ymd(1990 + i, 12, 31));
  }
  CHECK(infer_frequency(monthly) == Frequency::kMonthly);
  CHECK(infer_frequency(quarterly) == Frequency::kQuarterly);
  CHECK(infer_frequency(annual) == Frequency::kAnnual);
  CHECK(infer_frequency(every(50, 20)) == Frequency::kIrregular);
  CHECK(infer_frequency(every(1, 2)) == Frequency::kIrregular);  // too few
  CHECK(dorq::parse_frequency("Monthly") == Frequency::kMonthly);
  CHECK_FALSE(dorq::parse_frequency("fortnightly").has_value());
}

TEST_CASE("counts with commas, whatever their size") {
  CHECK(dorq::with_commas(1234567) == "1,234,567");
  CHECK(dorq::with_commas(-1234) == "-1,234");
  CHECK(dorq::with_commas(std::numeric_limits<long long>::min()) == "-9,223,372,036,854,775,808");
  CHECK(dorq::whole_number(605833700.4) == "605,833,700");
  CHECK(dorq::whole_number(1e30) == dorq::format_number(1e30));
  CHECK(dorq::whole_number(std::numeric_limits<double>::infinity()) ==
        dorq::format_number(std::numeric_limits<double>::infinity()));
}
