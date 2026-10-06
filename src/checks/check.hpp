#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "checks/coverage_model.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/frequency.hpp"
#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "price/model.hpp"

namespace dorq {

// What a check sees: one series, and the settings that apply to it.
struct SeriesContext {
  const Series& series;
  const IntegritySettings& integrity;
  const CoverageSettings& coverage;
  const Calendar& calendar;
  const SeverityThresholds& thresholds;
  const PriceSettings& price;
  Frequency frequency = Frequency::kDaily;  // resolved: never kAuto
  // The coverage model's view of a daily series, computed once when any DQ30x
  // check runs; nullptr otherwise.
  const CoverageAnalysis* analysis = nullptr;
  GapReport gap_report = GapReport::kRun;
  // The price model's findings, computed once when any DQ2xx price check runs;
  // nullptr otherwise.
  const PriceAnalysis* price_analysis = nullptr;
  // Context inputs (doc/context.md): whether --actions was given, and this
  // series' actions and metadata (nullptr when the files have none for it).
  bool have_actions = false;
  const SeriesActions* actions = nullptr;
  const SeriesMeta* meta = nullptr;

  // The exchange tick from --meta, if given.
  [[nodiscard]] std::optional<double> meta_tick() const noexcept {
    return meta != nullptr ? meta->tick_size : std::nullopt;
  }
};

// A check reads one series and appends violations. Checks hold no state and are
// called concurrently on different series, so run() must not touch anything but
// its arguments.
class Check {
 public:
  Check() = default;
  Check(const Check&) = delete;
  Check& operator=(const Check&) = delete;
  Check(Check&&) = delete;
  Check& operator=(Check&&) = delete;
  virtual ~Check() = default;

  [[nodiscard]] virtual const CheckInfo& info() const noexcept = 0;
  virtual void run(const SeriesContext& context, std::vector<Violation>& out) const = 0;
};

// Every check, in code order.
[[nodiscard]] std::span<const Check* const> all_checks();

// A check by code ("DQ101", any case) or name ("ohlc-bounds"), or nullptr.
[[nodiscard]] const Check* find_check(std::string_view code_or_name);

// The page doc/checks/<code>.md, compiled in; empty if there is none.
[[nodiscard]] std::string_view check_doc(std::string_view code);

// Which checks run, from select / extend_select / ignore lists. Each entry is a
// code, a code prefix ("DQ1") or a name. As in flake8, the most specific entry
// wins: a check is enabled when its longest matching select entry is longer than
// its longest matching ignore entry. A name is the most specific match there is.
class Selection {
 public:
  Selection() = default;
  // Returns an error message for an entry that is neither a code prefix nor a
  // check name.
  [[nodiscard]] static std::string validate(std::span<const std::string> entries);

  Selection(std::span<const std::string> select, std::span<const std::string> ignore);

  [[nodiscard]] bool enabled(const CheckInfo& info) const;

 private:
  std::vector<std::string> select_;
  std::vector<std::string> ignore_;
};

}  // namespace dorq
