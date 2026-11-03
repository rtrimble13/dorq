// M9, DR-0906: the statement value model, DQ911 bad-value, DQ912 unit-error,
// DQ913 sign-error and DQ919 large-change, through the CLI.

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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
                    double season = 0.0, bool annual = false) {
  std::string text;
  Values year{};  // the year's income and cash flow, as filed before any edit
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
      if (s != "balance_sheet") {
        for (std::size_t k = 0; k < 4; ++k) {
          if (!std::isnan(v.at(k))) {
            year.at(k) += static_cast<double>(std::llround(v.at(k)));
          }
        }
      }
      if (edit) {
        edit(q, s, v);
      }
      text += head;
      text += s;
      text += ",quarter";
      text += tail;
      for (const double x : v) {
        text += ',';
        if (!std::isnan(x)) {
          text += whole(x);
        }
      }
      text += "\n";
    }
    if (annual && q % 4 == 3) {
      // The year: its quarters' sums, as a year's flows are.
      text += id;
      text += ",income,annual,";
      text += end;
      text += ',';
      text += end.substr(0, 4);
      text += ",FY,1,2020-01-01,,,";
      for (std::size_t k = 0; k < 7; ++k) {
        text += ',';
        if (k < 3) {
          text += whole(year.at(k));
        }
      }
      text += "\n";
      year = {};
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
             const std::string& extra = {}, const std::string& settings = {}) {
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "st.toml", settings + kConfig);
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

