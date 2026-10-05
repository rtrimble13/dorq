#include "checks/price.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "checks/coverage.hpp"
#include "dorq/number.hpp"
#include "price/model.hpp"

namespace dorq {
namespace {

using H = PriceHypothesis;

const CheckInfo& info_for(H hypothesis) noexcept {
  switch (hypothesis) {
    case H::kBadPrint:
      return bad_print_info();
    case H::kBadClose:
      return ohlc_close_mismatch_info();
    case H::kUnreportedSplit:
      return unreported_split_info();
    case H::kScaleError:
      return scale_shift_info();
    case H::kHistorySegment:
      return history_segment_info();
    case H::kMarketMove:
    case H::kTickMove:
      break;
  }
  return large_move_info();
}

// What a price finding is reported as: its most probable error, or DQ209 when an
// error is improbable.
const CheckInfo& reported_as(const PriceFinding& finding, const SeverityThresholds& thresholds) {
  if (finding.p_error < thresholds.info) {
    return large_move_info();
  }
  return info_for(finding.hypothesis);
}

std::string price(double value) { return significant(value, 6); }

std::string times(double factor) { return "×" + significant(factor, 4); }

class Builder {
 public:
  Builder(const SeriesContext& context, const PriceAnalysis& analysis, const PriceFinding& finding)
      : s_(context.series), settings_(context.price), f_(analysis.features), finding_(finding) {}

  [[nodiscard]] std::size_t row(std::size_t bar) const { return f_.row[bar]; }
  [[nodiscard]] double close(std::size_t bar) const { return s_.close[row(bar)]; }
  [[nodiscard]] Date date(std::size_t bar) const { return s_.date[row(bar)]; }
  [[nodiscard]] std::size_t bars() const { return f_.size(); }

  // "close" for bars, "value" for a point series.
  [[nodiscard]] std::string field() const {
    return std::string{field_name(Field::kClose, s_.kind)};
  }

  // A move from a to b: a ratio on the log scale, a signed change on the value
  // scale.
  [[nodiscard]] std::string change(double a, double b) const {
    if (f_.log_scale) {
      return times(b / a);
    }
    const double d = b - a;
    return (d >= 0.0 ? "+" : "−") + significant(std::fabs(d), 4);
  }

  [[nodiscard]] std::string move() const {
    const std::size_t t = finding_.bar;
    return field() + " " + price(close(t - 1)) + "→" + price(close(t)) + " (" +
           change(close(t - 1), close(t));
  }

  void bad_print(Violation& v) const {
    const std::size_t t = finding_.bar;
    const std::size_t last = finding_.end_bar;
    const bool reverted = last + 1 < bars();
    if (finding_.block == 1 && reverted) {
      const bool back = (close(t) - close(t - 1)) * (close(t + 1) - close(t)) < 0.0;
      v.message = field() + " " + price(close(t)) + " between " + price(close(t - 1)) + " and " +
                  price(close(t + 1)) + " (" + change(close(t - 1), close(t)) +
                  (back ? ", then back on the next bar)"
                        : ", then " + change(close(t), close(t + 1)) + ")");
    } else if (reverted) {
      v.message = std::to_string(finding_.block) + " bars " +
                  (f_.log_scale ? "at " + times(finding_.factor) + " the level"
                                : "off the level by " + change(close(t - 1), close(t))) +
                  " either side (" + field() + " " + price(close(t - 1)) + "→" +
                  price(close(t)) + ", back to " + price(close(last + 1)) + ")";
    } else {
      v.message = move() + "), not yet confirmed by a later bar";
    }
    v.end_date = last != t ? std::optional<Date>{date(last)} : std::nullopt;
    SuggestedAction action;
    action.kind = "delete_bars";
    action.fields = {{"first", date(t).to_string()},
                     {"last", date(last).to_string()},
                     {"bars", std::int64_t{finding_.block}}};
    action.text = "delete or re-fetch " + date(t).to_string() +
                  (last != t ? ".." + date(last).to_string() : std::string{});
    v.suggested_action = std::move(action);
  }

