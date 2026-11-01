// M9, DR-0906: the statement value model, DQ911 bad-value, DQ912 unit-error,
// DQ913 sign-error and DQ919 large-change, through the CLI.

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
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

std::string record_key(const std::string& line) {
  const auto at = line.find(R"("record_key":)");
  return at == std::string::npos ? std::string{}
                                 : line.substr(at + 13, line.find('}', at) - at - 12);
}

// "SERIES {record_key}" of each report of `code` at warn or above, sorted.
std::vector<std::string> reports_of(const std::string& out, const std::string& code) {
  std::vector<std::string> got;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    if (!contains(line, R"("severity":"info")")) {
      got.push_back(field_of(line, "series") + " " + record_key(line));
    }
  }
  std::sort(got.begin(), got.end());
  return got;
}

std::string report(const std::string& out, const std::string& code, const std::string& series) {
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    if (field_of(line, "series") == series) {
      return line;
    }
  }
  return {};
}

constexpr const char* kHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,revenue,cost_of_revenue,gross_profit,"
    "capital_expenditure,total_assets,total_liabilities,total_equity_parent\n";

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A quarter's end: 2010 Q1 is quarter 0.
std::string quarter_end(int q) {
  const int year = 2010 + q / 4;
  const std::array<const char*, 4> ends{"-03-31", "-06-30", "-09-30", "-12-31"};
  return std::to_string(year) + ends.at(static_cast<std::size_t>(q % 4));
}

std::string whole(double v) { return std::to_string(std::llround(v)); }

// One company's quarters, 2010 to 2016: its three statements, smooth and growing,
// with a little noise. `edit` changes a quarter's values before they are written:
// (quarter, statement, values in the header's order from revenue).
using Values = std::array<double, 7>;
using Edit = std::function<void(int, const std::string&, Values&)>;

std::string company(const std::string& id, const Edit& edit = {}, int quarters = 28,
                    double season = 0.0) {
  std::string text;
  for (int q = 0; q < quarters; ++q) {
    const double t = q;
    const double seasonal = 1.0 + season * (q % 4 == 3 ? 1.0 : -season / 3.0);
    const double revenue = 1e9 * (1.0 + 0.02 * t) * (1.0 + 0.01 * std::sin(t * 1.7)) * seasonal;
    const double cost = 0.6 * revenue * (1.0 + 0.005 * std::cos(t));
    const double assets = 1e10 * (1.0 + 0.01 * t) * (1.0 + 0.004 * std::sin(t * 2.3));
    const double liabilities = 0.6 * assets * (1.0 + 0.003 * std::cos(t * 1.3));
    const double capex = -5e7 * (1.0 + 0.01 * t) * (1.0 + 0.02 * std::sin(t));
    const std::string end = quarter_end(q);
    const std::string head = id + ",";
    const std::string tail =
        "," + end + "," + end.substr(0, 4) + ",Q" + std::to_string(q % 4 + 1) + ",1,2020-01-01,,,";
    for (const char* statement : {"income", "cash_flow", "balance_sheet"}) {
      Values v{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
      const std::string s = statement;
      if (s == "income") {
        v = {revenue, cost, revenue - cost, kNaN, kNaN, kNaN, kNaN};
      } else if (s == "cash_flow") {
        v = {kNaN, kNaN, kNaN, capex, kNaN, kNaN, kNaN};
      } else {
        v = {kNaN, kNaN, kNaN, kNaN, assets, liabilities, assets - liabilities};
      }
      if (edit) {
        edit(q, s, v);
      }
      text += head + s + ",quarter" + tail;
      for (std::size_t k = 0; k < v.size(); ++k) {
        text += "," + (std::isnan(v.at(k)) ? std::string{} : whole(v.at(k)));
      }
      text += "\n";
    }
  }
  return text;
}

constexpr const char* kItems =
    "column,statement,item_class,sign_convention,is_subtotal\n"
    "revenue,income,flow,positive_is_inflow,false\n"
    "cost_of_revenue,income,flow,positive_is_outflow,false\n"
    "gross_profit,income,flow,signed,true\n"
    "capital_expenditure,cash_flow,flow,positive_is_inflow,false\n"
    "total_assets,balance_sheet,stock,signed,true\n"
    "total_liabilities,balance_sheet,stock,signed,true\n"
    "total_equity_parent,balance_sheet,stock,signed,true\n";

constexpr const char* kConfig =
    "[statements.identities]\n"
    "gross_profit = \"gross_profit = revenue - cost_of_revenue\"\n"
    "balance_sheet = \"total_assets = total_liabilities + total_equity_parent\"\n";

Result check(const TempDir& dir, const std::string& csv, bool items = true,
             const std::string& extra = {}) {
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "st.toml", kConfig);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items_file = (dir.path() / "items.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  if (items && !extra.empty()) {
    return run_in(dir.path(),
                  {"--config", config.c_str(), "--items", items_file.c_str(), "--select", "DQ91",
                   "--show-info", "--format", "jsonl", "--exit-zero", extra.c_str(), file.c_str()});
  }
  if (items) {
    return run_in(dir.path(),
                  {"--config", config.c_str(), "--items", items_file.c_str(), "--select", "DQ91",
                   "--show-info", "--format", "jsonl", "--exit-zero", file.c_str()});
  }
  return run_in(dir.path(), {"--config", config.c_str(), "--select", "DQ91", "--show-info",
                             "--format", "jsonl", "--exit-zero", file.c_str()});
}

