// M5: context inputs (--actions, --meta, --market), DQ601-DQ602 and DQ701-DQ705.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "io/input_error.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

class Noise {
 public:
  explicit Noise(std::uint64_t seed) : state_(seed) {}
  double uniform() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11U) * 0x1.0p-53;
  }
  double normal() {
    double sum = 0.0;
    for (int i = 0; i < 12; ++i) {
      sum += uniform();
    }
    return sum - 6.0;
  }

 private:
  std::uint64_t state_;
};

std::vector<dorq::Date> sessions(int n) {
  static const dorq::Calendar kCalendar;
  std::vector<dorq::Date> days;
  dorq::Date day = dorq::Date::from_ymd(2020, 1, 2);
  while (static_cast<int>(days.size()) < n) {
    if (kCalendar.is_session(day)) {
      days.push_back(day);
    }
    day = dorq::Date::from_days(day.days() + 1);
  }
  return days;
}

std::string fixed(double value, int decimals) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << value;
  return out.str();
}

struct Bar {
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

// A liquid stock: a random walk at `vol` a day plus `beta` times `market`'s
// moves, on volume about 2M.
std::vector<Bar> stock(int n, double start, double vol, std::uint64_t seed,
                       const std::vector<double>& market = {}, double beta = 1.0) {
  Noise noise(seed);
  std::vector<Bar> bars;
  double price = start;
  for (int i = 0; i < n; ++i) {
    const double prev = price;
    const double m = market.empty() ? 0.0 : market.at(static_cast<std::size_t>(i));
    price *= std::exp(vol * noise.normal() + beta * m);
    Bar b;
    b.close = price;
    b.open = prev * std::exp(0.3 * vol * noise.normal());
    b.high = std::max(b.open, b.close) * (1.0 + 0.4 * vol * noise.uniform());
    b.low = std::min(b.open, b.close) * (1.0 - 0.4 * vol * noise.uniform());
    b.volume = std::round(2e6 * std::exp(0.3 * noise.normal()));
    bars.push_back(b);
  }
  return bars;
}

// From bar `from` on, every price times `factor` and volume divided by it; or,
// with `raw_volume` false, volume as it was (a feed whose unadjusted volume is
// already adjusted for the split).
void split(std::vector<Bar>& bars, std::size_t from, double factor, bool raw_volume = true) {
  for (std::size_t i = from; i < bars.size(); ++i) {
    bars[i].open *= factor;
    bars[i].high *= factor;
    bars[i].low *= factor;
    bars[i].close *= factor;
    if (raw_volume) {
      bars[i].volume = std::round(bars[i].volume / factor);
    }
  }
}

// A distribution of `share` of the price ex at bar `from`: every price from there
// on lower by that amount, as a share of the close before. Returns the amount, on
// the close as written (two decimals).
double distribute(std::vector<Bar>& bars, std::size_t from, double share) {
  const double amount = std::round(bars[from - 1].close * 100.0) / 100.0 * share;
  split(bars, from, 1.0 - share, false);
  return amount;
}

std::string bars_csv(const std::string& id, const std::vector<Bar>& bars, bool header = true,
                     int decimals = 2) {
  const auto days = sessions(static_cast<int>(bars.size()));
  std::string csv = header ? "series,date,open,high,low,close,volume\n" : "";
  for (std::size_t i = 0; i < bars.size(); ++i) {
    const Bar& b = bars[i];
    csv += id + "," + days[i].to_string() + "," + fixed(b.open, decimals) + "," +
           fixed(b.high, decimals) + "," + fixed(b.low, decimals) + "," + fixed(b.close, decimals) +
           "," + fixed(b.volume, 0) + "\n";
  }
  return csv;
}

std::string day(int i) { return sessions(i + 1).back().to_string(); }

// dorq on `csv` (stdin) in a directory holding `files`, with `args`.
Result check(const std::string& csv, const std::vector<std::pair<std::string, std::string>>& files,
             std::initializer_list<const char*> args) {
  const TempDir dir;
  for (const auto& [name, text] : files) {
    write_file(dir.path() / name, text);
  }
  return run_in(dir.path(), args, csv);
}

const char* const kActionsHeader =
    "security_id,ex_date,action_type,split_numerator,split_denominator,dividend_amount\n";

}  // namespace

// ---------------------------------------------------------------------------
// Readers

