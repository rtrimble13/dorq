#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/date.hpp"

namespace dorq {

// Financial statements, the third series kind (doc/statements.md, ADR 0005). A
// row is one version of one statement of one period.

enum class Statement : std::uint8_t { kIncome, kBalanceSheet, kCashFlow };
enum class PeriodType : std::uint8_t { kQuarter, kAnnual };
enum class FiscalPeriod : std::uint8_t { kUnknown, kQ1, kQ2, kQ3, kQ4, kFY };
// How a version's known_from was set (fafnir's ADR 0018): the filing date, the day
// a restatement was first loaded, or the issuer's next filing.
enum class KnowledgeSource : std::uint8_t { kUnknown, kFiled, kObserved, kEstimated };

// What a line item is, from --items (fafnir's ref.statement_line_item). A flow
// sums over a period's quarters; a stock and a share count are read at its end.
enum class ItemClass : std::uint8_t { kFlow, kStock, kShareCount, kPerShare, kMemo };
// Whether a positive value is money coming in (revenue, a cash inflow), going out
// (an expense, written positive), or either (a subtotal, a balance).
enum class SignConvention : std::uint8_t { kPositiveIsInflow, kPositiveIsOutflow, kSigned };

// "income", "balance_sheet", "cash_flow"; the parsers also take fafnir's table
// names ("core.income_statement"), ignoring case, spaces, '_', '-' and '.'.
[[nodiscard]] std::string_view to_string(Statement statement) noexcept;
[[nodiscard]] std::optional<Statement> parse_statement(std::string_view text);
// "quarter", "annual".
[[nodiscard]] std::string_view to_string(PeriodType type) noexcept;
[[nodiscard]] std::optional<PeriodType> parse_period_type(std::string_view text);
// "Q1".."Q4", "FY"; "" for unknown.
[[nodiscard]] std::string_view to_string(FiscalPeriod period) noexcept;
[[nodiscard]] std::optional<FiscalPeriod> parse_fiscal_period(std::string_view text);
// "filed", "observed", "estimated"; "" for unknown.
[[nodiscard]] std::string_view to_string(KnowledgeSource source) noexcept;
[[nodiscard]] std::optional<KnowledgeSource> parse_knowledge_source(std::string_view text);
// "flow", "stock", "share_count", "per_share", "memo".
[[nodiscard]] std::string_view to_string(ItemClass item_class) noexcept;
[[nodiscard]] std::optional<ItemClass> parse_item_class(std::string_view text);
// "positive_is_inflow", "positive_is_outflow", "signed".
[[nodiscard]] std::string_view to_string(SignConvention sign) noexcept;
[[nodiscard]] std::optional<SignConvention> parse_sign_convention(std::string_view text);

// Where a statement finding sits (DR-0902): one statement of one period, and the
// version when the finding is about one. Its fiscal date is the finding's date.
struct StatementPeriod {
  Statement statement = Statement::kIncome;
  PeriodType period_type = PeriodType::kQuarter;
  std::int32_t fiscal_year = 0;  // 0 when unknown
  FiscalPeriod fiscal_period = FiscalPeriod::kUnknown;
  std::optional<std::int32_t> version;  // nullopt: about the period, not one version

  // For people: "Q3 FY2020", "FY2020", or the period type where the labels are
  // unknown ("quarter").
  [[nodiscard]] std::string label() const;
  bool operator==(const StatementPeriod&) const = default;
};

// A statements series' rows, as columns beside Series::date (the fiscal date: the
// period's end), Series::close (the raw close on or before known_from, when the
// input has one) and Series::line. Sorted with the series by fiscal date, then
// statement, period type and version.
struct StatementRows {
  std::vector<Statement> statement;
  std::vector<PeriodType> period_type;
  std::vector<std::int32_t> fiscal_year;  // 0 when unknown
  std::vector<FiscalPeriod> fiscal_period;
  std::vector<std::int32_t> version;  // 1 is as first reported
  // The knowledge interval, half-open: from known_from up to, not including,
  // known_to. No known_to: the version is in force.
  std::vector<Date> known_from;
  std::vector<std::optional<Date>> known_to;
  std::vector<KnowledgeSource> knowledge_source;
  std::vector<std::optional<Date>> filing_date;
  std::vector<std::optional<Date>> loaded_on;  // the day the row was loaded
  std::vector<std::string> currency;           // the reporting currency, or empty
  // The line items, each a column of values by row. NaN is NULL ("does not
  // apply"), which is not 0.
  std::vector<std::string> item_names;
  std::vector<std::vector<double>> items;

  [[nodiscard]] std::size_t size() const noexcept { return statement.size(); }
  // An item's column by name, or nullptr when the series has no such item.
  [[nodiscard]] const std::vector<double>* item(std::string_view name) const noexcept;
};

}  // namespace dorq