  void bad_close(Violation& v) const {
    const std::size_t t = finding_.bar;
    const std::size_t r = row(t);
    const bool outside = s_.close[r] < s_.low[r] || s_.close[r] > s_.high[r];
    if (outside) {
      v.message = "close " + price(close(t)) + " outside the bar's range " + price(s_.low[r]) +
                  ".." + price(s_.high[r]) + ", whose open " + price(s_.open[r]) +
                  " held the prior close " + price(close(t - 1));
    } else {
      v.message = "close " + price(close(t)) + " is " + times(close(t) / s_.open[r]) +
                  " the bar's open " + price(s_.open[r]) + ", which held the prior close " +
                  price(close(t - 1));
    }
    SuggestedAction action;
    action.kind = "refetch_bar";
    action.fields = {{"date", date(t).to_string()}, {"field", std::string{"close"}}};
    action.text = "re-fetch the close of " + date(t).to_string();
    v.suggested_action = std::move(action);
  }

  void split(Violation& v) const {
    const std::size_t t = finding_.bar;
    const std::string ratio = finding_.split ? finding_.split->to_string() : "?";
    v.message = move() + " ≈ a " + ratio + " split)";
    if (finding_.volume_ratio) {
      v.message += ", volume " + times(*finding_.volume_ratio) + " after";
    }
    SuggestedAction action;
    action.kind = "add_split";
    action.fields = {{"ratio", ratio}, {"ex_date", date(t).to_string()}};
    action.text = "add split " + ratio + " ex " + date(t).to_string();
    v.suggested_action = std::move(action);
  }

  void scale(Violation& v) const {
    const std::size_t t = finding_.bar;
    const double power = std::pow(10.0, finding_.power_of_ten);
    const bool near_power = std::fabs(std::log(finding_.factor / power)) < 0.1;
    std::size_t first = t;
    std::size_t last = finding_.end_bar;
    double rescale = 1.0 / finding_.factor;
    if (finding_.end_bar > t) {
      v.message = field() + "s " + date(t).to_string() + ".." + date(last).to_string() + " are " +
                  times(finding_.factor) + " the level either side" +
                  (near_power ? " (×" + format_number(power) + ")" : std::string{}) +
                  ": an era at the wrong scale";
    } else {
      const bool exact = times(finding_.factor) == "×" + format_number(power);
      v.message =
          move() + (near_power && !exact ? " ≈ ×" + format_number(power) : std::string{}) + ")";
      if (finding_.volume_ratio) {
        v.message += ", volume " + times(*finding_.volume_ratio) + " after";
      }
      // Which side is wrong: the implausible one, or else the shorter.
      const auto implausible = [this](double p) {
        return p < settings_.min_price || p > settings_.max_price;
      };
      const bool pre_bad = implausible(close(t - 1));
      const bool post_bad = implausible(close(t));
      const bool before = pre_bad || (!post_bad && t < bars() - t);
      if (before) {
        first = 0;
        last = t - 1;
        rescale = finding_.factor;
      } else {
        last = bars() - 1;
      }
    }
    if (finding_.end_bar > t) {
      v.end_date = date(last);
    }
    SuggestedAction action;
    action.kind = "rescale";
    action.fields = {
        {"first", date(first).to_string()}, {"last", date(last).to_string()}, {"factor", rescale}};
    action.text = "rescale " + date(first).to_string() + ".." + date(last).to_string() + " by " +
                  times(rescale);
    v.suggested_action = std::move(action);
  }

  void segment(Violation& v) const {
    const std::size_t t = finding_.bar;
    v.message = "after " + std::to_string(f_.elapsed[t] - 1) + " sessions without a bar, " +
                move() + "): another security's history may continue here";
    SuggestedAction action;
    action.kind = "split_history";
    action.fields = {{"at", date(t).to_string()}};
    action.text = "start a new history at " + date(t).to_string();
    v.suggested_action = std::move(action);
  }

  void market(Violation& v) const {
    const bool tick = finding_.posterior.at(static_cast<std::size_t>(H::kTickMove)) >
                      finding_.posterior.at(static_cast<std::size_t>(H::kMarketMove));
    v.message = move() + "): probably " + (tick ? "a move of a tick or two" : "a real move");
  }