TEST_CASE("actions: fafnir's columns or dorq's, sorted, a bad row skipped, a bad file an error") {
  dorq::Context context;
  std::istringstream fafnir(std::string{kActionsHeader} +
                            "A,2021-06-01,split,2,1,\n"
                            "A,2020-06-01,split,1,10,\n"
                            "A,2020-03-02,dividend,,,0.25\n"
                            "B,2020-03-02,Dividend,,,1.5\n");
  dorq::read_actions(fafnir, "actions.csv", context);
  CHECK(context.have_actions);
  REQUIRE(context.actions.contains("A"));
  const dorq::SeriesActions& a = context.actions.at("A");
  REQUIRE(a.splits.size() == 2);
  CHECK(a.splits[0].ex_date.to_string() == "2020-06-01");
  CHECK(a.splits[0].ratio_text() == "1:10");
  CHECK(a.splits[0].price_factor() == 10.0);
  CHECK(a.splits[1].price_factor() == 0.5);
  REQUIRE(a.dividends.size() == 1);
  CHECK(a.dividends[0].amount == 0.25);
  CHECK(context.actions.at("B").dividends.size() == 1);

  // A zero or negative amount is read, for DQ705 to report: fafnir's
  // core.corporate_action allows a zero, written at the column's scale.
  dorq::Context odd;
  std::istringstream zero(std::string{kActionsHeader} +
                          "A,2020-03-02,dividend,,,0.000000\n"
                          "A,2020-06-01,dividend,,,-0.25\n");
  dorq::read_actions(zero, "actions.csv", odd);
  REQUIRE(odd.actions.at("A").dividends.size() == 2);
  CHECK(odd.actions.at("A").dividends[0].amount == 0.0);
  CHECK(odd.actions.at("A").dividends[1].amount == -0.25);

  dorq::Context mine;
  std::istringstream tsv(
      "series\tex_date\ttype\tnumerator\tdenominator\tamount\nX\t2020-01-02\tsplit\t3\t2\t\n");
  dorq::read_actions(tsv, "a.tsv", mine);
  CHECK(mine.actions.at("X").splits.at(0).ratio_text() == "3:2");

  // A row that cannot be used is skipped and kept as an issue (DQ109), under the
  // file's name without its directory; the rows around it are read.
  dorq::Context bad;
  std::istringstream rows(std::string{kActionsHeader} +
                          "A,2020-01-02,spinoff,,,\n"
                          "A,2020-01-03,split,2,,\n"
                          "A,2020-01-06,split,0,1,\n"
                          "B,soon,split,2,1,\n"
                          "B,2020-01-07,dividend,,,x\n"
                          "B,2020-01-08,dividend,,,\n"
                          ",2020-01-09,dividend,,,0.5\n"
                          "A,2020-01-10,split,2,1,\n");
  dorq::read_actions(rows, "/tmp/run-1234/a.csv", bad);
  REQUIRE(bad.actions.contains("A"));
  CHECK(bad.actions.at("A").splits.size() == 1);
  CHECK_FALSE(bad.actions.contains("B"));
  CHECK_FALSE(bad.actions.contains(""));
  REQUIRE(bad.issues.size() == 7);
  const auto issue = [&bad](std::size_t i) {
    const dorq::ContextIssue& e = bad.issues.at(i);
    return e.series + " " + e.source + ":" + std::to_string(e.line) + " " +
           (e.date ? e.date->to_string() : std::string{"-"}) + " " +
           std::string{dorq::to_string(e.severity)} + " " + e.message;
  };
  CHECK(issue(0) ==
        "A a.csv:2 2020-01-02 error type \"spinoff\" is not split or dividend; the row is skipped");
  CHECK(issue(1) ==
        "A a.csv:3 2020-01-03 error a split needs a numerator and a denominator; the row is "
        "skipped");
  CHECK(issue(2) ==
        "A a.csv:4 2020-01-06 error numerator \"0\" is not a positive number; the row is skipped");
  CHECK(issue(3) == "B a.csv:5 - error ex_date \"soon\" is not a date; the row is skipped");
  CHECK(issue(4) == "B a.csv:6 2020-01-07 error amount \"x\" is not a number; the row is skipped");
  CHECK(issue(5) == "B a.csv:7 2020-01-08 error a dividend needs an amount; the row is skipped");
  CHECK(issue(6) == " a.csv:8 2020-01-09 error the series is empty; the row is skipped");

  // Only a file that cannot be read as a whole is an error.
  const auto error = [](const std::string& text) {
    dorq::Context c;
    std::istringstream in(text);
    try {
      dorq::read_actions(in, "a.csv", c);
    } catch (const dorq::InputError& e) {
      return std::string{e.what()};
    }
    return std::string{};
  };
  CHECK(contains(error("series,date\nA,2020-01-02\n"), "needs series, ex_date and type"));
  CHECK(error("") == "a.csv is empty");
  CHECK(contains(error(std::string{kActionsHeader} + "A,2020-01-02,\"split,2,1,\n"),
                 "a.csv: unterminated quoted field"));
}

TEST_CASE("metadata: fields, by id or label, and duplicates") {
  dorq::Context context;
  std::istringstream in(
      "security_id,asset_type,nav_priced,tick_size,peer_group,exchange_code\n"
      "1,ETF,false,0.01,FAM1,ARCX\n"
      "2,fund,true,,,\n");
  dorq::read_meta(in, "meta.csv", context);
  dorq::Series one;
  one.id = "1";
  const dorq::SeriesMeta* meta = context.meta_for(one);
  REQUIRE(meta != nullptr);
  CHECK(meta->asset_type == "etf");
  CHECK(meta->nav_priced == false);
  CHECK(meta->tick_size == 0.01);
  CHECK(meta->peer_group == "FAM1");
  CHECK(meta->exchange == "ARCX");
  dorq::Series by_label;
  by_label.id = "99";
  by_label.label = "2";
  REQUIRE(context.meta_for(by_label) != nullptr);
  CHECK(context.meta_for(by_label)->nav_priced == true);
  CHECK_FALSE(context.meta_for(by_label)->tick_size.has_value());

  // A series listed twice keeps its first row; a field that cannot be read is
  // left unset. Each is an issue (DQ109) at warn.
  dorq::Context twice;
  std::istringstream dup(
      "series,asset_type,nav_priced,tick_size\nA,etf,maybe,0.01\nA,fund,true,\nB,etf,,0\n,etf,,\n");
  dorq::read_meta(dup, "m.csv", twice);
  REQUIRE(twice.meta.contains("A"));
  CHECK(twice.meta.at("A").asset_type == "etf");
  CHECK_FALSE(twice.meta.at("A").nav_priced.has_value());
  CHECK(twice.meta.at("A").tick_size == 0.01);
  REQUIRE(twice.meta.contains("B"));
  CHECK(twice.meta.at("B").asset_type == "etf");
  CHECK_FALSE(twice.meta.at("B").tick_size.has_value());
  REQUIRE(twice.issues.size() == 4);
  for (const dorq::ContextIssue& e : twice.issues) {
    CHECK(e.severity == dorq::Severity::kWarn);
    CHECK_FALSE(e.date.has_value());
  }
  CHECK(twice.issues[0].message == "nav_priced \"maybe\" is not true or false; it is left unset");
  CHECK(twice.issues[1].line == 3);
  CHECK(twice.issues[1].message ==
        "series \"A\" appears twice; the row is skipped, and the first kept");
  CHECK(twice.issues[2].series == "B");
  CHECK(twice.issues[2].message == "tick_size \"0\" is not a positive number; it is left unset");
  CHECK(twice.issues[3].message == "the series is empty; the row is skipped");
  CHECK_FALSE(twice.meta.contains(""));
}

