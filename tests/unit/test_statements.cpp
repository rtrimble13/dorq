// M9, DR-0901: statements as a third series kind (ADR 0005): the reader, the
// views, --items, [statements] and how the engine treats them.

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "io/columns.hpp"
#include "io/input_error.hpp"
#include "io/reader.hpp"
#include "statements/views.hpp"
#include "test_support.hpp"

using dorq::Date;
using dorq::Field;
using dorq::Series;
using dorq::SeriesKind;
using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

constexpr int kViolations = 1;
constexpr int kUsage = 2;
constexpr int kInput = 3;

std::vector<Series> read(const std::string& text, const dorq::ReadOptions& options = {},
                         dorq::Grouping grouping = dorq::Grouping::kBuffer,
                         std::string_view extension = ".csv") {
  std::vector<Series> out;
  dorq::SeriesAssembler assembler(grouping, [&](Series&& s) { out.push_back(std::move(s)); });
  std::istringstream in(text);
  dorq::read_input(in, "test", "stem", extension, options, assembler);
  assembler.finish();
  return out;
}

Date day(const char* text) { return *dorq::parse_date(text); }

// fafnir's export: every current version of the three tables, items by name.
constexpr const char* kHeader =
    "security_id,symbol,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,loaded_at,reported_currency,close,"
    "revenue,net_income,total_assets\n";

}  // namespace

TEST_CASE("statements: detected from their columns, or named by --kind") {
  const std::vector<std::string> fafnir = {"security_id", "statement",  "period_type",
                                           "fiscal_date", "known_from", "revenue"};
  const auto m = dorq::map_columns(fafnir, {}, dorq::KindOption::kAuto, "t");
  CHECK(m.kind == SeriesKind::kStatements);
  CHECK(m.index(Field::kSeries) == 0);
  CHECK(m.index(Field::kDate) == 3);
  REQUIRE(m.items.size() == 1);
  CHECK(m.items[0].second == "revenue");

  // A bars file is still bars, and --kind statements needs the key columns.
  const std::vector<std::string> bars = {"date", "open", "high", "low", "close"};
  CHECK(dorq::map_columns(bars, {}, dorq::KindOption::kAuto, "t").kind == SeriesKind::kOhlcv);
  CHECK_THROWS_AS((void)dorq::map_columns(bars, {}, dorq::KindOption::kStatements, "t"),
                  dorq::InputError);
  const std::vector<std::string> no_known = {"statement", "period_type", "fiscal_date", "revenue"};
  try {
    (void)dorq::map_columns(no_known, {}, dorq::KindOption::kAuto, "t");
    FAIL("expected an InputError");
  } catch (const dorq::InputError& e) {
    CHECK(contains(e.what(), "missing known_from"));
  }

  // --columns names them too.
  dorq::ColumnOverrides overrides;
  REQUIRE_FALSE(dorq::parse_column_overrides("fiscal_date=reportperiod,statement=tbl,"
                                             "period_type=dim,known_from=kf",
                                             overrides)
                    .has_value());
  const std::vector<std::string> renamed = {"tbl", "dim", "reportperiod", "kf", "ebit"};
  const auto r = dorq::map_columns(renamed, overrides, dorq::KindOption::kAuto, "t");
  CHECK(r.kind == SeriesKind::kStatements);
  REQUIRE(r.items.size() == 1);
  CHECK(r.items[0].second == "ebit");
}

TEST_CASE("statements: the line items are --items' columns, or every other one") {
  const std::vector<std::string> names = {
      "security_id", "statement", "period_type", "fiscal_date", "known_from", "content_hash",
      "vendor_key",  "Revenue",   "net_income",  "extra",       "loaded_at",  "calendar_date"};
  const auto all = dorq::map_columns(names, {}, dorq::KindOption::kAuto, "t");
  std::vector<std::string> found;
  found.reserve(all.items.size());
  for (const auto& item : all.items) {
    found.push_back(item.second);
  }
  // fafnir's bookkeeping columns are not items; loaded_at is loaded_on.
  CHECK(found == std::vector<std::string>{"Revenue", "net_income"});
  CHECK(all.has(Field::kLoadedOn));

  const std::vector<std::string> listed = {"revenue", "total_assets"};
  const auto only = dorq::map_columns(names, {}, dorq::KindOption::kAuto, "t", &listed);
  REQUIRE(only.items.size() == 1);
  CHECK(only.items[0].first == 7);
  CHECK(only.items[0].second == "revenue");  // as the items file spells it
}

