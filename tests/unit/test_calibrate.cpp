// M6: labels, --restore, [calibration], config include, dorq calibrate.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "calibrate/calibrate.hpp"
#include "calibrate/labels.hpp"
#include "config/config.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "io/input_error.hpp"
#include "price/model.hpp"
#include "test_support.hpp"

using dorq::test::contains;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

std::vector<dorq::Label> labels_of(const std::string& text) {
  std::istringstream in(text);
  return dorq::read_labels(in, "labels.jsonl");
}

std::string label_error(const std::string& text) {
  try {
    (void)labels_of(text);
  } catch (const dorq::InputError& e) {
    return e.what();
  }
  return {};
}

dorq::Date d(const char* text) { return dorq::parse_date(text).value(); }

dorq::Series series_of(const std::string& id,
                       const std::vector<std::pair<const char*, double>>& rows) {
  dorq::Series s;
  s.id = id;
  s.kind = dorq::SeriesKind::kPoint;
  for (const auto& [date, value] : rows) {
    s.date.push_back(d(date));
    s.close.push_back(value);
    s.close_decimals.push_back(2);
    s.close_sig_figs.push_back(4);
    s.line.push_back(static_cast<std::uint32_t>(s.line.size() + 2));
  }
  return s;
}

std::string read_text(const std::filesystem::path& path) {
  const std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

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

std::string fixed(double value, int decimals) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << value;
  return out.str();
}

// Eight liquid stocks over 400 sessions; in each, one close is a bad print
// (times 1.4, back the next day), labelled. Market facts are labelled on two
// real 9% moves.
struct Universe {
  std::string bars;
  std::string labels;
};

Universe universe() {
  static const dorq::Calendar kCalendar;
  std::vector<dorq::Date> days;
  for (dorq::Date day = dorq::Date::from_ymd(2020, 1, 2); days.size() < 400;
       day = dorq::Date::from_days(day.days() + 1)) {
    if (kCalendar.is_session(day)) {
      days.push_back(day);
    }
  }
  Universe u;
  u.bars = "series,date,open,high,low,close,volume\n";
  for (int k = 0; k < 8; ++k) {
    Noise noise(static_cast<std::uint64_t>(k) + 11U);
    const std::string id = "S" + std::to_string(k);
    const std::size_t bad = 150 + static_cast<std::size_t>(k) * 25;
    const std::size_t jump = 120 + static_cast<std::size_t>(k) * 7;
    double price = 50.0 + k;
    for (std::size_t t = 0; t < days.size(); ++t) {
      const double prev = price;
      price *= std::exp(0.015 * noise.normal());
      if (t == jump && k < 2) {
        price *= 1.09;  // news: a real move
      }
      double close = price;
      if (t == bad) {
        close *= 1.4;
      }
      const double open = prev * std::exp(0.004 * noise.normal());
      const double high = std::max(open, close) * (1.0 + 0.004 * noise.uniform());
      const double low = std::min(open, close) * (1.0 - 0.004 * noise.uniform());
      u.bars += id + "," + days[t].to_string() + "," + fixed(open, 2) + "," + fixed(high, 2) + "," +
                fixed(low, 2) + "," + fixed(close, 2) + "," +
                fixed(std::round(2e6 * std::exp(0.3 * noise.normal())), 0) + "\n";
    }
    u.labels += R"({"series":")" + id + R"(","date":")" + days[bad].to_string() +
                R"(","class":"data_error","expect":"DQ201","kind":"bad_print"})" + "\n";
    if (k < 2) {
      u.labels += R"({"series":")" + id + R"(","date":")" + days[jump].to_string() +
                  R"(","class":"market_fact","kind":"news"})" + "\n";
    }
  }
  return u;
}

}  // namespace

// ---------------------------------------------------------------------------
// Labels

