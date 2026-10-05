#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "price/tick.hpp"
#include "stats/nig.hpp"
#include "stats/robust.hpp"
#include "stats/student_t.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run;

namespace {

// ---------------------------------------------------------------------------
// A deterministic market: a random walk with fat-ish tails, and bars around it.

class Noise {
 public:
  explicit Noise(std::uint64_t seed) : state_(seed) {}
  double uniform() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(state_ >> 11U) * 0x1.0p-53;
  }
  double normal() {  // Irwin-Hall: plenty for test data
    double sum = 0.0;
    for (int i = 0; i < 12; ++i) {
      sum += uniform();
    }
    return sum - 6.0;
  }

 private:
  std::uint64_t state_;
};

struct Row {
  dorq::Date date;
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

std::vector<Row> walk(int n, double start, double vol, double volume, std::uint64_t seed) {
  static const dorq::Calendar kCalendar;
  Noise noise(seed);
  std::vector<Row> rows;
  double price = start;
  dorq::Date day = dorq::Date::from_ymd(2020, 1, 2);
  while (static_cast<int>(rows.size()) < n) {
    if (kCalendar.is_session(day)) {
      const double prev = price;
      price *= std::exp(vol * noise.normal());
      Row row;
      row.date = day;
      row.close = price;
      row.open = prev * std::exp(0.3 * vol * noise.normal());
      row.high = std::max(row.open, row.close) * (1.0 + 0.4 * vol * noise.uniform());
      row.low = std::min(row.open, row.close) * (1.0 - 0.4 * vol * noise.uniform());
      row.volume = std::round(volume * std::exp(0.3 * noise.normal()));
      rows.push_back(row);
    }
    day = dorq::Date::from_days(day.days() + 1);
  }
  return rows;
}

std::string price_text(double price, int decimals) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << price;
  return out.str();
}

std::string to_csv(const std::string& id, const std::vector<Row>& rows, int decimals = 2,
                   bool header = true) {
  std::string csv = header ? "series,date,open,high,low,close,volume\n" : "";
  for (const Row& r : rows) {
    csv += id + "," + r.date.to_string() + "," + price_text(r.open, decimals) + "," +
           price_text(r.high, decimals) + "," + price_text(r.low, decimals) + "," +
           price_text(r.close, decimals) + "," + price_text(r.volume, 0) + "\n";
  }
  return csv;
}

void scale_bar(Row& row, double factor) {
  row.open *= factor;
  row.high *= factor;
  row.low *= factor;
  row.close *= factor;
}

// The price checks' findings as JSON lines, optionally with info.
Result check_prices(const std::string& csv, bool show_info = false) {
  if (show_info) {
    return run(
        {"--isolated", "--select", "DQ2", "--ignore", "DQ206", "--format", "jsonl", "--show-info"},
        csv);
  }
  return run({"--isolated", "--select", "DQ2", "--ignore", "DQ206", "--format", "jsonl"}, csv);
}

}  // namespace

// ---------------------------------------------------------------------------
// Statistics

TEST_CASE("Student t against SciPy") {
  // scipy.stats.t.logpdf(1.5, 4, scale=2.0), t.logpdf(-0.3, 1, scale=0.3)
  CHECK(dorq::stats::student_t_log_pdf(1.5, 4.0, 2.0) == doctest::Approx(-2.00291732804347));
  CHECK(dorq::stats::student_t_log_pdf(-0.3, 1.0, 0.3) == doctest::Approx(-0.6339042620834092));
  // 2 * t.sf(2.7764451, 4) and 2 * t.sf(8.6103016, 4)
  CHECK(dorq::stats::student_t_two_sided_tail(2.776445105197793, 4.0) ==
        doctest::Approx(0.05).epsilon(1e-10));
  CHECK(dorq::stats::student_t_two_sided_tail(-8.610301581, 4.0) ==
        doctest::Approx(0.001).epsilon(1e-8));
  // t.cdf(-1.2, 7.5), t.cdf(3.0, 30)
  CHECK(dorq::stats::student_t_cdf(-1.2, 7.5) == doctest::Approx(0.13333446139661093));
  CHECK(dorq::stats::student_t_cdf(3.0, 30.0) == doctest::Approx(0.9973050179671741));
  CHECK(dorq::stats::student_t_two_sided_tail(std::numeric_limits<double>::infinity(), 4.0) == 0.0);
}

