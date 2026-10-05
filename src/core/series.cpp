#include "dorq/series.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string_view>
#include <vector>

namespace dorq {
namespace {

template <typename T>
void permute(std::vector<T>& values, const std::vector<std::size_t>& order) {
  if (values.empty()) {
    return;
  }
  std::vector<T> sorted;
  sorted.reserve(values.size());
  for (const std::size_t i : order) {
    sorted.push_back(values[i]);
  }
  values.swap(sorted);
}

}  // namespace

std::string_view to_string(SeriesKind kind) noexcept {
  return kind == SeriesKind::kOhlcv ? "ohlcv" : "point";
}

std::string_view field_name(Field field, SeriesKind kind) noexcept {
  switch (field) {
    case Field::kSeries:
      return "series";
    case Field::kLabel:
      return "label";
    case Field::kDate:
      return "date";
    case Field::kOpen:
      return "open";
    case Field::kHigh:
      return "high";
    case Field::kLow:
      return "low";
    case Field::kClose:
      return kind == SeriesKind::kPoint ? "value" : "close";
    case Field::kVolume:
      return "volume";
    case Field::kVwap:
      return "vwap";
  }
  return "?";
}

void sort_by_date(Series& series) {
  if (std::is_sorted(series.date.begin(), series.date.end())) {
    return;
  }
  std::vector<std::size_t> order(series.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&series](std::size_t a, std::size_t b) {
    return series.date[a] < series.date[b];
  });
  permute(series.date, order);
  permute(series.open, order);
  permute(series.high, order);
  permute(series.low, order);
  permute(series.close, order);
  permute(series.volume, order);
  permute(series.vwap, order);
  permute(series.close_decimals, order);
  permute(series.close_sig_figs, order);
  permute(series.line, order);
}

}  // namespace dorq
