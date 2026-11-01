#include "dorq/statement.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/text.hpp"

namespace dorq {
namespace {

// Lower case, without spaces, '_', '-' or '.': "core.income_statement" is
// "coreincomestatement".
std::string squash(std::string_view text) {
  std::string out;
  for (const char ch : trim(text)) {
    if (ch != ' ' && ch != '_' && ch != '-' && ch != '.') {
      out.push_back(ascii_lower(ch));
    }
  }
  return out;
}

}  // namespace

std::string_view to_string(Statement statement) noexcept {
  switch (statement) {
    case Statement::kIncome:
      return "income";
    case Statement::kBalanceSheet:
      return "balance_sheet";
    case Statement::kCashFlow:
      return "cash_flow";
  }
  return "income";
}

std::optional<Statement> parse_statement(std::string_view text) {
  const std::string s = squash(text);
  if (s == "income" || s == "incomestatement" || s == "coreincomestatement") {
    return Statement::kIncome;
  }
  if (s == "balancesheet" || s == "corebalancesheet") {
    return Statement::kBalanceSheet;
  }
  if (s == "cashflow" || s == "cashflowstatement" || s == "corecashflowstatement") {
    return Statement::kCashFlow;
  }
  return std::nullopt;
}

std::string_view to_string(PeriodType type) noexcept {
  return type == PeriodType::kAnnual ? "annual" : "quarter";
}

std::optional<PeriodType> parse_period_type(std::string_view text) {
  const std::string s = squash(text);
  if (s == "quarter" || s == "quarterly") {
    return PeriodType::kQuarter;
  }
  if (s == "annual" || s == "year" || s == "yearly") {
    return PeriodType::kAnnual;
  }
  return std::nullopt;
}

std::string_view to_string(FiscalPeriod period) noexcept {
  switch (period) {
    case FiscalPeriod::kUnknown:
      return "";
    case FiscalPeriod::kQ1:
      return "Q1";
    case FiscalPeriod::kQ2:
      return "Q2";
    case FiscalPeriod::kQ3:
      return "Q3";
    case FiscalPeriod::kQ4:
      return "Q4";
    case FiscalPeriod::kFY:
      return "FY";
  }
  return "";
}

std::optional<FiscalPeriod> parse_fiscal_period(std::string_view text) {
  const std::string s = squash(text);
  if (s == "fy") {
    return FiscalPeriod::kFY;
  }
  if (s.size() == 2 && s[0] == 'q' && s[1] >= '1' && s[1] <= '4') {
    return static_cast<FiscalPeriod>(static_cast<int>(FiscalPeriod::kQ1) + (s[1] - '1'));
  }
  return std::nullopt;
}

std::string_view to_string(KnowledgeSource source) noexcept {
  switch (source) {
    case KnowledgeSource::kUnknown:
      return "";
    case KnowledgeSource::kFiled:
      return "filed";
    case KnowledgeSource::kObserved:
      return "observed";
    case KnowledgeSource::kEstimated:
      return "estimated";
  }
  return "";
}

std::optional<KnowledgeSource> parse_knowledge_source(std::string_view text) {
  const std::string s = squash(text);
  if (s == "filed") {
    return KnowledgeSource::kFiled;
  }
  if (s == "observed") {
    return KnowledgeSource::kObserved;
  }
  if (s == "estimated") {
    return KnowledgeSource::kEstimated;
  }
  return std::nullopt;
}

std::string_view to_string(ItemClass item_class) noexcept {
  switch (item_class) {
    case ItemClass::kFlow:
      return "flow";
    case ItemClass::kStock:
      return "stock";
    case ItemClass::kShareCount:
      return "share_count";
    case ItemClass::kPerShare:
      return "per_share";
    case ItemClass::kMemo:
      return "memo";
  }
  return "flow";
}

std::optional<ItemClass> parse_item_class(std::string_view text) {
  const std::string s = squash(text);
  if (s == "flow") {
    return ItemClass::kFlow;
  }
  if (s == "stock") {
    return ItemClass::kStock;
  }
  if (s == "sharecount") {
    return ItemClass::kShareCount;
  }
  if (s == "pershare") {
    return ItemClass::kPerShare;
  }
  if (s == "memo") {
    return ItemClass::kMemo;
  }
  return std::nullopt;
}

std::string_view to_string(SignConvention sign) noexcept {
  switch (sign) {
    case SignConvention::kPositiveIsInflow:
      return "positive_is_inflow";
    case SignConvention::kPositiveIsOutflow:
      return "positive_is_outflow";
    case SignConvention::kSigned:
      return "signed";
  }
  return "signed";
}

std::optional<SignConvention> parse_sign_convention(std::string_view text) {
  const std::string s = squash(text);
  if (s == "positiveisinflow") {
    return SignConvention::kPositiveIsInflow;
  }
  if (s == "positiveisoutflow") {
    return SignConvention::kPositiveIsOutflow;
  }
  if (s == "signed") {
    return SignConvention::kSigned;
  }
  return std::nullopt;
}

std::string StatementPeriod::label() const {
  std::string out;
  if (fiscal_period == FiscalPeriod::kUnknown) {
    out = to_string(period_type);
  } else if (fiscal_period != FiscalPeriod::kFY) {
    out = to_string(fiscal_period);
  }
  if (fiscal_year > 0) {
    out += out.empty() ? "FY" : " FY";
    out += std::to_string(fiscal_year);
  } else if (fiscal_period == FiscalPeriod::kFY) {
    out = "FY";
  }
  return out;
}

const std::vector<double>* StatementRows::item(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < item_names.size(); ++i) {
    if (item_names[i] == name) {
      return &items[i];
    }
  }
  return nullptr;
}

}  // namespace dorq
