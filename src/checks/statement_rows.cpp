#include "checks/statement_rows.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/number.hpp"

namespace dorq {

StatementPeriod period_of(const Series& s, std::size_t i, bool with_version) {
  const StatementRows& st = s.statements;
  StatementPeriod period;
  period.statement = st.statement[i];
  period.period_type = st.period_type[i];
  period.fiscal_year = st.fiscal_year[i];
  period.fiscal_period = st.fiscal_period[i];
  if (with_version) {
    period.version = st.version[i];
  }
  return period;
}

Date known_of(const Series& s, std::size_t i) {
  const StatementRows& st = s.statements;
  Date known = st.known_from[i];
  if (const std::optional<Date>& loaded = st.loaded_on[i]) {
    known = std::max(known, *loaded);
  }
  return known;
}

Violation statement_finding(const CheckInfo& info, const Series& s, std::size_t i,
                            bool with_version) {
  Violation v;
  v.check = &info;
  v.severity = Severity::kError;
  v.classification = Classification::kDataError;
  v.p_error = 1.0;
  v.date = s.date[i];
  v.line = s.line[i];
  v.period = period_of(s, i, with_version);
  v.known = known_of(s, i);
  return v;
}

std::string version_text(std::int32_t version) { return "v" + std::to_string(version); }

std::string amount_text(double value) {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(static_cast<std::int64_t>(value));
  }
  return format_number(value);
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) {
      out += separator;
    }
    out += part;
  }
  return out;
}

}  // namespace dorq
