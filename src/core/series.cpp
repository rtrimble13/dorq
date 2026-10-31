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
  switch (kind) {
    case SeriesKind::kOhlcv:
      return "ohlcv";
    case SeriesKind::kPoint:
      return "point";
    case SeriesKind::kStatements:
      return "statements";
  }
  return "ohlcv";
}

std::string_view field_name(Field field, SeriesKind kind) noexcept {
  switch (field) {
    case Field::kSeries:
      return "series";
    case Field::kLabel:
      return "label";
    case Field::kDate:
      return kind == SeriesKind::kStatements ? "fiscal_date" : "date";
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
    case Field::kStatement:
      return "statement";
    case Field::kPeriodType:
      return "period_type";
    case Field::kFiscalYear:
      return "fiscal_year";
    case Field::kFiscalPeriod:
      return "fiscal_period";
    case Field::kVersion:
      return "version_no";
    case Field::kKnownFrom:
      return "known_from";
    case Field::kKnownTo:
      return "known_to";
    case Field::kKnowledgeSource:
      return "knowledge_source";
    case Field::kFilingDate:
      return "filing_date";
    case Field::kLoadedOn:
      return "loaded_on";
    case Field::kCurrency:
      return "reported_currency";
    case Field::kItem:
      return "item";
  }
  return "?";
}

namespace {

// A statements series: by fiscal date, statement, period type and version, input
// order breaking a tie (two rows for one version, which DQ901 reports).
void sort_statements(Series& series) {
  StatementRows& rows = series.statements;
  const auto before = [&series, &rows](std::size_t a, std::size_t b) {
    if (series.date[a] != series.date[b]) {
      return series.date[a] < series.date[b];
    }
    if (rows.statement[a] != rows.statement[b]) {
      return rows.statement[a] < rows.statement[b];
    }
    if (rows.period_type[a] != rows.period_type[b]) {
      return rows.period_type[a] < rows.period_type[b];
    }
    return rows.version[a] < rows.version[b];
  };
  std::vector<std::size_t> order(series.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  if (std::is_sorted(order.begin(), order.end(), before)) {
    return;
  }
  std::stable_sort(order.begin(), order.end(), before);
  permute(series.date, order);
  permute(series.close, order);
  permute(series.close_decimals, order);
  permute(series.close_sig_figs, order);
  permute(series.volume, order);
  permute(series.vwap, order);
  permute(series.line, order);
  permute(rows.statement, order);
  permute(rows.period_type, order);
  permute(rows.fiscal_year, order);
  permute(rows.fiscal_period, order);
  permute(rows.version, order);
  permute(rows.known_from, order);
  permute(rows.known_to, order);
  permute(rows.knowledge_source, order);
  permute(rows.filing_date, order);
  permute(rows.loaded_on, order);
  permute(rows.currency, order);
  for (std::vector<double>& values : rows.items) {
    permute(values, order);
  }
}

}  // namespace

void sort_by_date(Series& series) {
  if (series.kind == SeriesKind::kStatements) {
    sort_statements(series);
    return;
  }
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
