// M9, DR-0910: coverage and freshness, DQ941 period-gap, DQ942 statements-stale,
// DQ943 statements-missing and DQ944 reporting-currency, through the CLI.

#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "dorq/date.hpp"
#include "test_support.hpp"

using dorq::Date;
using dorq::test::contains;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

std::vector<std::string> lines_with(const std::string& text, const std::string& needle) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (contains(line, needle)) {
      out.push_back(line);
    }
  }
  return out;
}

std::string field_of(const std::string& line, const std::string& name) {
  const std::string open = "\"" + name + "\":\"";
  const auto from = line.find(open);
  if (from == std::string::npos) {
    return {};
  }
  const auto start = from + open.size();
  return line.substr(start, line.find('"', start) - start);
}

// The reports of `code` on `series`, at warn or above unless `info`.
std::vector<std::string> reports(const std::string& out, const std::string& code,
                                 const std::string& series, bool info = false) {
  std::vector<std::string> got;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    if (field_of(line, "series") == series && (info || !contains(line, R"("severity":"info")"))) {
      got.push_back(line);
    }
  }
  return got;
}

constexpr const char* kHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,knowledge_source,filing_date,reported_currency,revenue,total_assets\n";

constexpr const char* kItems =
    "column,statement,item_class,sign_convention,is_subtotal\n"
    "revenue,income,flow,positive_is_inflow,false\n"
    "total_assets,balance_sheet,stock,signed,true\n";

Date quarter_end(int q) {
  const int year = 2010 + q / 4;
  const std::array<std::pair<int, int>, 4> ends{{{3, 31}, {6, 30}, {9, 30}, {12, 31}}};
  const auto [month, day] = ends.at(static_cast<std::size_t>(q % 4));
  return Date::from_ymd(year, month, day);
}

Date plus(Date d, int days) { return Date::from_days(d.days() + days); }

// A company's quarters 0 to `quarters` - 1 (2010 Q1 on), a December year, each filed
// 40 days after it ends (a year and its last quarter 70), with its years.
struct Company {
  std::string id;
  int quarters = 24;
  std::string currency = "USD";
  std::function<bool(int)> has = [](int) { return true; };  // the quarter is in the input
  std::function<std::string(int)> label = [](int q) {       // its fiscal period
    return "Q" + std::to_string(q % 4 + 1);
  };
};

std::string company(const Company& c) {
  std::string text;
  for (int q = 0; q < c.quarters; ++q) {
    if (!c.has(q)) {
      continue;
    }
    const Date end = quarter_end(q);
    const Date known = plus(end, q % 4 == 3 ? 70 : 40);
    const std::string fy = std::to_string(2010 + q / 4);
    const std::string tail =
        "," + known.to_string() + ",filed," + known.to_string() + "," + c.currency + ",";
    text += c.id + ",income,quarter," + end.to_string() + "," + fy + "," + c.label(q) + ",1" +
            tail + std::to_string(1000000 + 1000 * q) + ",\n";
    text += c.id + ",balance_sheet,quarter," + end.to_string() + "," + fy + "," + c.label(q) +
            ",1" + tail + "," + std::to_string(5000000 + 1000 * q) + "\n";
    if (q % 4 == 3) {
      text += c.id + ",income,annual," + end.to_string() + "," + fy + ",FY,1" + tail +
              std::to_string(4000000 + 4000 * q) + ",\n";
    }
  }
  return text;
}

Result check(const TempDir& dir, const std::string& csv, const std::string& meta,
             const std::string& as_of = "2016-03-01") {
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "meta.csv", meta);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items = (dir.path() / "items.csv").string();
  const std::string meta_file = (dir.path() / "meta.csv").string();
  if (as_of.empty()) {
    return run_in(dir.path(),
                  {"--items", items.c_str(), "--meta", meta_file.c_str(), "--select", "DQ94",
                   "--show-info", "--format", "jsonl", "--exit-zero", file.c_str()});
  }
  return run_in(dir.path(), {"--items", items.c_str(), "--meta", meta_file.c_str(), "--select",
                             "DQ94", "--as-of", as_of.c_str(), "--show-info", "--format", "jsonl",
                             "--exit-zero", file.c_str()});
}

constexpr const char* kMetaHeader =
    "security_id,currency,first_trade_date,last_trade_date,active,delisted_date,"
    "statements_expected\n";

}  // namespace