constexpr const char* kIncomeQ = R"({"statement":"income","period_type":"quarter",)";

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
      std::string{"VALUE "} + kIncomeQ +
          R"("fiscal_date":"2012-09-30","version_no":1,"item":"revenue"})",
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
        std::vector<std::string>{std::string{"BAD "} + kIncomeQ +
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
  for (const char* code : {"DQ911", "DQ912", "DQ913", "DQ914", "DQ915", "DQ916"}) {
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
  const std::string fixed = company("FIXED", [](int q, const std::string& s, Values& v) {
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
  // copy_min_items (DQ914) round-trips, and leaves the hash alone at its default.
  const dorq::Config copies =
      dorq::parse_config("[statements]\ncopy_min_items = 7\n", "c.toml", false);
  CHECK(copies.statements.copy_min_items == 7);
  CHECK(dorq::parse_config(dorq::to_toml(copies), "again.toml", false).statements ==
        copies.statements);
  CHECK(dorq::config_hash(copies) != dorq::config_hash(dorq::Config{}));
  CHECK_THROWS_AS((void)dorq::parse_config("[statements]\ncopy_min_items = 1\n", "bad.toml", false),
                  dorq::ConfigError);
  for (const char* bad :
       {"[statements.priors]\nbad_value = 0.6\n",
        "[statements.priors]\nbad_value = 0.4\nunit_error = 0.4\nsign_error = 0.2\n",
        "[statements.priors]\nflow_scale = 0\n", "[statements.priors]\nreal_change = 0.5\n",
        "[statements.priors]\nidentity_structural = 0\n",
        "[statements.priors]\nidentity_unexplained = 2\n",
        "[statements.priors]\nfiling_late = 0.6\n",
        "[statements.priors]\nfiling_late = 0.5\nfiling_early = 0.3\nfiling_amendment = 0.2\n",
        "[statements.calibration]\ndq95 = [[0.1, 0.2]]\n",
        "[statements.calibration]\ndq91 = [[0.5, 0.4], [0.2, 0.6]]\n",
        "[statements.calibration]\ndq91 = [[0.2, 0.6], [0.5, 0.4]]\n",
        "[statements.calibration]\nversion = 3\n"}) {
    CAPTURE(bad);
    CHECK_THROWS_AS((void)dorq::parse_config(bad, "bad.toml", false), dorq::ConfigError);
  }
}

TEST_CASE("statements: the identity and filing priors and [statements.calibration] (DR-0911)") {
  // The defaults are the weights the checks shipped with, so a config without the
  // new keys judges as before.
  const dorq::StatementPriors defaults;
  CHECK(std::log(defaults.identity_structural) == doctest::Approx(-1.4));
  CHECK(std::log(defaults.identity_reclassification) == doctest::Approx(-3.0));
  CHECK(std::log(defaults.identity_field_error) == doctest::Approx(-1.05));
  CHECK(std::log(defaults.identity_unexplained) == doctest::Approx(-1.05));
  CHECK(defaults.filing_late == doctest::Approx(0.01));
  CHECK(defaults.filing_early == doctest::Approx(0.002));
  CHECK(defaults.filing_amendment == doctest::Approx(0.002));
  CHECK(dorq::StatementSettings{}.calibration.empty());

  const dorq::Config config = dorq::parse_config(R"(
[statements.priors]
identity_structural = 0.5
identity_reclassification = 0.01
identity_field_error = 1
identity_unexplained = 0.2
filing_late = 0.02
filing_early = 0.001
filing_amendment = 0.003

[statements.calibration]
version = "fafnir 2026-11"
dq90 = [[0.01, 0.0], [0.9, 1.0]]
dq93 = [[0.1, 0.05], [0.5, 0.25], [0.9, 0.95]]
)",
                                                 "s.toml", false);
  const dorq::StatementPriors& p = config.statements.priors;
  CHECK(p.identity_structural == doctest::Approx(0.5));
  CHECK(p.identity_field_error == doctest::Approx(1.0));
  CHECK(p.filing_late == doctest::Approx(0.02));
  CHECK(p.filing_amendment == doctest::Approx(0.003));
  const dorq::StatementCalibration& c = config.statements.calibration;
  CHECK(c.version == "fafnir 2026-11");
  CHECK(c.maps.at(0).size() == 2);
  CHECK(c.maps.at(1).empty());
  CHECK(c.maps.at(3).size() == 3);
  CHECK_FALSE(c.empty());
  // The map is its sub-family's: linear between knots, flat beyond them.
  CHECK(c.apply("DQ931", 0.0) == doctest::Approx(0.05));
  CHECK(c.apply("DQ932", 0.3) == doctest::Approx(0.15));
  CHECK(c.apply("DQ931", 0.7) == doctest::Approx(0.6));
  CHECK(c.apply("DQ931", 1.0) == doctest::Approx(0.95));
  CHECK(c.apply("DQ903", 0.005) == doctest::Approx(0.0));
  CHECK(c.apply("DQ904", 0.455) == doctest::Approx(0.5));
  CHECK(c.apply("DQ912", 0.37) == 0.37);  // DQ91x has no map
  CHECK(c.apply("DQ901", 0.5) == 0.5);    // a rule's p_error, not a model's
  CHECK(c.apply("DQ944", 0.5) == 0.5);
  CHECK(c.apply("DQ951", 0.37) == 0.37);  // DQ95x is not mapped
  CHECK(c.apply("DQ201", 0.37) == 0.37);  // nor is a bars check
  CHECK(dorq::StatementCalibration::family("DQ903") == std::optional<std::size_t>{0});
  CHECK(dorq::StatementCalibration::family("DQ944") == std::optional<std::size_t>{4});
  CHECK_FALSE(dorq::StatementCalibration::family("DQ951").has_value());
  CHECK_FALSE(dorq::StatementCalibration::family("DQ90").has_value());
  CHECK_FALSE(dorq::StatementCalibration::family("DQ203").has_value());
  for (const char* code :
       {"DQ903", "DQ904", "DQ911", "DQ916", "DQ919", "DQ921", "DQ922", "DQ931", "DQ932", "DQ942"}) {
    CAPTURE(code);
    CHECK(dorq::StatementCalibration::mapped(code));
  }
  for (const char* code : {"DQ901", "DQ902", "DQ941", "DQ943", "DQ944", "DQ951", "DQ201"}) {
    CAPTURE(code);
    CHECK_FALSE(dorq::StatementCalibration::mapped(code));
  }

  // Both round-trip through config show, and change the hash.
  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(again.statements == config.statements);
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(dorq::config_hash(config) != dorq::config_hash(dorq::Config{}));
  dorq::Config mapped_only;
  mapped_only.statements.calibration = c;
  CHECK(dorq::config_hash(mapped_only) != dorq::config_hash(dorq::Config{}));
  // A table that sets only the value model's keys hashes as it did before them.
  const dorq::Config values_only =
      dorq::parse_config("[statements.priors]\nbad_value = 0.002\n", "v.toml", false);
  CHECK(dorq::config_hash(values_only) == "1d37e0c08c3bd549");  // dorq 0.13.0's
  dorq::Config filings_only;
  filings_only.statements.priors.filing_late = 0.02;
  CHECK(dorq::config_hash(filings_only) != dorq::config_hash(dorq::Config{}));
}

TEST_CASE("DQ914, DQ915 and DQ916: a copied quarter, an absent zero, quarters to date") {
  const TempDir dir;
  std::string csv;
  // 2012 Q3's income statement repeats Q2's, to the unit.
  Values copied{};
  csv += company("COPY", [&copied](int q, const std::string& s, Values& v) {
    if (s == "income" && q == 9) {
      copied = v;
    }
    if (s == "income" && q == 10) {
      v = copied;
    }
  });
  // Cost of revenue left at 0 in 2013 Q1, with gross profit as filed.
  csv += company("ZERO", [](int q, const std::string& s, Values& v) {
    if (s == "income" && q == 12) {
      v[1] = 0.0;
    }
  });
  // FY2012's Q2 to Q4 hold the year to date.
  Values to_date{};
  csv += company(
      "YTD",
      [&to_date](int q, const std::string& s, Values& v) {
        if (s != "income" || q < 8 || q > 11) {
          return;
        }
        for (std::size_t k = 0; k < 3; ++k) {
          to_date.at(k) =
              (q == 8 ? 0.0 : to_date.at(k)) + static_cast<double>(std::llround(v.at(k)));
          v.at(k) = to_date.at(k);
        }
      },
      28, 0.0, true);
  const Result r = check(dir, csv, true, {}, "[statements]\ncopy_min_items = 3\n");
  INFO(r.err);
  REQUIRE(r.status == 0);
  CHECK(reports_of(r.out, "DQ914") ==
        std::vector<std::string>{std::string{"COPY "} + kIncomeQ +
                                 R"("fiscal_date":"2012-09-30","version_no":1})"});
  CHECK(reports_of(r.out, "DQ915") ==
        std::vector<std::string>{
            std::string{"ZERO "} + kIncomeQ +
            R"("fiscal_date":"2013-03-31","version_no":1,"item":"cost_of_revenue"})"});
  CHECK(reports_of(r.out, "DQ916") ==
        std::vector<std::string>{std::string{"YTD "} + kIncomeQ +
                                 R"("fiscal_date":"2012-06-30","version_no":1})"});
  const std::string copy = report(r.out, "DQ914", "COPY");
  CHECK(contains(copy, "the row repeats the period before it, to the unit, on 3 of 3 items"));
  CHECK(contains(copy, R"("hypotheses":{"real_change":)"));
  const std::string zero = report(r.out, "DQ915", "ZERO");
  CHECK(contains(zero, "cost_of_revenue is 0, reported either side"));
  CHECK(contains(zero, R"("feature":"identity_repair","value":"gross_profit")"));
  const std::string ytd = report(r.out, "DQ916", "YTD");
  CHECK(contains(ytd, "the quarters of FY2012 are stored year to date"));
  CHECK(contains(ytd, R"("end_date":"2012-12-31")"));
  // The rows they explain are no bad values, and no large changes either.
  for (const char* series : {"COPY", "YTD"}) {
    CAPTURE(series);
    CHECK(report(r.out, "DQ911", series).empty());
    CHECK(report(r.out, "DQ919", series).empty());
  }
}

TEST_CASE("DQ916: a quarter holding its year, and a year with two years in force") {
  const TempDir dir;
  // FY2013's Q3 holds the year: the quarters as filed, Q3's put to their sum.
  const auto holds_year = [](const std::string& id, bool second_year) {
    const std::string text = company(id, {}, 28, 0.0, true);
    std::vector<std::string> rows;
    std::istringstream in(text);
    std::array<long long, 3> year{};
    for (std::string line; std::getline(in, line);) {
      rows.push_back(line);
    }
    for (const std::string& row : rows) {
      if (contains(row, ",income,annual,2013-12-31,")) {
        std::vector<std::string> f;
        std::stringstream ss(row);
        for (std::string x; std::getline(ss, x, ',');) {
          f.push_back(x);
        }
        for (std::size_t k = 0; k < 3; ++k) {
          year.at(k) = std::stoll(f.at(11 + k));
        }
      }
    }
    std::string out;
    for (std::string row : rows) {
      if (contains(row, ",income,quarter,2013-09-30,")) {
        std::vector<std::string> f;
        std::stringstream ss(row);
        for (std::string x; std::getline(ss, x, ',');) {
          f.push_back(x);
        }
        for (std::size_t k = 0; k < 3; ++k) {
          f.at(11 + k) = std::to_string(year.at(k));
        }
        row.clear();
        for (std::size_t k = 0; k < f.size(); ++k) {
          row += (k > 0 ? "," : "") + f[k];
        }
        while (std::count(row.begin(), row.end(), ',') < 17) {
          row += ",";
        }
      }
      out += row + "\n";
      if (second_year && contains(row, ",income,annual,2013-12-31,")) {
        // A transition period labelled with the year it shares: two years in force.
        std::string transition = row;
        transition.replace(transition.find("2013-12-31"), 10, "2013-06-30");
        out += transition + "\n";
      }
    }
    return out;
  };
  const Result r = check(dir, holds_year("AIQ", false) + holds_year("TWO", true));
  INFO(r.err);
  REQUIRE(r.status == 0);
  CHECK(reports_of(r.out, "DQ916") ==
        std::vector<std::string>{std::string{"AIQ "} + kIncomeQ +
                                 R"("fiscal_date":"2013-09-30","version_no":1})"});
  CHECK(contains(report(r.out, "DQ916", "AIQ"),
                 "the quarter holds FY2013's flows (3 of 3 equal the year's)"));
  CHECK(report(r.out, "DQ916", "TWO").empty());
}

// ---------------------------------------------------------------------------
// DR-0911: statements as they stood (--restore), and dorq calibrate on statements

namespace {

// The rows of `csv` whose fiscal date is `date` (and statement, when given).
std::string rows_at(const std::string& csv, const std::string& date,
                    const std::string& statement = {}) {
  std::string out;
  for (const std::string& line : lines_with(csv, "," + date + ",")) {
    if (statement.empty() || contains(line, "," + statement + ",")) {
      out += line + "\n";
    }
  }
  return out;
}

std::string read_text(const std::filesystem::path& path) {
  const std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// `csv` without the rows of fiscal date `date`.
std::string without(const std::string& csv, const std::string& date) {
  std::string out;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    if (!contains(line, "," + date + ",")) {
      out += line + "\n";
    }
  }
  return out;
}

std::string statements_label(const std::string& series, const std::string& date,
                             const std::string& statement, const std::string& item,
                             const std::string& expect) {
  return R"({"series":")" + series + R"(","date":")" + date +
         R"(","class":"data_error","statement":")" + statement +
         R"(","period_type":"quarter","version_no":1,)" +
         (item.empty() ? std::string{} : R"("item":")" + item + "\",") + R"("expect":")" + expect +
         "\"}\n";
}

}  // namespace

