#include <string>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "test_support.hpp"

using dorq::Config;
using dorq::ConfigError;
using dorq::parse_config;

TEST_CASE("defaults") {
  const Config config;
  CHECK(config.select == std::vector<std::string>{"DQ"});
  CHECK(config.min_severity == dorq::Severity::kWarn);
  CHECK(config.fail_on == dorq::Severity::kWarn);
  CHECK(config.integrity.precision_high_decimals == 5);
}

TEST_CASE("a dorq.toml, top-level or under [tool.dorq]") {
  const std::string body = R"(
select = ["DQ1"]
ignore = "DQ106, DQ107"
min_severity = "error"
fail_on = "never"
threads = 4
[columns]
date = "trade_date"
value = "DGS10"
[integrity]
precision_min_segment = 30
precision_min_contrast = 1
)";
  std::string nested = body;
  for (const std::string table : {"columns", "integrity"}) {
    const auto at = nested.find("[" + table + "]");
    nested.replace(at, table.size() + 2, "[tool.dorq." + table + "]");
  }
  for (const std::string& text : {body, "[tool.dorq]\n" + nested}) {
    const Config config = parse_config(text, "t.toml", false);
    CHECK(config.select == std::vector<std::string>{"DQ1"});
    CHECK(config.ignore == std::vector<std::string>{"DQ106", "DQ107"});
    CHECK(config.min_severity == dorq::Severity::kError);
    CHECK_FALSE(config.fail_on.has_value());
    CHECK(config.threads == 4);
    REQUIRE(config.columns.size() == 2);
    CHECK(config.columns[1].first == dorq::Field::kClose);
    CHECK(config.integrity.precision_min_segment == 30);
    CHECK(config.integrity.precision_min_contrast == 1.0);
  }
}

TEST_CASE("pyproject.toml without [tool.dorq] is the defaults") {
  const Config config = parse_config("[tool.black]\nline-length = 88\n", "pyproject.toml", true);
  CHECK(config.select == std::vector<std::string>{"DQ"});
}

TEST_CASE("mistakes are errors that say where") {
  const auto message = [](const std::string& text) -> std::string {
    try {
      static_cast<void>(parse_config(text, "t.toml", false));
    } catch (const ConfigError& error) {
      return error.what();
    }
    return "";
  };
  CHECK(message("selct = []") == "t.toml:1: unknown key \"selct\"");
  CHECK(message("\n[integrity]\nprecision_min_segment = 1\n") ==
        "t.toml:3: \"integrity.precision_min_segment\" must be a whole number from 2 to 1000000");
  CHECK(message("min_severity = \"loud\"").find("min_severity") != std::string::npos);
  CHECK(message("[columns]\ncolour = \"x\"").find("unknown field") != std::string::npos);
  CHECK(message("select = [1]").find("list of strings") != std::string::npos);
  CHECK(message("[profiles.a]\nmatch = { colour = \"x\" }").find("profiles.a.match.colour") !=
        std::string::npos);
  CHECK(message("select = [").find("t.toml:1:") == 0);
  CHECK(message("[tool.dorq]\nselect = []\n[columns]\ndate = \"d\"\n")
            .find("\"columns\" is outside [tool.dorq]") != std::string::npos);
}

TEST_CASE("profiles are sorted by name and match on kind or series") {
  const Config config = parse_config(R"(
[profiles.zeta]
match = { series = ["A", "B"] }
ignore = ["DQ107"]
[profiles.alpha]
match = { kind = "point" }
integrity = { positive_point_series = true }
)",
                                     "t.toml", false);
  REQUIRE(config.profiles.size() == 2);
  CHECK(config.profiles[0].name == "alpha");
  CHECK(config.profiles[1].name == "zeta");

  dorq::Series point;
  point.kind = dorq::SeriesKind::kPoint;
  point.id = "C";
  CHECK(config.profiles[0].matches(point));
  CHECK_FALSE(config.profiles[1].matches(point));
  point.id = "B";
  CHECK(config.profiles[1].matches(point));

  dorq::IntegritySettings settings;
  config.profiles[0].integrity.apply_to(settings);
  CHECK(settings.positive_point_series);
}