TEST_CASE("discounted Normal-Inverse-Gamma volatility") {
  using dorq::stats::DiscountedNig;
  DiscountedNig nig = DiscountedNig::prior(0.0004, 10.0);
  CHECK(nig.variance() == doctest::Approx(0.0004));
  // Returns of 0.02 keep the estimate where it is; the forgetting factor bounds
  // the weight at about 1 / (1 - discount) returns.
  for (int i = 0; i < 500; ++i) {
    nig.update(i % 2 == 0 ? 0.02 : -0.02, 0.97, 4.0);
  }
  CHECK(nig.variance() == doctest::Approx(0.0004).epsilon(1e-6));
  CHECK(2.0 * nig.a == doctest::Approx(1.0 / 0.03).epsilon(1e-3));
  // A clipped outlier moves it by at most clip^2 / weight.
  DiscountedNig clipped = nig;
  clipped.update(5.0, 0.97, 4.0);
  CHECK(clipped.variance() < 1.6 * nig.variance());
  DiscountedNig unclipped = nig;
  unclipped.update(5.0, 0.97, 0.0);
  CHECK(unclipped.variance() > 100.0 * nig.variance());
  const DiscountedNig both = dorq::stats::combine(nig, nig);
  CHECK(both.variance() == doctest::Approx(nig.variance()));
  CHECK(both.a == doctest::Approx(2.0 * nig.a));
}

TEST_CASE("median and MAD") {
  CHECK(dorq::stats::median({3.0, 1.0, 2.0}) == 2.0);
  CHECK(dorq::stats::median({4.0, 1.0, 3.0, 2.0}) == 2.0);  // the lower median
  CHECK(std::isnan(dorq::stats::median({})));
  const std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 100.0};
  CHECK(dorq::stats::mad(values) == doctest::Approx(1.4826));
}

TEST_CASE("tick size by era and price") {
  using dorq::Date;
  CHECK(dorq::tick_size(Date::from_ymd(1995, 3, 1), 20.0) == 0.125);
  CHECK(dorq::tick_size(Date::from_ymd(1999, 3, 1), 20.0) == 0.0625);
  CHECK(dorq::tick_size(Date::from_ymd(2010, 3, 1), 20.0) == 0.01);
  CHECK(dorq::tick_size(Date::from_ymd(2010, 3, 1), 0.5) == 0.0001);
}

// ---------------------------------------------------------------------------
// Configuration

TEST_CASE("split ratios") {
  const auto two = dorq::parse_split_ratio("2:1");
  REQUIRE(two);
  CHECK(two->price_factor() == 0.5);
  CHECK(two->to_string() == "2:1");
  const auto reverse = dorq::parse_split_ratio(" 1 : 10 ");
  REQUIRE(reverse);
  CHECK(reverse->price_factor() == 10.0);
  CHECK_FALSE(dorq::parse_split_ratio("2"));
  CHECK_FALSE(dorq::parse_split_ratio("2:2"));
  CHECK_FALSE(dorq::parse_split_ratio("0:1"));
  CHECK_FALSE(dorq::parse_split_ratio("a:1"));
  CHECK_FALSE(dorq::parse_split_ratio("1234567:1"));
}