TEST_CASE("profiles match on metadata, and never without it") {
  const dorq::Config config = dorq::parse_config(R"(
[profiles.funds]
match = { nav_priced = true }
ignore = ["DQ107"]

[profiles.etfs]
match = { asset_type = ["ETF", "etn"], exchange = "ARCX", kind = "ohlcv" }
)",
                                                 "dorq.toml", false);
  REQUIRE(config.profiles.size() == 2);
  const dorq::Profile& etfs = config.profiles[0];
  const dorq::Profile& funds = config.profiles[1];
  dorq::Series s;
  s.id = "X";
  dorq::SeriesMeta meta;
  meta.asset_type = "etf";
  meta.exchange = "ARCX";
  CHECK(etfs.matches(s, &meta));
  CHECK_FALSE(funds.matches(s, &meta));
  meta.nav_priced = true;
  CHECK(funds.matches(s, &meta));
  CHECK_FALSE(funds.matches(s, nullptr));
  meta.exchange = "XNAS";
  CHECK_FALSE(etfs.matches(s, &meta));

  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK_THROWS_AS(static_cast<void>(dorq::parse_config("[profiles.x]\nmatch = { sector = \"a\" }",
                                                       "d.toml", false)),
                  dorq::ConfigError);
}

TEST_CASE("context files that cannot be used are input errors") {
  const std::string csv = bars_csv("A", stock(100, 50.0, 0.015, 1));
  Result result = check(csv, {}, {"--isolated", "--actions", "nope.csv"});
  CHECK(result.status == 3);
  CHECK(contains(result.err, "nope.csv: cannot open the actions file"));
  result = check(csv, {{"a.csv", "security_id,ex_date\nA,2020-01-02\n"}},
                 {"--isolated", "--actions", "a.csv"});
  CHECK(result.status == 3);
  CHECK(contains(result.err, "a.csv line 1: an actions file needs series, ex_date and type"));
  result = check(csv, {{"m.csv", "series,date,close\nA,2020-01-02,1\nB,2020-01-02,1\n"}},
                 {"--isolated", "--market", "m.csv"});
  CHECK(result.status == 3);
  CHECK(contains(result.err, "the market file must hold one series (it holds 2)"));
}

TEST_CASE("DQ109: a context row that cannot be used is reported, and the run goes on") {
  std::vector<Bar> b = stock(300, 40.0, 0.015, 3);
  b[200].close *= 3.0;  // a bad print, for the rest of the run to find
  b[200].high = b[200].close;
  const std::string csv = bars_csv("A", stock(300, 50.0, 0.015, 1)) + bars_csv("B", b, false);
  std::string actions = kActionsHeader;
  actions += "A," + day(100) + ",split,0,1,\n";       // line 2
  actions += "A,2020-02-30,dividend,,,0.25\n";        // line 3
  actions += "Z," + day(50) + ",split,2,,\n";         // line 4, a series not in the bars
  actions += "A," + day(150) + ",dividend,,,0.25\n";  // line 5, usable
  const std::string meta = "series,nav_priced\nB,sometimes\n";
  const std::vector<std::pair<std::string, std::string>> files = {{"a.csv", actions},
                                                                  {"m.csv", meta}};

  Result r = check(csv, files,
                   {"--isolated", "--format", "json", "--actions", "a.csv", "--meta", "m.csv"});
  CAPTURE(r.out);
  CAPTURE(r.err);
  CHECK(r.status == 1);
  CHECK(r.err.empty());
  CHECK(count_matching(r.out, R"("code":"DQ109")") == 4);
  CHECK(contains(r.out,
                 R"({"series":"A","label":null,"source":"a.csv","date":")" + day(100) +
                     R"(","line":2,"code":"DQ109","check":"bad-context-row","severity":"error")"));
  CHECK(contains(
      r.out, R"("message":"a.csv: numerator \"0\" is not a positive number; the row is skipped")"));
  CHECK(contains(
      r.out, R"({"series":"A","label":null,"source":"a.csv","date":null,"line":3,"code":"DQ109")"));
  CHECK(contains(r.out, R"({"series":"Z","label":null,"source":"a.csv","date":")" + day(50) +
                            R"(","line":4,"code":"DQ109")"));
  CHECK(contains(
      r.out,
      R"({"series":"B","label":null,"source":"m.csv","date":null,"line":2,"code":"DQ109","check":"bad-context-row","severity":"warn")"));
  CHECK(
      contains(r.out, R"("suggested_action":{"kind":"fix_context_row","file":"a.csv","line":2})"));
  // The bars are still checked, and the rows are not counted as series.
  CHECK(contains(r.out, R"({"series":"B","label":null,"source":"<stdin>","date":")" + day(200)));
  CHECK(contains(r.out, R"("summary":{"inputs":1,"series":2,"rows":600,)"));

  r = check(csv, files, {"--isolated", "--format", "fafnir", "--actions", "a.csv"});
  CHECK(contains(
      r.out, R"({"security_id":"Z","table_name":"core.daily_price","record_key":{"trade_date":")" +
                 day(50) + R"("},"check_name":"dorq_bad_context_row","severity":"error")"));
  CHECK(contains(r.out, R"("record_key":{"line":3},"check_name":"dorq_bad_context_row")"));

  r = check(csv, files,
            {"--isolated", "--ignore", "DQ109", "--actions", "a.csv", "--meta", "m.csv"});
  CHECK_FALSE(contains(r.out, "DQ109"));
  CHECK(contains(r.out, "B  " + day(200) + "  DQ2"));
  const std::string since = day(60);
  r = check(csv, files,
            {"--isolated", "--select", "DQ109", "--since", since.c_str(), "--actions", "a.csv"});
  // In the order of the file.
  CHECK(r.out == "A  " + day(100) +
                     "  DQ109 error  bad-context-row  a.csv: numerator \"0\" is not a positive "
                     "number; the row is skipped  (line 2)\n"
                     "A  line 3  DQ109 error  bad-context-row  a.csv: ex_date \"2020-02-30\" is "
                     "not a date; the row is skipped\n");
}

// ---------------------------------------------------------------------------
// Splits on file

TEST_CASE("a split on file explains its move, and one missing from the file is DQ203") {
  std::vector<Bar> bars = stock(400, 80.0, 0.015, 2);
  split(bars, 200, 0.5);
  const std::string csv = bars_csv("A", bars);
  const Result alone = check(csv, {}, {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl"});
  CHECK(count_matching(alone.out, R"("code":"DQ203")") == 1);

  const std::string actions = std::string{kActionsHeader} + "A," + day(200) + ",split,2,1,\n";
  const Result explained = check(csv, {{"a.csv", actions}},
                                 {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl",
                                  "--show-info", "--actions", "a.csv"});
  CAPTURE(explained.out);
  CHECK(count_matching(explained.out, R"("code":"DQ203")") == 0);
  CHECK(count_matching(explained.out, R"("code":"DQ70)") == 0);
  CHECK(contains(explained.out, "the 2:1 split on file, ex " + day(200)));
  CHECK(contains(explained.out, R"("explained_split":)"));
}

TEST_CASE("a split on file between two stored bars explains the move across them") {
  std::vector<Bar> bars = stock(400, 80.0, 0.015, 3);
  split(bars, 200, 0.5);
  std::string csv = "series,date,open,high,low,close,volume\n";
  const auto days = sessions(400);
  for (std::size_t i = 0; i < bars.size(); ++i) {
    if (i == 199) {  // the ex-date's session has no bar
      continue;
    }
    const Bar& b = bars[i];
    csv += "A," + days[i].to_string() + "," + fixed(b.open, 2) + "," + fixed(b.high, 2) + "," +
           fixed(b.low, 2) + "," + fixed(b.close, 2) + "," + fixed(b.volume, 0) + "\n";
  }
  const std::string actions = std::string{kActionsHeader} + "A," + day(199) + ",split,2,1,\n";
  const Result result =
      check(csv, {{"a.csv", actions}},
            {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv"});
  CAPTURE(result.out);
  CHECK(result.out.empty());
}

TEST_CASE("DQ701-DQ704: a split on file the bars contradict") {
  const auto run_with = [](const std::vector<Bar>& bars, const std::string& actions) {
    return check(bars_csv("A", bars), {{"a.csv", std::string{kActionsHeader} + actions}},
                 {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv"});
  };
  std::vector<Bar> bars = stock(400, 80.0, 0.015, 4);
  split(bars, 200, 0.5);

  SUBCASE("misdated") {
    const Result r = run_with(bars, "A," + day(203) + ",split,2,1,\n");
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ701")") == 1);
    CHECK(count_matching(r.out, R"("code":"DQ203")") == 0);
    CHECK(contains(r.out, R"("kind":"redate_split")"));
    CHECK(contains(r.out, R"("new_ex_date":")" + day(200) + "\""));
  }
  SUBCASE("ratio inverted") {
    const Result r = run_with(bars, "A," + day(200) + ",split,1,2,\n");
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ703")") == 1);
    CHECK(count_matching(r.out, R"("code":"DQ203")") == 0);
    CHECK(contains(r.out, "(≈ 2:1)"));
  }
  SUBCASE("applied twice") {
    std::vector<Bar> twice = bars;
    split(twice, 206, 0.5);
    const Result r = run_with(twice, "A," + day(200) + ",split,2,1,\n");
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ704")") == 1);
    CHECK(count_matching(r.out, R"("code":"DQ203")") == 0);
    CHECK(contains(r.out, R"("kind":"rescale")"));
  }
  SUBCASE("not in the bars") {
    const Result r = run_with(stock(400, 80.0, 0.015, 4), "A," + day(200) + ",split,2,1,\n");
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ702")") == 1);
    CHECK(contains(r.out, R"("kind":"check_split")"));
  }
}

