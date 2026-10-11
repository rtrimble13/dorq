#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "checks/check.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "engine/cross_section.hpp"

namespace dorq {

// What checking one series produced, minus the series' data.
struct SeriesResult {
  std::size_t sequence = 0;  // the series' position in the input
  std::string id;
  std::string label;
  std::string source;
  SeriesKind kind = SeriesKind::kOhlcv;
  std::size_t rows = 0;
  std::vector<Violation> violations;  // sorted; filtered by the time a sink sees them
  // The DQ303 violations, which belong to no one series, arrive last as a result
  // of their own with this set.
  bool cross_sectional = false;
  // A DQ109 report of a context file's row: `id` is the series the row names and
  // `source` the file. Not a series of the input, so not counted as one.
  bool from_context = false;
};

// Receives results in input order, on the thread that calls Engine::submit() and
// Engine::finish().
class ResultSink {
 public:
  ResultSink() = default;
  ResultSink(const ResultSink&) = delete;
  ResultSink& operator=(const ResultSink&) = delete;
  ResultSink(ResultSink&&) = delete;
  ResultSink& operator=(ResultSink&&) = delete;
  virtual ~ResultSink() = default;
  virtual void series_done(SeriesResult&& result) = 0;
};

struct EngineOptions {
  unsigned threads = 1;
  Severity min_severity = Severity::kWarn;
  std::optional<Date> since;           // drop violations dated before this
  std::optional<Date> as_of;           // DQ304's reference date; default: the latest bar
  const Calendar* calendar = nullptr;  // default: built-in XNYS
  const Context* context = nullptr;    // --actions, --meta, --market; none by default
};

// Runs the enabled checks over each series, on a pool of worker threads, and hands
// results to the sink in input order. The output therefore never depends on the
// number of threads or on scheduling (doc/adr/0001).
//
// The cross-sectional checks (DQ303, DQ304) can only be judged once every series is
// in. While either is enabled, results are held until finish() and handed on then,
// still in input order, followed by the DQ109 reports of the context files' rows
// and then the DQ303 result.
class Engine {
 public:
  Engine(const Config& config, EngineOptions options, ResultSink& sink);
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  ~Engine();

  // Takes the next series. Blocks while too many are in flight.
  void submit(Series&& series);
  // Waits for every submitted series and delivers the remaining results.
  void finish();

  // The as-of date DQ304 used, once finish() has run.
  [[nodiscard]] std::optional<Date> as_of() const;

 private:
  // The resolved settings for one combination of kind and matching profiles.
  struct Settings {
    IntegritySettings integrity;
    CoverageSettings coverage;
    PriceSettings price;
    std::vector<const Check*> checks;  // per-series checks to run
    bool coverage_model = false;       // a DQ30x check needs the coverage analysis
    bool price_model = false;          // a DQ2xx price check needs the price analysis
    bool cohort = false;               // DQ303 enabled
    bool stale = false;                // DQ304 enabled
    bool cohort_move = false;          // DQ601 enabled
  };
  struct Work {
    std::size_t sequence = 0;
    Series series;
    const Settings* settings = nullptr;
  };
  struct Processed {
    SeriesResult result;
    CrossSummary summary;
  };

  const Settings& settings_for(const Series& series);
  [[nodiscard]] Processed process(Work& work) const;
  void deliver(Processed processed);
  void drop_unreported(SeriesResult& result) const;  // below --min-severity, before --since
  void filter_and_send(SeriesResult&& result);
  void worker_loop();
  void deliver_ready();  // caller holds no lock

  const Config& config_;
  EngineOptions options_;
  ResultSink& sink_;
  const Calendar& calendar_;
  const Context& context_;
  std::unordered_map<std::string, std::unique_ptr<Settings>> settings_;
  std::size_t next_sequence_ = 0;

  // Cross-sectional checks.
  bool holding_ = false;
  bool market_days_ = false;     // DQ602 enabled, and a market given
  bool context_issues_ = false;  // DQ109 enabled, and a context file has a row skipped
  std::unique_ptr<CrossSection> cross_;
  std::vector<SeriesResult> held_;

  // Threaded mode only.
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable result_ready_;
  std::deque<Work> queue_;
  std::map<std::size_t, Processed> done_;
  std::size_t next_to_deliver_ = 0;
  std::size_t in_flight_ = 0;
  bool stopping_ = false;
};

}  // namespace dorq