TEST_CASE("statements: rows, NULL apart from 0, versions in order") {
  const std::string text =
      std::string{kHeader} +
      "101,AAA,income,quarter,2024-06-30,2024,Q2,1,2024-08-01,,filed,2024-08-01,"
      "2026-10-25 03:12:45+00,USD,27,1100000,0,\n"
      "101,AAA,core.income_statement,quarter,2024-03-31,2024,Q1,2,2026-10-25,,observed,,"
      "2026-10-25,USD,31.4,1010000,121000,\n"
      "101,AAA,balance_sheet,quarter,2024-03-31,2024,Q1,1,2024-05-02,,filed,2024-05-02,"
      "2026-10-25,USD,25.1,,,5000000\n"
      "101,AAA,income,quarter,2024-03-31,2024,Q1,1,2024-05-02,2026-10-25,filed,2024-05-02,"
      "2026-10-25,USD,25.1,1000000,120000,\n";
  const auto out = read(text);
  REQUIRE(out.size() == 1);
  const Series& s = out[0];
  CHECK(s.kind == SeriesKind::kStatements);
  CHECK(s.label == "AAA");
  CHECK(s.issues.empty());
  REQUIRE(s.size() == 4);
  const dorq::StatementRows& rows = s.statements;
  // By fiscal date, then statement, period type and version.
  CHECK(s.date[0] == day("2024-03-31"));
  CHECK(rows.statement[0] == dorq::Statement::kIncome);
  CHECK(rows.version[0] == 1);
  CHECK(rows.known_to[0] == day("2026-10-25"));
  CHECK(rows.statement[1] == dorq::Statement::kIncome);
  CHECK(rows.version[1] == 2);
  CHECK_FALSE(rows.known_to[1].has_value());
  CHECK(rows.knowledge_source[1] == dorq::KnowledgeSource::kObserved);
  CHECK(rows.statement[2] == dorq::Statement::kBalanceSheet);
  CHECK(s.date[3] == day("2024-06-30"));
  CHECK(rows.fiscal_period[3] == dorq::FiscalPeriod::kQ2);
  CHECK(rows.fiscal_year[3] == 2024);
  CHECK(rows.loaded_on[3] == day("2026-10-25"));  // the day of a timestamp
  CHECK(rows.filing_date[3] == day("2024-08-01"));
  CHECK(rows.currency[3] == "USD");
  CHECK(s.close[3] == doctest::Approx(27.0));

  const auto* revenue = rows.item("revenue");
  const auto* net_income = rows.item("net_income");
  const auto* assets = rows.item("total_assets");
  REQUIRE(revenue != nullptr);
  REQUIRE(net_income != nullptr);
  REQUIRE(assets != nullptr);
  CHECK((*revenue)[0] == doctest::Approx(1000000.0));
  CHECK(std::isnan((*revenue)[2]));  // empty on the balance sheet: NULL
  CHECK((*net_income)[3] == 0.0);    // a zero is a zero
  CHECK_FALSE(std::isnan((*net_income)[3]));
  CHECK((*assets)[2] == doctest::Approx(5000000.0));
  CHECK(rows.item("ebit") == nullptr);
}

