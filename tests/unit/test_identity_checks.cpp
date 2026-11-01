// M9, DR-0905: DQ903 identity-break and DQ904 period-tie, through the CLI, and the
// [statements] settings they read (explained_by, cash_tie, restricted cash).

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "dorq/date.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

// The lines of `text` that hold `needle`.
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

// A JSON Lines record's record_key, as written.
std::string record_key(const std::string& line) {
  const auto at = line.find(R"("record_key":)");
  return at == std::string::npos ? std::string{}
                                 : line.substr(at + 13, line.find('}', at) - at - 12);
}

// "SERIES {record_key}" of each report of `code`, sorted.
std::vector<std::string> reports_of(const std::string& out, const std::string& code) {
  std::vector<std::string> got;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    got.push_back(field_of(line, "series") + " " + record_key(line));
  }
  std::sort(got.begin(), got.end());
  return got;
}

// The report of `code` on `series` whose record_key holds `needle`.
std::string report(const std::string& out, const std::string& code, const std::string& series,
                   const std::string& needle) {
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    if (field_of(line, "series") == series && contains(record_key(line), needle)) {
      return line;
    }
  }
  return {};
}

constexpr const char* kQuarters[] = {"2010-03-31", "2010-06-30", "2010-09-30", "2010-12-31",
                                     "2011-03-31"};

// ---------------------------------------------------------------------------
// DQ903

constexpr const char* kIdentityHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,total_assets,total_liabilities,"
    "total_equity_parent,net_income_noncontrolling\n";

// "Q3" for a quarter ending in September.
std::string quarter_of(const std::string& date) {
  return "Q" + std::to_string(std::stoi(date.substr(5, 2)) / 3);
}

// A quarterly balance sheet, version 1 unless `version` says otherwise.
std::string sheet(const std::string& id, const std::string& date, const std::string& assets,
                  const std::string& liabilities, const std::string& equity,
                  const std::string& version = "1,2013-01-01,") {
  return id + ",balance_sheet,quarter," + date + "," + date.substr(0, 4) + "," + quarter_of(date) +
         "," + version + ",,," +
         assets + "," + liabilities + "," + equity + ",\n";
}

// Five quarters that hold (10bn = 6bn + 4bn), but where `odd` replaces one.
std::string five_quarters(const std::string& id, std::size_t at, const std::string& odd) {
  std::string text;
  for (std::size_t q = 0; q < 5; ++q) {
    text += q == at ? odd : sheet(id, kQuarters[q], "10000000000", "6000000000", "4000000000");
  }
  return text;
}

std::string identity_rows() {
  std::string text = kIdentityHeader;
  // A field error: total_liabilities in thousands in one quarter.
  text += five_quarters("FIELD", 2,
                        sheet("FIELD", kQuarters[2], "10000000000", "6000000", "4000000000"));
  // ... and equity with its sign flipped.
  text +=
      five_quarters("SIGN", 1, sheet("SIGN", kQuarters[1], "10000000000", "6000000000", "-4000000000"));
  // Structural: the same gap (say, temporary equity) in every quarter.
  for (const char* q : kQuarters) {
    text += sheet("STRUCT", q, "10000000000", "6000000000", "3700000000");
  }
  // One quarter's gap, with noncontrolling interests reported in it.
  text += five_quarters("NCI", 2, sheet("NCI", kQuarters[2], "10000000000", "6000000000", "3750000000"));
  text += "NCI,income,quarter,2010-09-30,2010,,1,2013-01-01,,,,,,,50000000\n";
  // A reclassification: v1 breaks, and v2, with the same total, holds.
  text += five_quarters(
      "RECLASS", 2,
      sheet("RECLASS", kQuarters[2], "10000000000", "6300000000", "4000000000",
            "1,2013-01-01,2013-06-01") +
          sheet("RECLASS", kQuarters[2], "10000000000", "6000000000", "4000000000",
                "2,2013-06-01,"));
  // Unexplained: one quarter's gap that no term gives; and a NULL term, not judged.
  text += five_quarters("UNEXPL", 2,
                        sheet("UNEXPL", kQuarters[2], "10000000000", "6000000000", "3700000000"));
  text += sheet("UNEXPL", "2011-06-30", "10000000000", "1", "");
  return text;
}

