// M9, DR-0904: DQ901 statement-key and DQ902 impossible-value, through the CLI,
// with fafnir's own test shapes (test_dq_fundamentals.py) as the reference.

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

constexpr const char* kHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,revenue,net_income,"
    "weighted_average_shares,total_assets,shares_outstanding,total_current_assets,"
    "cash_and_equivalents\n";

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

// A JSON Lines record's record_key, as written.
std::string record_key(const std::string& line) {
  const auto at = line.find(R"("record_key":)");
  return at == std::string::npos ? std::string{}
                                 : line.substr(at + 13, line.find('}', at) - at - 12);
}

std::vector<std::string> keys_of(const std::string& out, const std::string& code) {
  std::vector<std::string> keys;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    keys.push_back(record_key(line));
  }
  return keys;
}

// Every period of DQ901's cases: one row a line.
const std::vector<std::string>& key_rows() {
  static const std::vector<std::string> kRows{
      // fafnir's broken chain: v1 closes on 2010-09-01, v2 starts on 2010-10-01.
      "ZZCHAIN,income,quarter,2010-03-31,2010,Q1,1,2010-05-01,2010-09-01,,,1,,,,,,",
      "ZZCHAIN,income,quarter,2010-03-31,2010,Q1,2,2010-10-01,,,,1,,,,,,",
      // and the clean chain beside it.
      "ZZCHAIN,income,quarter,2010-06-30,2010,Q2,1,2010-08-01,2010-10-01,,,1,,,,,,",
      "ZZCHAIN,income,quarter,2010-06-30,2010,Q2,2,2010-10-01,,,,1,,,,,,",
      // An overlap; two open; the last closed.
      "ZZCHAIN,income,quarter,2010-09-30,2010,Q3,1,2010-11-01,2011-03-15,,,1,,,,,,",
      "ZZCHAIN,income,quarter,2010-09-30,2010,Q3,2,2011-03-01,,,,1,,,,,,",
      "ZZCHAIN,cash_flow,quarter,2010-09-30,2010,Q3,1,2010-11-01,,,,,,,,,,",
      "ZZCHAIN,cash_flow,quarter,2010-09-30,2010,Q3,2,2011-03-01,,,,,,,,,,",
      "ZZCHAIN,balance_sheet,quarter,2010-09-30,2010,Q3,1,2010-11-01,2011-03-01,,,,,,5,,,",
      "ZZCHAIN,balance_sheet,quarter,2010-09-30,2010,Q3,2,2011-03-01,2011-04-01,,,,,,5,,,",
      // One version: open is a chain; closed is not.
      "ONE,income,annual,2010-12-31,2010,FY,1,2011-02-20,,filed,2011-02-20,1,,,,,,",
      "ONE,balance_sheet,annual,2010-12-31,2010,FY,1,2011-02-20,2012-01-01,filed,2011-02-20,,,,"
      "5,,,",
      // A row twice; known before the period ended; filed on another day; filed
      // with no filing date.
      "ROWS,income,quarter,2011-03-31,2011,Q1,1,2011-05-01,,,,1,,,,,,",
      "ROWS,income,quarter,2011-03-31,2011,Q1,1,2011-05-01,,,,1,,,,,,",
      "ROWS,income,quarter,2011-06-30,2011,Q2,1,2011-06-15,,,,1,,,,,,",
      "ROWS,income,quarter,2011-09-30,2011,Q3,1,2011-11-04,,filed,2011-11-01,1,,,,,,",
      "ROWS,income,quarter,2011-12-31,2011,Q4,1,2012-02-01,,filed,,1,,,,,,",
      // Observed and estimated versions have no filing date to agree with.
      "ROWS,income,quarter,2012-03-31,2012,Q1,1,2012-05-01,2012-06-01,observed,,1,,,,,,",
      "ROWS,income,quarter,2012-03-31,2012,Q1,2,2012-06-01,,estimated,,1,,,,,,",
  };
  return kRows;
}

std::string csv_of(const std::vector<std::string>& rows) {
  std::string text = kHeader;
  for (const std::string& row : rows) {
    text += row + "\n";
  }
  return text;
}

}  // namespace

