#include "engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "checks/coverage_model.hpp"
#include "checks/cross.hpp"
#include "checks/price.hpp"
#include "dorq/frequency.hpp"
#include "price/model.hpp"

namespace dorq {
namespace {

const Calendar& default_calendar() {
  static const Calendar kXnys(CalendarKind::kXnys);
  return kXnys;
}

const Context& no_context() {
  static const Context kNone;
  return kNone;
}

bool enables(const Selection& selection, std::string_view code) {
  const Check* check = find_check(code);
  return check != nullptr && selection.enabled(check->info());
}

std::vector<std::string> concat(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
  std::vector<std::string> out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

}  // namespace

Engine::Engine(const Config& config, EngineOptions options, ResultSink& sink)
    : config_(config),
      options_(options),
      sink_(sink),
      calendar_(options.calendar != nullptr ? *options.calendar : default_calendar()),
      context_(options.context != nullptr ? *options.context : no_context()) {
  // Hold results when a cross-sectional check can run for any series: under the
  // global selection, or under any profile's.
  const std::vector<std::string> select = concat(config.select, config.extend_select);
  std::vector<Selection> selections = {Selection(select, config.ignore)};
  for (const Profile& profile : config.profiles) {
    selections.emplace_back(concat(select, profile.select), concat(config.ignore, profile.ignore));
  }
  for (const Selection& selection : selections) {
    holding_ = holding_ || enables(selection, "DQ303") || enables(selection, "DQ304") ||
               enables(selection, "DQ601");
  }
  market_days_ = context_.market.has_value() && enables(selections.front(), "DQ602");
  if (holding_) {
    cross_ = std::make_unique<CrossSection>(calendar_, config.cohort, config.severity,
                                            config.coverage, options.as_of);
  }
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
  const SeriesMeta* meta = context_.meta_for(series);
  std::string key = std::string{to_string(series.kind)};
  for (const Profile& profile : config_.profiles) {
    if (profile.matches(series, meta)) {
      key += "|" + profile.name;
    }
  }
  auto& slot = settings_[key];
  if (!slot) {
    slot = std::make_unique<Settings>();
    slot->integrity = config_.integrity;
    slot->coverage = config_.coverage;
    slot->price = config_.price;
    std::vector<std::string> select = concat(config_.select, config_.extend_select);
    std::vector<std::string> ignore = config_.ignore;
    for (const Profile& profile : config_.profiles) {
      if (profile.matches(series, meta)) {
        select.insert(select.end(), profile.select.begin(), profile.select.end());
        ignore.insert(ignore.end(), profile.ignore.begin(), profile.ignore.end());
        profile.integrity.apply_to(slot->integrity);
        profile.coverage.apply_to(slot->coverage);
        profile.price.apply_to(slot->price);
      }
    }
    const Selection selection(select, ignore);
    for (const Check* check : all_checks()) {
      const CheckInfo& info = check->info();
      if (!selection.enabled(info)) {
        continue;
      }
      if (info.code == "DQ303") {
        slot->cohort = true;
      } else if (info.code == "DQ304") {
        slot->stale = true;
      } else if (info.code == "DQ601") {
        slot->cohort_move = true;
      }
      if (info.code == "DQ301" || info.code == "DQ302" || info.code == "DQ303" ||
          info.code == "DQ304") {
        slot->coverage_model = true;
      }
      // The price checks, and the volume and stale checks that read its features.
      if ((info.code.starts_with("DQ2") && info.code != "DQ206") || info.code == "DQ401" ||
          info.code == "DQ402" || info.code == "DQ501" || info.code == "DQ601" ||
          (info.code.starts_with("DQ70") && info.code != "DQ705")) {
        slot->price_model = true;
      }
      if (info.cross_sectional ||
          (info.applies == Applies::kOhlcvOnly && series.kind != SeriesKind::kOhlcv)) {
        continue;
      }
      slot->checks.push_back(check);
    }
  }
  return *slot;
}

Engine::Processed Engine::process(Work& work) const {
  const Series& series = work.series;
  const Settings& settings = *work.settings;
  Processed out;
  SeriesResult& result = out.result;
  result.sequence = work.sequence;
  result.id = series.id;
  result.label = series.label;
  result.source = series.source;
  result.kind = series.kind;
  result.rows = series.size() + static_cast<std::size_t>(std::count_if(
                                    series.issues.begin(), series.issues.end(),
                                    [](const FieldIssue& issue) { return !issue.has_date; }));

  const Frequency frequency = settings.coverage.frequency == Frequency::kAuto
                                  ? infer_frequency(series.date)
                                  : settings.coverage.frequency;
  std::optional<CoverageAnalysis> analysis;
  if (frequency == Frequency::kDaily && settings.coverage_model) {
    analysis =
        analyze_coverage(series, calendar_, settings.coverage, config_.cohort.confident_density);
  }
  const SeriesActions* actions = context_.actions_for(series);
  const SeriesMeta* meta = context_.meta_for(series);
  std::optional<PriceAnalysis> price;
  if (settings.price_model) {
    price = analyze_prices(series, calendar_, settings.price,
                           {.bounds = settings.integrity.bounds,
                            .have_actions = context_.have_actions,
                            .actions = actions,
                            .tick_size = meta != nullptr ? meta->tick_size : std::nullopt,
                            .market = context_.market ? &*context_.market : nullptr});
  }
  const SeriesContext context{
      .series = series,
      .integrity = settings.integrity,
      .coverage = settings.coverage,
      .calendar = calendar_,
      .thresholds = config_.severity,
      .price = settings.price,
      .frequency = frequency,
      .analysis = analysis ? &*analysis : nullptr,
      .gap_report = config_.gap_report,
      .price_analysis = price ? &*price : nullptr,
      .have_actions = context_.have_actions,
      .actions = actions,
      .meta = meta,
  };
  for (const Check* check : settings.checks) {
    check->run(context, result.violations);
  }

  CrossSummary& summary = out.summary;
  summary.cohort_move = settings.cohort_move;
  if (settings.cohort_move && price && price->applicable) {
    // DQ601: the scored moves near a clean split ratio, each with the DQ203 report
    // its siblings might make of it.
    const std::string peer_group = meta != nullptr ? meta->peer_group : std::string{};
    for (const PriceFinding& finding : price->findings) {
      const auto split = static_cast<std::size_t>(PriceHypothesis::kUnreportedSplit);
      // Not a move a split on file explains, nor a newest bar whose fate is open.
      const auto explained = static_cast<std::size_t>(PriceHypothesis::kExplainedSplit);
      if (finding.claimed || finding.provisional || !finding.split ||
          !finding.considered.at(split) || finding.posterior.at(explained) >= 0.5 ||
          std::fabs(std::log(finding.factor / finding.split->price_factor())) > 0.1) {
        continue;
      }
      PeerMove move;
      move.date = series.date[price->features.row[finding.bar]];
      move.ratio = finding.split->to_string();
      move.factor = finding.factor;
      move.series = series.display_name();
      move.peer_group = peer_group;
      move.posterior = finding.posterior;
      move.considered = finding.considered;
      move.provisional = finding.provisional;
      move.split = unreported_split_base(context, *price, finding);
      summary.moves.push_back(std::move(move));
    }
  }
  summary.cohort = settings.cohort;
  summary.stale = settings.stale;
  summary.coverage = settings.coverage;
  if (analysis && analysis->applicable) {
    summary.daily = true;
    summary.last = analysis->last;
    summary.tail_density = analysis->tail_density;
    summary.confident_ranges = std::move(analysis->confident_ranges);
    summary.confident_missing = std::move(analysis->confident_missing);
  }
  return out;
}

void Engine::filter_and_send(SeriesResult&& result) {
  std::erase_if(result.violations, [this](const Violation& v) {
    return v.severity < options_.min_severity ||
           (options_.since && v.date && *v.date < *options_.since);
  });
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
  sink_.series_done(std::move(result));
}

void Engine::deliver(Processed processed) {
  if (holding_) {
    cross_->add(std::move(processed.summary));
    held_.push_back(std::move(processed.result));
    return;
  }
  filter_and_send(std::move(processed.result));
}

void Engine::submit(Series&& series) {
  const Settings& settings = settings_for(series);
  Work work{next_sequence_++, std::move(series), &settings};
  if (workers_.empty()) {
    deliver(process(work));
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
    Processed processed = process(*work);
    work.reset();  // free the series before taking the next
    {
      const std::scoped_lock lock(mutex_);
      done_.emplace(processed.result.sequence, std::move(processed));
    }
    result_ready_.notify_all();
  }
}

void Engine::deliver_ready() {
  while (true) {
    std::optional<Processed> processed;
    {
      const std::scoped_lock lock(mutex_);
      const auto it = done_.find(next_to_deliver_);
      if (it == done_.end()) {
        return;
      }
      processed.emplace(std::move(it->second));
      done_.erase(it);
      ++next_to_deliver_;
      --in_flight_;
    }
    deliver(std::move(*processed));
  }
}

void Engine::finish() {
  if (!workers_.empty()) {
    while (true) {
      {
        std::unique_lock lock(mutex_);
        result_ready_.wait(lock, [this] {
          return next_to_deliver_ == next_sequence_ || done_.contains(next_to_deliver_);
        });
        if (next_to_deliver_ == next_sequence_) {
          break;
        }
      }
      deliver_ready();
    }
  }
  std::vector<Violation> cohort;
  if (holding_) {
    cohort = cross_->finalize(held_);
    for (SeriesResult& result : held_) {
      filter_and_send(std::move(result));
    }
    held_.clear();
  }
  if (market_days_) {
    std::vector<Violation> days = market_days(*context_.market, calendar_);
    cohort.insert(cohort.end(), std::make_move_iterator(days.begin()),
                  std::make_move_iterator(days.end()));
  }
  if (!cohort.empty()) {
    SeriesResult cross;
    cross.sequence = next_sequence_;
    cross.cross_sectional = true;
    cross.source = calendar_.description();
    cross.violations = std::move(cohort);
    filter_and_send(std::move(cross));
  }
}

std::optional<Date> Engine::as_of() const { return cross_ ? cross_->as_of() : std::nullopt; }

}  // namespace dorq
