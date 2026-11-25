#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// "a", "a and b", "a, b and c".
[[nodiscard]] std::string and_list(const std::vector<std::string>& parts);

// DE-12: one condition, one report.

// A statement check's report on one row, before the folds.
struct RowReport {
  Violation v;
  std::size_t row = 0;
  // What tells two conditions of one period apart ("identity=balance_sheet"):
  // reports whose conditions differ never fold.
  std::string condition;
  // The amount the condition is (a gap, a value) and its tolerance: two reports of
  // one condition fold where their amounts are within the larger tolerance.
  double amount = 0.0;
  double tolerance = 0.0;
};

// A fold's `covers`: those of its members known on `known` at warn or above, and 1
// at least.
[[nodiscard]] std::uint32_t covers_of(const std::vector<const Violation*>& members,
                                      std::optional<Date> known);

// The versions of a period that carry one condition, as one report (DE-12). Each
// group is the report of its strongest version (the highest p_error, the first on
// a tie), placed on the first version that carries it, in the order they became
// known: that version's date, line and period, so the record_key its own report
// has, which a later version repeating the condition does not change. It has the
// newest knowledge date of them, and where it folds more than one version,
// detail.version_nos ("1,2") and a message that says they carry "the same <what>".
[[nodiscard]] std::vector<Violation> fold_versions(const Series& s, std::vector<RowReport> reports,
                                                   std::string_view what);

}  // namespace dorq
