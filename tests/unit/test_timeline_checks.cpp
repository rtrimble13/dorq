// M9, DR-0909: the knowledge timeline and revisions, DQ931 filing-date-implausible,
// DQ932 restatement-implausible and DQ951 revision-cohort, through the CLI.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
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

// The reports of `code` on `series` ("" for a cross-sectional report), at warn or
// above unless `info`.
std::vector<std::string> reports(const std::string& out, const std::string& code,
                                 const std::string& series, bool info = false) {
  std::vector<std::string> got;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    const bool cross = contains(line, R"("series":null)") || field_of(line, "series").empty();
    const bool mine = series.empty() ? cross : field_of(line, "series") == series;
    if (mine && (info || !contains(line, R"("severity":"info")"))) {
      got.push_back(line);
    }
  }
  return got;
}

constexpr const char* kHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,loaded_on,revenue,cost_of_revenue,"
    "gross_profit,total_assets,total_liabilities,total_equity_parent\n";

constexpr const char* kItems =
    "column,statement,item_class,sign_convention,is_subtotal\n"
    "revenue,income,flow,positive_is_inflow,false\n"
    "cost_of_revenue,income,flow,positive_is_outflow,false\n"
    "gross_profit,income,flow,signed,true\n"
    "total_assets,balance_sheet,stock,signed,true\n"
    "total_liabilities,balance_sheet,stock,signed,true\n"
    "total_equity_parent,balance_sheet,stock,signed,true\n";

constexpr const char* kConfig =
    "[statements.identities]\n"
    "gross_profit = \"gross_profit = revenue - cost_of_revenue\"\n"
    "balance_sheet = \"total_assets = total_liabilities + total_equity_parent\"\n";

Date quarter_end(int q) {
  const int year = 2010 + q / 4;
  const std::array<std::pair<int, int>, 4> ends{{{3, 31}, {6, 30}, {9, 30}, {12, 31}}};
  const auto [month, day] = ends.at(static_cast<std::size_t>(q % 4));
  return Date::from_ymd(year, month, day);
}

Date plus(Date d, int days) { return Date::from_days(d.days() + days); }

using Values = std::array<double, 3>;  // a statement's three items, in the header's order

// A new version of one quarter's statement: known from `known`, its values from the
// first version's by `change`.
struct Restatement {
  int quarter = 0;
  std::string statement = "income";
  Date known;
  std::function<void(Values&)> change;
};

struct Company {
  std::string id;
  int quarters = 28;
  // The first version's knowledge date of quarter q: 40 days after a 10-Q quarter,
  // 70 after a year's last (the 10-K).
  std::function<Date(int)> filed = [](int q) { return plus(quarter_end(q), q % 4 == 3 ? 70 : 40); };
  std::function<void(int, const std::string&, Values&)> first_edit;  // the first version's
  std::vector<Restatement> restated;
};

Values values_of(int q, const std::string& statement) {
  const double t = q;
  if (statement == "income") {
    const double revenue = 1e9 * (1.0 + 0.02 * t) * (1.0 + 0.01 * std::sin(t * 1.7));
    const double cost = 0.6 * revenue * (1.0 + 0.005 * std::cos(t));
    return {std::round(revenue), std::round(cost), std::round(revenue) - std::round(cost)};
  }
  const double assets = 1e10 * (1.0 + 0.01 * t) * (1.0 + 0.004 * std::sin(t * 2.3));
  const double liabilities = 0.6 * assets * (1.0 + 0.003 * std::cos(t * 1.3));
  return {std::round(assets), std::round(liabilities), std::round(assets) - std::round(liabilities)};
}

std::string number(double v) { return std::to_string(std::llround(v)); }

std::string row(const std::string& id, int q, const std::string& statement, int version,
                Date known, const std::optional<Date>& known_to, const char* source,
                const Values& v) {
  const Date end = quarter_end(q);
  std::string text = id + "," + statement + ",quarter," + end.to_string() + "," +
                     std::to_string(2010 + q / 4) + ",Q" + std::to_string(q % 4 + 1) + "," +
                     std::to_string(version) + "," + known.to_string() + "," +
                     (known_to ? known_to->to_string() : std::string{}) + "," + source + "," +
                     (std::string{source} == "filed" ? known.to_string() : std::string{}) + "," +
                     known.to_string();
  if (statement == "income") {
    text += "," + number(v[0]) + "," + number(v[1]) + "," + number(v[2]) + ",,,\n";
  } else {
    text += ",,,," + number(v[0]) + "," + number(v[1]) + "," + number(v[2]) + "\n";
  }
  return text;
}

