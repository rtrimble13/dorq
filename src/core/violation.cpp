#include "dorq/violation.hpp"

#include <optional>
#include <string_view>

#include "core/text.hpp"

namespace dorq {

std::string_view to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::kInfo:
      return "info";
    case Severity::kWarn:
      return "warn";
    case Severity::kError:
      return "error";
  }
  return "error";
}

std::optional<Severity> parse_severity(std::string_view text) noexcept {
  if (iequals(text, "info")) {
    return Severity::kInfo;
  }
  if (iequals(text, "warn") || iequals(text, "warning")) {
    return Severity::kWarn;
  }
  if (iequals(text, "error")) {
    return Severity::kError;
  }
  return std::nullopt;
}

std::string_view to_string(Classification classification) noexcept {
  switch (classification) {
    case Classification::kDataError:
      return "data_error";
    case Classification::kMarketFact:
      return "market_fact";
    case Classification::kContextGap:
      return "context_gap";
  }
  return "data_error";
}

bool applies_to(Applies applies, SeriesKind kind) noexcept {
  switch (applies) {
    case Applies::kTimeSeries:
      return kind != SeriesKind::kStatements;
    case Applies::kOhlcvOnly:
      return kind == SeriesKind::kOhlcv;
    case Applies::kStatements:
      return kind == SeriesKind::kStatements;
    case Applies::kEveryKind:
      return true;
  }
  return false;
}

std::string_view to_string(Applies applies) noexcept {
  switch (applies) {
    case Applies::kTimeSeries:
      return "ohlcv,point";
    case Applies::kOhlcvOnly:
      return "ohlcv";
    case Applies::kStatements:
      return "statements";
    case Applies::kEveryKind:
      return "all";
  }
  return "ohlcv,point";
}

}  // namespace dorq