TEST_CASE("--restore on statements: superseded rows by their key, and missing ones added") {
  const TempDir dir;
  // SF1 corrected 2012 Q3's revenue, first loaded in thousands where units were
  // meant, and the series holds the corrected row. 2012 Q4 is missing from the
  // series; the restore file holds it as it stood.
  const std::string current = without(company("FIXED"), quarter_end(11));
  const std::string first = company("FIXED", [](int q, const std::string& s, Values& v) {
    if (q == 10 && s == "income") {
      v[0] *= 1000.0;
    }
  });
  const std::string restore = std::string{kHeader} + rows_at(first, quarter_end(10), "income") +
                              rows_at(first, quarter_end(11));
  write_file(dir.path() / "s.csv", std::string{kHeader} + current);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "st.toml", kConfig);
  write_file(dir.path() / "before.csv", restore);
  write_file(dir.path() / "labels.jsonl",
             statements_label("FIXED", quarter_end(10), "income", "revenue", "DQ912"));
  const auto run = [&dir](bool restored) {
    const std::string file = (dir.path() / "s.csv").string();
    const std::string items = (dir.path() / "items.csv").string();
    const std::string config = (dir.path() / "st.toml").string();
    const std::string before = (dir.path() / "before.csv").string();
    const std::string labels = (dir.path() / "labels.jsonl").string();
    if (restored) {
      return run_in(dir.path(), {"--config", config.c_str(), "--items", items.c_str(), "--select",
                                 "DQ9", "--format", "jsonl", "--exit-zero", "--labels",
                                 labels.c_str(), "--restore", before.c_str(), file.c_str()});
    }
    return run_in(dir.path(), {"--config", config.c_str(), "--items", items.c_str(), "--select",
                               "DQ9", "--format", "jsonl", "--exit-zero", file.c_str()});
  };
  const Result plain = run(false);
  INFO(plain.err);
  REQUIRE(plain.status == 0);
  CHECK(reports_of(plain.out, "DQ912").empty());
  CHECK_FALSE(reports_of(plain.out, "DQ941").empty());  // 2012 Q4 is missing

  const Result restored = run(true);
  INFO(restored.err);
  REQUIRE(restored.status == 0);
  CHECK(reports_of(restored.out, "DQ912") ==
        std::vector<std::string>{std::string{"FIXED "} + kIncomeQ +
                                 R"("fiscal_date":"2012-09-30","version_no":1,"item":"revenue"})"});
  CHECK(reports_of(restored.out, "DQ941").empty());  // added from the restore file
}