TEST_CASE("a split on file needs no volume to confirm it, and one missing from the file does") {
  // Share volume unchanged across the split: a feed whose "unadjusted" volume is
  // adjusted already, or a reverse split's burst of trading.
  const auto run_with = [](const std::vector<Bar>& bars, int decimals, const std::string& actions) {
    const std::string csv = bars_csv("A", bars, true, decimals);
    if (actions.empty()) {
      return check(csv, {}, {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl"});
    }
    return check(csv, {{"a.csv", std::string{kActionsHeader} + actions}},
                 {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv"});
  };

  SUBCASE("a microcap's 1:10 reverse split") {
    std::vector<Bar> bars = stock(400, 0.15, 0.04, 40);
    split(bars, 200, 10.0, false);
    const Result explained = run_with(bars, 4, "A," + day(200) + ",split,1,10,\n");
    CAPTURE(explained.out);
    CHECK(explained.out.empty());
    // The same bars with no split on file: a scale error, as before.
    const Result alone = run_with(bars, 4, "");
    CAPTURE(alone.out);
    CHECK(count_matching(alone.out, R"("code":"DQ202","check":"scale-shift","severity":"error")") ==
          1);
  }
  SUBCASE("a large cap's 10:1 forward split") {
    std::vector<Bar> bars = stock(400, 2400.0, 0.015, 41);
    split(bars, 200, 0.1, false);
    const Result explained = run_with(bars, 2, "A," + day(200) + ",split,10,1,\n");
    CAPTURE(explained.out);
    CHECK(explained.out.empty());
  }
  SUBCASE("a ratio on file the move does not match is still reported") {
    std::vector<Bar> bars = stock(400, 0.15, 0.04, 42);
    split(bars, 200, 100.0, false);
    const Result r = run_with(bars, 4, "A," + day(200) + ",split,1,10,\n");
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ703")") == 1);
    CHECK(count_matching(r.out, R"("code":"DQ20)") == 0);
  }
}

// ---------------------------------------------------------------------------
// Dividends on file

TEST_CASE(
    "a dividend on file explains its ex-date's drop, and one missing from the file does not") {
  // A fund's year-end distribution of about a third of its price.
  std::vector<Bar> bars = stock(400, 27.87, 0.006, 43);
  const double amount = distribute(bars, 200, 0.32);
  std::string regular = kActionsHeader;
  for (const int i : {30, 93, 156, 282, 345}) {
    regular += "A," + day(i) + ",dividend,,,0.12\n";
  }
  const std::string csv = bars_csv("A", bars);
  const auto run_with = [&csv](const std::string& actions) {
    return check(csv, {{"a.csv", actions}},
                 {"--isolated", "--select", "DQ2", "--format", "jsonl", "--show-info", "--actions",
                  "a.csv"});
  };

  const Result explained =
      run_with(regular + "A," + day(200) + ",dividend,,," + fixed(amount, 4) + "\n");
  CAPTURE(explained.out);
  CHECK(count_matching(explained.out, R"("code":"DQ2)") ==
        count_matching(explained.out, R"("code":"DQ209")"));
  CHECK(contains(explained.out, "the " + fixed(amount, 4) + " dividend on file, ex " + day(200)));
  CHECK(contains(explained.out, R"("explained_dividend":)"));

  // Without it, the drop looks like a 3:2 split, as before.
  const Result missing = run_with(regular);
  CAPTURE(missing.out);
  CHECK(count_matching(missing.out,
                       R"("code":"DQ203","check":"unreported-split","severity":"error")") == 1);

  // A dividend DQ705 reports at error (here, a negative one) is not credited.
  const Result negative =
      run_with(regular + "A," + day(200) + ",dividend,,,-" + fixed(amount, 4) + "\n");
  CAPTURE(negative.out);
  CHECK(count_matching(negative.out, R"("code":"DQ203")") == 1);
}

TEST_CASE("a dividend on file as the newest bar is credited before any bar confirms it") {
  // A closed-end fund's special dividend of a fifth of its price, ex on the newest
  // bar (IAE, 2026-10-09).
  std::vector<Bar> bars = stock(201, 8.85, 0.01, 44);
  const double amount = distribute(bars, 200, 0.19);
  const std::string csv = bars_csv("A", bars);
  const Result alone = check(csv, {}, {"--isolated", "--select", "DQ2", "--format", "jsonl"});
  CAPTURE(alone.out);
  CHECK(count_matching(alone.out, R"("provisional":true)") == 1);
  const Result explained =
      check(csv,
            {{"a.csv", std::string{kActionsHeader} + "A," + day(200) + ",dividend,,," +
                           fixed(amount, 4) + "\n"}},
            {"--isolated", "--select", "DQ2", "--format", "jsonl", "--actions", "a.csv"});
  CAPTURE(explained.out);
  CHECK(explained.out.empty());
}

TEST_CASE("a dividend on file invents no move where the price did not drop") {
  // A third of the price on file as a dividend, and no drop: a wrong row. The bar
  // is not even a candidate, and nothing is reported.
  const std::vector<Bar> bars = stock(400, 27.87, 0.006, 45);
  const std::string amount = fixed(std::round(bars[199].close * 100.0) / 100.0 * 0.32, 4);
  const Result r = check(
      bars_csv("A", bars),
      {{"a.csv", std::string{kActionsHeader} + "A," + day(200) + ",dividend,,," + amount + "\n"}},
      {"--isolated", "--select", "DQ2", "--format", "jsonl", "--show-info", "--actions", "a.csv"});
  CAPTURE(r.out);
  CHECK_FALSE(contains(r.out, R"("date":")" + day(200)));
}

TEST_CASE("a dividend is weighed only when larger than an ordinary move, and hides no error") {
  std::vector<Bar> bars = stock(400, 50.0, 0.015, 46);
  bars[200].open *= 3.0;  // a bad print on an ex-date
  bars[200].high *= 3.0;
  bars[200].low *= 3.0;
  bars[200].close *= 3.0;
  const auto run_with = [&bars](const std::string& amount) {
    return check(
        bars_csv("A", bars),
        {{"a.csv", std::string{kActionsHeader} + "A," + day(200) + ",dividend,,," + amount + "\n"}},
        {"--isolated", "--select", "DQ2", "--format", "jsonl", "--actions", "a.csv"});
  };
  // A regular quarterly dividend, smaller than a day's ordinary move: not weighed,
  // so it cannot dilute the errors' share of the prior.
  const Result small = run_with("0.25");
  CAPTURE(small.out);
  CHECK(count_matching(small.out, R"("code":"DQ201","check":"bad-print","severity":"error")") == 1);
  CHECK_FALSE(contains(small.out, "explained_dividend"));
  // A large one is weighed, and does not explain a move the wrong way.
  const Result large = run_with("15");
  CAPTURE(large.out);
  CHECK(count_matching(large.out, R"("code":"DQ201","check":"bad-print","severity":"error")") == 1);
  CHECK(contains(large.out, R"("explained_dividend":)"));
}

TEST_CASE("a fund family's distributions on file are not a family splitting together") {
  std::string csv;
  std::string meta = "series,peer_group\n";
  std::string actions = kActionsHeader;
  for (int s = 0; s < 4; ++s) {
    std::vector<Bar> bars = stock(400, 60.0, 0.006, 50 + static_cast<std::uint64_t>(s));
    const double amount = distribute(bars, 250, 0.2);  // ≈ a 5:4 split
    const std::string id = "F" + std::to_string(s);
    csv += bars_csv(id, bars, s == 0);
    meta += id + ",FAM\n";
    actions += id + "," + day(250) + ",dividend,,," + fixed(amount, 4) + "\n";
  }
  const Result missing =
      check(csv, {{"meta.csv", meta}},
            {"--isolated", "--select", "DQ2,DQ6", "--format", "jsonl", "--meta", "meta.csv"});
  CAPTURE(missing.out);
  CHECK(count_matching(missing.out, R"("code":"DQ601")") == 1);
  CHECK(contains(missing.out, R"("members":"F0,F1,F2,F3")"));
  const Result on_file = check(csv, {{"meta.csv", meta}, {"a.csv", actions}},
                               {"--isolated", "--select", "DQ2,DQ6", "--format", "jsonl", "--meta",
                                "meta.csv", "--actions", "a.csv"});
  CAPTURE(on_file.out);
  CHECK(on_file.out.empty());
}

TEST_CASE("DQ705: a dividend above the price, or a hundred times its others") {
  const std::vector<Bar> bars = stock(400, 50.0, 0.015, 5);
  std::string actions = kActionsHeader;
  for (int i = 30; i < 400; i += 63) {
    double amount = 0.25;
    if (i == 156) {
      amount = 25.0;  // a slip
    } else if (i == 282) {
      amount = 80.0;  // above the price
    }
    actions += "A," + day(i) + ",dividend,,," + fixed(amount, 2) + "\n";
  }
  const Result r =
      check(bars_csv("A", bars), {{"a.csv", actions}},
            {"--isolated", "--select", "DQ705", "--format", "jsonl", "--actions", "a.csv"});
  CAPTURE(r.out);
  CHECK(count_matching(r.out, R"("code":"DQ705")") == 2);
  CHECK(contains(r.out, "is at or above the close before it"));
  CHECK(contains(r.out, "×100 the series' usual 0.25"));
}

TEST_CASE("DQ705: a zero or negative dividend is reported, and the run goes on") {
  std::vector<Bar> bars = stock(400, 50.0, 0.015, 5);
  bars[300].close *= 3.0;  // a bad print, for the rest of the run to find
  bars[300].high = bars[300].close;
  std::string actions = kActionsHeader;
  // Six zeros among the usual 0.25: a zero is no measure of the others, so the
  // slip is still ×100 the usual 0.25, not ×inf a median of 0.
  for (int i = 30; i < 400; i += 63) {
    actions += "A," + day(i) + ",dividend,,," + (i == 156 ? "25.000000" : "0.250000") + "\n";
    actions += "A," + day(i + 5) + ",dividend,,,0.000000\n";
  }
  actions += "A,2019-06-03,dividend,,,0\n";  // before the bars: no close needed
  actions += "A," + day(40) + ",dividend,,,-0.25\n";
  const Result r = check(bars_csv("A", bars), {{"a.csv", actions}},
                         {"--isolated", "--format", "jsonl", "--actions", "a.csv"});
  CAPTURE(r.out);
  CHECK(r.status == 1);
  CHECK(count_matching(r.out, "pays nothing") == 7);
  CHECK(
      count_matching(
          r.out,
          R"("severity":"warn","p_error":1,"classification":"data_error","message":"dividend 0 ex)") ==
      7);
  CHECK(contains(r.out, R"("date":"2019-06-03")"));
  CHECK(contains(
      r.out,
      R"("severity":"error","p_error":1,"classification":"data_error","message":"dividend -0.25 ex )" +
          day(40) + " is negative\""));
  CHECK(contains(r.out, "×100 the series' usual 0.25 (the median of 5 others)"));
  CHECK(contains(r.out, R"("date":")" + day(300) + R"(","line":302,"code":"DQ2)"));
}

// ---------------------------------------------------------------------------
// Cross-sectional

TEST_CASE("DQ601: a family splitting together, and the siblings' evidence") {
  std::string csv;
  std::string meta = "series,peer_group\n";
  for (int s = 0; s < 4; ++s) {
    std::vector<Bar> bars = stock(400, 60.0, 0.015, 10 + static_cast<std::uint64_t>(s));
    split(bars, 250, 0.5);
    const std::string id = "F" + std::to_string(s);
    csv += bars_csv(id, bars, s == 0);
    meta += id + ",FAM\n";
  }
  csv += bars_csv("Z", stock(400, 60.0, 0.015, 20), false);
  const Result r =
      check(csv, {{"meta.csv", meta}},
            {"--isolated", "--select", "DQ2,DQ6", "--format", "jsonl", "--meta", "meta.csv"});
  CAPTURE(r.out);
  CHECK(count_matching(r.out, R"("code":"DQ601")") == 1);
  CHECK(contains(r.out, "4 series move"));
  CHECK(contains(r.out, "all of peer group FAM"));
  CHECK(contains(r.out, R"("classification":"context_gap")"));
  CHECK(count_matching(r.out, R"("code":"DQ203")") == 4);
  CHECK(count_matching(r.out, R"("feature":"peer_group")") == 4);
}

TEST_CASE("--market: a crash the market explains, and DQ602") {
  // A market that falls 25% one day; a stock with beta 1.5 falls with it.
  Noise noise(30);
  std::vector<double> market(400);
  for (double& m : market) {
    m = 0.008 * noise.normal();
  }
  market[250] = std::log(0.75);
  std::string market_csv = "date,value\n";
  double level = 100.0;
  const auto days = sessions(400);
  for (std::size_t i = 0; i < market.size(); ++i) {
    level *= std::exp(market[i]);
    market_csv += days[i].to_string() + "," + fixed(level, 2) + "\n";
  }
  const std::string csv = bars_csv("A", stock(400, 60.0, 0.01, 31, market, 1.5));
  const Result alone = check(csv, {}, {"--isolated", "--select", "DQ2", "--format", "jsonl"});
  const Result with = check(csv, {{"spy.csv", market_csv}},
                            {"--isolated", "--select", "DQ2,DQ6", "--format", "jsonl",
                             "--show-info", "--market", "spy.csv"});
  CAPTURE(alone.out);
  CAPTURE(with.out);
  CHECK(count_matching(with.out, R"("code":"DQ602")") == 1);
  CHECK(contains(with.out, "the market (spy) moved −25%"));
  // Judged net of the market, the stock's day is ordinary: not even DQ209.
  CHECK_FALSE(contains(with.out, R"("code":"DQ20)"));
}

// ---------------------------------------------------------------------------
// fafnir's labelled shadow nights (0.10.0): split rows, dividends, cohorts.

namespace {

Result check_actions(const std::vector<Bar>& bars, const std::string& actions, int decimals = 2,
                     bool show_info = false) {
  const std::string csv = bars_csv("A", bars, true, decimals);
  if (show_info) {
    return check(csv, {{"a.csv", std::string{kActionsHeader} + actions}},
                 {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv",
                  "--show-info"});
  }
  return check(csv, {{"a.csv", std::string{kActionsHeader} + actions}},
               {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv"});
}

}  // namespace

TEST_CASE("P2: a duplicate split row is DQ702, deleted, never re-dated onto its twin") {
  std::vector<Bar> bars = stock(400, 0.4, 0.03, 50);
  split(bars, 200, 10.0);  // 1:10, in the bars at bar 200
  SUBCASE("the stray row before the real one") {
    const Result r =
        check_actions(bars, "A," + day(199) + ",split,1,10,\nA," + day(200) + ",split,1,10,\n", 4);
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ702")") == 1);
    CHECK(count_matching(r.out, R"("code":"DQ701")") == 0);
    CHECK(count_matching(r.out, R"("code":"DQ20)") == 0);
    CHECK(contains(r.out, R"("date":")" + day(199) + R"(",)"));
    CHECK(contains(r.out, R"("duplicate_of_ex_date":")" + day(200) + "\""));
    CHECK(
        contains(r.out, R"("suggested_action":{"kind":"delete_split","ratio":"1:10","ex_date":")" +
                            day(199) + "\"}"));
    CHECK_FALSE(contains(r.out, "redate_split"));
  }
  SUBCASE("the stray row after it") {
    const Result r =
        check_actions(bars, "A," + day(200) + ",split,1,10,\nA," + day(201) + ",split,1,10,\n", 4);
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ702")") == 1);
    CHECK(contains(r.out, R"("duplicate_of_ex_date":")" + day(200) + "\""));
  }
  SUBCASE("one row, a session early, is still misdated") {
    const Result r = check_actions(bars, "A," + day(199) + ",split,1,10,\n", 4);
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ701")") == 1);
    CHECK(contains(r.out, R"("new_ex_date":")" + day(200) + "\""));
  }
}