const std::string kIncomeQ = R"({"statement":"income","period_type":"quarter",)";

}  // namespace

TEST_CASE("DQ912: a value, a row and a run of quarters at a power of ten, each reported once") {
  const TempDir dir;
  std::string csv;
  // One value: revenue in 2012 Q3 stored in thousands where units were meant.
  csv += company("VALUE", [](int q, const std::string& s, Values& v) {
    if (q == 10 && s == "income") {
      v[0] *= 1000.0;
    }
  });
  // A whole row: the 2013 Q2 balance sheet at x0.001.
  csv += company("ROW", [](int q, const std::string& s, Values& v) {
    if (q == 13 && s == "balance_sheet") {
      for (double& x : v) {
        x *= 0.001;
      }
    }
  });
  // A run: the income statement of 2011 Q2 to 2012 Q1 at x1000.
  csv += company("ERA", [](int q, const std::string& s, Values& v) {
    if (q >= 5 && q <= 8 && s == "income") {
      for (double& x : v) {
        x *= 1000.0;
      }
    }
  });
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> expected{
      R"(ERA {"statement":"income","period_type":"quarter","fiscal_date":"2011-06-30","version_no":1,"scope":"era"})",
      R"(ROW {"statement":"balance_sheet","period_type":"quarter","fiscal_date":"2013-06-30","version_no":1,"scope":"row"})",
      "VALUE " + kIncomeQ + R"("fiscal_date":"2012-09-30","version_no":1,"item":"revenue"})",
  };
  CHECK(reports_of(r.out, "DQ912") == expected);
  CHECK(reports_of(r.out, "DQ911").empty());
  CHECK(reports_of(r.out, "DQ913").empty());

  const std::string value = report(r.out, "DQ912", "VALUE");
  CHECK(contains(value, "revenue is"));
  CHECK(contains(value, ": stored at x1000"));
  CHECK(contains(value, R"("power":3)"));
  CHECK(contains(
      value,
      R"("suggested_action":{"kind":"check_filing","item":"revenue","fiscal_date":"2012-09-30")"));
  CHECK(contains(value, R"("classification":"data_error")"));
  const std::string row = report(r.out, "DQ912", "ROW");
  CHECK(
      contains(row, "the row's money items are at x0.001 against the periods around it (3 items)"));
  const std::string era = report(r.out, "DQ912", "ERA");
  CHECK(contains(era, "the money items of 4 quarters, 2011-06-30 to 2012-03-31, are at x1000"));
  CHECK(contains(era, R"("end_date":"2012-03-31")"));
  CHECK(contains(era, R"("rows":4)"));
}