TEST_CASE("DQ901: duplicates, chains and knowledge dates, as fafnir keys them") {
  const TempDir dir;
  write_file(dir.path() / "s.csv", csv_of(key_rows()));
  const std::string file = (dir.path() / "s.csv").string();
  const Result r = run_in(dir.path(), {"--isolated", "--select", "DQ901", "--format", "jsonl",
                                       "--exit-zero", file.c_str()});
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> keys = keys_of(r.out, "DQ901");
  // fafnir's shape: its broken chain is one condition, on the period, and the clean
  // chain beside it is none.
  CHECK(std::count(
            keys.begin(), keys.end(),
            R"({"statement":"income","period_type":"quarter","fiscal_date":"2010-03-31"})") == 1);
  CHECK(std::none_of(keys.begin(), keys.end(),
                     [](const std::string& k) { return contains(k, "2010-06-30"); }));
  CHECK(keys.size() == 9);

  const auto message = [&r](const std::string& needle) {
    const auto lines = lines_with(r.out, needle);
    return lines.empty() ? std::string{} : lines.front();
  };
  CHECK(contains(message("2010-03-31"), "v1 is known to 2010-09-01 and v2 from 2010-10-01: a gap"));
  // The suggested action names each version and its known_to: a day, or empty to
  // reopen it.
  CHECK(contains(message("2010-03-31"),
                 R"("suggested_action":{"kind":"set_known_to","v1":"2010-10-01"})"));
  CHECK(contains(message("2010-03-31"),
                 R"("versions":"v1 2010-05-01..2010-09-01, v2 2010-10-01..open")"));
  CHECK(contains(
      message(R"("statement":"income","period_type":"quarter","fiscal_date":"2010-09-30")"),
      "v1 is known to 2011-03-15 and v2 from 2011-03-01: an overlap"));
  CHECK(contains(
      message(R"("statement":"cash_flow","period_type":"quarter","fiscal_date":"2010-09-30")"),
      "v1 is open, but v2 is known from 2011-03-01"));
  CHECK(contains(message("balance_sheet\",\"period_type\":\"quarter\""),
                 "the last, v2, is closed at 2011-04-01: none is in force"));
  CHECK(contains(message("balance_sheet\",\"period_type\":\"quarter\""),
                 R"("suggested_action":{"kind":"set_known_to","v2":""})"));
  // One version: closed is a break, open is not.
  CHECK(contains(message(R"("statement":"balance_sheet","period_type":"annual")"),
                 "the last, v1, is closed at 2012-01-01"));
  CHECK_FALSE(contains(r.out, R"({"statement":"income","period_type":"annual")"));
  // The version's problems, keyed by its version.
  CHECK(
      contains(message(R"("fiscal_date":"2011-03-31","version_no":1})"), "2 rows (lines 14, 15)"));
  CHECK(contains(message("2011-06-30"), "known from 2011-06-15, before the period ends"));
  CHECK(contains(message("2011-09-30"), "filed on 2011-11-01, but known from 2011-11-04"));
  CHECK(contains(message("2011-12-31"), "filed, but with no filing date"));
  CHECK_FALSE(contains(r.out, "2012-03-31"));
  CHECK(contains(r.out, R"("severity":"error")"));
  CHECK_FALSE(contains(r.out, R"("severity":"warn")"));

  // The text line names the period, with a version or without.
  const Result text =
      run_in(dir.path(), {"--isolated", "--select", "DQ901", "--exit-zero", file.c_str()});
  CHECK(contains(text.out,
                 "ZZCHAIN  2010-03-31 Q1 FY2010 income  DQ901 error  statement-key  versions do "
                 "not chain"));
  CHECK(contains(text.out,
                 "ROWS  2011-03-31 Q1 FY2011 income v1  DQ901 error  statement-key  "
                 "2 rows (lines 14, 15)"));

  // The rows in another order give the same reports, on any thread count.
  std::vector<std::string> shuffled = key_rows();
  std::reverse(shuffled.begin(), shuffled.end());
  std::rotate(shuffled.begin(), shuffled.begin() + 5, shuffled.end());
  write_file(dir.path() / "shuffled.csv", csv_of(shuffled));
  const std::string other = (dir.path() / "shuffled.csv").string();
  const Result again = run_in(dir.path(), {"--isolated", "--select", "DQ901", "--format", "jsonl",
                                           "--exit-zero", "--threads", "4", other.c_str()});
  std::vector<std::string> again_keys = keys_of(again.out, "DQ901");
  std::vector<std::string> sorted_keys = keys;
  std::sort(again_keys.begin(), again_keys.end());
  std::sort(sorted_keys.begin(), sorted_keys.end());
  CHECK(again_keys == sorted_keys);
  CHECK(count_matching(again.out, "a gap") == 1);

  // --since by knowledge: a chain is kept while any version is newer than the date.
  const Result since = run_in(dir.path(), {"--isolated", "--select", "DQ901", "--format", "jsonl",
                                           "--exit-zero", "--since", "2011-04-01", file.c_str()});
  const std::vector<std::string> recent = keys_of(since.out, "DQ901");
  CHECK(std::none_of(recent.begin(), recent.end(),
                     [](const std::string& k) { return contains(k, "2010-03-31"); }));
  CHECK(std::any_of(recent.begin(), recent.end(),
                    [](const std::string& k) { return contains(k, "2011-12-31"); }));
}