constexpr const char* kIdentityConfig =
    "[statements]\n"
    "[statements.identities]\n"
    "balance_sheet = \"total_assets = total_liabilities + total_equity_parent\"\n"
    "[statements.explained_by]\n"
    "balance_sheet = [\"net_income_noncontrolling\"]\n";

// ---------------------------------------------------------------------------
// DQ904

constexpr const char* kTieHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,cash_and_equivalents,net_change_in_cash,"
    "revenue,cost_of_revenue\n";

std::string tie_row(const std::string& id, const std::string& statement, const std::string& type,
                    const std::string& date, const std::string& fiscal_year,
                    const std::string& items) {
  return id + "," + statement + "," + type + "," + date + "," + fiscal_year + ",,1,2020-01-01,,,," +
         items + "\n";
}

std::string cash(const std::string& id, const std::string& date, const std::string& balance) {
  return tie_row(id, "balance_sheet", "quarter", date, date.substr(0, 4), balance + ",,,");
}

std::string flow(const std::string& id, const std::string& date, const std::string& change) {
  return tie_row(id, "cash_flow", "quarter", date, date.substr(0, 4), "," + change + ",,");
}

std::string income(const std::string& id, const std::string& type, const std::string& date,
                   const std::string& fiscal_year, const std::string& revenue) {
  // Cost of revenue is 500M a quarter, and 2bn a year.
  return tie_row(id, "income", type, date, fiscal_year,
                 ",," + revenue + (type == "annual" ? ",2000000000" : ",500000000"));
}

std::string tie_rows() {
  std::string text = kTieHeader;
  // One cash flow off: the change since 2010-03-31 is 100M, the flow says 300M. A
  // balance sheet nine months after the last is beyond the spacing, so not tied.
  text += cash("CASH", "2009-12-31", "1000000000");
  const char* balances[] = {"1100000000", "1200000000", "1300000000", "1400000000"};
  for (std::size_t q = 0; q < 4; ++q) {
    text += cash("CASH", kQuarters[q], balances[q]);
    text += flow("CASH", kQuarters[q], q == 1 ? "300000000" : "100000000");
  }
  text += cash("CASH", "2011-09-30", "5000000000");
  text += flow("CASH", "2011-09-30", "100000000");
  // One balance sheet's cash out of line with both flows around it: one report.
  text += cash("PAIR", "2009-12-31", "1000000000");
  for (std::size_t q = 0; q < 4; ++q) {
    text += cash("PAIR", kQuarters[q], q == 1 ? "1500000000" : balances[q]);
    text += flow("PAIR", kQuarters[q], "100000000");
  }
  // Restricted cash: a 5% gap is a break in 2017, and within its share from 2018.
  const char* restricted[][2] = {{"2017-06-30", ""},
                                 {"2017-09-30", "0"},
                                 {"2017-12-31", "50000000"},
                                 {"2018-03-31", "50000000"},
                                 {"2018-06-30", "-50000000"}};
  for (const auto& [date, change] : restricted) {
    text += cash("RESTR", date, "1000000000");
    if (std::string{change}.empty()) {
      continue;
    }
    text += flow("RESTR", date, change);
  }
  // FY2010: Q3's revenue stored twice over. FY2011: the year is wrong.
  const char* fy2010[] = {"1000000000", "1100000000", "2400000000", "1300000000"};
  const char* fy2011[] = {"1000000000", "1050000000", "1100000000", "1150000000"};
  const char* q2011[] = {"2011-03-31", "2011-06-30", "2011-09-30", "2011-12-31"};
  for (std::size_t q = 0; q < 4; ++q) {
    text += income("QSUM", "quarter", kQuarters[q], "2010", fy2010[q]);
    text += income("QSUM", "quarter", q2011[q], "2011", fy2011[q]);
  }
  text += income("QSUM", "annual", "2010-12-31", "2010", "4600000000");
  text += income("QSUM", "annual", "2011-12-31", "2011", "5300000000");
  // fafnir#217's shape: a change of fiscal year leaves FY2012 with two years in
  // force, a transition period and the year. Not judged, though nothing sums.
  for (const char* date : {"2012-03-31", "2012-06-30", "2012-09-30", "2012-12-31"}) {
    text += income("QSUM", "quarter", date, "2012", "1000000000");
  }
  text += income("QSUM", "annual", "2012-06-30", "2012", "9000000000");
  text += income("QSUM", "annual", "2012-12-31", "2012", "9000000000");
  // FY2013 has three quarters: not judged.
  for (const char* date : {"2013-03-31", "2013-06-30", "2013-09-30"}) {
    text += income("QSUM", "quarter", date, "2013", "1000000000");
  }
  text += income("QSUM", "annual", "2013-12-31", "2013", "9000000000");
  return text;
}

