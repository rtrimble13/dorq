#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/date.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"
#include "dorq/violation.hpp"

namespace dorq {

// What the statement checks (DQ9xx) share about a statements series' rows.

// A row's period, with its version or (a finding on the whole period) without.
[[nodiscard]] StatementPeriod period_of(const Series& s, std::size_t i, bool with_version);

// The newer of a row's known_from and loaded_on: what --since judges it by.
[[nodiscard]] Date known_of(const Series& s, std::size_t i);

// A deterministic error on row i: its date, line, period and knowledge date.
[[nodiscard]] Violation statement_finding(const CheckInfo& info, const Series& s, std::size_t i,
                                          bool with_version);

// "v2".
[[nodiscard]] std::string version_text(std::int32_t version);

// An amount as a filing writes it: a whole number where it is one.
[[nodiscard]] std::string amount_text(double value);

[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

}  // namespace dorq