TEST_CASE("statements: a row without its key is skipped, a bad field reported") {
  const std::string text = std::string{kHeader} +
                           "101,AAA,income,quarter,2024-03-31,2024,Q1,1,,,,,,,,1,2,\n"
                           "101,AAA,ledger,quarter,2024-03-31,2024,Q1,0,2024-05-02,,,,,,,1,2,\n"
                           "101,AAA,income,quarter,2024-06-30,20x4,Q5,1,2024-08-01,,guessed,"
                           "soon,later,USD,n/a,1e6,abc,\n";
  const auto out = read(text);
  REQUIRE(out.size() == 1);
  const Series& s = out[0];
  REQUIRE(s.size() == 1);  // only the last row
  CHECK(s.statements.fiscal_year[0] == 0);
  CHECK(s.statements.fiscal_period[0] == dorq::FiscalPeriod::kUnknown);
  CHECK(std::isnan((*s.statements.item("net_income"))[0]));
  CHECK(std::isnan(s.close[0]));  // "n/a" is missing, not wrong

  std::vector<Field> fields;
  int skipping = 0;
  for (const dorq::FieldIssue& issue : s.issues) {
    fields.push_back(issue.field);
    skipping += issue.skips_row ? 1 : 0;
  }
  CHECK(fields == std::vector<Field>{Field::kKnownFrom, Field::kStatement, Field::kVersion,
                                     Field::kFiscalYear, Field::kFiscalPeriod,
                                     Field::kKnowledgeSource, Field::kFilingDate, Field::kLoadedOn,
                                     Field::kItem});
  CHECK(skipping == 3);
  CHECK(s.issues.back().column == "net_income");
  CHECK(s.issues.back().known == day("2024-08-01"));
}

TEST_CASE("statements: items from another source join the series, NULL where absent") {
  // Two files for one security, each with its own items.
  std::vector<Series> out;
  dorq::SeriesAssembler assembler(dorq::Grouping::kBuffer,
                                  [&](Series&& s) { out.push_back(std::move(s)); });
  // Each file also has a row it cannot use on its line 3: two skipped rows.
  std::istringstream a(
      "security_id,statement,period_type,fiscal_date,known_from,revenue\n"
      "7,income,quarter,2024-03-31,2024-05-01,10\n7,income,quarter,2024-06-30,,11\n");
  std::istringstream b(
      "security_id,statement,period_type,fiscal_date,known_from,total_assets\n"
      "7,balance_sheet,quarter,2024-03-31,2024-05-01,99\n7,ledger,quarter,2024-06-30,,98\n");
  dorq::read_input(a, "a.csv", "a", ".csv", {}, assembler);
  dorq::read_input(b, "b.csv", "b", ".csv", {}, assembler);
  assembler.finish();
  REQUIRE(out.size() == 1);
  CHECK(out[0].skipped_rows == 2);
  const dorq::StatementRows& rows = out[0].statements;
  REQUIRE(rows.size() == 2);
  REQUIRE(rows.item("revenue") != nullptr);
  REQUIRE(rows.item("total_assets") != nullptr);
  CHECK((*rows.item("revenue"))[0] == 10.0);
  CHECK(std::isnan((*rows.item("revenue"))[1]));
  CHECK(std::isnan((*rows.item("total_assets"))[0]));
  CHECK((*rows.item("total_assets"))[1] == 99.0);
}

TEST_CASE("statements: JSON null is NULL, and streaming needs a series together") {
  const auto json = read(
      R"({"security_id":"1","statement":"income","period_type":"annual","fiscal_date":"2023-12-31","known_from":"2024-02-20","revenue":null,"net_income":0})"
      "\n",
      {}, dorq::Grouping::kBuffer, ".jsonl");
  REQUIRE(json.size() == 1);
  CHECK(std::isnan((*json[0].statements.item("revenue"))[0]));
  CHECK((*json[0].statements.item("net_income"))[0] == 0.0);

  const std::string interleaved =
      "security_id,statement,period_type,fiscal_date,known_from\n"
      "1,income,annual,2023-12-31,2024-02-20\n2,income,annual,2023-12-31,2024-02-20\n"
      "1,income,annual,2022-12-31,2023-02-20\n";
  CHECK_THROWS_AS(read(interleaved, {}, dorq::Grouping::kStream), dorq::InputError);
  CHECK(read(interleaved, {}, dorq::Grouping::kBuffer).size() == 2);
}