constexpr const char* kTieItems =
    "column,statement,item_class,sign_convention,is_subtotal\n"
    "cash_and_equivalents,balance_sheet,stock,signed,false\n"
    "net_change_in_cash,cash_flow,flow,signed,true\n"
    "revenue,income,flow,positive_is_inflow,false\n"
    "cost_of_revenue,income,flow,positive_is_outflow,false\n";

constexpr const char* kTieConfig =
    "[statements]\n"
    "cash_tie = [\"cash_and_equivalents\", \"net_change_in_cash\"]\n";

const std::string kQuarter = R"("period_type":"quarter",)";

}  // namespace

TEST_CASE("DQ903: a gap is weighed as structural, a reclassification, a field error or none") {
  const TempDir dir;
  write_file(dir.path() / "s.csv", identity_rows());
  write_file(dir.path() / "st.toml", kIdentityConfig);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  const Result r = run_in(dir.path(), {"--config", config.c_str(), "--select", "DQ903", "--show-info",
                                       "--format", "jsonl", "--exit-zero", file.c_str()});
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::string sheet_key = R"({"statement":"balance_sheet",)" + kQuarter;
  const std::string q3 = R"("fiscal_date":"2010-09-30","version_no":1,"identity":"balance_sheet")";
  std::vector<std::string> expected{
      "FIELD " + sheet_key + q3 + R"(,"item":"total_liabilities"})",
      "NCI " + sheet_key + q3 + "}",
      "RECLASS " + sheet_key + q3 + "}",
      "SIGN " + sheet_key +
          R"("fiscal_date":"2010-06-30","version_no":1,"identity":"balance_sheet","item":"total_equity_parent"})",
      "UNEXPL " + sheet_key + q3 + "}",
  };
  for (const char* q : kQuarters) {
    expected.push_back("STRUCT " + sheet_key + R"("fiscal_date":")" + q +
                       R"(","version_no":1,"identity":"balance_sheet"})");
  }
  std::sort(expected.begin(), expected.end());
  CHECK(reports_of(r.out, "DQ903") == expected);

  // A field error names its field, at the severity its probability gives, with
  // the item to check.
  const std::string field = report(r.out, "DQ903", "FIELD", "2010-09-30");
  CHECK(contains(field, "balance_sheet is off by 5994000000 (tolerance 50000000): the gap is "
                        "total_liabilities stored at x0.001"));
  CHECK(contains(field, R"("severity":"error")"));
  CHECK(contains(field, R"("classification":"data_error")"));
  CHECK(contains(field, R"("suggested_action":{"kind":"check_item","item":"total_liabilities")"));
  CHECK(contains(field, R"("field":"total_liabilities")"));
  CHECK(contains(field, R"("neighbours_gap":0)"));
  CHECK(contains(report(r.out, "DQ903", "SIGN", "2010-06-30"),
                 "the gap is twice total_equity_parent: its sign"));

  // Filing facts are info: the company's structure, with NCI or without, and a
  // reclassification.
  for (const std::string& line : lines_with(r.out, R"("series":"STRUCT")")) {
    CHECK(contains(line, "as in the periods around it"));
    CHECK(contains(line, R"("severity":"info")"));
    CHECK(contains(line, R"("classification":"market_fact")"));
  }
  const std::string nci = report(r.out, "DQ903", "NCI", "2010-09-30");
  CHECK(contains(nci, "as in the periods around it, with net_income_noncontrolling reported"));
  CHECK(contains(nci, R"("severity":"info")"));
  const std::string reclass = report(r.out, "DQ903", "RECLASS", "2010-09-30");
  CHECK(contains(reclass, "another version holds, with the same total_assets"));
  CHECK(contains(reclass, R"("severity":"info")"));
  CHECK(contains(reclass, R"("classification":"market_fact")"));

  // Unexplained: a data error that names no field. The NULL term is not judged.
  const std::string unexplained = report(r.out, "DQ903", "UNEXPL", "2010-09-30");
  CHECK(contains(unexplained, "no term explains it"));
  CHECK(contains(unexplained, R"("classification":"data_error")"));
  CHECK(contains(unexplained, R"("suggested_action":null)"));
  CHECK_FALSE(contains(r.out, "2011-06-30"));

  // The text line names the identity and the period's version.
  const Result text = run_in(dir.path(), {"--config", config.c_str(), "--select", "DQ903",
                                          "--exit-zero", file.c_str()});
  CHECK(contains(text.out, "FIELD  2010-09-30 Q3 FY2010 balance_sheet v1  DQ903 error  "
                           "identity-break  balance_sheet is off by 5994000000"));

  // No identities, nothing to judge.
  const Result none =
      run_in(dir.path(), {"--isolated", "--select", "DQ903", "--exit-zero", file.c_str()});
  CHECK(count_matching(none.out, "DQ903") == 0);
}