TEST_CASE("[price] and [priors] tables") {
  const dorq::Config config = dorq::parse_config(R"(
[price]
floor_move = 0.8
split_ratios = ["2:1", "1:10"]
provisional_bars = 2

[priors]
bad_print = 0.1

[profiles.funds]
match = { kind = "point" }
price = { segment_gap = 30 }
priors = { scale_error = 0.05 }
)",
                                                 "dorq.toml", false);
  CHECK(config.price.floor_move == 0.8);
  CHECK(config.price.split_ratios.size() == 2);
  CHECK(config.price.provisional_bars == 2);
  CHECK(config.price.priors.bad_print == 0.1);
  CHECK(config.price.priors.market_move == 0.9);
  REQUIRE(config.profiles.size() == 1);
  dorq::PriceSettings settings = config.price;
  config.profiles[0].price.apply_to(settings);
  CHECK(settings.segment_gap == 30);
  CHECK(settings.priors.scale_error == 0.05);
  CHECK(settings.floor_move == 0.8);

  // The effective config reads back the same.
  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  // And the starter file is the defaults.
  CHECK(dorq::to_toml(dorq::parse_config(dorq::starter_config(), "starter.toml", false)) ==
        dorq::to_toml(dorq::Config{}));

  CHECK_THROWS_WITH_AS(
      static_cast<void>(dorq::parse_config("[price]\nsplit_ratios = [\"2-1\"]", "d.toml", false)),
      doctest::Contains("ratios like"), dorq::ConfigError);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(dorq::parse_config("[priors]\nbogus = 1", "d.toml", false)),
      doctest::Contains("unknown key \"priors.bogus\""), dorq::ConfigError);
  CHECK_THROWS_AS(
      static_cast<void>(dorq::parse_config("[price]\nrevert_max_bars = 0", "d.toml", false)),
      dorq::ConfigError);
}

// ---------------------------------------------------------------------------
// The checks

TEST_CASE("a clean random walk reports nothing at warn") {
  std::string csv = to_csv("A", walk(600, 50.0, 0.015, 1e6, 1));
  csv += to_csv("B", walk(600, 8.0, 0.03, 5e4, 2), 2, false);
  const Result result = check_prices(csv);
  CAPTURE(result.out);
  CHECK(result.status == 0);
  CHECK(result.out.empty());
}

TEST_CASE("DQ201: a bad print, and a block of them") {
  std::vector<Row> rows = walk(300, 40.0, 0.015, 1e6, 3);
  scale_bar(rows[100], 10.0);  // a decimal slip, one bar
  scale_bar(rows[200], 0.6);   // three bars at a plausible but wrong level
  scale_bar(rows[201], 0.6);
  scale_bar(rows[202], 0.6);
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ201")") == 2);
  CHECK(count_matching(result.out, R"("severity":"error")") == 2);
  CHECK(contains(result.out, "\"date\":\"" + rows[100].date.to_string() + "\""));
  CHECK(contains(result.out, "\"date\":\"" + rows[200].date.to_string() + "\",\"end_date\":\"" +
                                 rows[202].date.to_string() + "\""));
  CHECK(contains(result.out, R"("suggested_action":{"kind":"delete_bars")"));
  CHECK(contains(result.out, R"("bars":3})"));
  CHECK(contains(result.out, R"("feature":"next_bars")"));
  CHECK(contains(result.out, R"("classification":"data_error")"));
  // The return back from each is explained, not reported again.
  CHECK(count_matching(result.out, R"("code":"DQ2)") == 2);
}

TEST_CASE("DQ203: a split the data does not record") {
  std::vector<Row> rows = walk(300, 120.0, 0.015, 1e6, 4);
  for (std::size_t i = 150; i < rows.size(); ++i) {
    scale_bar(rows[i], 0.5);
    rows[i].volume *= 2.0;
  }
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ203")") == 1);
  CHECK(count_matching(result.out, R"("code":"DQ2)") == 1);
  CHECK(contains(result.out, R"("classification":"context_gap")"));
  CHECK(contains(result.out, R"("suggested_action":{"kind":"add_split","ratio":"2:1","ex_date":")" +
                                 rows[150].date.to_string() + "\"}"));
  CHECK(contains(result.out, "≈ a 2:1 split"));

  // A reverse split of a penny stock.
  std::vector<Row> penny = walk(300, 0.4, 0.03, 2e5, 5);
  for (std::size_t i = 150; i < penny.size(); ++i) {
    scale_bar(penny[i], 10.0);
    penny[i].volume /= 10.0;
  }
  const Result reverse = check_prices(to_csv("P", penny, 4));
  CAPTURE(reverse.out);
  CHECK(count_matching(reverse.out, R"("code":"DQ203")") == 1);
  CHECK(contains(reverse.out, R"("ratio":"1:10")"));
}

