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

// From bar `from` on, every price times `factor` and volume divided by it.
void split(std::vector<Bar>& bars, std::size_t from, double factor) {
  for (std::size_t i = from; i < bars.size(); ++i) {
    bars[i].open *= factor;
    bars[i].high *= factor;
    bars[i].low *= factor;
    bars[i].close *= factor;
    bars[i].volume = std::round(bars[i].volume / factor);
  }
}

std::string bars_csv(const std::string& id, const std::vector<Bar>& bars, bool header = true) {
  const auto days = sessions(static_cast<int>(bars.size()));
  std::string csv = header ? "series,date,open,high,low,close,volume\n" : "";
  for (std::size_t i = 0; i < bars.size(); ++i) {
    const Bar& b = bars[i];
    csv += id + "," + days[i].to_string() + "," + fixed(b.open, 2) + "," + fixed(b.high, 2) + "," +
           fixed(b.low, 2) + "," + fixed(b.close, 2) + "," + fixed(b.volume, 0) + "\n";
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

TEST_CASE("actions: fafnir's columns or dorq's, sorted, and errors that name the line") {
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
  CHECK(error(std::string{kActionsHeader} + "A,2020-01-02,spinoff,,,\n") ==
        "a.csv line 2: type \"spinoff\" is not split or dividend");
  CHECK(contains(error(std::string{kActionsHeader} + "A,2020-01-02,split,2,,\n"),
                 "a split needs a numerator and a denominator"));
  CHECK(contains(error(std::string{kActionsHeader} + "A,soon,split,2,1,\n"), "is not a date"));
  CHECK(error(std::string{kActionsHeader} + "A,2020-01-02,dividend,,,x\n") ==
        "a.csv line 2: amount \"x\" is not a number");
  CHECK(error(std::string{kActionsHeader} + "A,2020-01-02,dividend,,,\n") ==
        "a.csv line 2: a dividend needs an amount");
  CHECK(contains(error("series,date\nA,2020-01-02\n"), "needs series, ex_date and type"));
  CHECK(error("") == "a.csv is empty");
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

  dorq::Context twice;
  std::istringstream dup("series,asset_type\nA,etf\nA,fund\n");
  CHECK_THROWS_WITH_AS(dorq::read_meta(dup, "m.csv", twice),
                       "m.csv line 3: series \"A\" appears twice", dorq::InputError);
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
  result = check(csv, {{"m.csv", "series,date,close\nA,2020-01-02,1\nB,2020-01-02,1\n"}},
                 {"--isolated", "--market", "m.csv"});
  CHECK(result.status == 3);
  CHECK(contains(result.err, "the market file must hold one series (it holds 2)"));
}

// ---------------------------------------------------------------------------
// Splits on file

TEST_CASE("a split on file explains its move; one missing from the file is DQ203") {
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
  CHECK(contains(explained.out, R"("name":"explained_split")"));
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
