#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "checks/check.hpp"
#include "checks/integrity.hpp"
#include "config/config.hpp"
#include "io/reader.hpp"

namespace {

dorq::Series one_series(const std::string& csv) {
  std::vector<dorq::Series> out;
  dorq::SeriesAssembler assembler(dorq::Grouping::kBuffer,
                                  [&](dorq::Series&& s) { out.push_back(std::move(s)); });
  std::istringstream in(csv);
  dorq::read_input(in, "test", "s", ".csv", {}, assembler);
  assembler.finish();
  REQUIRE(out.size() == 1);
  return std::move(out.front());
}

std::vector<dorq::Violation> run_check(const dorq::Check& check, const dorq::Series& series,
                                       const dorq::IntegritySettings& settings = {}) {
  static const dorq::Calendar kCalendar;
  static const dorq::CoverageSettings kCoverage;
  static const dorq::SeverityThresholds kThresholds;
  static const dorq::PriceSettings kPrice;
  std::vector<dorq::Violation> out;
  const dorq::SeriesContext context{
      .series = series,
      .integrity = settings,
      .coverage = kCoverage,
      .calendar = kCalendar,
      .thresholds = kThresholds,
      .price = kPrice,
      .frequency = dorq::infer_frequency(series.date),
  };
  check.run(context, out);
  return out;
}

std::string bars(const std::vector<std::string>& rows) {
  std::string out = "date,open,high,low,close,volume\n";
  for (const auto& row : rows) {
    out += row + "\n";
  }
  return out;
}

}  // namespace

TEST_CASE("the registry: codes and names unique, in code order, every one documented") {
  const auto checks = dorq::all_checks();
  REQUIRE_FALSE(checks.empty());
  for (std::size_t i = 0; i < checks.size(); ++i) {
    const dorq::CheckInfo& info = checks[i]->info();
    CAPTURE(info.code);
    if (i > 0) {
      CHECK(checks[i - 1]->info().code < info.code);
    }
    CHECK(dorq::find_check(info.code) == checks[i]);
    CHECK(dorq::find_check(info.name) == checks[i]);
    const std::string heading = "# " + std::string{info.code} + " " + std::string{info.name} + "\n";
    CHECK(dorq::check_doc(info.code).rfind(heading, 0) == 0);
  }
  CHECK(dorq::find_check("dq101") != nullptr);
  CHECK(dorq::find_check("nope") == nullptr);
}

TEST_CASE("selection: the most specific entry wins, as in flake8") {
  const auto& dq101 = dorq::find_check("DQ101")->info();
  const auto& dq102 = dorq::find_check("DQ102")->info();
  const auto enabled = [](std::vector<std::string> select, std::vector<std::string> ignore,
                          const dorq::CheckInfo& info) {
    return dorq::Selection(select, ignore).enabled(info);
  };
  CHECK(enabled({"DQ"}, {}, dq101));
  CHECK_FALSE(enabled({"DQ"}, {"DQ101"}, dq101));
  CHECK(enabled({"DQ"}, {"DQ101"}, dq102));
  CHECK(enabled({"DQ101"}, {"DQ1"}, dq101));  // longer select beats shorter ignore
  CHECK_FALSE(enabled({"DQ1"}, {"DQ1"}, dq101));
  CHECK(enabled({"ohlc-bounds"}, {"DQ101"}, dq101));  // a name is the most specific
  CHECK_FALSE(enabled({"DQ2"}, {}, dq101));

  CHECK(dorq::Selection::validate(std::vector<std::string>{"DQ", "DQ1", "DQ101", "ohlc-bounds"})
            .empty());
  CHECK_FALSE(dorq::Selection::validate(std::vector<std::string>{"E501"}).empty());
  CHECK_FALSE(dorq::Selection::validate(std::vector<std::string>{"DQ1x"}).empty());
}

TEST_CASE("DQ101 reports every failing relation, and skips incomplete bars") {
  const auto series = one_series(bars({"2024-01-02,10,9.5,9,10,1", "2024-01-03,10,11,10.5,10,1",
                                       "2024-01-04,10,,9,10,1", "2024-01-05,10,11,9,10.5,1"}));
  const auto found = run_check(dorq::OhlcBounds{}, series);
  REQUIRE(found.size() == 2);
  CHECK(found[0].message == "high 9.5 < open 10; high 9.5 < close 10");
  CHECK(found[0].line == 2);
  CHECK(found[1].message == "low 10.5 > open 10; low 10.5 > close 10");
  CHECK(found[1].p_error == 1.0);
}