TEST_CASE("DQ941: a missing quarter, a mislabelled one, and a gap before the security traded") {
  const TempDir dir;
  std::string csv;
  Company gap;  // 2012 Q3 missing
  gap.id = "GAP";
  gap.has = [](int q) { return q != 10; };
  csv += company(gap);
  Company mislabel;  // 2013 Q2 labelled Q3
  mislabel.id = "MISLABEL";
  mislabel.label = [](int q) { return "Q" + std::to_string((q == 13 ? q + 1 : q) % 4 + 1); };
  csv += company(mislabel);
  Company early;  // 2010 Q3 missing, before the security first traded
  early.id = "EARLY";
  early.has = [](int q) { return q != 2; };
  csv += company(early);
  const std::string meta = std::string{kMetaHeader} + "GAP,USD,2009-01-02,2016-03-01,true,,true\n" +
                           "MISLABEL,USD,2009-01-02,2016-03-01,true,,true\n" +
                           "EARLY,USD,2010-11-01,2016-03-01,true,,true\n";
  const Result r = check(dir, csv, meta);
  INFO(r.err);
  REQUIRE(r.status == 0);

  const std::vector<std::string> g = reports(r.out, "DQ941", "GAP");
  REQUIRE(g.size() == 1);
  CHECK(contains(g[0], R"("date":"2012-09-30")"));
  CHECK(contains(g[0],
                 "1 quarter missing between 2012-06-30 and 2012-12-31 (184 days), while "
                 "the security traded"));
  CHECK(contains(g[0], R"("hypothesis":"missing_period")"));

  const std::vector<std::string> m = reports(r.out, "DQ941", "MISLABEL");
  REQUIRE(m.size() == 1);  // the quarter after it is judged against the label it should have had
  CHECK(contains(m[0], R"("date":"2013-06-30")"));
  CHECK(contains(m[0],
                 "is labelled Q3 one quarter after Q1: Q2 was due, and no period is "
                 "missing (the labels after it resume the cycle)"));
  CHECK(contains(m[0], R"("hypothesis":"mislabel")"));

  CHECK(reports(r.out, "DQ941", "EARLY", true).empty());
}

TEST_CASE("DQ941: a change of fiscal year is info at most") {
  const TempDir dir;
  // From a December year to a September one: 2013 Q1 to Q3 are the transition year,
  // which ends 2013-09-30, and 2013-12-31 is Q1 of the new year.
  std::string csv;
  const std::string id = "CHANGE";
  for (int q = 0; q < 24; ++q) {
    const Date end = quarter_end(q);
    const Date known = plus(end, 40);
    const bool new_year = q >= 15;  // from 2013-12-31
    const int label = new_year ? (q - 15) % 4 + 1 : q % 4 + 1;
    const std::string tail = "," + known.to_string() + ",filed," + known.to_string() + ",USD,";
    csv += id + ",income,quarter," + end.to_string() + ",2010,Q" + std::to_string(label) + ",1" +
           tail + std::to_string(1000000 + 1000 * q) + ",\n";
    const bool year_end = new_year ? (q - 15) % 4 == 3 : (q % 4 == 3 && q < 12) || q == 14;
    if (year_end) {
      csv += id + ",income,annual," + end.to_string() + ",2010,FY,1" + tail +
             std::to_string(4000000 + 4000 * q) + ",\n";
    }
  }
  const Result r = check(dir, csv, std::string{kMetaHeader} + "CHANGE,USD,,,true,,true\n");
  INFO(r.err);
  REQUIRE(r.status == 0);
  CHECK(reports(r.out, "DQ941", id).empty());
  const std::vector<std::string> info = reports(r.out, "DQ941", id, true);
  REQUIRE_FALSE(info.empty());
  for (const std::string& line : info) {
    CHECK(contains(line, "a change of fiscal year"));
    CHECK(contains(line, R"("hypothesis":"fiscal_year_change")"));
  }
}

TEST_CASE("DQ942: a filer stopped while trading is stale; a delisted one is not") {
  const TempDir dir;
  std::string csv;
  Company stale;  // its last quarter 2014 Q4, still trading in 2016
  stale.id = "STALE";
  stale.quarters = 20;
  csv += company(stale);
  Company delisted;  // the same, delisted in 2015
  delisted.id = "DELISTED";
  delisted.quarters = 20;
  csv += company(delisted);
  Company fresh;  // 2015 Q4 filed by the as-of date
  fresh.id = "FRESH";
  csv += company(fresh);
  const std::string meta = std::string{kMetaHeader} +
                           "STALE,USD,2009-01-02,2016-03-01,true,,true\n" +
                           "DELISTED,USD,2009-01-02,2015-04-02,false,2015-04-03,true\n" +
                           "FRESH,USD,2009-01-02,2016-03-01,true,,true\n";
  const Result r = check(dir, csv, meta);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> s = reports(r.out, "DQ942", "STALE");
  REQUIRE(s.size() == 1);
  CHECK(contains(s[0], R"("date":"2014-12-31")"));  // keyed on the latest fiscal date
  CHECK(contains(s[0], R"("severity":"error")"));
  CHECK(contains(s[0],
                 "no period after 2014-12-31 by 2016-03-01: the next, ending 2015-04-01 "
                 "(a 10-Q), was due about 2015-05-11 at this issuer's usual 40 days"));
  CHECK(reports(r.out, "DQ942", "DELISTED", true).empty());
  CHECK(reports(r.out, "DQ942", "FRESH", true).empty());

  // Without --as-of there is no date to be stale by.
  const Result without = check(dir, csv, meta, "");
  CHECK(reports(without.out, "DQ942", "STALE", true).empty());
}

