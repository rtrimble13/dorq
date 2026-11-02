// DQ941 to DQ944: the period sequence, coverage and freshness of a statements
// series (fundamentals plan §4.5; doc/checks/DQ941.md to DQ944.md).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "context/context.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"

namespace dorq {

const CheckInfo& StatementsMissing::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ943",
      .name = "statements-missing",
      .summary = "a security the metadata expects statements for, with none in the input "
                 "(--meta's statements_expected)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void StatementsMissing::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const SeriesMeta* meta = context.meta;
  if (s.kind != SeriesKind::kStatements || s.size() != 0 || meta == nullptr ||
      meta->statements_expected != true) {
    return;
  }
  // From its first trade, or the coverage floor, whichever is later, to its last.
  Date from = context.statement_settings != nullptr ? context.statement_settings->coverage_floor
                                                    : Date::from_ymd(1998, 1, 1);
  if (meta->first_trade) {
    from = std::max(from, *meta->first_trade);
  }
  if (meta->last_trade && *meta->last_trade < from) {
    return;  // it stopped trading before statements were expected
  }
  Violation v;
  v.check = &info();
  v.severity = Severity::kWarn;
  v.classification = Classification::kDataError;
  v.p_error = 1.0;
  v.date = from;
  v.end_date = meta->last_trade;
  v.message = "the metadata expects statements for this security, and the input has none";
  if (meta->first_trade || meta->last_trade) {
    v.message += ": it traded from " + from.to_string() +
                 (meta->last_trade ? " to " + meta->last_trade->to_string() : "");
  }
  v.detail.push_back({"from", from.to_string()});
  if (meta->last_trade) {
    v.detail.push_back({"to", meta->last_trade->to_string()});
  }
  SuggestedAction action{"load_statements", {}, {}};
  action.text = "load the vendor's statements for this security, or mark it as not expected";
  v.suggested_action = std::move(action);
  out.push_back(std::move(v));
}

const CheckInfo& ReportingCurrency::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ944",
      .name = "reporting-currency",
      .summary = "info: statements reported in a currency other than the one the security trades "
                 "in (--meta's currency)",
      .default_severity = Severity::kInfo,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void ReportingCurrency::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const SeriesMeta* meta = context.meta;
  if (s.kind != SeriesKind::kStatements || meta == nullptr || meta->currency.empty()) {
    return;
  }
  const StatementRows& st = s.statements;
  // Each reporting currency other than the trading one: its first and last periods.
  struct Span {
    std::string currency;
    std::size_t first = 0;
    std::size_t last = 0;
  };
  std::vector<Span> spans;
  for (std::size_t r = 0; r < st.size(); ++r) {
    std::string currency = st.currency[r];
    std::transform(currency.begin(), currency.end(), currency.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (currency.empty() || currency == meta->currency) {
      continue;
    }
    const auto it = std::find_if(spans.begin(), spans.end(),
                                 [&](const Span& span) { return span.currency == currency; });
    if (it == spans.end()) {
      spans.push_back({currency, r, r});
    } else {
      if (s.date[r] < s.date[it->first]) {
        it->first = r;
      }
      if (s.date[r] > s.date[it->last]) {
        it->last = r;
      }
    }
  }
  for (const Span& span : spans) {
    Violation v = statement_finding(info(), s, span.first, false);
    v.severity = Severity::kInfo;
    v.classification = Classification::kMarketFact;
    v.p_error = 0.0;
    if (s.date[span.last] != s.date[span.first]) {
      v.end_date = s.date[span.last];
    }
    v.message = "the statements are reported in " + span.currency + ", and the security trades in " +
                meta->currency + ": a per-share figure or a market cap needs the exchange rate";
    v.key.push_back({"currency", span.currency});
    v.detail.push_back({"reported_currency", span.currency});
    v.detail.push_back({"trading_currency", meta->currency});
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
