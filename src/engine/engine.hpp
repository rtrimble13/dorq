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
#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"

namespace dorq {

// What checking one series produced, minus the series' data.
struct SeriesResult {
  std::size_t sequence = 0;  // the series' position in the input
  std::string id;
  std::string label;
  std::string source;
  SeriesKind kind = SeriesKind::kOhlcv;
  std::size_t rows = 0;
  std::vector<Violation> violations;  // sorted; already filtered
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
  std::optional<Date> since;  // drop violations dated before this
};

// Runs the enabled checks over each series, on a pool of worker threads, and hands
// results to the sink in input order. The output therefore never depends on the
// number of threads or on scheduling (doc/adr/0001).
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

 private:
  // The resolved settings for one combination of matching profiles.
  struct Settings {
    IntegritySettings integrity;
    std::vector<const Check*> checks;
  };
  struct Work {
    std::size_t sequence = 0;
    Series series;
    const Settings* settings = nullptr;
  };

  const Settings& settings_for(const Series& series);
  [[nodiscard]] SeriesResult process(Work& work) const;
  void worker_loop();
  void deliver_ready();  // caller holds no lock

  const Config& config_;
  EngineOptions options_;
  ResultSink& sink_;
  std::unordered_map<std::string, std::unique_ptr<Settings>> settings_;
  std::size_t next_sequence_ = 0;

  // Threaded mode only.
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable result_ready_;
  std::deque<Work> queue_;
  std::map<std::size_t, SeriesResult> done_;
  std::size_t next_to_deliver_ = 0;
  std::size_t in_flight_ = 0;
  bool stopping_ = false;
};

}  // namespace dorq