TEST_CASE("DQ911 and DQ913: one value off, and one sign flipped, with the evidence") {
  const TempDir dir;
  std::string csv;
  // Revenue at 2.5 times in 2012 Q3, with gross profit as filed: the identity breaks
  // and holds again with the value it needs.
  csv += company("BAD", [](int q, const std::string& s, Values& v) {
    if (q == 10 && s == "income") {
      v[0] *= 2.5;
    }
  });
  // Capital expenditure positive for one quarter, against its history.
  csv += company("SIGN", [](int q, const std::string& s, Values& v) {
    if (q == 14 && s == "cash_flow") {
      v[3] = -v[3];
    }
  });
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);
  CHECK(reports_of(r.out, "DQ911") ==
        std::vector<std::string>{"BAD " + kIncomeQ +
                                 R"("fiscal_date":"2012-09-30","version_no":1,"item":"revenue"})"});
  CHECK(
      reports_of(r.out, "DQ913") ==
      std::vector<std::string>{
          R"(SIGN {"statement":"cash_flow","period_type":"quarter","fiscal_date":"2013-09-30","version_no":1,"item":"capital_expenditure"})"});
  const std::string bad = report(r.out, "DQ911", "BAD");
  CHECK(contains(bad, "times): one value off the periods around it"));
  CHECK(contains(bad, R"("feature":"identity_repair","value":"gross_profit")"));
  CHECK(contains(bad, R"("hypotheses":{"real_change":)"));
  const std::string sign = report(r.out, "DQ913", "SIGN");
  CHECK(contains(sign, ": its sign is flipped"));
  CHECK(contains(sign, R"("feature":"sign_history")"));

  // DQ913 needs each item's sign convention: without --items, no sign is judged.
  const Result plain = check(dir, csv, false);
  CHECK(reports_of(plain.out, "DQ913").empty());
}

TEST_CASE("the value model: no warn on a real step, a seasonal business or a zero item") {
  const TempDir dir;
  std::string csv;
  // An acquisition: revenue, cost and gross profit up 40% from 2012 Q3, and they stay.
  csv += company("STEP", [](int q, const std::string& s, Values& v) {
    if (q >= 10 && s == "income") {
      for (double& x : v) {
        x *= 1.4;
      }
    }
  });
  // A retailer: every fourth quarter 60% above the rest.
  csv += company("SEASON", {}, 28, 0.6);
  // Capital expenditure that is mostly zero: DQ915's ground, not this model's.
  csv += company("ZEROS", [](int q, const std::string& s, Values& v) {
    if (s == "cash_flow" && q % 3 != 0) {
      v[3] = 0.0;
    }
  });
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);
  for (const char* code : {"DQ911", "DQ912", "DQ913"}) {
    CAPTURE(code);
    CHECK(reports_of(r.out, code).empty());
  }
  // A large change the evidence calls real is kept for review, at info.
  for (const std::string& line : lines_with(r.out, R"("code":"DQ919")")) {
    CHECK(contains(line, R"("severity":"info")"));
    CHECK(contains(line, R"("classification":"market_fact")"));
  }
}