TEST_CASE("statements: the views, in force and as first reported") {
  // Two periods. Q1 has three versions; Q2 one, known only from 2024-08-01.
  const std::string text =
      "security_id,statement,period_type,fiscal_date,version_no,known_from,known_to\n"
      "1,income,quarter,2024-03-31,1,2024-05-02,2025-03-01\n"
      "1,income,quarter,2024-03-31,2,2025-03-01,2026-10-25\n"
      "1,income,quarter,2024-03-31,3,2026-10-25,\n"
      "1,balance_sheet,quarter,2024-03-31,1,2024-05-02,\n"
      "1,income,quarter,2024-06-30,1,2024-08-01,\n";
  const auto out = read(text);
  REQUIRE(out.size() == 1);
  const Series& s = out[0];

  const auto latest = dorq::build_statement_views(s, std::nullopt);
  CHECK(latest.first_reported == std::vector<std::size_t>{0, 3, 4});
  CHECK(latest.in_force == std::vector<std::size_t>{2, 3, 4});

  const auto mid = dorq::build_statement_views(s, day("2025-06-30"));
  CHECK(mid.in_force == std::vector<std::size_t>{1, 3, 4});
  // known_to is not included: on 2025-03-01 version 2 is in force, not 1.
  CHECK(dorq::build_statement_views(s, day("2025-03-01")).in_force ==
        std::vector<std::size_t>{1, 3, 4});

  // Before Q2 was known, it is in no view of what was in force.
  const auto early = dorq::build_statement_views(s, day("2024-06-01"));
  CHECK(early.in_force == std::vector<std::size_t>{0, 3});

  // Versions that do not chain (two open): the highest is in force.
  const auto broken = read(
      "security_id,statement,period_type,fiscal_date,version_no,known_from\n"
      "1,income,quarter,2024-03-31,1,2024-05-02\n1,income,quarter,2024-03-31,2,2025-01-01\n");
  CHECK(dorq::build_statement_views(broken[0], std::nullopt).in_force ==
        std::vector<std::size_t>{1});
}

TEST_CASE("statements: --items, and DQ109 for rows it cannot use") {
  dorq::Context context;
  std::istringstream in(
      "column,statement,item_class,sign_convention,is_subtotal\n"
      "revenue,income,flow,positive_is_inflow,false\n"
      "total_assets,balance_sheet,stock,signed,true\n"
      "revenue,income,flow,signed,false\n"
      "weird,income,vibe,signed,false\n"
      "cash,core.balance_sheet,stock,signed,maybe\n"
      ",income,flow,signed,false\n");
  dorq::read_items(in, "/tmp/run-1/items.csv", context);
  CHECK(context.have_items);
  REQUIRE(context.items.size() == 3);
  CHECK(context.item("REVENUE") != nullptr);
  CHECK(context.item("revenue")->sign == dorq::SignConvention::kPositiveIsInflow);
  CHECK(context.item("total_assets")->is_subtotal);
  CHECK(context.item("cash")->statement == dorq::Statement::kBalanceSheet);
  CHECK_FALSE(context.item("cash")->is_subtotal);
  CHECK(context.item_names() == std::vector<std::string>{"revenue", "total_assets", "cash"});
  REQUIRE(context.issues.size() == 4);
  CHECK(context.issues[0].source == "items.csv");
  CHECK(contains(context.issues[0].message, "appears twice"));
  CHECK(context.issues[0].severity == dorq::Severity::kWarn);
  CHECK(contains(context.issues[1].message, "item_class \"vibe\""));
  CHECK(context.issues[1].severity == dorq::Severity::kError);
  CHECK(contains(context.issues[2].message, "is_subtotal \"maybe\""));
  CHECK(contains(context.issues[3].message, "column is empty"));

  dorq::Context bad;
  std::istringstream no_class("column,statement\nrevenue,income\n");
  CHECK_THROWS_AS(dorq::read_items(no_class, "items.csv", bad), dorq::InputError);
}