TEST_CASE("labels: the JSONL schema, its defaults and its errors") {
  const auto labels = labels_of(
      R"({"series":"AKR","first":"2020-03-02","last":"2020-03-04","class":"data_error",)"
      R"("expect":"DQ202","kind":"scale_era","source":"operator_override","note":"x","extra":1})"
      "\n\n"
      R"({"series":"AKR","date":"2020-05-01","class":"market_fact"})"
      "\n"
      R"({"series":"B","date":"2021-01-04","class":"context_gap","remove":"2021-01-05, 2021-01-06"})"
      "\n");
  REQUIRE(labels.size() == 3);
  CHECK(labels[0].first == d("2020-03-02"));
  CHECK(labels[0].last == d("2020-03-04"));
  CHECK(labels[0].label_class == dorq::LabelClass::kDataError);
  CHECK(labels[0].expect == "DQ202");
  CHECK(labels[0].kind == "scale_era");
  CHECK(labels[0].source == "operator_override");
  CHECK(labels[0].line == 1);
  CHECK(labels[1].last == labels[1].first);
  CHECK(labels[1].codes == "DQ2");  // a market fact's default
  CHECK(labels[1].line == 3);
  CHECK(labels[2].label_class == dorq::LabelClass::kContextGap);
  CHECK(labels[2].remove == std::vector<dorq::Date>{d("2021-01-05"), d("2021-01-06")});

  CHECK(label_error(R"({"series":"A","date":"2020-01-02","class":"oops"})") ==
        "labels.jsonl line 1: class \"oops\" is not data_error, context_gap or market_fact");
  CHECK(label_error("\n"
                    R"({"date":"2020-01-02","class":"data_error"})") ==
        "labels.jsonl line 2: a label needs series, first (or date) and class");
  CHECK(contains(label_error(R"({"series":"A","date":"2020-13-02","class":"data_error"})"),
                 "is not a date"));
  CHECK(label_error(
            R"({"series":"A","first":"2020-01-03","last":"2020-01-02","class":"data_error"})") ==
        "labels.jsonl line 1: last is before first");
  CHECK(contains(label_error("{\"series\":"), "labels.jsonl line 1"));
}

TEST_CASE("--restore: rows as they stood before repair, by date, and removed dates") {
  const auto labels =
      labels_of(R"({"series":"A","date":"2020-01-03","class":"data_error","remove":"2020-01-07"})");
  const dorq::Restorer restorer(
      labels, {series_of("A", {{"2020-01-03", 99.0}, {"2020-01-02", 7.0}, {"2019-12-31", 5.0}})});
  CHECK_FALSE(restorer.empty());
  dorq::Series a = series_of(
      "A",
      {{"2020-01-02", 10.0}, {"2020-01-03", 11.0}, {"2020-01-06", 12.0}, {"2020-01-07", 13.0}});
  restorer.apply(a);
  REQUIRE(a.size() == 4);
  CHECK(a.date[0] == d("2019-12-31"));  // added
  CHECK(a.close[0] == 5.0);
  CHECK(a.close[1] == 7.0);   // replaced
  CHECK(a.close[2] == 99.0);  // replaced
  CHECK(a.date[3] == d("2020-01-06"));
  CHECK(a.close[3] == 12.0);  // 2020-01-07 removed

  dorq::Series b = series_of("B", {{"2020-01-02", 1.0}});
  restorer.apply(b);
  CHECK(b.size() == 1);
  CHECK(dorq::Restorer{}.empty());
}

// ---------------------------------------------------------------------------
// The p_error map

TEST_CASE("calibrated: linear between knots, flat beyond them, identity without a map") {
  CHECK(dorq::calibrated({}, 0.37) == 0.37);
  const std::vector<std::pair<double, double>> map = {{0.1, 0.05}, {0.5, 0.25}, {0.9, 0.95}};
  CHECK(dorq::calibrated(map, 0.0) == doctest::Approx(0.05));
  CHECK(dorq::calibrated(map, 0.3) == doctest::Approx(0.15));
  CHECK(dorq::calibrated(map, 0.7) == doctest::Approx(0.6));
  CHECK(dorq::calibrated(map, 1.0) == doctest::Approx(0.95));
}