TEST_CASE("DQ102: prices at or below zero, negative volume; point series only on request") {
  const auto ohlcv = one_series(bars({"2024-01-02,0,1,0,1,0", "2024-01-03,1,1,1,1,-5"}));
  const auto found = run_check(dorq::NonPositive{}, ohlcv);
  REQUIRE(found.size() == 2);
  CHECK(found[0].message == "open is 0; low is 0");  // zero volume is fine
  CHECK(found[1].message == "volume is -5");

  const auto point = one_series("date,value\n2024-01-02,-0.25\n");
  CHECK(run_check(dorq::NonPositive{}, point).empty());
  dorq::IntegritySettings positive;
  positive.positive_point_series = true;
  REQUIRE(run_check(dorq::NonPositive{}, point, positive).size() == 1);
  CHECK(run_check(dorq::NonPositive{}, point, positive)[0].message == "value is -0.25");
}

TEST_CASE("DQ103: disagreeing duplicates are errors, identical ones info") {
  const auto series = one_series(bars({"2024-01-02,1,2,1,1.5,10", "2024-01-03,1,2,1,1.5,10",
                                       "2024-01-02,1,2,1,1.6,10", "2024-01-03,1,2,1,1.5,10"}));
  const auto found = run_check(dorq::DuplicateDate{}, series);
  REQUIRE(found.size() == 2);
  CHECK(found[0].severity == dorq::Severity::kError);
  CHECK(found[0].message == "2 rows for this date with different values (lines 2, 4)");
  CHECK(found[1].severity == dorq::Severity::kInfo);
  CHECK(found[1].message == "2 identical rows for this date (lines 3, 5)");
}

TEST_CASE("DQ104: one violation per row, severity by field") {
  const auto series = one_series(bars({"2024-01-02,1,2,1,,abc", "2024-01-03,1,2,1,1.5,",
                                       "2024-02-30,1,2,1,1.5,1", "2024-01-04,1,2,x,1.5,NA"}));
  const auto found = run_check(dorq::MissingField{}, series);
  REQUIRE(found.size() == 4);
  CHECK(found[0].message == "close is empty; volume \"abc\" is not a number");
  CHECK(found[0].severity == dorq::Severity::kError);
  CHECK(found[1].message == "volume is empty");
  CHECK(found[1].severity == dorq::Severity::kWarn);
  CHECK(found[2].message == "date \"2024-02-30\" is not a date; the row is skipped");
  CHECK_FALSE(found[2].date.has_value());
  CHECK(found[2].line == 4);
  CHECK(found[3].message == "low \"x\" is not a number; volume is \"NA\"");
}

TEST_CASE("DQ106: a regime change in written precision") {
  std::string csv = "date,close\n";
  for (int d = 1; d <= 30; ++d) {
    csv += "2023-0" + std::to_string(1 + (d - 1) / 28) + "-" + (d % 28 + 1 < 10 ? "0" : "") +
           std::to_string(d % 28 + 1) + ",12.3456" + std::to_string(10 + d) + "\n";
  }
  for (int d = 1; d <= 25; ++d) {
    csv += "2024-01-" + std::string(d < 10 ? "0" : "") + std::to_string(d) + ",12." +
           std::to_string(10 + d) + "0000\n";
  }
  const auto series = one_series(csv);
  const auto found = run_check(dorq::PrecisionShift{}, series);
  REQUIRE(found.size() == 1);
  CHECK(found[0].date->to_string() == "2024-01-01");
  CHECK(found[0].severity == dorq::Severity::kWarn);
  CHECK(found[0].message.find("100% of 30 earlier closes need 5+ decimals, 0% of 25 later") !=
        std::string::npos);

  dorq::IntegritySettings strict;
  strict.precision_min_segment = 28;  // 55 rows cannot hold two segments of 28
  CHECK(run_check(dorq::PrecisionShift{}, series, strict).empty());
}

TEST_CASE("DQ106: steady precision, or sub-penny quotes, are not a shift") {
  std::string csv = "date,close\n";
  for (int d = 1; d <= 28; ++d) {
    csv += "2024-02-" + std::string(d < 10 ? "0" : "") + std::to_string(d) +
           (d <= 14 ? ",0.000123\n" : ",0.0125\n");
  }
  CHECK(run_check(dorq::PrecisionShift{}, one_series(csv), {.precision_min_segment = 10}).empty());
}

TEST_CASE("DQ107: a flat bar on typical volume, not on a thin day") {
  std::vector<std::string> rows;
  for (int d = 1; d <= 25; ++d) {
    rows.push_back("2024-01-" + std::string(d < 10 ? "0" : "") + std::to_string(d) +
                   ",10,11,9,10.5,100");
  }
  rows.emplace_back("2024-02-01,10,10,10,10,150");  // flat on above-median volume
  rows.emplace_back("2024-02-02,10,10,10,10,3");    // flat on a thin day: normal
  const auto found = run_check(dorq::ZeroRangeWithVolume{}, one_series(bars(rows)));
  REQUIRE(found.size() == 1);
  CHECK(found[0].date->to_string() == "2024-02-01");

  const auto short_series = one_series(bars({"2024-01-02,10,10,10,10,150"}));
  CHECK(run_check(dorq::ZeroRangeWithVolume{}, short_series).empty());
}
