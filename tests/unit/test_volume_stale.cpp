// M4: point series on the difference scale and bounds (DQ108), volume (DQ401-DQ403)
// and stale values (DQ501, DQ502).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::count_matching;
using dorq::test::Result;
using dorq::test::run;
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

// A liquid stock: a random walk at `vol` a day, quoted in cents, on volume about
// `volume` with day-to-day noise.
std::vector<Bar> stock(int n, double start, double vol, double volume, std::uint64_t seed) {
  Noise noise(seed);
  std::vector<Bar> bars;
  double price = start;
  for (int i = 0; i < n; ++i) {
    const double prev = price;
    price *= std::exp(vol * noise.normal());
    Bar b;
    b.close = std::round(price * 100.0) / 100.0;
    b.open = std::round(prev * std::exp(0.3 * vol * noise.normal()) * 100.0) / 100.0;
    b.high = std::max(b.open, b.close) + 0.01 + std::round(40.0 * vol * price * noise.uniform()) / 100.0;
    b.low = std::min(b.open, b.close) - 0.01 - std::round(40.0 * vol * price * noise.uniform()) / 100.0;
    b.volume = std::round(volume * std::exp(0.3 * noise.normal()));
    bars.push_back(b);
  }
  return bars;
}

std::string bars_csv(const std::string& id, const std::vector<Bar>& bars) {
  const auto days = sessions(static_cast<int>(bars.size()));
  std::string csv = "series,date,open,high,low,close,volume\n";
  for (std::size_t i = 0; i < bars.size(); ++i) {
    const Bar& b = bars[i];
    csv += id + "," + days[i].to_string() + "," + fixed(b.open, 2) + "," + fixed(b.high, 2) + "," +
           fixed(b.low, 2) + "," + fixed(b.close, 2) + "," + fixed(b.volume, 0) + "\n";
  }
  return csv;
}

std::string points_csv(const std::vector<double>& values) {
  const auto days = sessions(static_cast<int>(values.size()));
  std::string csv = "date,value\n";
  for (std::size_t i = 0; i < values.size(); ++i) {
    csv += days[i].to_string() + "," + fixed(values[i] + 0.0, 2) + "\n";
  }
  return csv;
}

// A short-rate: near `level`, moving 0.03 a day, quoted to 0.01.
std::vector<double> rate(int n, double level, std::uint64_t seed) {
  Noise noise(seed);
  std::vector<double> values;
  double x = level;
  for (int i = 0; i < n; ++i) {
    x += 0.01 * (level - x) + 0.03 * noise.normal();
    values.push_back(std::round(x * 100.0) / 100.0);
  }
  return values;
}

// Runs dorq on `csv` (standard input) with a dorq.toml holding `config`.
Result run_with_config(const std::string& config, const std::string& csv,
                       const char* select = "DQ") {
  const TempDir dir;
  write_file(dir.path() / "dorq.toml", config);
  return run_in(dir.path(), {"--select", select, "--format", "jsonl", "--show-info"}, csv);
}

const char* const kRates = R"(
[profiles.rates]
match = { kind = "point" }
price = { transform = "diff" }
integrity = { bounds = [-5, 25] }
)";

}  // namespace

// ---------------------------------------------------------------------------
// Configuration

