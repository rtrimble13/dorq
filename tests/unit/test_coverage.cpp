#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "dorq/calendar.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run;

namespace {

// Deterministic pseudo-random numbers, so the universe is the same on every run.
class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}
  double next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11U) / static_cast<double>(1ULL << 53U);
  }

 private:
  std::uint64_t state_;
};

std::vector<dorq::Date> sessions(const char* from, const char* to,
                                 const dorq::Calendar& calendar = dorq::Calendar{}) {
  std::vector<dorq::Date> out;
  for (auto d = dorq::parse_date(from)->days(); d <= dorq::parse_date(to)->days(); ++d) {
    if (calendar.is_session(dorq::Date::from_days(d))) {
      out.push_back(dorq::Date::from_days(d));
    }
  }
  return out;
}

void bar(std::string& csv, const std::string& id, dorq::Date date, double volume) {
  csv += id + "," + date.to_string() + ",10,11,9,10.5," +
         std::to_string(static_cast<long>(volume)) + "\n";
}

// The plan's M2 acceptance universe: 30 liquid names that trade every session,
// 10 thin ones that trade about 30% of sessions on a few shares, and one failed
// load (2023-06-15) that 20 of the liquid names miss.
std::string universe() {
  std::string csv = "id,date,open,high,low,close,volume\n";
  const auto days = sessions("2023-01-03", "2023-12-29");
  const auto failed_load = *dorq::parse_date("2023-06-15");
  const auto lone_gap = *dorq::parse_date("2023-03-14");
  Lcg rng(42);
  for (int s = 0; s < 30; ++s) {
    const std::string id = "L" + std::to_string(s);
    for (const dorq::Date d : days) {
      if ((d == failed_load && s < 20) || (d == lone_gap && s == 25)) {
        continue;
      }
      bar(csv, id, d, 500000 + 1000000 * rng.next());
    }
  }
  const auto hole_from = *dorq::parse_date("2023-08-01");
  const auto hole_to = *dorq::parse_date("2023-12-08");
  for (int s = 0; s < 10; ++s) {
    const std::string id = "T" + std::to_string(s);
    for (const dorq::Date d : days) {
      const bool in_hole = s == 0 && d >= hole_from && d <= hole_to;
      if (rng.next() < 0.3 && !in_hole) {
        bar(csv, id, d, 1 + 3 * rng.next());
      }
    }
  }
  return csv;
}

}  // namespace

TEST_CASE("M2 acceptance: one cohort, the lone liquid gap, no thin-name noise") {
  const Result result = run({"--isolated", "--format", "jsonl"}, universe());
  CAPTURE(result.err);
  // DQ303 fires once, for the failed load.
  CHECK(count_matching(result.out, R"("code":"DQ303")") == 1);
  CHECK(contains(result.out, R"("series":null,"label":null,"source":"XNYS","date":"2023-06-15")"));
  // The single missing day on a liquid name is reported.
  CHECK(count_matching(result.out, R"("series":"L25")") == 1);
  CHECK(contains(result.out,
                 R"("series":"L25","label":null,"source":"<stdin>","date":"2023-03-14")"));
  // The cohort's members are explained by it: no DQ301 at warn or above for them.
  CHECK(count_matching(result.out, R"("date":"2023-06-15","end_date")") == 0);
  for (int s = 0; s < 20; ++s) {
    if (s != 25) {
      CHECK_FALSE(contains(result.out, "\"series\":\"L" + std::to_string(s) + "\""));
    }
  }
  // Thin names: their ordinary gaps are not reported; T0's 90-session hole is.
  for (int s = 1; s < 10; ++s) {
    CHECK_FALSE(contains(result.out, "\"series\":\"T" + std::to_string(s) + "\""));
  }
  CHECK(count_matching(result.out, R"("series":"T0")") == 1);
  CHECK(contains(result.out, R"("check":"missing-run")"));
}

TEST_CASE("info shows the downgraded members and the sparse series") {
  const Result result = run({"--isolated", "--show-info", "--format", "jsonl"}, universe());
  CHECK(count_matching(result.out, "part of the DQ303 cohort on 2023-06-15") == 20);
  CHECK(count_matching(result.out, R"("code":"DQ302")") == 10);
  CHECK(count_matching(result.out, R"("code":"DQ302")") ==
        count_matching(result.out, R"("classification":"market_fact")"));
}

