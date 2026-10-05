#include "checks/stale.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "checks/coverage.hpp"
#include "checks/price.hpp"
#include "core/text.hpp"
#include "dorq/number.hpp"
#include "price/model.hpp"

namespace dorq {
namespace {

// "1 in 250" for a probability of 0.004.
std::string one_in(double log_p) {
  const double odds = std::exp(-log_p);
  if (!std::isfinite(odds) || odds > 1e12) {
    return "less than 1 in a trillion";
  }
  return "1 in " + with_commas(std::llround(odds));
}

}  // namespace

const CheckInfo& RepeatedPrice::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ501",
      .name = "repeated-price",
      .summary = "the same close on traded bars running, more often than the series' moves allow",
      .default_severity = Severity::kWarn,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void RepeatedPrice::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.price_analysis == nullptr) {
    return;
  }
  const PriceAnalysis& analysis = *context.price_analysis;
  const Series& s = context.series;
  for (const StaleRun& run : analysis.stale_runs) {
    const auto severity = context.thresholds.for_probability(run.p_error);
    if (!severity) {
      continue;
    }
    const std::size_t first = analysis.features.row[run.first];
    const std::size_t last = analysis.features.row[run.last];
    const std::size_t before = analysis.features.row[run.first - 1];
    const auto bars = static_cast<std::int64_t>(run.last - run.first + 2);
    double low = s.volume[first];
    double high = s.volume[first];
    for (std::size_t j = run.first; j <= run.last; ++j) {
      const double v = s.volume[analysis.features.row[j]];
      low = std::min(low, v);
      high = std::max(high, v);
    }
    Violation v;
    v.check = &info();
    v.severity = *severity;
    v.p_error = run.p_error;
    v.date = s.date[first];
    if (last != first) {
      v.end_date = s.date[last];
    }
    v.line = s.line[first];
    v.message = "close " + significant(s.close[first], 6) + " on " + std::to_string(bars) +
                " traded bars running (from " + s.date[before].to_string() + "), on volume " +
                with_commas(std::llround(low)) +
                (high > low ? "–" + with_commas(std::llround(high)) : std::string{}) +
                ", where the series' moves make that " + one_in(run.log_q) + "; P(error) = " +
                format_probability(run.p_error);
    if (run.full_bars > 0) {
      v.message += " (" + std::to_string(run.full_bars) + " whole bar" +
                   (run.full_bars == 1 ? "" : "s") + " repeated)";
    }
    v.detail = {{"close", s.close[first]},
                {"bars", bars},
                {"whole_bars_repeated", std::int64_t{run.full_bars}},
                {"log_probability_healthy", run.log_q}};
    SuggestedAction action;
    action.kind = "delete_bars";
    action.fields = {{"first", s.date[first].to_string()},
                     {"last", s.date[last].to_string()},
                     {"bars", bars - 1}};
    action.text = "re-fetch " + s.date[first].to_string() +
                  (last != first ? ".." + s.date[last].to_string() : std::string{}) +
                  "; a stale feed repeated the close";
    v.suggested_action = std::move(action);
    out.push_back(std::move(v));
  }
}

const CheckInfo& CarryBar::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ502",
      .name = "carry-bar",
      .summary = "info: bars with no volume that repeat the last close",
      .default_severity = Severity::kInfo,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void CarryBar::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (!s.has_volume) {
    return;
  }
  // The last usable close before row i, by row; the first row has none.
  std::size_t previous = s.size();
  for (std::size_t i = 0; i < s.size();) {
    const bool carry = previous < s.size() && s.volume[i] == 0.0 &&
                       std::isfinite(s.close[i]) && s.close[i] == s.close[previous] &&
                       s.date[i] != s.date[previous];
    if (!carry) {
      if (std::isfinite(s.close[i])) {
        previous = i;
      }
      ++i;
      continue;
    }
    std::size_t last = i;
    while (last + 1 < s.size() && s.volume[last + 1] == 0.0 && s.close[last + 1] == s.close[i] &&
           s.date[last + 1] != s.date[last]) {
      ++last;
    }
    Violation v;
    v.check = &info();
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    v.p_error = 0.0;
    v.date = s.date[i];
    if (last != i) {
      v.end_date = s.date[last];
    }
    v.line = s.line[i];
    const std::size_t count = last - i + 1;
    v.message = std::to_string(count) + (count == 1 ? " bar with no trade carries" : " bars with no trade carry") +
                " the close " + significant(s.close[i], 6) + " of " +
                s.date[previous].to_string();
    v.detail = {{"close", s.close[i]},
                {"bars", static_cast<std::int64_t>(count)},
                {"traded", s.date[previous].to_string()}};
    out.push_back(std::move(v));
    previous = last;
    i = last + 1;
  }
}

}  // namespace dorq
