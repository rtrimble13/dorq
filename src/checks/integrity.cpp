#include "checks/integrity.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/number.hpp"
#include "price/tick.hpp"

namespace dorq {
namespace {

Violation make_violation(const CheckInfo& info, Severity severity, const Series& series,
                         std::size_t row, std::string message) {
  Violation v;
  v.check = &info;
  v.severity = severity;
  v.date = series.date[row];
  v.line = series.line[row];
  v.message = std::move(message);
  return v;
}

void append_part(std::string& message, const std::string& part) {
  if (!message.empty()) {
    message += "; ";
  }
  message += part;
}

double at(const std::vector<double>& column, std::size_t row) {
  return row < column.size() ? column[row] : std::nan("");
}

// NaN equals NaN here: two rows that both lack a volume agree about it.
bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

double median(std::vector<double> values) {
  if (values.empty()) {
    return std::nan("");
  }
  const std::size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
  return values[mid];
}

std::string percent(double share) {
  return std::to_string(static_cast<int>(std::lround(share * 100.0))) + "%";
}

}  // namespace

// ---------------------------------------------------------------------------
// DQ101 ohlc-bounds

const CheckInfo& OhlcBounds::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ101",
      .name = "ohlc-bounds",
      .summary = "high below open, low or close, or low above open or close",
      .default_severity = Severity::kError,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void OhlcBounds::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double o = s.open[i];
    const double h = s.high[i];
    const double l = s.low[i];
    const double c = s.close[i];
    if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) {
      continue;  // DQ104 reports the missing field
    }
    std::string message;
    if (h < l) {
      append_part(message, "high " + format_number(h) + " < low " + format_number(l));
    }
    if (h < o) {
      append_part(message, "high " + format_number(h) + " < open " + format_number(o));
    }
    if (h < c) {
      append_part(message, "high " + format_number(h) + " < close " + format_number(c));
    }
    if (l > o) {
      append_part(message, "low " + format_number(l) + " > open " + format_number(o));
    }
    if (l > c) {
      append_part(message, "low " + format_number(l) + " > close " + format_number(c));
    }
    if (message.empty()) {
      continue;
    }
    Violation v = make_violation(info(), Severity::kError, s, i, std::move(message));
    v.detail = {{"open", o}, {"high", h}, {"low", l}, {"close", c}};
    out.push_back(std::move(v));
  }
}

// ---------------------------------------------------------------------------
// DQ102 non-positive

const CheckInfo& NonPositive::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ102",
      .name = "non-positive",
      .summary = "a price at or below zero, or a negative volume",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void NonPositive::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const bool prices = s.kind == SeriesKind::kOhlcv || context.integrity.positive_point_series;
  for (std::size_t i = 0; i < s.size(); ++i) {
    std::string message;
    std::vector<DetailField> detail;
    const auto check_price = [&](Field field, double value) {
      if (std::isfinite(value) && value <= 0.0) {
        const std::string name{field_name(field, s.kind)};
        append_part(message, name + " is " + format_number(value));
        detail.push_back({name, value});
      }
    };
    if (prices) {
      if (s.kind == SeriesKind::kOhlcv) {
        check_price(Field::kOpen, s.open[i]);
        check_price(Field::kHigh, s.high[i]);
        check_price(Field::kLow, s.low[i]);
      }
      check_price(Field::kClose, s.close[i]);
      if (s.has_vwap) {
        check_price(Field::kVwap, at(s.vwap, i));
      }
    }
    if (s.has_volume) {
      const double volume = at(s.volume, i);
      if (std::isfinite(volume) && volume < 0.0) {
        append_part(message, "volume is " + format_number(volume));
        detail.push_back({"volume", volume});
      }
    }
    if (!message.empty()) {
      Violation v = make_violation(info(), Severity::kError, s, i, std::move(message));
      v.detail = std::move(detail);
      out.push_back(std::move(v));
    }
  }
}

// ---------------------------------------------------------------------------
// DQ103 duplicate-date

