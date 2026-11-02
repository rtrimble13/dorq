// The engine's delivery: what streams, and what is held for the cross-sectional
// checks (DQ303, DQ304, DQ601, DQ951) until every series is in.

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "engine/engine.hpp"
#include "io/reader.hpp"

namespace {

// Records the series it is handed, in order.
class Recorder final : public dorq::ResultSink {
 public:
  void series_done(dorq::SeriesResult&& result) override {
    const dorq::SeriesResult taken = std::move(result);
    ids.push_back(taken.id);
  }
  std::vector<std::string> ids;
};

std::vector<dorq::Series> read(const std::string& text, const std::string& extension = ".csv") {
  std::vector<dorq::Series> out;
  dorq::SeriesAssembler assembler(dorq::Grouping::kBuffer,
                                  [&out](dorq::Series&& s) { out.push_back(std::move(s)); });
  std::istringstream in(text);
  dorq::read_input(in, "input" + extension, "input", extension, dorq::ReadOptions{}, assembler);
  assembler.finish();
  return out;
}

dorq::EngineOptions one_thread() {
  dorq::EngineOptions options;
  options.threads = 1;
  return options;
}

constexpr const char* kStatements =
    "security_id,statement,period_type,fiscal_date,version_no,known_from,revenue\n"
    "A,income,quarter,2020-03-31,1,2020-05-10,100\n"
    "A,income,quarter,2020-06-30,1,2020-08-10,110\n"
    "B,income,quarter,2020-03-31,1,2020-05-12,200\n"
    "B,income,quarter,2020-06-30,1,2020-08-12,210\n";

constexpr const char* kBars =
    "series,date,open,high,low,close,volume\n"
    "X,2020-01-02,10,11,9,10,1000\n"
    "X,2020-01-03,10,11,9,10.5,1000\n";

}  // namespace

TEST_CASE("the engine streams a kind no enabled cross-sectional check applies to") {
  // DQ303, DQ304 and DQ601 are enabled, but judge only bars: without DQ951 a
  // statements series is handed on as soon as it is checked.
  dorq::Config config;
  config.ignore = {"DQ951"};
  Recorder sink;
  {
    dorq::Engine engine(config, one_thread(), sink);
    std::vector<dorq::Series> series = read(kStatements);
    REQUIRE(series.size() == 2);
    engine.submit(std::move(series[0]));
    CHECK(sink.ids == std::vector<std::string>{"A"});
    engine.submit(std::move(series[1]));
    CHECK(sink.ids == std::vector<std::string>{"A", "B"});
    engine.finish();
  }
  CHECK(sink.ids == std::vector<std::string>{"A", "B"});
}

TEST_CASE("the engine holds from the first series a cross-sectional check applies to") {
  SUBCASE("statements with DQ951") {
    const dorq::Config config;
    Recorder sink;
    dorq::Engine engine(config, one_thread(), sink);
    for (dorq::Series& s : read(kStatements)) {
      engine.submit(std::move(s));
    }
    CHECK(sink.ids.empty());  // held for DQ951
    engine.finish();
    CHECK(sink.ids == std::vector<std::string>{"A", "B"});
  }
  SUBCASE("statements, then bars: the statements stream, the bars are held, in input order") {
    dorq::Config config;
    config.ignore = {"DQ951"};
    Recorder sink;
    dorq::Engine engine(config, one_thread(), sink);
    for (dorq::Series& s : read(kStatements)) {
      engine.submit(std::move(s));
    }
    CHECK(sink.ids == std::vector<std::string>{"A", "B"});
    for (dorq::Series& s : read(kBars)) {
      engine.submit(std::move(s));
    }
    CHECK(sink.ids == std::vector<std::string>{"A", "B"});  // X held for DQ303, DQ304, DQ601
    engine.finish();
    CHECK(sink.ids == std::vector<std::string>{"A", "B", "X"});
  }
  SUBCASE("bars, then statements: once one series is held, every one after it is") {
    dorq::Config config;
    config.ignore = {"DQ951"};
    Recorder sink;
    dorq::Engine engine(config, one_thread(), sink);
    for (dorq::Series& s : read(kBars)) {
      engine.submit(std::move(s));
    }
    for (dorq::Series& s : read(kStatements)) {
      engine.submit(std::move(s));
    }
    CHECK(sink.ids.empty());
    engine.finish();
    CHECK(sink.ids == std::vector<std::string>{"X", "A", "B"});
  }
}