TEST_CASE("P2: sessions before a split stored at its post-split scale are a DQ202 era") {
  // A real 1:50 at bar 200; the vendor served 195..199 at the post-split scale
  // with their own volume, the level before the split.
  std::vector<Bar> bars = stock(400, 0.09, 0.04, 51);
  split(bars, 200, 50.0);
  std::vector<Bar> era = bars;
  for (std::size_t i = 195; i < 200; ++i) {
    era[i].open *= 50.0;
    era[i].high *= 50.0;
    era[i].low *= 50.0;
    era[i].close *= 50.0;
  }
  const std::string actions = "A," + day(200) + ",split,1,50,\n";
  const Result r = check_actions(era, actions, 4);
  CAPTURE(r.out);
  CHECK(count_matching(r.out, R"("code":"DQ701")") == 0);
  CHECK(count_matching(r.out, R"("code":"DQ202")") == 1);
  CHECK(contains(r.out, R"("date":")" + day(195) + R"(","end_date":")" + day(199) + "\""));
  CHECK(contains(r.out, "the sessions before the 1:50 split on file ex " + day(200)));
  CHECK(contains(r.out, R"("kind":"rescale","first":")" + day(195) + R"(","last":")" + day(199) +
                            R"(","factor":0.02})"));

  // The same move at 195 with the volume already at the post-split level: the
  // split took effect there, and the row on file is misdated.
  std::vector<Bar> early = stock(400, 0.09, 0.04, 51);
  split(early, 195, 50.0);
  const Result misdated = check_actions(early, actions, 4);
  CAPTURE(misdated.out);
  CHECK(count_matching(misdated.out, R"("code":"DQ701")") == 1);
  CHECK(count_matching(misdated.out, R"("code":"DQ202")") == 0);
}