const CheckInfo& DuplicateDate::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ103",
      .name = "duplicate-date",
      .summary = "more than one row for a date (error when they disagree)",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void DuplicateDate::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const auto row_equal = [&s](std::size_t a, std::size_t b) {
    const bool ohlc =
        s.kind != SeriesKind::kOhlcv ||
        (same(s.open[a], s.open[b]) && same(s.high[a], s.high[b]) && same(s.low[a], s.low[b]));
    return ohlc && same(s.close[a], s.close[b]) && same(at(s.volume, a), at(s.volume, b)) &&
           same(at(s.vwap, a), at(s.vwap, b));
  };
  std::size_t start = 0;
  while (start < s.size()) {
    std::size_t end = start + 1;
    while (end < s.size() && s.date[end] == s.date[start]) {
      ++end;
    }
    if (end - start > 1) {
      bool identical = true;
      std::string lines;
      for (std::size_t i = start; i < end; ++i) {
        identical = identical && row_equal(start, i);
        if (i > start) {
          lines += ", ";
        }
        lines += std::to_string(s.line[i]);
      }
      std::string message = std::to_string(end - start);
      message += identical ? " identical rows for this date (lines "
                           : " rows for this date with different values (lines ";
      message += lines;
      message += ')';
      Violation v = make_violation(info(), identical ? Severity::kInfo : Severity::kError, s, start,
                                   std::move(message));
      v.detail = {{"rows", static_cast<std::int64_t>(end - start)},
                  {"lines", lines},
                  {"identical", identical}};
      out.push_back(std::move(v));
    }
    start = end;
  }
}

// ---------------------------------------------------------------------------
// DQ104 missing-field

const CheckInfo& MissingField::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ104",
      .name = "missing-field",
      .summary = "an empty or unparseable value (a row with no usable date is skipped)",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void MissingField::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const auto required = [&s](Field field) {
    if (field == Field::kClose || field == Field::kDate) {
      return true;
    }
    return s.kind == SeriesKind::kOhlcv &&
           (field == Field::kOpen || field == Field::kHigh || field == Field::kLow);
  };
  std::size_t i = 0;
  while (i < s.issues.size()) {
    std::size_t end = i + 1;
    while (end < s.issues.size() && s.issues[end].line == s.issues[i].line) {
      ++end;
    }
    Violation v;
    v.check = &info();
    v.severity = Severity::kInfo;
    v.line = s.issues[i].line;
    if (s.issues[i].has_date) {
      v.date = s.issues[i].date;
    }
    std::string fields;
    for (std::size_t j = i; j < end; ++j) {
      const FieldIssue& issue = s.issues[j];
      const std::string name{field_name(issue.field, s.kind)};
      fields += (j == i ? "" : ",") + name;
      Severity severity = Severity::kError;
      std::string part;
      if (issue.kind == FieldIssue::Kind::kInvalid) {
        part = name + " \"" + issue.text + "\" is not " +
               (issue.field == Field::kDate ? "a date" : "a number");
      } else {
        part = name + (issue.text.empty() ? " is empty" : " is \"" + issue.text + "\"");
        severity = required(issue.field) ? Severity::kError : Severity::kWarn;
      }
      if (issue.field == Field::kDate) {
        part += "; the row is skipped";
      }
      append_part(v.message, part);
      v.detail.push_back({name, issue.text});
      v.severity = std::max(v.severity, severity);
    }
    v.detail.push_back({"fields", fields});
    out.push_back(std::move(v));
    i = end;
  }
}

// ---------------------------------------------------------------------------
// DQ106 precision-shift

