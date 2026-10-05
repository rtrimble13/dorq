#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "io/columns.hpp"
#include "io/input_error.hpp"
#include "io/reader.hpp"

namespace {

std::vector<dorq::Series> read(const std::string& text, std::string_view extension = "",
                               dorq::Grouping grouping = dorq::Grouping::kBuffer,
                               const dorq::ReadOptions& options = {}) {
  std::vector<dorq::Series> out;
  dorq::SeriesAssembler assembler(grouping, [&](dorq::Series&& s) { out.push_back(std::move(s)); });
  std::istringstream in(text);
  dorq::read_input(in, "test", "stem", extension, options, assembler);
  assembler.finish();
  return out;
}

}  // namespace

TEST_CASE("columns: aliases, series and label") {
  const std::vector<std::string> names = {"security_id", "Symbol", "Trade Date", "Open", "High",
                                          "Low",         "Close",  "Adj Close",  "Vol"};
  const auto m = dorq::map_columns(names, {}, dorq::KindOption::kAuto, "t");
  CHECK(m.kind == dorq::SeriesKind::kOhlcv);
  CHECK(m.index(dorq::Field::kSeries) == 0);
  CHECK(m.index(dorq::Field::kLabel) == 1);
  CHECK(m.index(dorq::Field::kDate) == 2);
  CHECK(m.index(dorq::Field::kClose) == 6);  // Close, not Adj Close
  CHECK(m.index(dorq::Field::kVolume) == 8);
}

TEST_CASE("columns: a lone symbol column names the series") {
  const std::vector<std::string> names = {"symbol", "date", "value"};
  const auto m = dorq::map_columns(names, {}, dorq::KindOption::kAuto, "t");
  CHECK(m.kind == dorq::SeriesKind::kPoint);
  CHECK(m.index(dorq::Field::kSeries) == 0);
  CHECK(m.index(dorq::Field::kLabel) == -1);
}

TEST_CASE("columns: a FRED download's leftover column is the value") {
  const std::vector<std::string> names = {"observation_date", "DGS10"};
  const auto m = dorq::map_columns(names, {}, dorq::KindOption::kAuto, "t");
  CHECK(m.kind == dorq::SeriesKind::kPoint);
  CHECK(m.index(dorq::Field::kClose) == 1);
}

TEST_CASE("columns: overrides, and the errors that name what is wrong") {
  dorq::ColumnOverrides overrides;
  REQUIRE_FALSE(dorq::parse_column_overrides("date=d, value = x", overrides).has_value());
  const std::vector<std::string> names = {"d", "x", "y"};
  const auto m = dorq::map_columns(names, overrides, dorq::KindOption::kAuto, "t");
  CHECK(m.index(dorq::Field::kDate) == 0);
  CHECK(m.index(dorq::Field::kClose) == 1);

  dorq::ColumnOverrides bad;
  CHECK(dorq::parse_column_overrides("colour=x", bad).has_value());
  CHECK(dorq::parse_column_overrides("date", bad).has_value());

  const std::vector<std::string> no_date = {"close"};
  CHECK_THROWS_AS(static_cast<void>(dorq::map_columns(no_date, {}, dorq::KindOption::kAuto, "t")),
                  dorq::InputError);
  const std::vector<std::string> partial = {"date", "high", "close"};
  CHECK_THROWS_AS(static_cast<void>(dorq::map_columns(partial, {}, dorq::KindOption::kAuto, "t")),
                  dorq::InputError);
  const std::vector<std::string> point = {"date", "close"};
  CHECK_THROWS_AS(static_cast<void>(dorq::map_columns(point, {}, dorq::KindOption::kOhlcv, "t")),
                  dorq::InputError);
}

TEST_CASE("reader: OHLCV CSV with two series, unsorted dates, issues kept") {
  const auto series = read(
      "symbol,date,open,high,low,close,volume\n"
      "AAA,2024-01-03,1,2,0.5,1.5,100\n"
      "AAA,2024-01-02,1,2,0.5,1.25,NA\n"
      "AAA,not-a-date,1,2,0.5,1.5,100\n"
      "BBB,2024-01-02,10,11,9,10.500000,abc\n");
  REQUIRE(series.size() == 2);
  const auto& a = series[0];
  CHECK(a.id == "AAA");
  CHECK(a.kind == dorq::SeriesKind::kOhlcv);
  REQUIRE(a.size() == 2);
  CHECK(a.date[0].to_string() == "2024-01-02");  // sorted
  CHECK(a.close[0] == 1.25);
  CHECK(a.line[0] == 3);
  CHECK(std::isnan(a.volume[0]));
  REQUIRE(a.issues.size() == 2);
  CHECK(a.issues[0].field == dorq::Field::kVolume);
  CHECK(a.issues[0].kind == dorq::FieldIssue::Kind::kMissing);
  CHECK(a.issues[1].field == dorq::Field::kDate);
  CHECK(a.issues[1].kind == dorq::FieldIssue::Kind::kInvalid);
  CHECK(a.issues[1].line == 4);

  const auto& b = series[1];
  CHECK(b.close_decimals[0] == 1);
  REQUIRE(b.issues.size() == 1);
  CHECK(b.issues[0].kind == dorq::FieldIssue::Kind::kInvalid);
}

TEST_CASE("reader: no series column means one series named after the file") {
  const auto series = read("Date\tValue\n2024-01-02\t4.5\n", "");
  REQUIRE(series.size() == 1);
  CHECK(series[0].id == "stem");
  CHECK(series[0].kind == dorq::SeriesKind::kPoint);
  CHECK(series[0].close[0] == 4.5);
}

TEST_CASE("reader: JSONL and JSON arrays read like CSV") {
  const std::string csv = "id,date,value\nx,2024-01-02,1.50\nx,2024-01-03,2\n";
  const std::string jsonl =
      "{\"id\":\"x\",\"date\":\"2024-01-02\",\"value\":1.50}\n\n"
      "{\"id\":\"x\",\"date\":\"2024-01-03\",\"value\":\"2\"}";
  const std::string json = "[" + std::string{R"({"id":"x","date":"2024-01-02","value":1.50},)"} +
                           R"({"id":"x","value":2,"date":"2024-01-03"}])";
  for (const auto& text : {csv, jsonl, json}) {
    const auto series = read(text);
    REQUIRE(series.size() == 1);
    CHECK(series[0].id == "x");
    CHECK(series[0].close == std::vector<double>{1.5, 2.0});
    CHECK(series[0].close_decimals[0] == 1);
  }
}

TEST_CASE("reader: streaming rejects a series that reappears, but buffering accepts it") {
  const std::string text = "id,date,value\na,2024-01-02,1\nb,2024-01-02,1\na,2024-01-03,1\n";
  CHECK_THROWS_AS(read(text, ".csv", dorq::Grouping::kStream), dorq::InputError);
  const auto series = read(text, ".csv", dorq::Grouping::kBuffer);
  REQUIRE(series.size() == 2);
  CHECK(series[0].size() == 2);
}

TEST_CASE("reader: empty input is an error, but a header alone is not") {
  CHECK_THROWS_AS(read(""), dorq::InputError);
  CHECK_THROWS_AS(read(" \n\n"), dorq::InputError);
  CHECK(read("date,value\n").empty());
  CHECK(read("[]").empty());
}
