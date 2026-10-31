#include "statements/views.hpp"

#include <cstddef>
#include <optional>

namespace dorq {

StatementViews build_statement_views(const Series& series, std::optional<Date> as_of) {
  StatementViews views;
  const StatementRows& rows = series.statements;
  const auto in_force = [&rows, as_of](std::size_t i) {
    const std::optional<Date>& known_to = rows.known_to[i];
    if (!as_of) {
      return !known_to.has_value();
    }
    return rows.known_from[i] <= *as_of && (!known_to || *as_of < *known_to);
  };
  // The series is sorted by fiscal date, statement, period type and version, so a
  // period's versions are together and in order.
  std::size_t start = 0;
  while (start < series.size()) {
    std::size_t end = start + 1;
    while (end < series.size() && series.date[end] == series.date[start] &&
           rows.statement[end] == rows.statement[start] &&
           rows.period_type[end] == rows.period_type[start]) {
      ++end;
    }
    views.first_reported.push_back(start);
    std::size_t chosen = end;  // none
    for (std::size_t i = start; i < end; ++i) {
      if (in_force(i)) {
        chosen = i;
      }
    }
    if (chosen != end) {
      views.in_force.push_back(chosen);
    }
    start = end;
  }
  return views;
}

}  // namespace dorq