TEST_CASE("M4 config keys: transform, bounds, stale_run") {
  const dorq::Config config = dorq::parse_config(R"(
[integrity]
bounds = [0, 100]

[price]
transform = "log"

[priors]
stale_run = 1e-05

[profiles.rates]
match = { kind = "point" }
price = { transform = "diff" }
integrity = { bounds = [-5, 25] }
)",
                                                 "dorq.toml", false);
  REQUIRE(config.integrity.bounds.has_value());
  CHECK(config.integrity.bounds->low == 0.0);
  CHECK(config.integrity.bounds->high == 100.0);
  CHECK(config.price.transform == dorq::Transform::kLog);
  CHECK(config.price.priors.stale_run == 1e-05);
  REQUIRE(config.profiles.size() == 1);
  dorq::PriceSettings price = config.price;
  config.profiles[0].price.apply_to(price);
  CHECK(price.transform == dorq::Transform::kDiff);
  dorq::IntegritySettings integrity = config.integrity;
  config.profiles[0].integrity.apply_to(integrity);
  CHECK(integrity.bounds == dorq::Bounds{-5.0, 25.0});

  // The effective config reads back the same.
  const dorq::Config again = dorq::parse_config(dorq::to_toml(config), "again.toml", false);
  CHECK(dorq::to_toml(again) == dorq::to_toml(config));
  CHECK(dorq::config_hash(again) == dorq::config_hash(config));
  CHECK(dorq::config_hash(again) != dorq::config_hash(dorq::Config{}));

  CHECK(dorq::parse_transform("auto") == dorq::Transform::kAuto);
  CHECK_FALSE(dorq::parse_transform("sqrt").has_value());
  CHECK_THROWS_AS(
      static_cast<void>(dorq::parse_config("[price]\ntransform = \"sqrt\"", "d.toml", false)),
      dorq::ConfigError);
  CHECK_THROWS_AS(
      static_cast<void>(dorq::parse_config("[integrity]\nbounds = [5, 1]", "d.toml", false)),
      dorq::ConfigError);
  CHECK_THROWS_AS(
      static_cast<void>(dorq::parse_config("[integrity]\nbounds = [1]", "d.toml", false)),
      dorq::ConfigError);
}

// ---------------------------------------------------------------------------
// Point series

TEST_CASE("DQ108: a value outside the bounds, and nothing without bounds") {
  std::vector<double> values = rate(300, 4.0, 1);
  values[150] = 470.0;  // basis points
  const Result result = run_with_config(kRates, points_csv(values));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ108")") == 1);
  CHECK(contains(result.out, "value 470 is outside the bounds [-5, 25]"));
  CHECK(count_matching(result.out, R"("code":"DQ201")") == 1);

  const Result unbounded = run({"--isolated", "--select", "DQ108", "--format", "jsonl"},
                               points_csv(values));
  CHECK(unbounded.out.empty());
}

TEST_CASE("rates on the difference scale: a slip is found, a cut to zero is not") {
  // A rate near 0.2 that crosses zero. A value ×100 is a bad print.
  std::vector<double> values = rate(400, 0.2, 2);
  values[200] = std::round(values[200] * 10000.0) / 100.0;
  if (std::fabs(values[200]) < 5.0) {
    values[200] = 25.0;
  }
  Result result = run_with_config(kRates, points_csv(values), "DQ2");
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ201")") == 1);

  // A quarter-point cut to zero, held: a policy move, not a change of units.
  std::vector<double> cut = rate(400, 0.25, 3);
  for (std::size_t i = 200; i < cut.size(); ++i) {
    cut[i] -= 0.25;
  }
  result = run_with_config(kRates, points_csv(cut), "DQ2");
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("severity":"warn")") == 0);
  CHECK(count_matching(result.out, R"("severity":"error")") == 0);

  // An era stored as a decimal (×0.01) is one DQ202.
  std::vector<double> era = rate(400, 3.0, 4);
  std::string csv = "date,value\n";
  const auto days = sessions(400);
  for (std::size_t i = 0; i < era.size(); ++i) {
    const bool decimal = i >= 150 && i < 250;
    csv += days[i].to_string() + "," + fixed(decimal ? era[i] / 100.0 : era[i], 4) + "\n";
  }
  result = run_with_config(kRates, csv, "DQ2");
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ202")") == 1);
}

// ---------------------------------------------------------------------------
// Volume

TEST_CASE("DQ401: an era of volume in other units is reported once") {
  std::vector<Bar> bars = stock(500, 40.0, 0.015, 2e6, 5);
  for (std::size_t i = 200; i < 320; ++i) {
    bars[i].volume *= 100.0;
  }
  const Result result =
      run({"--isolated", "--select", "DQ4", "--format", "jsonl", "--show-info"}, bars_csv("V", bars));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ401")") == 1);
  CHECK(contains(result.out, R"("nearest_ratio":"×100")"));
  CHECK(contains(result.out, R"("kind":"rescale_volume")"));
  // The era's bars are explained: no volume spikes inside it.
  CHECK(count_matching(result.out, R"("code":"DQ402")") == 0);

  const Result clean = run({"--isolated", "--select", "DQ4", "--format", "jsonl"},
                           bars_csv("V", stock(500, 40.0, 0.015, 2e6, 5)));
  CHECK(clean.out.empty());
}