TEST_CASE("DQ202: an era at the wrong scale is reported once") {
  std::vector<Row> rows = walk(300, 30.0, 0.015, 1e6, 6);
  for (std::size_t i = 100; i < 160; ++i) {
    scale_bar(rows[i], 100.0);
  }
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ202")") == 1);
  CHECK(count_matching(result.out, R"("code":"DQ2)") == 1);
  CHECK(contains(result.out, "\"date\":\"" + rows[100].date.to_string() + "\",\"end_date\":\"" +
                                 rows[159].date.to_string() + "\""));
  CHECK(contains(result.out, R"("suggested_action":{"kind":"rescale")"));
  CHECK(contains(result.out, "an era at the wrong scale"));
}

TEST_CASE("DQ204: only the close is wrong") {
  std::vector<Row> rows = walk(300, 60.0, 0.015, 1e6, 7);
  rows[120].close *= 0.5;  // open, high and low untouched
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ204")") == 1);
  CHECK(count_matching(result.out, R"("code":"DQ2)") == 1);
  CHECK(contains(result.out, R"("suggested_action":{"kind":"refetch_bar")"));
}

TEST_CASE("DQ205: a new history after a long gap") {
  std::vector<Row> rows = walk(500, 4.0, 0.04, 3e4, 8);
  rows.erase(rows.begin() + 200, rows.begin() + 320);  // 120 sessions with no bar
  const double scale = 25.0 / rows[200].close;
  for (std::size_t i = 200; i < rows.size(); ++i) {
    scale_bar(rows[i], scale);
    rows[i].volume *= 20.0;
  }
  rows[200].close = 25.0;
  rows[200].high = std::max(rows[200].high, 25.0);
  rows[200].low = std::min(rows[200].low, 25.0);
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ205")") == 1);
  CHECK(contains(result.out, "\"date\":\"" + rows[200].date.to_string() + "\""));
  CHECK(contains(result.out, R"("kind":"split_history")"));
}

TEST_CASE("the newest bar is provisional, and at most a warning") {
  std::vector<Row> rows = walk(300, 50.0, 0.015, 1e6, 9);
  scale_bar(rows.back(), 100.0);
  const Result result = check_prices(to_csv("A", rows));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("provisional":true)") == 1);
  CHECK(count_matching(result.out, R"("severity":"warn")") == 1);
  CHECK(contains(result.out, ", provisional"));
}

TEST_CASE("tick moves on a coarse grid are not errors") {
  // A sub-dime stock quoted in cents: every move is 10-50%.
  std::vector<Row> rows = walk(400, 0.05, 0.05, 3e5, 10);
  for (Row& r : rows) {
    r.close = std::max(0.01, std::round(r.close * 100.0) / 100.0);
    r.open = std::max(0.01, std::round(r.open * 100.0) / 100.0);
    r.high = std::max({r.open, r.close}) + 0.01;
    r.low = std::max(0.01, std::min({r.open, r.close}) - 0.01);
  }
  const Result result = check_prices(to_csv("P", rows));
  CAPTURE(result.out);
  CHECK(result.out.empty());
}

TEST_CASE("carry bars are not prices") {
  // A thin name: most days nothing trades and the last close is repeated on zero
  // volume; when it trades, the move covers all those sessions.
  std::vector<Row> rows = walk(400, 6.0, 0.03, 2000, 11);
  Noise noise(12);
  double last = rows.front().close;
  for (Row& r : rows) {
    if (noise.uniform() < 0.7) {
      r.open = r.high = r.low = r.close = last;
      r.volume = 0;
    } else {
      last = r.close;
    }
  }
  const Result result = check_prices(to_csv("T", rows));
  CAPTURE(result.out);
  CHECK(result.out.empty());
}