TEST_CASE("DQ943 and DQ944: a security only the metadata has, and a currency of its own") {
  const TempDir dir;
  Company euro;
  euro.id = "EURO";
  euro.currency = "EUR";
  const std::string csv = company(euro);
  const std::string meta = std::string{kMetaHeader} +
                           "EURO,USD,2009-01-02,2016-03-01,true,,true\n" +
                           "MISSING,USD,2012-03-01,2016-03-01,true,,true\n" +
                           "NOT_EXPECTED,USD,2012-03-01,2016-03-01,true,,false\n" +
                           "GONE,USD,1990-01-02,1996-03-01,false,1996-03-02,true\n";
  const Result r = check(dir, csv, meta);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> missing = reports(r.out, "DQ943", "MISSING");
  REQUIRE(missing.size() == 1);
  CHECK(contains(missing[0], R"("date":"2012-03-01")"));
  CHECK(contains(missing[0],
                 "the metadata expects statements for this security, and the input "
                 "has none: it traded from 2012-03-01 to 2016-03-01"));
  CHECK(reports(r.out, "DQ943", "NOT_EXPECTED", true).empty());
  CHECK(reports(r.out, "DQ943", "GONE", true).empty());  // it stopped before the coverage floor
  CHECK(reports(r.out, "DQ943", "EURO", true).empty());

  CHECK(reports(r.out, "DQ944", "EURO").empty());
  const std::vector<std::string> currency = reports(r.out, "DQ944", "EURO", true);
  REQUIRE(currency.size() == 1);
  CHECK(contains(currency[0],
                 "the statements are reported in EUR, and the security trades in "
                 "USD"));
  CHECK(contains(currency[0], R"("classification":"market_fact")"));
}

TEST_CASE("the new metadata fields leave a bars run as it was, and visit no series") {
  const TempDir dir;
  const std::string bars =
      "series,date,open,high,low,close,volume\n"
      "X,2020-01-02,10,11,9,10,1000\n"
      "X,2020-01-03,10,11,9,10.5,1000\n"
      "X,2020-01-06,10.5,11,10,0.105,1000\n"
      "X,2020-01-07,10.5,11,10,10.6,1000\n";
  write_file(dir.path() / "bars.csv", bars);
  write_file(dir.path() / "old.csv", "series,asset_type,exchange\nX,equity,XNYS\nY,equity,XNYS\n");
  write_file(dir.path() / "new.csv",
             "series,asset_type,exchange,currency,first_trade_date,last_trade_date,active,"
             "delisted_date,sector,industry,statements_expected\n"
             "X,equity,XNYS,USD,2000-01-03,2020-01-07,true,,Technology,Software,true\n"
             "Y,equity,XNYS,USD,2000-01-03,2020-01-07,true,,Technology,Software,true\n");
  const std::string file = (dir.path() / "bars.csv").string();
  const std::string old_meta = (dir.path() / "old.csv").string();
  const std::string new_meta = (dir.path() / "new.csv").string();
  const Result before = run_in(dir.path(), {"--meta", old_meta.c_str(), "--format", "jsonl",
                                            "--exit-zero", "--show-info", file.c_str()});
  const Result after = run_in(dir.path(), {"--meta", new_meta.c_str(), "--format", "jsonl",
                                           "--exit-zero", "--show-info", file.c_str()});
  INFO(before.err);
  INFO(after.err);
  REQUIRE(before.status == 0);
  REQUIRE(after.status == 0);
  REQUIRE_FALSE(before.out.empty());
  // The run's summary names its inputs, which differ: compare the findings.
  CHECK(lines_with(after.out, R"("code":")") == lines_with(before.out, R"("code":")"));
  CHECK_FALSE(contains(after.out, "DQ943"));
}