TEST_CASE("DQ402: a spike on a quiet day is info") {
  std::vector<Bar> bars = stock(300, 40.0, 0.015, 2e6, 6);
  bars[200].close = bars[199].close;
  bars[200].volume = 2e6 * 37.0;
  const Result info =
      run({"--isolated", "--select", "DQ402", "--format", "jsonl", "--show-info"}, bars_csv("S", bars));
  CAPTURE(info.out);
  CHECK(count_matching(info.out, R"("code":"DQ402")") == 1);
  CHECK(contains(info.out, R"("classification":"market_fact")"));
  // Not shown by default.
  CHECK(run({"--isolated", "--select", "DQ402"}, bars_csv("S", bars)).out.empty());
}

TEST_CASE("DQ403: a move on zero volume, where the series has none") {
  std::vector<Bar> bars = stock(400, 40.0, 0.015, 2e6, 7);
  bars[250].volume = 0.0;
  bars[250].close = bars[249].close + 0.37;
  bars[250].high = std::max(bars[250].high, bars[250].close);
  const Result result = run({"--isolated", "--select", "DQ403", "--format", "jsonl"},
                            bars_csv("Z", bars));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ403")") == 1);
  CHECK(contains(result.out, R"("kind":"refetch_bar")"));

  // A feed that quotes every untraded day: a habit, not an error.
  Noise noise(8);
  for (std::size_t i = 10; i < bars.size(); i += 8) {
    bars[i].volume = 0.0;
  }
  CHECK(run({"--isolated", "--select", "DQ403"}, bars_csv("Z", bars)).out.empty());
}

// ---------------------------------------------------------------------------
// Stale values

TEST_CASE("DQ501: a liquid close repeated on volume, but a coarse price is not judged") {
  std::vector<Bar> bars = stock(400, 40.0, 0.02, 2e6, 9);
  for (std::size_t i = 201; i <= 204; ++i) {
    bars[i].close = bars[200].close;
    bars[i].open = bars[i].close;
    bars[i].high = std::max(bars[i].high, bars[i].close);
    bars[i].low = std::min(bars[i].low, bars[i].close);
  }
  const Result result = run({"--isolated", "--select", "DQ501", "--format", "jsonl"},
                            bars_csv("R", bars));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ501")") == 1);
  CHECK(contains(result.out, R"("kind":"delete_bars")"));
  CHECK(contains(result.out, "on 5 traded bars running"));

  // A ten-cent stock quoted in cents: runs are how it trades.
  std::vector<Bar> penny = stock(400, 0.10, 0.02, 2e6, 10);
  for (std::size_t i = 201; i <= 204; ++i) {
    penny[i].close = penny[200].close;
  }
  CHECK(run({"--isolated", "--select", "DQ501"}, bars_csv("P", penny)).out.empty());
}

TEST_CASE("DQ502: untraded bars carrying the close are info, and a market fact") {
  std::vector<Bar> bars = stock(100, 5.0, 0.03, 2000, 11);
  for (std::size_t i = 50; i <= 52; ++i) {
    bars[i] = bars[49];
    bars[i].open = bars[i].high = bars[i].low = bars[49].close;
    bars[i].volume = 0.0;
  }
  const Result result = run({"--isolated", "--select", "DQ502", "--format", "jsonl", "--show-info"},
                            bars_csv("C", bars));
  CAPTURE(result.out);
  CHECK(count_matching(result.out, R"("code":"DQ502")") == 1);
  CHECK(contains(result.out, "3 bars with no trade carry the close"));
  CHECK(contains(result.out, R"("classification":"market_fact")"));
}