const CheckInfo& PrecisionShift::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ106",
      .name = "precision-shift",
      .summary = "the precision closes are written with changes regime (computed vs quoted prices)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void PrecisionShift::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const IntegritySettings& settings = context.integrity;
  // Rows with a close, and whether each close is "high precision".
  std::vector<std::size_t> rows;
  std::vector<int> prefix = {0};
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!std::isfinite(s.close[i])) {
      continue;
    }
    const bool high = s.close_decimals[i] >= settings.precision_high_decimals &&
                      s.close_sig_figs[i] >= settings.precision_high_sig_figs;
    rows.push_back(i);
    prefix.push_back(prefix.back() + (high ? 1 : 0));
  }
  const auto n = static_cast<std::ptrdiff_t>(rows.size());
  const auto min_segment = static_cast<std::ptrdiff_t>(settings.precision_min_segment);
  if (n < 2 * min_segment) {
    return;
  }
  // The split that best separates the two regimes; the earliest one on ties.
  std::ptrdiff_t best = -1;
  double best_contrast = -1.0;
  double before_share = 0.0;
  double after_share = 0.0;
  const int total = prefix.back();
  for (std::ptrdiff_t k = min_segment; k <= n - min_segment; ++k) {
    const int high_before = prefix[static_cast<std::size_t>(k)];
    const double before = static_cast<double>(high_before) / static_cast<double>(k);
    const double after = static_cast<double>(total - high_before) / static_cast<double>(n - k);
    const double contrast = std::fabs(after - before);
    if (contrast > best_contrast) {
      best_contrast = contrast;
      best = k;
      before_share = before;
      after_share = after;
    }
  }
  if (best < 0 || best_contrast < settings.precision_min_contrast) {
    return;
  }
  const std::size_t row = rows[static_cast<std::size_t>(best)];
  const auto before_rows = std::int64_t{best};
  const auto after_rows = std::int64_t{n - best};
  const std::string threshold = std::to_string(settings.precision_high_decimals) + "+ decimals";
  std::string message = "close precision changes here: " + percent(before_share) + " of " +
                        std::to_string(before_rows) + " earlier closes need " + threshold + ", " +
                        percent(after_share) + " of " + std::to_string(after_rows) +
                        " later ones do; ";
  message += after_share > before_share
                 ? "the later prices look computed (adjusted) rather than quoted"
                 : "the earlier prices look computed (a back-adjusted history?) rather than quoted";
  Violation v = make_violation(info(), Severity::kWarn, s, row, std::move(message));
  v.detail = {{"before_rows", before_rows},
              {"before_high_precision_share", before_share},
              {"after_rows", after_rows},
              {"after_high_precision_share", after_share},
              {"high_decimals", static_cast<std::int64_t>(settings.precision_high_decimals)},
              {"high_sig_figs", static_cast<std::int64_t>(settings.precision_high_sig_figs)}};
  out.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// DQ107 zero-range-with-volume

const CheckInfo& ZeroRangeWithVolume::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ107",
      .name = "zero-range-with-volume",
      .summary = "open = high = low = close on at least the series' typical volume",
      .default_severity = Severity::kWarn,
      .applies = Applies::kOhlcvOnly,
  };
  return kInfo;
}