TEST_CASE("statements: [statements] parses, round-trips, and leaves old hashes alone") {
  // The hash of the defaults, and of fafnir's shipped config, as v0.10.0 printed
  // them: the release that added [statements] changed neither.
  CHECK(dorq::config_hash(dorq::Config{}) == "803c581d98ba409a");

  const dorq::Config config = dorq::parse_config(R"(
kind = "statements"
[statements]
nonnegative = ["revenue", "total_assets"]
never_absent = ["revenue"]
bounds = ["total_current_assets<=total_assets"]
identity_share = 0.01
quarter_spacing = [50, 130]
coverage_floor = "2000-01-01"
[statements.identities]
cash_flow = "net_change_in_cash = net_cash_operating + net_cash_investing + net_cash_financing"
balance_sheet = "total_assets=total_liabilities+total_equity_parent"
odd = "-a + b = c - d"
[profiles.statements]
match = { kind = "statements" }
ignore = ["DQ104"]
)",
                                                 "s.toml", false);
  CHECK(config.kind == dorq::KindOption::kStatements);
  const dorq::StatementSettings& st = config.statements;
  REQUIRE(st.identities.size() == 3);
  CHECK(st.identities[0].name == "balance_sheet");  // sorted
  CHECK(st.identities[0].to_string() == "total_assets = total_liabilities + total_equity_parent");
  CHECK(st.identities[2].to_string() == "-a + b = c - d");
  CHECK(st.identities[2].left[0].negative);
  CHECK(st.bounds.at(0).to_string() == "total_current_assets <= total_assets");
  CHECK(st.identity_share == doctest::Approx(0.01));
  CHECK(st.quarter_spacing == std::pair<int, int>{50, 130});
  CHECK(st.coverage_floor == day("2000-01-01"));
  REQUIRE(config.profiles.size() == 1);
  CHECK(config.profiles[0].match_kind == SeriesKind::kStatements);

  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(again.statements == config.statements);

  // The hash covers the settings once they are not the defaults, and the items' content.
  dorq::Config other;
  CHECK(contains(dorq::to_toml(other), "[statements]"));  // config show prints them
  other.statements.tie_floor = 5e5;
  CHECK(dorq::config_hash(other) != dorq::config_hash(dorq::Config{}));
  dorq::Config with_items;
  with_items.items_digest = "abc";
  CHECK(dorq::config_hash(with_items) != dorq::config_hash(dorq::Config{}));

  // An included file's identities stay, unless named again.
  const TempDir dir;
  write_file(dir.path() / "base.toml",
             "[statements]\nnonnegative = [\"revenue\"]\n[statements.identities]\n"
             "a = \"x = y\"\nb = \"p = q\"\n");
  write_file(
      dir.path() / "main.toml",
      "include = \"base.toml\"\n[statements.identities]\nb = \"p = q + r\"\nc = \"m = n\"\n");
  const dorq::Config merged = dorq::load_config(dir.path() / "main.toml");
  REQUIRE(merged.statements.identities.size() == 3);
  CHECK(merged.statements.identities[0].to_string() == "x = y");
  CHECK(merged.statements.identities[1].to_string() == "p = q + r");
  CHECK(merged.statements.identities[2].name == "c");
  CHECK(merged.statements.nonnegative == std::vector<std::string>{"revenue"});

  for (const char* bad :
       {"[statements.identities]\nx = \"a + b\"\n", "[statements.identities]\nx = \"a = b = c\"\n",
        "[statements.identities]\nx = \"a = b +\"\n", "[statements.identities]\nx = \"a = 3b\"\n",
        "[statements]\nbounds = [\"a < b\"]\n", "[statements]\nquarter_spacing = [130, 50]\n",
        "[statements]\ncoverage_floor = \"1998\"\n", "[statements]\nno_such_key = 1\n"}) {
    CAPTURE(bad);
    CHECK_THROWS_AS((void)dorq::parse_config(bad, "bad.toml", false), dorq::ConfigError);
  }
}