TEST_CASE("dorq calibrate on statements: the priors and maps, which dorq check reads") {
  const TempDir dir;
  std::string csv;
  std::string labels;
  // Units: revenue in thousands where units were meant, in four companies.
  for (int c = 0; c < 4; ++c) {
    const std::string id = "UNIT" + std::to_string(c);
    const int at = 8 + 3 * c;
    csv += company(id, [at](int q, const std::string& s, Values& v) {
      if (q == at && s == "income") {
        v[0] *= 1000.0;
      }
    });
    labels += statements_label(id, quarter_end(at), "income", "revenue", "DQ912");
  }
  // Bad values: capital expenditure five times too large, in three.
  for (int c = 0; c < 3; ++c) {
    const std::string id = "BAD" + std::to_string(c);
    const int at = 9 + 4 * c;
    csv += company(id, [at](int q, const std::string& s, Values& v) {
      if (q == at && s == "cash_flow") {
        v[3] *= 5.0;
      }
    });
    labels += statements_label(id, quarter_end(at), "cash_flow", "capital_expenditure", "DQ911");
  }
  // A dropped term: gross profit stored as revenue. The label names the period, not
  // an item: DQ903 keys the term it suspects (cost_of_revenue).
  csv += company("GAP", [](int q, const std::string& s, Values& v) {
    if (q == 12 && s == "income") {
      v[2] = v[0];
    }
  });
  labels += statements_label("GAP", quarter_end(12), "income", "", "DQ903");
  for (int c = 0; c < 6; ++c) {
    csv += company("CLEAN" + std::to_string(c));
  }
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  write_file(dir.path() / "st.toml", kConfig);
  write_file(dir.path() / "labels.jsonl", labels);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items = (dir.path() / "items.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  const std::string labels_file = (dir.path() / "labels.jsonl").string();
  const auto fit = [&](const char* out, const char* threads) {
    return run_in(dir.path(),
                  {"calibrate", file.c_str(), "--labels", labels_file.c_str(), "--items",
                   items.c_str(), "--config", config.c_str(), "--clean-unlabelled", "--threads",
                   threads, "--name", "synthetic", "--out", out});
  };
  const Result first = fit("fit.toml", "2");
  INFO(first.err);
  REQUIRE(first.status == 0);
  CHECK(contains(first.out, "labels: 8 (8 about statements, 8 matched)"));
  CHECK(contains(first.out, "values: 7 observations (7 errors)"));
  CHECK(contains(first.out, "identities: 1 observations (1 errors)"));
  CHECK(contains(first.out, "filings: 0 observations"));
  const std::string toml = read_text(dir.path() / "fit.toml");
  CHECK(contains(toml, "[statements.priors]"));
  CHECK(contains(toml, "unit_error = "));
  CHECK(contains(toml, "identity_field_error = "));
  CHECK(contains(toml, "filing_early = "));
  CHECK(contains(toml, "[statements.calibration]"));
  CHECK(contains(toml, "version = \"synthetic\""));
  CHECK(contains(toml, "dq91 = "));
  // Deterministic: one thread writes the same file.
  const Result again = fit("again.toml", "1");
  REQUIRE(again.status == 0);
  CHECK(read_text(dir.path() / "again.toml") == toml);

  // dorq check reads it, and still finds the faults.
  write_file(dir.path() / "fitted.toml", "include = \"fit.toml\"\n" + std::string{kConfig});
  const std::string fitted = (dir.path() / "fitted.toml").string();
  const Result checked =
      run_in(dir.path(), {"--config", fitted.c_str(), "--items", items.c_str(), "--select", "DQ9",
                          "--format", "jsonl", "--exit-zero", file.c_str()});
  INFO(checked.err);
  REQUIRE(checked.status == 0);
  CHECK(reports_of(checked.out, "DQ912").size() == 4);
  CHECK(reports_of(checked.out, "DQ911").size() == 3);
  CHECK_FALSE(reports_of(checked.out, "DQ903").empty());
  const Result shown = run_in(dir.path(), {"config", "show", "--config", fitted.c_str()});
  CHECK(contains(shown.out, "[statements.calibration]"));
  CHECK(contains(shown.out, "version = \"synthetic\""));
}