void ZeroRangeWithVolume::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (!s.has_volume) {
    return;
  }
  // A single trade prints a flat bar on small volume, and on a thin name that is
  // normal. A flat bar on the volume the series usually trades is not -- unless
  // the price grid is coarse for the price, as for a sub-dime stock quoted in
  // cents, whose bars span a tick or two and are often flat.
  constexpr std::size_t kMinTradedRows = 20;
  constexpr std::size_t kNearby = 20;        // bars either side that judge the usual range
  constexpr std::size_t kMinNearby = 5;      // bars with a range needed to judge it
  constexpr std::size_t kMinFlatNearby = 3;  // and other flat bars that make them a habit
  std::vector<double> traded;
  std::vector<std::uint8_t> decimals;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (std::isfinite(s.volume[i]) && s.volume[i] > 0.0) {
      traded.push_back(s.volume[i]);
    }
    if (std::isfinite(s.close[i])) {
      decimals.push_back(s.close_decimals[i]);
    }
  }
  if (traded.size() < kMinTradedRows) {
    return;
  }
  const double typical = median(traded);
  const double written = written_grid(std::move(decimals));
  const auto flat = [&s](std::size_t i) {
    const double c = s.close[i];
    return s.open[i] == c && s.high[i] == c && s.low[i] == c;
  };
  // Each traded bar's range, as a share of its close. NaN for a flat or unusable
  // bar.
  std::vector<double> range(s.size(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double c = s.close[i];
    if (!std::isfinite(s.volume[i]) || s.volume[i] <= 0.0 || !std::isfinite(c) || c <= 0.0 ||
        !std::isfinite(s.high[i]) || !std::isfinite(s.low[i]) || flat(i)) {
      continue;
    }
    range[i] = (s.high[i] - s.low[i]) / c;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double c = s.close[i];
    const double volume = s.volume[i];
    if (!std::isfinite(c) || !std::isfinite(volume) || volume <= 0.0 || volume < typical ||
        !flat(i)) {
      continue;
    }
    // The ranges nearby, at this bar's price, in steps of its price grid: the
    // exchange's tick, or the grid the prices are written on. Rounded, since 0.05 -
    // 0.03 is not exactly two cents in binary.
    const std::size_t from = i >= kNearby ? i - kNearby : 0;
    const std::size_t to = std::min(s.size(), i + kNearby + 1);
    const double grid = std::max({tick_size(s.date[i], c), written, observed_step(s, from, to)});
    std::vector<double> nearby;
    std::size_t traded_nearby = 0;
    std::size_t flat_nearby = 0;
    for (std::size_t j = from; j < to; ++j) {
      if (j == i || !std::isfinite(s.volume[j]) || s.volume[j] <= 0.0) {
        continue;
      }
      ++traded_nearby;
      if (std::isfinite(range[j])) {
        nearby.push_back(std::round(range[j] * c / grid * 1e6) / 1e6);
      } else if (flat(j)) {
        ++flat_nearby;
      }
    }
    const double usual =
        nearby.size() >= kMinNearby ? median(nearby) : std::numeric_limits<double>::quiet_NaN();
    if (usual <= context.integrity.flat_bar_steps) {
      continue;  // the bars here span a step or two: a flat one is ordinary
    }
    // Flat bars among ranged ones are a habit of this stretch (a price only a few
    // ticks wide); a run of flat bars with no ranged bar nearby is not.
    if (nearby.size() >= kMinNearby && flat_nearby >= kMinFlatNearby &&
        10 * flat_nearby >= traded_nearby) {
      continue;
    }
    std::string message = "open = high = low = close = " + format_number(c) + " on volume " +
                          format_number(volume) + " (median " + format_number(typical) + ")";
    message += std::isfinite(usual)
                   ? ", where the bars nearby span a median " +
                         format_number(std::round(usual * 10.0) / 10.0) + " steps of the price grid"
                   : ", with no bar nearby that has a range";
    Violation v = make_violation(info(), Severity::kWarn, s, i, std::move(message));
    v.detail = {{"close", c}, {"volume", volume}, {"median_volume", typical}};
    if (std::isfinite(usual)) {
      v.detail.push_back({"nearby_range_steps", usual});
    }
    out.push_back(std::move(v));
  }
}

// ---------------------------------------------------------------------------
// DQ108 out-of-bounds

const CheckInfo& OutOfBounds::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ108",
      .name = "out-of-bounds",
      .summary = "a value outside the range configured for the series ([integrity] bounds)",
      .default_severity = Severity::kError,
      .applies = Applies::kAny,
  };
  return kInfo;
}

void OutOfBounds::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const std::optional<Bounds>& bounds = context.integrity.bounds;
  if (!bounds) {
    return;
  }
  const Series& s = context.series;
  const std::string_view field = field_name(Field::kClose, s.kind);
  const std::string range =
      "[" + format_number(bounds->low) + ", " + format_number(bounds->high) + "]";
  for (std::size_t i = 0; i < s.size(); ++i) {
    const double value = s.close[i];
    if (!std::isfinite(value) || (value >= bounds->low && value <= bounds->high)) {
      continue;
    }
    Violation v = make_violation(
        info(), Severity::kError, s, i,
        std::string{field} + " " + format_number(value) + " is outside the bounds " + range);
    v.detail = {{std::string{field}, value}, {"low", bounds->low}, {"high", bounds->high}};
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
