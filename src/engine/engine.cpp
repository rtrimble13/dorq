#include "engine/engine.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dorq {

Engine::Engine(const Config& config, EngineOptions options, ResultSink& sink)
    : config_(config), options_(options), sink_(sink) {
  if (options_.threads > 1) {
    workers_.reserve(options_.threads);
    for (unsigned i = 0; i < options_.threads; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }
}

Engine::~Engine() {
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  work_ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

const Engine::Settings& Engine::settings_for(const Series& series) {
  std::string key = std::string{to_string(series.kind)};
  for (const Profile& profile : config_.profiles) {
    if (profile.matches(series)) {
      key += "|" + profile.name;
    }
  }
  auto& slot = settings_[key];
  if (!slot) {
    slot = std::make_unique<Settings>();
    slot->integrity = config_.integrity;
    std::vector<std::string> select = config_.select;
    select.insert(select.end(), config_.extend_select.begin(), config_.extend_select.end());
    std::vector<std::string> ignore = config_.ignore;
    for (const Profile& profile : config_.profiles) {
      if (profile.matches(series)) {
        select.insert(select.end(), profile.select.begin(), profile.select.end());
        ignore.insert(ignore.end(), profile.ignore.begin(), profile.ignore.end());
        profile.integrity.apply_to(slot->integrity);
      }
    }
    const Selection selection(select, ignore);
    for (const Check* check : all_checks()) {
      const CheckInfo& info = check->info();
      if (info.applies == Applies::kOhlcvOnly && series.kind != SeriesKind::kOhlcv) {
        continue;
      }
      if (selection.enabled(info)) {
        slot->checks.push_back(check);
      }
    }
  }
  return *slot;
}

SeriesResult Engine::process(Work& work) const {
  const Series& series = work.series;
  SeriesResult result;
  result.sequence = work.sequence;
  result.id = series.id;
  result.label = series.label;
  result.source = series.source;
  result.kind = series.kind;
  result.rows = series.size() + static_cast<std::size_t>(std::count_if(
                                    series.issues.begin(), series.issues.end(),
                                    [](const FieldIssue& issue) { return !issue.has_date; }));

  const SeriesContext context{series, work.settings->integrity};
  std::vector<Violation> found;
  for (const Check* check : work.settings->checks) {
    check->run(context, found);
  }
  for (Violation& v : found) {
    if (v.severity < options_.min_severity) {
      continue;
    }
    if (options_.since && v.date && *v.date < *options_.since) {
      continue;
    }
    result.violations.push_back(std::move(v));
  }
  // By date (rows without one first, by line), then code. Stable, so a check's
  // own order breaks any remaining tie.
  std::stable_sort(result.violations.begin(), result.violations.end(),
                   [](const Violation& a, const Violation& b) {
                     if (a.date.has_value() != b.date.has_value()) {
                       return !a.date.has_value();
                     }
                     if (a.date && *a.date != *b.date) {
                       return *a.date < *b.date;
                     }
                     if (!a.date && a.line != b.line) {
                       return a.line < b.line;
                     }
                     return a.check->code < b.check->code;
                   });
  return result;
}

void Engine::submit(Series&& series) {
  const Settings& settings = settings_for(series);
  Work work{next_sequence_++, std::move(series), &settings};
  if (workers_.empty()) {
    sink_.series_done(process(work));
    return;
  }
  {
    std::unique_lock lock(mutex_);
    // Bound what is held in memory: two series per worker, queued, being checked
    // or checked but waiting for an earlier one. Only this thread delivers, so it
    // delivers while it waits.
    while (in_flight_ >= 2 * workers_.size()) {
      result_ready_.wait(lock, [this] { return done_.contains(next_to_deliver_); });
      lock.unlock();
      deliver_ready();
      lock.lock();
    }
    ++in_flight_;
    queue_.push_back(std::move(work));
  }
  work_ready_.notify_one();
  deliver_ready();
}

void Engine::worker_loop() {
  while (true) {
    std::optional<Work> work;
    {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;  // stopping, nothing left
      }
      work.emplace(std::move(queue_.front()));
      queue_.pop_front();
    }
    SeriesResult result = process(*work);
    work.reset();  // free the series before taking the next
    {
      const std::scoped_lock lock(mutex_);
      done_.emplace(result.sequence, std::move(result));
    }
    result_ready_.notify_all();
  }
}

void Engine::deliver_ready() {
  while (true) {
    SeriesResult result;
    {
      const std::scoped_lock lock(mutex_);
      const auto it = done_.find(next_to_deliver_);
      if (it == done_.end()) {
        return;
      }
      result = std::move(it->second);
      done_.erase(it);
      ++next_to_deliver_;
      --in_flight_;
    }
    sink_.series_done(std::move(result));
  }
}

void Engine::finish() {
  if (workers_.empty()) {
    return;
  }
  while (true) {
    {
      std::unique_lock lock(mutex_);
      result_ready_.wait(lock, [this] {
        return next_to_deliver_ == next_sequence_ || done_.contains(next_to_deliver_);
      });
      if (next_to_deliver_ == next_sequence_) {
        return;
      }
    }
    deliver_ready();
  }
}

}  // namespace dorq