TEST_CASE("the value model: the newest period is provisional, and versions are one report") {
  const TempDir dir;
  std::string csv;
  // The newest quarter at x1000: provisional, so at most warn.
  csv += company("NEW", [](int q, const std::string& s, Values& v) {
    if (q == 27 && s == "income") {
      v[0] *= 1000.0;
    }
  });
  // A x1000 in version 1 that version 2 fixes: one report, on version 1, which the
  // first-reported view still holds.
  std::string fixed = company("FIXED", [](int q, const std::string& s, Values& v) {
    if (q == 12 && s == "income") {
      v[0] *= 1000.0;
    }
  });
  const std::string q13 = quarter_end(12);
  std::string restated;
  std::istringstream in(fixed);
  for (std::string line; std::getline(in, line);) {
    if (contains(line, "FIXED,income,quarter," + q13)) {
      // v1 closed when v2 is known; v2 is v1 with revenue put right.
      std::string v1 = line;
      v1.replace(v1.find(",1,2020-01-01,,"), 15, ",1,2020-01-01,2021-01-01,");
      std::string v2 = line;
      v2.replace(v2.find(",1,2020-01-01,,"), 15, ",2,2021-01-01,,");
      restated += v1 + "\n";
      // Revenue is the 12th field: replace it with the value the others imply.
      std::vector<std::string> fields;
      std::stringstream ss(v2);
      for (std::string f; std::getline(ss, f, ',');) {
        fields.push_back(f);
      }
      fields.at(9) = "observed";
      fields.at(11) = std::to_string(std::stoll(fields.at(11)) / 1000);
      std::string joined;
      for (std::size_t k = 0; k < fields.size(); ++k) {
        joined += (k > 0 ? "," : "") + fields[k];
      }
      restated += joined + "\n";
    } else {
      restated += line + "\n";
    }
  }
  csv += restated;
  const Result r = check(dir, csv);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::string newest = report(r.out, "DQ912", "NEW");
  CHECK(contains(newest, R"("provisional":true)"));
  CHECK_FALSE(contains(newest, R"("severity":"error")"));
  const std::vector<std::string> fixed_reports = reports_of(r.out, "DQ912");
  CHECK(std::count_if(fixed_reports.begin(), fixed_reports.end(),
                      [](const std::string& k) { return contains(k, "FIXED"); }) == 1);
  CHECK(contains(report(r.out, "DQ912", "FIXED"),
                 R"("fiscal_date":")" + q13 + R"(","version_no":1,"item":"revenue"})"));
  CHECK(contains(report(r.out, "DQ912", "FIXED"), R"("views":"first_reported")"));
}

TEST_CASE("the value model: the same reports from rows in any order, on any thread count") {
  const TempDir dir;
  std::string csv = company("A", [](int q, const std::string& s, Values& v) {
    if (q == 9 && s == "income") {
      v[0] *= 1000.0;
    }
  });
  csv += company("B", [](int q, const std::string& s, Values& v) {
    if (q == 15 && s == "cash_flow") {
      v[3] = -v[3];
    }
  });
  const Result first = check(dir, csv);
  std::vector<std::string> rows;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    rows.push_back(line);
  }
  std::reverse(rows.begin(), rows.end());
  std::string shuffled;
  for (const std::string& row : rows) {
    shuffled += row + "\n";
  }
  const Result second = check(dir, shuffled, true, "--threads=4");
  INFO(second.err);
  for (const char* code : {"DQ911", "DQ912", "DQ913", "DQ919"}) {
    CAPTURE(code);
    CHECK(reports_of(first.out, code) == reports_of(second.out, code));
  }
  CHECK(count_matching(first.out, R"("code":"DQ912")") == 1);
}

TEST_CASE("statements: [statements.priors] parses, round-trips, and holds the old hashes") {
  const dorq::Config config = dorq::parse_config(R"(
[statements.priors]
bad_value = 0.002
unit_error = 0.0005
sign_error = 0.0001
flow_scale = 0.1
stock_scale = 0.04
prior_weight = 8
)",
                                                 "s.toml", false);
  const dorq::StatementPriors& p = config.statements.priors;
  CHECK(p.bad_value == doctest::Approx(0.002));
  CHECK(p.unit_error == doctest::Approx(0.0005));
  CHECK(p.prior_weight == doctest::Approx(8.0));
  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(again.statements == config.statements);
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(contains(dorq::to_toml(dorq::Config{}), "[statements.priors]"));  // config show
  CHECK(dorq::config_hash(dorq::Config{}) == "803c581d98ba409a");
  CHECK(dorq::config_hash(config) != dorq::config_hash(dorq::Config{}));
  for (const char* bad :
       {"[statements.priors]\nbad_value = 0.6\n",
        "[statements.priors]\nbad_value = 0.4\nunit_error = 0.4\nsign_error = 0.2\n",
        "[statements.priors]\nflow_scale = 0\n", "[statements.priors]\nreal_change = 0.5\n"}) {
    CAPTURE(bad);
    CHECK_THROWS_AS((void)dorq::parse_config(bad, "bad.toml", false), dorq::ConfigError);
  }
}