TEST_CASE("isotonic_map: rising knots at six decimals, pooling violators") {
  std::vector<std::pair<double, double>> points;
  for (int i = 0; i < 200; ++i) {
    const double p = (i + 0.5) / 200.0;
    // Observed: zero below 0.5 except a bump, one above.
    const bool bump = i % 10 == 0 && p > 0.2 && p < 0.3;
    const double y = p >= 0.5 || bump ? 1.0 : 0.0;
    points.emplace_back(p, y);
  }
  const auto map = dorq::isotonic_map(points, 20);
  REQUIRE(map.size() >= 2);
  CHECK(map.size() <= 20);
  for (std::size_t i = 1; i < map.size(); ++i) {
    CHECK(map[i].first > map[i - 1].first);
    CHECK(map[i].second >= map[i - 1].second);
  }
  for (const auto& [x, y] : map) {
    CHECK(std::round(x * 1e6) / 1e6 == x);
    CHECK(std::round(y * 1e6) / 1e6 == y);
  }
  CHECK(map.front().second == doctest::Approx(0.0).epsilon(0.1));
  CHECK(map.back().second == 1.0);
  CHECK(dorq::isotonic_map({}, 20).empty());
}

TEST_CASE("expected calibration error") {
  CHECK(dorq::expected_calibration_error({}) == 0.0);
  // Perfect: p 0.25 with a quarter of them faults.
  CHECK(dorq::expected_calibration_error({{0.25, 1}, {0.25, 0}, {0.25, 0}, {0.25, 0}}) ==
        doctest::Approx(0.0));
  // Half the points at p 0.9 but never faults, half at 0.1 and always.
  CHECK(dorq::expected_calibration_error({{0.9, 0}, {0.1, 1}}) == doctest::Approx(0.9));
}

// ---------------------------------------------------------------------------
// Configuration: [calibration] and include

TEST_CASE("[calibration]: a version and a monotone p_error map, checked") {
  const dorq::Config config = dorq::parse_config(R"(
[calibration]
version = "2026-10 fafnir"
p_error_map = [[0.1, 0.02], [0.5, 0.3], [0.9, 0.9]]
)",
                                                 "dorq.toml", false);
  CHECK(config.calibration_version == "2026-10 fafnir");
  REQUIRE(config.price.p_error_map.size() == 3);
  CHECK(config.price.p_error_map[1] == std::pair{0.5, 0.3});
  const std::string toml = dorq::to_toml(config);
  CHECK(contains(toml, "[calibration]"));
  const dorq::Config again = dorq::parse_config(toml, "again.toml", false);
  CHECK(again.price.p_error_map == config.price.p_error_map);
  CHECK(again.calibration_version == config.calibration_version);

  const auto error = [](const std::string& map) {
    try {
      (void)dorq::parse_config("[calibration]\np_error_map = " + map + "\n", "dorq.toml", false);
    } catch (const dorq::ConfigError& e) {
      return std::string{e.what()};
    }
    return std::string{};
  };
  CHECK_FALSE(error("[[0.5, 0.1], [0.4, 0.2]]").empty());  // raw not rising
  CHECK_FALSE(error("[[0.1, 0.5], [0.4, 0.2]]").empty());  // calibrated falling
  CHECK_FALSE(error("[[0.1, 1.5]]").empty());              // outside [0, 1]
  CHECK(error("[[0.1, 0.1], [0.4, 0.4]]").empty());
}

TEST_CASE("include: files read first, relative to the including file, then overridden") {
  const TempDir dir;
  std::filesystem::create_directories(dir.path() / "fits");
  write_file(dir.path() / "fits" / "priors.toml", R"(
threads = 3
[price]
jump_prob = 0.05
[calibration]
p_error_map = [[0.2, 0.1], [0.8, 0.7]]
)");
  write_file(dir.path() / "base.toml", "min_severity = \"error\"\nthreads = 2\n");
  write_file(dir.path() / "dorq.toml",
             "include = [\"base.toml\", \"fits/priors.toml\"]\nthreads = 4\n");
  const dorq::Config config = dorq::load_config(dir.path() / "dorq.toml");
  CHECK(config.threads == 4);  // the including file wins
  CHECK(config.min_severity == dorq::Severity::kError);
  CHECK(config.price.jump_prob == 0.05);
  CHECK(config.price.p_error_map.size() == 2);

  write_file(dir.path() / "loop.toml", "include = \"loop.toml\"\n");
  CHECK_THROWS_AS((void)dorq::load_config(dir.path() / "loop.toml"), dorq::ConfigError);
  write_file(dir.path() / "missing.toml", "include = \"nope.toml\"\n");
  CHECK_THROWS_AS((void)dorq::load_config(dir.path() / "missing.toml"), dorq::ConfigError);
}