TEST_CASE("P2: a bad print's revert neither confirms nor misdates a split") {
  // A phantom 2:1 on file, with two bars at twice the price a few sessions
  // before it: the revert from them is no split.
  std::vector<Bar> bars = stock(400, 10.0, 0.015, 52);
  for (const std::size_t i : {196U, 197U}) {
    bars[i].open *= 2.07;
    bars[i].high *= 2.07;
    bars[i].low *= 2.07;
    bars[i].close *= 2.07;
  }
  const Result r = check_actions(bars, "A," + day(199) + ",split,2,1,\n");
  CAPTURE(r.out);
  CHECK(count_matching(r.out, R"("code":"DQ201")") == 1);
  CHECK(count_matching(r.out, R"("code":"DQ702")") == 1);
  CHECK(count_matching(r.out, R"("code":"DQ701")") == 0);
}

TEST_CASE("--restore-actions: the actions as they stood, rows replaced or taken out") {
  std::vector<Bar> bars = stock(400, 0.4, 0.03, 53);
  split(bars, 200, 10.0);
  const std::string csv = bars_csv("A", bars, true, 4);
  const std::string actions = std::string{kActionsHeader} + "A," + day(200) + ",split,1,10,\n";
  // An operator deleted a duplicate row: as it stood, it was on file.
  const std::string deleted =
      "security_id,ex_date,action_type,split_numerator,"
      "split_denominator,dividend_amount,deleted_at_utc\n"
      "A," +
      day(199) + ",split,1.000000,10.000000,,2026-10-14T17:15:47\n";
  const Result before = check(csv, {{"a.csv", actions}, {"before.csv", deleted}},
                              {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl",
                               "--actions", "a.csv", "--restore-actions", "before.csv"});
  CAPTURE(before.out);
  CHECK(count_matching(before.out, R"("code":"DQ702")") == 1);
  const Result after =
      check(csv, {{"a.csv", actions}},
            {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl", "--actions", "a.csv"});
  CHECK(after.out.empty());

  // A split the repair added (to fix a DQ203) was not on file before it.
  const std::string added =
      std::string{"series,ex_date,type,numerator,denominator,added_by_repair\n"} + "A," + day(200) +
      ",split,1,10,true\n";
  const Result missing = check(csv, {{"a.csv", actions}, {"before.csv", added}},
                               {"--isolated", "--select", "DQ2,DQ7", "--format", "jsonl",
                                "--actions", "a.csv", "--restore-actions", "before.csv"});
  CAPTURE(missing.out);
  CHECK(count_matching(missing.out, R"("code":"DQ203")") == 1);

  // A row the reader cannot use is DQ109, named by its file.
  const Result bad = check(csv, {{"a.csv", actions}, {"before.csv", added + "A,x,split,1,2,\n"}},
                           {"--isolated", "--select", "DQ109", "--format", "jsonl", "--actions",
                            "a.csv", "--restore-actions", "before.csv"});
  CAPTURE(bad.out);
  CHECK(count_matching(bad.out, R"("code":"DQ109")") == 1);
  CHECK(contains(bad.out, "before.csv"));
}

TEST_CASE("P3: a dividend ten times the usual that the close fell by is corroborated") {
  // A fund paying 0.11 a quarter, then a 3.0 distribution its price falls by.
  std::vector<Bar> bars = stock(400, 35.0, 0.004, 54);
  std::string actions;
  for (int i = 20; i < 380; i += 63) {
    actions += "A," + day(i) + ",dividend,,,0.110000\n";
  }
  const double amount = distribute(bars, 390, 3.0 / bars[389].close);
  const std::string paid = actions + "A," + day(390) + ",dividend,,," + fixed(amount, 4) + "\n";
  const auto run_705 = [](const std::vector<Bar>& b, const std::string& a, bool info) {
    return check(
        bars_csv("A", b), {{"a.csv", std::string{kActionsHeader} + a}},
        info ? std::initializer_list<const char*>{"--isolated", "--select", "DQ705", "--format",
                                                  "jsonl", "--actions", "a.csv", "--show-info"}
             : std::initializer_list<const char*>{"--isolated", "--select", "DQ705", "--format",
                                                  "jsonl", "--actions", "a.csv"});
  };
  const Result corroborated = run_705(bars, paid, true);
  CAPTURE(corroborated.out);
  CHECK(count_matching(corroborated.out, R"("code":"DQ705")") == 1);
  CHECK(contains(corroborated.out, R"("severity":"info")"));
  CHECK(contains(corroborated.out, R"("corroborated":true)"));
  CHECK(contains(corroborated.out, R"("kind":"special")"));
  CHECK(run_705(bars, paid, false).out.empty());

  // The same amount where the price did not fall: a slip, at warn.
  const std::vector<Bar> held = stock(400, 35.0, 0.004, 54);
  const Result slip = run_705(held, paid, false);
  CAPTURE(slip.out);
  CHECK(count_matching(slip.out, R"("severity":"warn")") == 1);
  CHECK(contains(slip.out, R"("corroborated":false)"));
  CHECK(contains(slip.out, R"("kind":"slip")"));
  CHECK(contains(slip.out, "did not fall by it"));

  // A tenth of the usual comes off within a day's noise either way: no close can
  // corroborate it, so it is reported.
  const Result small = run_705(held, actions + "A," + day(390) + ",dividend,,,0.003000\n", false);
  CAPTURE(small.out);
  CHECK(count_matching(small.out, R"("severity":"warn")") == 1);
}

TEST_CASE("P3: a switch to daily accruals is one report, at the first accrual") {
  const std::vector<Bar> bars = stock(400, 9.2, 0.002, 55);
  std::string monthly;
  for (int i = 10; i < 390; i += 21) {
    monthly += "A," + day(i) + ",dividend,,,0.041000\n";
  }
  // The first accrual alone looks like a slip, at its own date.
  const Result first =
      check(bars_csv("A", bars),
            {{"a.csv",
              std::string{kActionsHeader} + monthly + "A," + day(392) + ",dividend,,,0.001447\n"}},
            {"--isolated", "--select", "DQ705", "--format", "fafnir", "--actions", "a.csv"});
  CAPTURE(first.out);
  CHECK(count_matching(first.out, R"("check_name":"dorq_dividend_implausible")") == 1);
  CHECK(contains(first.out, R"("record_key":{"trade_date":")" + day(392) + "\"}"));
  CHECK(contains(first.out, R"("kind":"slip")"));

  // With more of them, a weekend's three-day accrual among them: one report, under
  // the same key.
  std::string daily = monthly;
  for (int i = 392; i < 400; ++i) {
    daily += "A," + day(i) + ",dividend,,," + (i == 395 ? "0.003985" : "0.001340") + "\n";
  }
  const Result r =
      check(bars_csv("A", bars), {{"a.csv", std::string{kActionsHeader} + daily}},
            {"--isolated", "--select", "DQ705", "--format", "fafnir", "--actions", "a.csv"});
  CAPTURE(r.out);
  CHECK(count_matching(r.out, R"("check_name":"dorq_dividend_implausible")") == 1);
  CHECK(contains(r.out, R"("record_key":{"trade_date":")" + day(392) + "\"}"));
  CHECK(contains(r.out, R"("kind":"frequency_change")"));
  CHECK(contains(r.out, R"("severity":"warn")"));
  CHECK(contains(r.out, "counted twice"));
}