TEST_CASE("statements through the CLI: DQ104, --since, --items and no bars checks") {
  const TempDir dir;
  // Two versions of one period on one fiscal date, which DQ103 would call a
  // duplicate date, and a date on a Sunday, which DQ105 would report: neither
  // runs on statements.
  write_file(dir.path() / "s.csv",
             std::string{kHeader} +
                 "101,AAA,income,quarter,2024-03-31,2024,Q1,1,2024-05-02,2026-10-25,filed,"
                 "2024-05-02,2026-10-25,USD,25,1000000,120000,\n"
                 "101,AAA,income,quarter,2024-03-31,2024,Q1,2,2026-10-25,,observed,,2026-10-25,"
                 "USD,31,1010000,121000,\n"
                 "101,AAA,income,quarter,2024-06-30,2024,Q2,1,2024-08-01,,filed,2024-08-01,"
                 "2024-08-02,USD,27,1100000,x,\n"
                 "101,AAA,income,quarter,2024-09-29,2024,Q3,1,,,filed,,,USD,,1200000,1,\n");
  write_file(dir.path() / "items.csv",
             "column,statement,item_class,sign_convention,is_subtotal\n"
             "revenue,income,flow,positive_is_inflow,false\n"
             "total_assets,balance_sheet,stock,signed,true\n");
  // Inputs and --config are opened from the process's directory, context files
  // from the run's.
  const std::string file = (dir.path() / "s.csv").string();
  const std::string st_toml = (dir.path() / "st.toml").string();
  const char* s_csv = file.c_str();

  const Result all = run_in(dir.path(), {"--isolated", "--show-info", s_csv});
  CHECK(all.status == kViolations);
  CHECK(count_matching(all.out, "DQ104") == 2);
  CHECK(contains(all.out,
                 R"(AAA  2024-06-30  DQ104 error  missing-field  net_income "x" is not a number)"));
  CHECK(contains(all.out, "known_from is empty; the row is skipped"));
  CHECK_FALSE(contains(all.out, "DQ103"));
  CHECK_FALSE(contains(all.out, "DQ105"));
  CHECK_FALSE(contains(all.out, "DQ3"));

  // --since by knowledge: Q2 was known and loaded in 2024, and the skipped row
  // has no known_from, so it stays.
  const Result since =
      run_in(dir.path(), {"--isolated", "--show-info", "--since", "2026-01-01", s_csv});
  CHECK(count_matching(since.out, "DQ104") == 1);
  CHECK(contains(since.out, "2024-09-29"));

  // With --items, net_income is not an item: nothing to report on it.
  const Result listed =
      run_in(dir.path(), {"--isolated", "--show-info", "--items", "items.csv", s_csv});
  CHECK(count_matching(listed.out, "DQ104") == 1);
  CHECK_FALSE(contains(listed.out, "net_income"));

  // [statements] may name only items --items lists.
  write_file(dir.path() / "st.toml", "[statements]\nnonnegative = [\"revenue\", \"revnue\"]\n");
  const Result typo =
      run_in(dir.path(), {"--config", st_toml.c_str(), "--items", "items.csv", s_csv});
  CHECK(typo.status == kUsage);
  CHECK(contains(typo.err, "statements.nonnegative names \"revnue\""));
  CHECK(run_in(dir.path(), {"--config", st_toml.c_str(), s_csv}).status == kViolations);

  // The rows read, the skipped one included; the same on any thread count.
  const Result json = run_in(dir.path(), {"--isolated", "--format", "json", s_csv});
  CHECK(contains(json.out, R"("rows":4)"));
  const Result one = run_in(dir.path(), {"--isolated", "--threads", "1", "--show-info", s_csv});
  const Result many = run_in(dir.path(), {"--isolated", "--threads", "16", "--show-info", s_csv});
  CHECK(one.out == many.out);

  // A bad items file as a whole is an input error.
  write_file(dir.path() / "bad-items.csv", "column\nrevenue\n");
  CHECK(run_in(dir.path(), {"--isolated", "--items", "bad-items.csv", s_csv}).status == kInput);
}

TEST_CASE("statements: a series is one kind, and list-checks names the kinds") {
  const TempDir dir;
  write_file(dir.path() / "bars.csv", "series,date,close\nX,2024-01-02,10\n");
  write_file(dir.path() / "s.csv",
             "series,statement,period_type,fiscal_date,known_from\nX,income,annual,2023-12-31,"
             "2024-02-20\n");
  const std::string bars = (dir.path() / "bars.csv").string();
  const std::string statements = (dir.path() / "s.csv").string();
  const Result mixed =
      run_in(dir.path(), {"--isolated", "--buffer", bars.c_str(), statements.c_str()});
  CHECK(mixed.status == kInput);
  CHECK(contains(mixed.err, "is point elsewhere but statements here"));

  const Result list = run_in(dir.path(), {"list-checks"});
  CHECK(contains(list.out, "DQ104  missing-field           error  all"));
  CHECK(contains(list.out, "DQ101  ohlc-bounds             error  ohlcv "));
  CHECK(contains(list.out, "DQ102  non-positive            error  ohlcv,point "));
  const Result json = run_in(dir.path(), {"list-checks", "--format", "json"});
  CHECK(contains(json.out,
                 R"("code":"DQ104","name":"missing-field","severity":"error","applies":"all")"));
}
