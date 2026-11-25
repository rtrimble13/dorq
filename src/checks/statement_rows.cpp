#include "checks/statement_rows.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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

std::string and_list(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t k = 0; k < parts.size(); ++k) {
    if (k > 0) {
      out += k + 1 == parts.size() ? " and " : ", ";
    }
    out += parts[k];
  }
  return out;
}

std::uint32_t covers_of(const std::vector<const Violation*>& members, std::optional<Date> known) {
  std::uint32_t covers = 0;
  for (const Violation* m : members) {
    if (m->known == known && m->severity >= Severity::kWarn) {
      covers += m->covers;
    }
  }
  return std::max<std::uint32_t>(covers, 1);
}

std::vector<Violation> fold_versions(const Series& s, std::vector<RowReport> reports,
                                     std::string_view what) {
  const StatementRows& st = s.statements;
  // By period, then in the order its versions became known.
  std::vector<std::size_t> order(reports.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  const auto place = [&](std::size_t k) {
    const std::size_t r = reports[k].row;
    return std::tuple{s.date[r], st.statement[r], st.period_type[r], st.known_from[r],
                      st.version[r]};
  };
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return place(a) < place(b); });
  const auto same_period = [&](std::size_t a, std::size_t b) {
    const std::size_t x = reports[a].row;
    const std::size_t y = reports[b].row;
    return s.date[x] == s.date[y] && st.statement[x] == st.statement[y] &&
           st.period_type[x] == st.period_type[y];
  };
  // Each group's members, its first the version it is placed on.
  std::vector<std::vector<std::size_t>> groups;
  std::size_t period_begin = 0;  // the first group of the current period
  for (const std::size_t k : order) {
    if (period_begin < groups.size() && !same_period(groups[period_begin].front(), k)) {
      period_begin = groups.size();
    }
    const RowReport& r = reports[k];
    const auto joins = [&](const std::vector<std::size_t>& g) {
      const RowReport& first = reports[g.front()];
      return first.condition == r.condition &&
             std::fabs(first.amount - r.amount) <= std::max(first.tolerance, r.tolerance);
    };
    const auto it = std::find_if(groups.begin() + static_cast<std::ptrdiff_t>(period_begin),
                                 groups.end(), joins);
    if (it == groups.end()) {
      groups.push_back({k});
    } else {
      it->push_back(k);
    }
  }
  std::vector<Violation> out;
  out.reserve(groups.size());
  for (const std::vector<std::size_t>& g : groups) {
    std::size_t best = g.front();
    std::optional<Date> known;
    std::vector<const Violation*> members;
    std::vector<std::int32_t> versions;
    for (const std::size_t k : g) {
      const RowReport& r = reports[k];
      if (r.v.p_error > reports[best].v.p_error) {
        best = k;
      }
      if (r.v.known && (!known || *known < *r.v.known)) {
        known = r.v.known;
      }
      members.push_back(&r.v);
      if (std::find(versions.begin(), versions.end(), st.version[r.row]) == versions.end()) {
        versions.push_back(st.version[r.row]);
      }
    }
    const std::uint32_t covers = g.size() > 1 ? covers_of(members, known) : reports[best].v.covers;
    const RowReport& first = reports[g.front()];
    const std::optional<Date> date = first.v.date;
    const std::uint32_t line = first.v.line;
    const std::optional<StatementPeriod> period = first.v.period;
    Violation v = std::move(reports[best].v);
    v.date = date;
    v.line = line;
    v.period = period;
    v.known = known;
    v.covers = covers;
    if (versions.size() > 1) {
      std::string nos;
      std::vector<std::string> names;
      for (const std::int32_t n : versions) {
        nos += (nos.empty() ? "" : ",") + std::to_string(n);
        names.push_back(version_text(n));
      }
      v.detail.push_back({"version_nos", nos});
      v.message += "; " + and_list(names) + " carry the same " + std::string{what};
    }
    out.push_back(std::move(v));
  }
  return out;
}

}  // namespace dorq