TEST_CASE("N3: DQ601 reports a cohort only beyond the day's base rate") {
  SUBCASE("fifty unrelated series split 2:1 on one date") {
    std::string csv;
    for (int s = 0; s < 50; ++s) {
      std::vector<Bar> bars = stock(300, 60.0, 0.012, 100 + static_cast<std::uint64_t>(s));
      split(bars, 250, 0.5);
      csv += bars_csv("S" + std::to_string(s), bars, s == 0);
    }
    const Result r = check(csv, {}, {"--isolated", "--select", "DQ601", "--format", "fafnir"});
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("check_name":"dorq_cohort_move")") == 1);
    CHECK(contains(r.out, R"("record_key":{"trade_date":")" + day(250) + R"(","ratio":"2:1"})"));
    CHECK(contains(r.out, R"("base_rate":)"));
    CHECK(contains(r.out, R"("p_value":)"));
    CHECK(contains(r.out, R"("members_dropped_as_market":"")"));
  }
  SUBCASE("as many moves near the ratio as on the days around it are chance") {
    // Forty names, three of which fall about a third on each of twenty days, and
    // four on one of them: the band is busy every day, so four is no cohort.
    std::vector<std::vector<Bar>> all;
    all.reserve(40);
    for (int s = 0; s < 40; ++s) {
      all.push_back(stock(300, 30.0, 0.01, 300 + static_cast<std::uint64_t>(s)));
    }
    for (int d = 240; d < 260; ++d) {
      for (int j = 0; j < (d == 250 ? 4 : 3); ++j) {
        split(all[static_cast<std::size_t>((d * 3 + j) % 40)], static_cast<std::size_t>(d), 0.65,
              false);
      }
    }
    std::string csv;
    for (std::size_t s = 0; s < all.size(); ++s) {
      csv += bars_csv("B" + std::to_string(s), all[s], s == 0);
    }
    const Result r = check(csv, {}, {"--isolated", "--select", "DQ601", "--format", "jsonl"});
    CAPTURE(r.out);
    CHECK(count_matching(r.out, R"("code":"DQ601")") == 0);
    // Only the base rate holds them back: with any tail allowed, the days' trios
    // are cohorts, each with about one or two such moves a day around it.
    const Result any = check(csv, {{"dorq.toml", "[coverage]\ncohort_move_max_tail = 1\n"}},
                             {"--select", "DQ601", "--format", "jsonl"});
    CAPTURE(any.out);
    CHECK(count_matching(any.out, R"("code":"DQ601")") >= 3);
    CHECK(count_matching(any.out, R"("base_rate":0)") +
              count_matching(any.out, R"("base_rate":1)") >=
          3);
  }
}