TEST_CASE("DQ304: series that stop before the as-of date") {
  const Result result =
      run({"--isolated", "--select", "DQ304", "--as-of", "2024-01-05"}, universe());
  // Every liquid name is 4 sessions short of 2024-01-05 (3 after the publication
  // lag); the thin names are not overdue at 3 sessions.
  CHECK(count_matching(result.out, "DQ304") == 30);
  CHECK(count_matching(result.out, "L0  2024-01-02..2024-01-05  DQ304") == 1);
  CHECK_FALSE(contains(result.out, "T1 "));
  // Without --as-of, the latest bar is the as-of date and nothing is stale.
  CHECK(run({"--isolated", "--select", "DQ304"}, universe()).out.empty());
}

TEST_CASE("DQ206 and DQ105: a history dated a day late") {
  std::string csv = "date,open,high,low,close,volume\n";
  for (const dorq::Date d : sessions("2023-01-03", "2023-06-30")) {
    csv += dorq::Date::from_days(d.days() + 1).to_string() + ",10,11,9,10.5,1000000\n";
  }
  const Result result = run({"--isolated"}, csv);
  CHECK(count_matching(result.out, "DQ206 error  date-shift") == 1);
  CHECK(contains(result.out, "shift the history back a day"));
  CHECK(count_matching(result.out, "DQ105 warn  non-session-bar") == 1);  // one summary
}

TEST_CASE("DQ305: missing months in a monthly series") {
  std::string csv = "date,value\n";
  for (int m = 1; m <= 12; ++m) {
    if (m != 4 && m != 5) {
      csv += "2023-" + std::string(m < 10 ? "0" : "") + std::to_string(m) + "-01,3.5\n";
    }
  }
  const Result result = run({"--isolated"}, csv);
  CHECK(
      contains(result.out,
               "2023-04-01..2023-05-01  DQ305 warn  frequency-gap  2 monthly observations missing "
               "between 2023-03-01 and 2023-06-01"));
}

TEST_CASE("calendars: weekdays sees holidays as gaps; a reference file decides within its span") {
  std::string csv = "date,open,high,low,close,volume\n";
  for (const dorq::Date d : sessions("2023-06-01", "2023-07-31")) {
    csv += d.to_string() + ",10,11,9,10.5,1000000\n";
  }
  // XNYS knows July 4th: clean. Weekdays does not: a one-session gap.
  CHECK(run({"--isolated"}, csv).out.empty());
  const Result weekdays = run({"--isolated", "--calendar", "weekdays"}, csv);
  CHECK(contains(weekdays.out, "2023-06-19  DQ301"));  // Juneteenth
  CHECK(contains(weekdays.out, "2023-07-04  DQ301"));

  const dorq::test::TempDir dir;
  dorq::test::write_file(dir.path() / "cal.csv",
                         "exchange_code,trade_date,is_open\nNYSE,2023-07-03,t\nNYSE,2023-07-05,t\n"
                         "NYSE,2023-07-04,t\n");
  const auto path = (dir.path() / "cal.csv").string();
  // The file says July 4th was a session; the series has no bar on it.
  const Result with_file =
      dorq::test::run_in(dir.path(), {"--isolated", "--calendar-file", path.c_str()}, csv);
  CHECK(contains(with_file.out, "2023-07-04  DQ301"));
  CHECK_FALSE(contains(with_file.out, "2023-06-19"));  // outside the file: XNYS
  CHECK(dorq::test::run_in(dir.path(), {"--isolated", "--calendar-file", "nope.csv"}, csv).status ==
        2);
}

TEST_CASE("the cohort row in the fafnir format, and per-session reporting") {
  const Result fafnir = run({"--isolated", "--format", "fafnir"}, universe());
  CHECK(contains(
      fafnir.out,
      R"({"security_id":null,"table_name":"core.daily_price","record_key":{"trade_date":"2023-06-15"},"check_name":"dorq_cohort_gap")"));

  std::string csv = "date,open,high,low,close,volume\n";
  const auto days = sessions("2023-01-03", "2023-06-30");
  for (std::size_t i = 0; i < days.size(); ++i) {
    if (i < 50 || i > 52) {  // three consecutive sessions missing
      csv += days[i].to_string() + ",10,11,9,10.5,2000000\n";
    }
  }
  const dorq::test::TempDir dir;
  dorq::test::write_file(dir.path() / "dorq.toml", "[coverage]\nreport = \"session\"\n");
  const Result per_session = dorq::test::run_in(dir.path(), {}, csv);
  CHECK(count_matching(per_session.out, "DQ301") == 3);
  const Result per_run = run({"--isolated"}, csv);
  CHECK(count_matching(per_run.out, "DQ301") == 1);
  CHECK(contains(per_run.out, "3 sessions with no bar"));
}
