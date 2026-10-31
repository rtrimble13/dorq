#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "dorq/date.hpp"
#include "dorq/series.hpp"

namespace dorq {

// The rows of a statements series that the checks read (ADR 0005), as indexes
// into its rows, in their order: by fiscal date, then statement and period type.
// Every version is every row. A period is a statement, a period type and a fiscal
// date.
struct StatementViews {
  // Each period's version in force at the as-of date: known_from on or before it,
  // and known_to after it (or none). With no as-of date, each period's open
  // version (no known_to): on an input of every current version, the view at its
  // latest knowledge date. Where more than one qualifies (versions that do not
  // chain, which DQ901 reports), the highest version. A period with none is not
  // in the view.
  std::vector<std::size_t> in_force;
  // Each period's earliest version, normally version 1: what a reader saw before
  // any restatement.
  std::vector<std::size_t> first_reported;
};

[[nodiscard]] StatementViews build_statement_views(const Series& series, std::optional<Date> as_of);

}  // namespace dorq