TEST_CASE("DQ902: fafnir's impossible negatives, and bounds, per version and item") {
  const TempDir dir;
  // fafnir's shapes: ZZNEG's revenue and share count; a bank's revenue (fafnir's
  // SQL calls it info, being net of losses; DQ902 has no sector yet); a mortgage
  // REIT's revenue and share count. A negative net income is no fault.
  write_file(
      dir.path() / "s.csv",
      std::string{kHeader} +
          "ZZNEG,income,annual,2010-12-31,2010,FY,1,2011-02-01,,,,-5,-50,,,,,\n"
          "ZZNEG,balance_sheet,annual,2010-12-31,2010,FY,1,2011-02-01,,,,,,,100,-1,,\n"
          "ZZBANK,income,annual,2010-12-31,2010,FY,1,2011-02-01,,,,-7,,,,,,\n"
          "ZZMREIT,income,annual,2010-12-31,2010,FY,1,2011-02-01,,,,-7,,-3,,,,\n"
          // Bounds: above beyond rounding; within it; and a NULL total.
          "BOUND,balance_sheet,quarter,2011-03-31,2011,Q1,1,2011-05-01,,,,,,,1000,,1200,900\n"
          "BOUND,balance_sheet,quarter,2011-06-30,2011,Q2,1,2011-08-01,,,,,,,1000000,,1000500,"
          "1\n"
          "BOUND,balance_sheet,quarter,2011-09-30,2011,Q3,1,2011-11-01,,,,,,,,,1200,900\n");
  write_file(dir.path() / "st.toml",
             "[statements]\n"
             "nonnegative = [\"revenue\", \"weighted_average_shares\", \"total_assets\", "
             "\"shares_outstanding\"]\n"
             "bounds = [\"total_current_assets <= total_assets\", "
             "\"cash_and_equivalents <= total_assets\"]\n");
  const std::string file = (dir.path() / "s.csv").string();
  const std::string config = (dir.path() / "st.toml").string();
  const Result r = run_in(dir.path(), {"--config", config.c_str(), "--select", "DQ902", "--format",
                                       "jsonl", "--exit-zero", file.c_str()});
  INFO(r.err);
  REQUIRE(r.status == 0);
  const auto lines = lines_with(r.out, R"("code":"DQ902")");
  std::vector<std::string> got;
  for (const std::string& line : lines) {
    const auto from = line.find(R"("series":")") + 10;
    got.push_back(line.substr(from, line.find('"', from) - from) + " " + record_key(line));
  }
  std::sort(got.begin(), got.end());
  const std::string v1 = R"("period_type":"annual","fiscal_date":"2010-12-31","version_no":1,)";
  const std::vector<std::string> expected{
      R"(BOUND {"statement":"balance_sheet","period_type":"quarter","fiscal_date":"2011-03-31","version_no":1,"item":"total_current_assets","bound":"total_assets"})",
      R"(ZZBANK {"statement":"income",)" + v1 + R"("item":"revenue"})",
      R"(ZZMREIT {"statement":"income",)" + v1 + R"("item":"revenue"})",
      R"(ZZMREIT {"statement":"income",)" + v1 + R"("item":"weighted_average_shares"})",
      R"(ZZNEG {"statement":"balance_sheet",)" + v1 + R"("item":"shares_outstanding"})",
      R"(ZZNEG {"statement":"income",)" + v1 + R"("item":"revenue"})",
  };
  CHECK(got == expected);
  CHECK(contains(r.out, "revenue is -5, and cannot be negative"));
  CHECK(contains(r.out, "total_current_assets (1200) is above total_assets (1000)"));
  CHECK(contains(r.out, R"("detail":{"item":"revenue","value":-5})"));
  CHECK_FALSE(contains(r.out, "net_income"));

  // Nothing to judge without the lists.
  const Result none =
      run_in(dir.path(), {"--isolated", "--select", "DQ902", "--exit-zero", file.c_str()});
  CHECK(count_matching(none.out, "DQ902") == 0);
}