std::string company(const Company& c) {
  std::string text;
  for (int q = 0; q < c.quarters; ++q) {
    for (const std::string statement : {"income", "balance_sheet"}) {
      Values v = values_of(q, statement);
      if (c.first_edit) {
        c.first_edit(q, statement, v);
      }
      std::optional<Date> to;
      const Restatement* again = nullptr;
      for (const Restatement& r : c.restated) {
        if (r.quarter == q && r.statement == statement) {
          to = r.known;
          again = &r;
        }
      }
      text += row(c.id, q, statement, 1, c.filed(q), to, "filed", v);
      if (again != nullptr) {
        Values w = values_of(q, statement);
        again->change(w);
        text += row(c.id, q, statement, 2, again->known, std::nullopt, "observed", w);
      }
    }
  }
  return text;
}

void times(Values& v, double f) {
  for (double& x : v) {
    x *= f;
  }
}

Result check(const TempDir& dir, const std::string& csv) {
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "st.toml", kConfig);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items = (dir.path() / "items.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  return run_in(dir.path(), {"--config", config.c_str(), "--items", items.c_str(), "--select",
                             "DQ93,DQ951", "--show-info", "--format", "jsonl", "--exit-zero",
                             file.c_str()});
}

}  // namespace

TEST_CASE("DQ931: a filing date at the period end is an error, an amendment's a warn, a late "
          "filing info") {
  const TempDir dir;
  std::string csv;
  Company early;  // 2013 Q2's 10-Q dated its period's end
  early.id = "EARLY";
  early.filed = [](int q) { return q == 13 ? quarter_end(q) : plus(quarter_end(q), 40); };
  csv += company(early);
  Company amended;  // 2014 Q1 known from an amendment, after 2014 Q2's 10-Q
  amended.id = "AMENDED";
  amended.filed = [](int q) { return plus(quarter_end(q), q == 16 ? 150 : 40); };
  csv += company(amended);
  Company late;  // 2012 Q3's 10-Q a month late, before the next quarter's
  late.id = "LATE";
  late.filed = [](int q) { return plus(quarter_end(q), q == 10 ? 75 : 40); };
  csv += company(late);
  Company clean;
  clean.id = "CLEAN";
  csv += company(clean);
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);

  const std::vector<std::string> e = reports(r.out, "DQ931", "EARLY");
  REQUIRE(e.size() == 1);  // the period's income statement and balance sheet: one filing
  CHECK(contains(e[0], R"("severity":"error")"));
  CHECK(contains(e[0], R"("fiscal_date":"2013-06-30")"));
  CHECK(contains(e[0], "0 days after the period ends, against this issuer's usual 40 days"));
  CHECK(contains(e[0], "every backtest knows the figures too soon"));
  CHECK(contains(e[0], R"("kind":"check_filing_date")"));

  const std::vector<std::string> a = reports(r.out, "DQ931", "AMENDED");
  REQUIRE_FALSE(a.empty());
  CHECK(contains(a[0], R"("severity":"warn")"));
  CHECK(contains(a[0], "an amendment's date taken for the original's, known after the next period"));
  CHECK(contains(a[0], R"("misdated_amendment")"));

  CHECK(reports(r.out, "DQ931", "LATE").empty());
  const std::vector<std::string> l = reports(r.out, "DQ931", "LATE", true);
  REQUIRE_FALSE(l.empty());
  CHECK(contains(l[0], R"("severity":"info")"));
  CHECK(contains(l[0], "a late filing, a filing fact"));
  CHECK(contains(l[0], R"("classification":"market_fact")"));

  CHECK(reports(r.out, "DQ931", "CLEAN", true).empty());
}