TEST_CASE("to_toml round-trips, and the hash follows only what changes the report") {
  const Config config = parse_config(R"(
select = ["DQ1"]
[columns]
date = "d"
[profiles."nav funds"]
match = { kind = "point", series = ["X"] }
ignore = ["DQ107"]
integrity = { precision_high_decimals = 6 }
)",
                                     "t.toml", false);
  const Config again = parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(dorq::config_hash(config).size() == 16);

  Config other = config;
  other.format = dorq::OutputFormat::kJson;
  other.threads = 3;
  other.fail_on.reset();
  CHECK(dorq::config_hash(other) == dorq::config_hash(config));
  other.ignore.emplace_back("DQ104");
  CHECK(dorq::config_hash(other) != dorq::config_hash(config));
}

TEST_CASE("the starter config loads, and equals the defaults") {
  const Config starter = parse_config(dorq::starter_config(), "dorq.toml", false);
  CHECK(dorq::config_hash(starter) == dorq::config_hash(Config{}));
}

TEST_CASE("discovery: nearest dorq.toml or pyproject [tool.dorq], stopping at .git") {
  const dorq::test::TempDir dir;
  const auto nested = dir.path() / "a" / "b";
  std::filesystem::create_directories(nested);
  dorq::test::write_file(dir.path() / "a" / "pyproject.toml", "[tool.black]\n");
  dorq::test::write_file(dir.path() / "dorq.toml", "select = [\"DQ1\"]\n");
  CHECK(dorq::discover_config(nested) == dir.path() / "dorq.toml");
  dorq::test::write_file(dir.path() / "a" / "pyproject.toml", "[tool.dorq]\nselect = [\"DQ2\"]\n");
  CHECK(dorq::discover_config(nested) == dir.path() / "a" / "pyproject.toml");
  CHECK(dorq::load_config(dir.path() / "a" / "pyproject.toml").select ==
        std::vector<std::string>{"DQ2"});
}

TEST_CASE("calendar, coverage and severity settings") {
  const Config config = parse_config(R"(
[calendar]
name = "weekdays"
file = "cal.csv"
exchange = "NYSE"
[coverage]
report = "session"
outage_start = 0.001
trade_size = 100
frequency = "daily"
cohort_min_series = 5
[severity]
warn = 0.5
[profiles.funds]
match = { series = ["VFIAX"] }
coverage = { publication_lag = 2 }
)",
                                     "t.toml", false);
  CHECK(config.calendar == dorq::CalendarKind::kWeekdays);
  CHECK(config.calendar_file == "cal.csv");
  CHECK(config.calendar_exchange == "NYSE");
  CHECK(config.gap_report == dorq::GapReport::kSession);
  CHECK(config.coverage.outage_start == 0.001);
  CHECK(config.coverage.trade_size == 100.0);
  CHECK(config.coverage.frequency == dorq::Frequency::kDaily);
  CHECK(config.cohort.min_series == 5);
  CHECK(config.severity.warn == 0.5);
  REQUIRE(config.profiles.size() == 1);
  dorq::CoverageSettings coverage;
  config.profiles[0].coverage.apply_to(coverage);
  CHECK(coverage.publication_lag == 2);
  // And the round trip keeps all of it.
  const Config again = parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
}

TEST_CASE("coverage mistakes") {
  const auto fails = [](const std::string& text) {
    try {
      static_cast<void>(parse_config(text, "t.toml", false));
    } catch (const ConfigError&) {
      return true;
    }
    return false;
  };
  CHECK(fails("[calendar]\nname = \"LSE\"\n"));
  CHECK(fails("[coverage]\nreport = \"day\"\n"));
  CHECK(fails("[coverage]\nfrequency = \"hourly\"\n"));
  CHECK(fails("[severity]\nwarn = 0.95\n"));  // above error
  // Cohort and report settings are global.
  CHECK(fails("[profiles.p]\ncoverage = { cohort_min_series = 4 }\n"));
  CHECK(fails("[profiles.p]\ncoverage = { report = \"session\" }\n"));
}

TEST_CASE("a calendar file named in a config file is relative to it") {
  const dorq::test::TempDir dir;
  dorq::test::write_file(dir.path() / "dorq.toml", "[calendar]\nfile = \"cal.csv\"\n");
  CHECK(dorq::load_config(dir.path() / "dorq.toml").calendar_file == dir.path() / "cal.csv");
}