 private:
  const Series& s_;
  const PriceSettings& settings_;
  const PriceFeatures& f_;
  const PriceFinding& finding_;
};

Violation make_violation(const SeriesContext& context, const PriceAnalysis& analysis,
                         const PriceFinding& finding, const CheckInfo& info) {
  const Builder b(context, analysis, finding);
  const std::size_t t = finding.bar;
  Violation v;
  v.check = &info;
  v.date = b.date(t);
  v.line = context.series.line[b.row(t)];
  v.p_error = finding.p_error;
  v.provisional = finding.provisional;
  const bool market = &info == &large_move_info();
  if (market) {
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    b.market(v);
  } else {
    v.severity = context.thresholds.for_probability(finding.p_error).value_or(Severity::kInfo);
    if (finding.provisional) {
      v.severity = std::min(v.severity, Severity::kWarn);
    }
    v.classification = finding.hypothesis == H::kUnreportedSplit ? Classification::kContextGap
                                                                 : Classification::kDataError;
    switch (finding.hypothesis) {
      case H::kBadPrint:
        b.bad_print(v);
        break;
      case H::kBadClose:
        b.bad_close(v);
        break;
      case H::kUnreportedSplit:
        b.split(v);
        break;
      case H::kScaleError:
        b.scale(v);
        break;
      case H::kHistorySegment:
        b.segment(v);
        break;
      case H::kMarketMove:
      case H::kTickMove:
        break;
    }
  }
  v.message += "; P(error) = " + format_probability(finding.p_error);
  if (finding.provisional) {
    v.message += ", provisional";
  }
  for (std::size_t h = 0; h < kPriceHypotheses; ++h) {
    if (finding.considered.at(h)) {
      v.hypotheses.push_back({to_string(static_cast<H>(h)), finding.posterior.at(h)});
    }
  }
  for (const PriceEvidence& e : finding.evidence) {
    v.evidence.push_back({e.feature, e.value, e.log_bf, e.note});
  }
  const std::string field = b.field();
  v.detail = {{field, b.close(t)},
              {"previous_" + field, b.close(t - 1)},
              {"factor", finding.factor}};
  if (!analysis.features.log_scale) {
    v.detail.push_back({"change", b.close(t) - b.close(t - 1)});
  }
  v.detail.push_back({"tail_probability", finding.tail});
  if (const int gap = analysis.features.elapsed[t]; gap > 1) {
    v.detail.push_back({"sessions_elapsed", std::int64_t{gap}});
  }
  if (finding.volume_ratio) {
    v.detail.push_back({"volume_ratio", *finding.volume_ratio});
  }
  if (finding.hypothesis == H::kBadPrint && !market) {
    v.detail.push_back({"bars", std::int64_t{finding.block}});
  }
  if (finding.hypothesis == H::kUnreportedSplit && finding.split && !market) {
    v.detail.push_back({"split_ratio", finding.split->to_string()});
  }
  return v;
}

}  // namespace

std::string significant(double value, int digits) {
  if (value == 0.0 || !std::isfinite(value)) {
    return format_number(value);
  }
  const double magnitude = std::floor(std::log10(std::fabs(value)));
  const double scale = std::pow(10.0, static_cast<double>(digits - 1) - magnitude);
  return format_number(std::round(value * scale) / scale);
}

void PriceCheck::run(const SeriesContext& context, std::vector<Violation>& out) const {
  if (context.price_analysis == nullptr || !context.price_analysis->applicable) {
    return;
  }
  const PriceAnalysis& analysis = *context.price_analysis;
  for (const PriceFinding& finding : analysis.findings) {
    if (&reported_as(finding, context.thresholds) != &info_) {
      continue;
    }
    out.push_back(make_violation(context, analysis, finding, info_));
  }
}

const CheckInfo& bad_print_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ201",
      .name = "bad-print",
      .summary = "a wrong value, or a block of up to five bars, that the series reverts from",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

const CheckInfo& scale_shift_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ202",
      .name = "scale-shift",
      .summary =
          "the level changes by a power of ten (or to an implausible price) without the "
          "volume shift a split causes",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

const CheckInfo& unreported_split_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ203",
      .name = "unreported-split",
      .summary =
          "the level changes by a split ratio, volume shifts inversely, and the new level "
          "holds",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

const CheckInfo& ohlc_close_mismatch_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ204",
      .name = "ohlc-close-mismatch",
      .summary = "the close is far from a bar whose open, high and low held the prior level",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

const CheckInfo& history_segment_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ205",
      .name = "history-segment",
      .summary = "after a long gap, a different security's history seems to continue",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

const CheckInfo& large_move_info() noexcept {
  static const CheckInfo kInfo{
      .code = "DQ209",
      .name = "large-move",
      .summary = "an anomalous move that is probably a market fact (info)",
      .default_severity = Severity::kInfo,
      .applies = Applies::kAny,
  };
  return kInfo;
}

}  // namespace dorq