TEST_CASE("DQ903: explained_by must name a configured identity") {
  const TempDir dir;
  write_file(dir.path() / "s.csv", identity_rows());
  write_file(dir.path() / "st.toml",
             "[statements.identities]\n"
             "balance_sheet = \"total_assets = total_liabilities + total_equity_parent\"\n"
             "[statements.explained_by]\n"
             "balance = [\"net_income_noncontrolling\"]\n");
  const std::string file = (dir.path() / "s.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  const Result r = run_in(dir.path(), {"--config", config.c_str(), file.c_str()});
  CHECK(r.status == 2);
  CHECK(contains(r.err, "statements.explained_by names the identity \"balance\", which "
                        "statements.identities does not define"));
}

TEST_CASE("DQ904: the cash tie, restricted cash, and a year's quarters against the year") {
  const TempDir dir;
  write_file(dir.path() / "s.csv", tie_rows());
  write_file(dir.path() / "items.csv", kTieItems);
  write_file(dir.path() / "st.toml", kTieConfig);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items = (dir.path() / "items.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  const Result r =
      run_in(dir.path(), {"--config", config.c_str(), "--items", items.c_str(), "--select", "DQ904",
                          "--format", "jsonl", "--exit-zero", file.c_str()});
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> expected{
      R"(CASH {"statement":"cash_flow",)" + kQuarter +
          R"("fiscal_date":"2010-06-30","version_no":1,"tie":"cash"})",
      R"(PAIR {"statement":"balance_sheet",)" + kQuarter +
          R"("fiscal_date":"2010-06-30","version_no":1,"tie":"cash","item":"cash_and_equivalents"})",
      R"(QSUM {"statement":"income","period_type":"annual","fiscal_date":"2011-12-31","version_no":1,"tie":"quarter_sum"})",
      R"(QSUM {"statement":"income",)" + kQuarter +
          R"("fiscal_date":"2010-09-30","version_no":1,"tie":"quarter_sum"})",
      R"(RESTR {"statement":"cash_flow",)" + kQuarter +
          R"("fiscal_date":"2017-12-31","version_no":1,"tie":"cash"})",
  };
  const std::vector<std::string> got = reports_of(r.out, "DQ904");
  CHECK(got == expected);
  // No two reports share a key: fafnir's open-condition index would refuse the second.
  CHECK(std::set<std::string>(got.begin(), got.end()).size() == got.size());

  CHECK(contains(report(r.out, "DQ904", "CASH", "2010-06-30"),
                 "the change in cash_and_equivalents since 2010-03-31 is not net_change_in_cash: "
                 "off by -200000000 (tolerance 24000000)"));
  const std::string pair = report(r.out, "DQ904", "PAIR", "2010-06-30");
  CHECK(contains(pair, "cash_and_equivalents is out of line with the cash flows either side: off "
                       "by 300000000, then by -300000000"));
  CHECK(contains(pair, R"("severity":"error")"));
  // Restricted cash: within 10% of cash from 2018, so only 2017's gap is a break.
  CHECK(contains(report(r.out, "DQ904", "RESTR", "2017-12-31"), "(tolerance 20000000)"));
  CHECK_FALSE(contains(r.out, "2018-0"));
  CHECK_FALSE(contains(r.out, "2011-09-30"));  // beyond the spacing

  // The quarter sum names the wrong quarter, or the year.
  const std::string quarter = report(r.out, "DQ904", "QSUM", "2010-09-30");
  CHECK(contains(quarter, "the quarters of FY2010 do not sum to the year: revenue by 1200000000; "
                          "this quarter is out of line"));
  CHECK(contains(quarter, R"("hypotheses":{"quarter_error":0.9,"unexplained":0.1})"));
  CHECK(contains(quarter, R"("year_fiscal_date":"2010-12-31")"));
  CHECK(contains(report(r.out, "DQ904", "QSUM", "2011-12-31"),
                 "the year is out of line with its quarters"));
  CHECK_FALSE(contains(r.out, "FY2012"));
  CHECK_FALSE(contains(r.out, "FY2013"));

  // Without --items no item is known to be a flow: the cash tie alone.
  const Result cash_only = run_in(dir.path(), {"--config", config.c_str(), "--select", "DQ904",
                                               "--format", "jsonl", "--exit-zero", file.c_str()});
  CHECK(reports_of(cash_only.out, "DQ904").size() == 3);
  CHECK_FALSE(contains(cash_only.out, "quarter_sum"));
  // And without cash_tie, nothing.
  const Result none =
      run_in(dir.path(), {"--isolated", "--select", "DQ904", "--exit-zero", file.c_str()});
  CHECK(count_matching(none.out, "DQ904") == 0);
}

TEST_CASE("statements: explained_by, cash_tie and restricted cash parse and round-trip") {
  const dorq::Config config = dorq::parse_config(R"(
[statements]
cash_tie = ["cash_and_equivalents", "net_change_in_cash"]
restricted_cash_from = "2019-01-01"
restricted_cash_share = 0.2
[statements.identities]
balance_sheet = "total_assets = total_liabilities + total_equity_parent"
net_income = "net_income = consolidated_income - net_income_noncontrolling"
[statements.explained_by]
net_income = ["preferred_dividends"]
balance_sheet = ["net_income_noncontrolling", "temporary_equity"]
)",
                                                 "s.toml", false);
  const dorq::StatementSettings& st = config.statements;
  CHECK(st.cash_tie == std::vector<std::string>{"cash_and_equivalents", "net_change_in_cash"});
  CHECK(st.restricted_cash_from == dorq::Date::from_ymd(2019, 1, 1));
  CHECK(st.restricted_cash_share == doctest::Approx(0.2));
  REQUIRE(st.explained_by.size() == 2);
  CHECK(st.explained_by[0].first == "balance_sheet");  // sorted
  CHECK(st.explained_by[0].second ==
        std::vector<std::string>{"net_income_noncontrolling", "temporary_equity"});

  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(again.statements == config.statements);
  CHECK(contains(dorq::to_toml(config), "[statements.explained_by]"));
  // The defaults leave the hash as it was.
  CHECK(dorq::config_hash(dorq::Config{}) == "803c581d98ba409a");
  dorq::Config other;
  other.statements.restricted_cash_share = 0.05;
  CHECK(dorq::config_hash(other) != dorq::config_hash(dorq::Config{}));

  for (const char* bad :
       {"[statements]\ncash_tie = [\"cash_and_equivalents\"]\n",
        "[statements]\nrestricted_cash_share = 1.5\n",
        "[statements]\nrestricted_cash_from = \"2018\"\n",
        "[statements.explained_by]\nbalance_sheet = [1]\n", "[statements]\nexplained_by = 3\n"}) {
    CAPTURE(bad);
    CHECK_THROWS_AS((void)dorq::parse_config(bad, "bad.toml", false), dorq::ConfigError);
  }
}
