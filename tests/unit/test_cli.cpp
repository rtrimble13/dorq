#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "cli/app.hpp"
#include "dorq/exit_code.hpp"
#include "dorq/version.hpp"
#include "test_support.hpp"

namespace {

using dorq::test::contains;
using dorq::test::Result;
using dorq::test::run;
using dorq::test::run_in;

std::size_t count_lines(const std::string& text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

constexpr int kUsage = 2;
constexpr int kInput = 3;

constexpr std::string_view kBadBars =
    "symbol,date,open,high,low,close,volume\n"
    "AAA,2024-01-02,10,11,9,10.5,100\n"
    "AAA,2024-01-03,10,9.5,9,10,100\n"  // DQ101: high < open, close
    "AAA,2024-01-04,10,11,9,0,100\n"    // DQ101 (low > close) and DQ102 (close 0)
    "AAA,2024-01-05,10,11,9,,100\n"     // DQ104 error: close empty
    "AAA,2024-01-08,10,11,9,10,\n"      // DQ104 warn: volume empty
    "BBB,2024-13-01,1,1,1,1,1\n";       // DQ104 error: bad date

}  // namespace

TEST_CASE("exit codes are a fixed contract") {
  CHECK(dorq::to_int(dorq::ExitCode::kOk) == 0);
  CHECK(dorq::to_int(dorq::ExitCode::kViolations) == 1);
  CHECK(dorq::to_int(dorq::ExitCode::kUsage) == 2);
  CHECK(dorq::to_int(dorq::ExitCode::kInput) == 3);
}

TEST_CASE("--version prints the bare version on stdout") {
  const Result result = run({"--version"});
  CHECK(result.status == 0);
  CHECK(result.out == std::string{dorq::kVersion} + "\n");
  CHECK(result.err.empty());
}

TEST_CASE("--help succeeds and documents the commands and exit codes") {
  const Result result = run({"--help"});
  CHECK(result.status == 0);
  CHECK(contains(result.out, "check"));
  CHECK(contains(result.out, "list-checks"));
  CHECK(contains(result.out, "Exit status"));
}

TEST_CASE("no input on a terminal is a usage error") {
  const Result result = run({}, "", true);
  CHECK(result.status == kUsage);
  CHECK(result.out.empty());
  CHECK(contains(result.err, "no input"));
}

TEST_CASE("an unknown option is a usage error") {
  CHECK(run({"--no-such-option"}).status == kUsage);
  CHECK(run({"check", "--format", "xml"}).status == kUsage);
}

TEST_CASE("version prints build information, as text or JSON") {
  const Result text = run({"version"});
  CHECK(text.status == 0);
  CHECK(text.out.rfind("dorq " + std::string{dorq::kVersion} + "\n", 0) == 0);
  const Result json = run({"version", "--format", "json"});
  CHECK(json.status == 0);
  CHECK(json.out.front() == '{');
  CHECK(contains(json.out, R"("version":")" + std::string{dorq::kVersion} + R"(")"));
  CHECK(run({"version", "--format", "xml"}).status == kUsage);
}

TEST_CASE("check is the default command, and stdin is read when piped") {
  const Result result = run({"--isolated"}, kBadBars);
  CHECK(result.status == 1);
  CHECK(contains(result.out, "DQ101 error  ohlc-bounds"));
  CHECK(contains(result.out, "DQ102 error  non-positive"));
  CHECK(contains(result.out, "BBB  line 7  DQ104 error  missing-field"));
  CHECK(contains(result.out, "DQ104 warn  missing-field  volume is empty"));
  CHECK(result.err.empty());
}

TEST_CASE("clean data exits 0 with no output") {
  const Result result = run({"--isolated", "-"}, "date,value\n2024-01-02,1.5\n2024-01-03,1.6\n");
  CHECK(result.status == 0);
  CHECK(result.out.empty());
}

TEST_CASE("severity threshold and exit policy") {
  const Result errors_only = run({"--isolated", "--min-severity", "error"}, kBadBars);
  CHECK_FALSE(contains(errors_only.out, " warn "));
  CHECK(run({"--isolated", "--exit-zero"}, kBadBars).status == 0);
  CHECK(run({"--isolated", "--fail-on", "never"}, kBadBars).status == 0);
  const std::string warn_only = "date,close,volume\n2024-01-02,1,\n";
  CHECK(run({"--isolated", "--fail-on", "error"}, warn_only).status == 0);
  CHECK(run({"--isolated"}, warn_only).status == 1);
}

TEST_CASE("select and ignore") {
  const Result only_101 = run({"--isolated", "--select", "DQ101"}, kBadBars);
  CHECK(contains(only_101.out, "DQ101"));
  CHECK_FALSE(contains(only_101.out, "DQ104"));
  const Result no_104 = run({"--isolated", "--ignore", "missing-field"}, kBadBars);
  CHECK_FALSE(contains(no_104.out, "DQ104"));
  CHECK(contains(no_104.out, "DQ101"));
  CHECK(run({"--isolated", "--select", "bogus"}, kBadBars).status == kUsage);
}

TEST_CASE("--since drops earlier violations") {
  const Result result = run({"--isolated", "--since", "2024-01-05"}, kBadBars);
  CHECK_FALSE(contains(result.out, "2024-01-03"));
  CHECK(contains(result.out, "2024-01-05"));
  CHECK(run({"--isolated", "--since", "yesterday"}, kBadBars).status == kUsage);
}

TEST_CASE("every output format reports the same violations") {
  const Result text = run({"--isolated"}, kBadBars);
  const std::size_t n = count_lines(text.out);
  REQUIRE(n == 6);

  const Result jsonl = run({"--isolated", "--format", "jsonl"}, kBadBars);
  CHECK(jsonl.status == 1);
  CHECK(count_lines(jsonl.out) == n);
  CHECK(contains(jsonl.out, R"("code":"DQ101","check":"ohlc-bounds","severity":"error")"));
  CHECK(contains(jsonl.out, R"("record_key":{"trade_date":"2024-01-03"})"));
  CHECK(contains(jsonl.out, R"("record_key":{"line":7})"));

  const Result json = run({"--isolated", "--format", "json"}, kBadBars);
  CHECK(json.out.rfind(R"({"violations":[)", 0) == 0);
  CHECK(contains(json.out, R"("summary":{"inputs":1,"series":2,"rows":6,"violations":6)"));

  const Result csv = run({"--isolated", "--format", "csv"}, kBadBars);
  CHECK(count_lines(csv.out) == n + 1);
  CHECK(csv.out.rfind("series,label,source,date", 0) == 0);

  const Result fafnir = run({"--isolated", "--format", "fafnir"}, kBadBars);
  CHECK(count_lines(fafnir.out) == n);
  CHECK(contains(
      fafnir.out,
      R"({"security_id":"AAA","table_name":"core.daily_price","record_key":{"trade_date":"2024-01-03"},"check_name":"dorq_ohlc_bounds","severity":"error","detail":{)"));
}

TEST_CASE("fafnir format writes an integer series id as a number") {
  const Result result =
      run({"--isolated", "--format", "fafnir"}, "security_id,date,close\n12345,2024-01-02,\n");
  CHECK(contains(result.out, R"({"security_id":12345,)"));
}

TEST_CASE("--statistics counts per check") {
  const Result text = run({"--isolated", "--statistics"}, kBadBars);
  CHECK(contains(text.out, "2     DQ101 ohlc-bounds\n"));
  const Result jsonl = run({"--isolated", "--statistics", "--format", "jsonl"}, kBadBars);
  CHECK(contains(jsonl.err, "DQ101 ohlc-bounds"));
}

TEST_CASE("unreadable input exits 3") {
  CHECK(run({"--isolated", "/no/such/file.csv"}).status == kInput);
  CHECK(run({"--isolated"}, "").status == kInput);
  const Result no_date = run({"--isolated"}, "x,close\n1,2\n");
  CHECK(no_date.status == kInput);
  CHECK(contains(no_date.err, "no date column"));
}

TEST_CASE("output does not depend on the thread count") {
  std::string input = "id,date,open,high,low,close,volume\n";
  for (int s = 0; s < 40; ++s) {
    for (int d = 1; d <= 28; ++d) {
      const std::string day = (d < 10 ? "0" : "") + std::to_string(d);
      const bool bad = (s * 7 + d) % 11 == 0;
      input += "S" + std::to_string(s) + ",2024-02-" + day + ",10," + (bad ? "9" : "11") +
               ",9,10.5,100\n";
    }
  }
  const Result one = run({"--isolated", "--threads", "1", "--format", "jsonl"}, input);
  const Result many = run({"--isolated", "--threads", "8", "--format", "jsonl"}, input);
  CHECK(one.status == 1);
  CHECK(count_lines(one.out) > 50);
  CHECK(one.out == many.out);
}

TEST_CASE("streamed and buffered reading report the same thing") {
  // stdin is streamed; --buffer reads it whole first.
  std::string input = "id,date,close,volume\n";
  for (int s = 0; s < 5; ++s) {
    input += "S" + std::to_string(s) + ",2024-01-02,1,\n";
    input += "S" + std::to_string(s) + ",2024-01-02,2,5\n";
  }
  const Result streamed = run({"--isolated", "--show-info"}, input);
  const Result buffered = run({"--isolated", "--show-info", "--buffer"}, input);
  CHECK(count_lines(streamed.out) == 10);  // DQ103 and DQ104 per series
  CHECK(streamed.out == buffered.out);
  // Only buffering accepts a series that comes back.
  const std::string interleaved = "id,date,close\na,2024-01-02,0\nb,2024-01-02,1\na,2024-01-03,1\n";
  CHECK(run({"--isolated"}, interleaved).status == kInput);
  CHECK(run({"--isolated", "--buffer"}, interleaved).status == 0);
}

TEST_CASE("list-checks and explain") {
  const Result list = run({"list-checks"});
  CHECK(list.status == 0);
  CHECK(contains(list.out, "DQ101  ohlc-bounds"));
  const Result json = run({"list-checks", "--format", "json"});
  CHECK(contains(json.out, R"({"code":"DQ101","name":"ohlc-bounds")"));
  const Result explain = run({"explain", "DQ101"});
  CHECK(explain.status == 0);
  CHECK(explain.out.rfind("# DQ101 ohlc-bounds", 0) == 0);
  CHECK(run({"explain", "ohlc-bounds"}).out == explain.out);
  CHECK(run({"explain", "DQ999"}).status == kUsage);
}

TEST_CASE("config: discovered, shown, and errors are usage errors") {
  const dorq::test::TempDir dir;
  dorq::test::write_file(dir.path() / "dorq.toml",
                         "select = [\"DQ104\"]\nmin_severity = \"error\"\n");
  const Result checked = run_in(dir.path(), {}, kBadBars);
  CHECK(contains(checked.out, "DQ104 error"));
  CHECK_FALSE(contains(checked.out, "DQ101"));
  CHECK_FALSE(contains(checked.out, " warn "));

  const Result shown = run_in(dir.path(), {"config", "show"});
  CHECK(shown.status == 0);
  CHECK(contains(shown.out, "dorq.toml"));
  CHECK(contains(shown.out, "select = [\"DQ104\"]"));

  dorq::test::write_file(dir.path() / "dorq.toml", "selct = [\"DQ1\"]\n");
  const Result typo = run_in(dir.path(), {}, kBadBars);
  CHECK(typo.status == kUsage);
  CHECK(contains(typo.err, "unknown key \"selct\""));
  CHECK(run_in(dir.path(), {"--isolated"}, kBadBars).status == 1);
}

TEST_CASE("config init writes a starter file that loads") {
  const dorq::test::TempDir dir;
  const Result init = run_in(dir.path(), {"config", "init"});
  CHECK(init.status == 0);
  CHECK(std::filesystem::exists(dir.path() / "dorq.toml"));
  CHECK(run_in(dir.path(), {"config", "init"}).status == kUsage);  // will not overwrite
  CHECK(run_in(dir.path(), {"config", "init", "--force"}).status == 0);
  CHECK(run_in(dir.path(), {"config", "show"}).status == 0);
}

TEST_CASE("config_hash covers a calendar file's content, not where it lives") {
  const dorq::test::TempDir dir;
  std::filesystem::create_directories(dir.path() / "a");
  std::filesystem::create_directories(dir.path() / "b");
  const std::string sessions = "trade_date\n2024-01-02\n2024-01-03\n2024-01-04\n2024-01-05\n";
  dorq::test::write_file(dir.path() / "a" / "cal.csv", sessions);
  dorq::test::write_file(dir.path() / "b" / "cal.csv", sessions);
  const auto hash = [&dir](const char* sub) {
    const std::string file = (dir.path() / sub / "cal.csv").string();
    const Result r =
        run_in(dir.path(),
               {"--isolated", "--calendar-file", file.c_str(), "--format", "jsonl", "--exit-zero"},
               kBadBars);
    const auto at = r.out.find(R"("config_hash":")");
    REQUIRE(at != std::string::npos);
    return r.out.substr(at + 15, 16);
  };
  const std::string a = hash("a");
  CHECK(hash("b") == a);  // the same sessions, another directory
  dorq::test::write_file(dir.path() / "b" / "cal.csv", sessions + "2024-01-08\n");
  CHECK(hash("b") != a);  // another session: other settings
}