TEST_CASE("point series: the log scale when positive, the difference scale otherwise") {
  std::string csv = "date,value\n";
  std::vector<Row> rows = walk(300, 100.0, 0.01, 0, 13);
  rows[150].close *= 100.0;
  for (const Row& r : rows) {
    csv += r.date.to_string() + "," + price_text(r.close, 4) + "\n";
  }
  const Result result = check_prices(csv);
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ201")") == 1);

  // A rate that crosses zero is judged on the difference scale (transform "auto"):
  // its ordinary moves are clean.
  std::string rates = "date,value\n";
  Noise noise(14);
  for (const Row& r : rows) {
    rates += r.date.to_string() + "," + price_text(noise.normal() * 0.5, 4) + "\n";
  }
  CHECK(check_prices(rates).out.empty());
}

TEST_CASE("DQ209 is info: shown with --show-info") {
  std::vector<Row> rows = walk(300, 50.0, 0.015, 1e6, 15);
  // A real crash: a 42% fall on heavy volume (no split ratio is near 0.58), and the
  // weeks after it volatile and busy.
  Noise noise(17);
  double drift = 0.58;
  for (std::size_t i = 150; i < rows.size(); ++i) {
    if (i > 150 && i < 180) {
      drift *= std::exp(0.06 * noise.normal());
      rows[i].volume *= 3.0;
    }
    scale_bar(rows[i], drift);
  }
  rows[150].volume *= 8.0;
  const Result quiet = check_prices(to_csv("A", rows));
  CHECK(quiet.out.empty());
  const Result info = check_prices(to_csv("A", rows), true);
  CAPTURE(info.out);
  CHECK(contains(info.out, R"("code":"DQ209","check":"large-move","severity":"info")"));
  CHECK(contains(info.out, R"("classification":"market_fact")"));
  CHECK(contains(info.out, R"("hypotheses":{"market_move":)"));
}

TEST_CASE("every record carries the model's keys, and --show-evidence prints them") {
  std::vector<Row> rows = walk(300, 40.0, 0.015, 1e6, 16);
  scale_bar(rows[100], 10.0);
  const std::string csv = to_csv("A", rows) + "A,2021-06-01,1,0.5,1,1,1\n";  // DQ101
  const Result jsonl = run({"--isolated", "--format", "jsonl"}, csv);
  CAPTURE(jsonl.out);
  CHECK(contains(jsonl.out,
                 R"("hypotheses":{},"evidence":[],"suggested_action":null,"provisional":false)"));
  CHECK(contains(jsonl.out, R"("evidence":[{"feature":"prior","value":)"));

  const Result text = run({"--isolated", "--show-evidence"}, csv);
  CAPTURE(text.out);
  CHECK(contains(text.out, "      → delete or re-fetch " + rows[100].date.to_string()));
  CHECK(contains(text.out, "      hypotheses: bad_print "));
  CHECK(contains(text.out, "      evidence (log Bayes factor against market_move):"));
  CHECK(contains(text.out, "        return "));
  // Without the flag, one line per violation.
  const Result plain = run({"--isolated"}, csv);
  CHECK_FALSE(contains(plain.out, "hypotheses:"));
}

TEST_CASE("price findings do not depend on the thread count") {
  std::string csv;
  for (int s = 0; s < 12; ++s) {
    const auto index = static_cast<std::size_t>(s);
    std::vector<Row> rows = walk(400, 20.0 + s, 0.02, 1e5 * (s + 1), 100 + index);
    scale_bar(rows[50 + 20 * index], s % 2 == 0 ? 10.0 : 0.1);
    csv += to_csv("S" + std::to_string(s), rows, 2, s == 0);
  }
  const Result one = run({"--isolated", "--show-info", "--format", "jsonl", "--threads", "1"}, csv);
  const Result many =
      run({"--isolated", "--show-info", "--format", "jsonl", "--threads", "8"}, csv);
  CHECK(count_matching(one.out, R"("code":"DQ201")") == 12);
  CHECK(one.out == many.out);
}