TEST_CASE("DQ932: a new version at x1000 is the new version's error, a corrected one the old's") {
  const TempDir dir;
  std::string csv;
  Company scaled;  // 2014 Q3's income statement restated at x1000
  scaled.id = "SCALED";
  scaled.restated = {{18, "income", Date::from_ymd(2016, 3, 1), [](Values& v) { times(v, 1000); }}};
  csv += company(scaled);
  Company fixed;  // 2013 Q1's balance sheet filed at x0.001 and restated right
  fixed.id = "FIXED";
  fixed.first_edit = [](int q, const std::string& s, Values& v) {
    if (q == 12 && s == "balance_sheet") {
      times(v, 0.001);
    }
  };
  fixed.restated = {{12, "balance_sheet", Date::from_ymd(2014, 2, 20), [](Values&) {}}};
  csv += company(fixed);
  Company reclassified;  // 10% of 2015 Q2's liabilities moved into equity, the total kept
  reclassified.id = "RECLASS";
  reclassified.restated = {{21, "balance_sheet", Date::from_ymd(2017, 2, 20), [](Values& v) {
                              const double moved = std::round(0.1 * v[1]);
                              v[1] -= moved;
                              v[2] += moved;
                            }}};
  csv += company(reclassified);
  Company revised;  // 2012 Q4's revenue and costs revised up 3%, the total with them
  revised.id = "REVISED";
  revised.restated = {{11, "income", Date::from_ymd(2014, 3, 1), [](Values& v) {
                         v[0] = std::round(v[0] * 1.03);
                         v[1] = std::round(v[1] * 1.03);
                         v[2] = v[0] - v[1];
                       }}};
  csv += company(revised);
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);

  const std::vector<std::string> s = reports(r.out, "DQ932", "SCALED");
  REQUIRE(s.size() == 1);
  CHECK(contains(s[0], R"("version_no":2)"));
  CHECK(contains(s[0], "v2, known from 2016-03-01, differs from v1 by x1000 on 3 of 3 items"));
  CHECK(contains(s[0], "the new version is wrong"));
  CHECK(contains(s[0], R"("pattern":"power_of_ten")"));

  const std::vector<std::string> f = reports(r.out, "DQ932", "FIXED");
  REQUIRE(f.size() == 1);
  CHECK(contains(f[0], R"("version_no":1)"));
  CHECK(contains(f[0], "v1, in force from 2013-05-10 to 2014-02-20, differs from the restated v2 "
                       "by x0.001 on 3 of 3 items"));
  CHECK(contains(f[0], "the old version was wrong while in force"));
  CHECK(contains(f[0], R"("in_force_to":"2014-02-20")"));

  CHECK(reports(r.out, "DQ932", "RECLASS", true).empty());
  CHECK(reports(r.out, "DQ932", "REVISED", true).empty());
}

TEST_CASE("DQ951: a backfill is one cohort at info; a night's load at x1000 one at warn") {
  const TempDir dir;
  std::string csv;
  const Date backfill = Date::from_ymd(2020, 6, 1);
  const Date night = Date::from_ymd(2020, 6, 9);
  // Ten companies the backfill restates (a reclassification each), four restated on
  // days of their own, and four whose new versions a night's load stored at x1000.
  for (int k = 0; k < 18; ++k) {
    Company c;
    c.id = "C" + std::to_string(100 + k);
    Restatement r;
    r.quarter = 8 + k;
    r.statement = "balance_sheet";
    r.change = [](Values& v) {
      const double moved = std::round(0.05 * v[1]);
      v[1] -= moved;
      v[2] += moved;
    };
    if (k < 10) {
      r.known = backfill;
    } else if (k < 14) {
      r.known = plus(Date::from_ymd(2018, 1, 10), 70 * k);
    } else {
      r.known = night;
      r.statement = "income";
      r.change = [](Values& v) { times(v, 1000); };
    }
    c.restated = {r};
    csv += company(c);
  }
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> cohorts = reports(r.out, "DQ951", "", true);
  REQUIRE(cohorts.size() == 2);
  const std::string& b = cohorts[0];
  CHECK(contains(b, R"("date":"2020-06-01")"));
  CHECK(contains(b, R"("severity":"info")"));
  CHECK(contains(b, "10 series revised on 2020-06-01"));
  CHECK(contains(b, "a vendor batch or a backfill"));
  CHECK(contains(b, R"("members":"C100,C101,C102,C103,C104,C105,C106,C107,C108,C109")"));

  const std::string& n = cohorts[1];
  CHECK(contains(n, R"("date":"2020-06-09")"));
  CHECK(contains(n, R"("severity":"warn")"));
  CHECK(contains(n, "4 carry DQ932"));
  CHECK(contains(n, "a load fault"));
  CHECK(contains(n, R"("members":"C114,C115,C116,C117")"));
  // The members' DQ932 reports: at info, linked to the cohort.
  for (const std::string id : {"C114", "C115", "C116", "C117"}) {
    INFO(id);
    CHECK(reports(r.out, "DQ932", id).empty());
    const std::vector<std::string> m = reports(r.out, "DQ932", id, true);
    REQUIRE(m.size() == 1);
    CHECK(contains(m[0], "part of the DQ951 cohort on 2020-06-09"));
    CHECK(contains(m[0], R"("cohort_date":"2020-06-09")"));
  }
  // An ordinary restatement in the backfill is no DQ932 at all.
  CHECK(reports(r.out, "DQ932", "C100", true).empty());
}