// ---------------------------------------------------------------------------
// dorq calibrate, end to end

TEST_CASE("dorq calibrate: fits the labels, writes priors.toml that dorq check reads") {
  const TempDir dir;
  const Universe u = universe();
  const auto path = [&dir](const char* name) { return (dir.path() / name).string(); };
  const std::string bars = path("bars.csv");
  const std::string labels = path("labels.jsonl");
  const std::string priors_path = path("priors.toml");
  const std::string again_path = path("again.toml");
  const std::string config_path = path("dorq.toml");
  write_file(bars, u.bars);
  write_file(labels, u.labels);
  const Result fit = run_in(dir.path(), {"calibrate", bars.c_str(), "--labels", labels.c_str(),
                                         "--out", priors_path.c_str(), "--isolated", "--threads",
                                         "2", "--clean-unlabelled", "--name", "test fit"});
  INFO(fit.err);
  REQUIRE(fit.status == 0);
  CHECK(contains(fit.out, "observations:"));
  CHECK(contains(fit.out, "expected calibration error:"));
  const std::string priors = read_text(priors_path);
  CHECK(contains(priors, "[priors]"));
  CHECK(contains(priors, "[calibration]"));
  CHECK(contains(priors, "version = \"test fit\""));

  // The same fit with one thread writes the same file.
  const Result again = run_in(dir.path(), {"calibrate", bars.c_str(), "--labels", labels.c_str(),
                                           "--out", again_path.c_str(), "--isolated", "--threads",
                                           "1", "--clean-unlabelled", "--name", "test fit"});
  REQUIRE(again.status == 0);
  CHECK(read_text(again_path) == priors);

  write_file(config_path, "include = \"priors.toml\"\n");
  const Result check =
      run_in(dir.path(), {bars.c_str(), "--config", config_path.c_str(), "--exit-zero"});
  INFO(check.err);
  CHECK(check.status == 0);
  CHECK(contains(check.out, "DQ201"));

  const Result no_labels = run_in(dir.path(), {"calibrate", bars.c_str(), "--isolated"});
  CHECK(no_labels.status == 2);
  const Result bad_labels =
      run_in(dir.path(), {"calibrate", bars.c_str(), "--labels", bars.c_str(), "--isolated"});
  CHECK(bad_labels.status != 0);
  CHECK(contains(bad_labels.err, "bars.csv line 1"));
}

TEST_CASE("dorq check --restore: checks the series as it stood before repair") {
  const TempDir dir;
  const std::string rates = (dir.path() / "rates.csv").string();
  const std::string before = (dir.path() / "before.csv").string();
  const std::string labels = (dir.path() / "labels.jsonl").string();
  // A quiet rate, repaired on one day that had been stored ten times too large.
  static const dorq::Calendar kCalendar;
  std::string csv = "date,value\n";
  std::string day;
  int n = 0;
  for (dorq::Date at = dorq::Date::from_ymd(2020, 1, 2); n < 80;
       at = dorq::Date::from_days(at.days() + 1)) {
    if (kCalendar.is_session(at)) {
      csv += at.to_string() + "," + fixed(1.5 + 0.01 * (n % 3), 2) + "\n";
      if (++n == 60) {
        day = at.to_string();
      }
    }
  }
  write_file(rates, csv);
  write_file(before, "series,date,value\nrates," + day + ",15.10\n");
  write_file(labels, R"({"series":"rates","date":")" + day + R"(","class":"data_error"})");
  const Result plain =
      run_in(dir.path(), {rates.c_str(), "--isolated", "--format", "csv", "--exit-zero"});
  const Result restored =
      run_in(dir.path(), {rates.c_str(), "--isolated", "--format", "csv", "--exit-zero", "--labels",
                          labels.c_str(), "--restore", before.c_str()});
  INFO(restored.err);
  CHECK_FALSE(contains(plain.out, day));
  CHECK(contains(restored.out, day));
}
