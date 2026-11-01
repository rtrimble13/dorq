#include "checks/statements.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "dorq/date.hpp"
#include "dorq/number.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"

namespace dorq {
namespace {

// A bound fails beyond rounding when the lesser item is above the greater by more
// than this share of the greater (doc/checks/DQ902.md).
constexpr double kBoundSlack = 0.001;

// A row's period, with its version or (a finding on the whole period) without.
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

// The newer of a row's known_from and loaded_on: what --since judges it by.
Date known_of(const Series& s, std::size_t i) {
  const StatementRows& st = s.statements;
  Date known = st.known_from[i];
  if (const std::optional<Date>& loaded = st.loaded_on[i]) {
    known = std::max(known, *loaded);
  }
  return known;
}

Violation finding(const CheckInfo& info, const Series& s, std::size_t i, bool with_version) {
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

// An amount as a filing writes it: a whole number where it is one.
std::string amount_text(double value) {
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    return std::to_string(static_cast<std::int64_t>(value));
  }
  return format_number(value);
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const std::string& part : parts) {
    out += (out.empty() ? "" : std::string{separator}) + part;
  }
  return out;
}

// The rows of a series by period, then version: indexes into its rows. The
// engine sorts a series this way; the check does not rely on it.
std::vector<std::size_t> by_period(const Series& s) {
  const StatementRows& st = s.statements;
  std::vector<std::size_t> order(st.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return std::tuple{s.date[a], st.statement[a], st.period_type[a], st.version[a]} <
           std::tuple{s.date[b], st.statement[b], st.period_type[b], st.version[b]};
  });
  return order;
}

bool same_period(const Series& s, std::size_t a, std::size_t b) {
  const StatementRows& st = s.statements;
  return s.date[a] == s.date[b] && st.statement[a] == st.statement[b] &&
         st.period_type[a] == st.period_type[b];
}

// A broken link of a chain: "v1 is known to 2010-09-01 and v2 from 2010-10-01: a
// gap".
std::string link_text(const std::string& name, Date to, const std::string& next, Date next_from) {
  std::string text = name;
  text += " is known to ";
  text += to.to_string();
  text += " and ";
  text += next;
  text += " from ";
  text += next_from.to_string();
  text += to < next_from ? ": a gap" : ": an overlap";
  return text;
}

// One version's rows: what is wrong with each, and that there is more than one.
void judge_version(const CheckInfo& info, const Series& s, std::span<const std::size_t> rows,
                   std::vector<Violation>& out) {
  const StatementRows& st = s.statements;
  std::vector<std::string> problems;
  const auto add = [&problems](std::string problem) {
    if (std::find(problems.begin(), problems.end(), problem) == problems.end()) {
      problems.push_back(std::move(problem));
    }
  };
  std::string lines;
  Date known = known_of(s, rows.front());
  for (const std::size_t r : rows) {
    lines += (lines.empty() ? "" : ", ") + std::to_string(s.line[r]);
    known = std::max(known, known_of(s, r));
    const Date from = st.known_from[r];
    if (from < s.date[r]) {
      add("known from " + from.to_string() + ", before the period ends");
    }
    if (st.knowledge_source[r] == KnowledgeSource::kFiled) {
      const std::optional<Date>& filed = st.filing_date[r];
      if (!filed) {
        add("filed, but with no filing date");
      } else if (*filed != from) {
        add("filed on " + filed->to_string() + ", but known from " + from.to_string());
      }
    }
  }
  if (rows.size() > 1) {
    problems.insert(problems.begin(), std::to_string(rows.size()) + " rows (lines " + lines + ")");
  }
  if (problems.empty()) {
    return;
  }
  Violation v = finding(info, s, rows.front(), true);
  v.known = known;
  v.message = join(problems, "; ");
  v.detail.push_back({"rows", static_cast<std::int64_t>(rows.size())});
  v.detail.push_back({"known_from", st.known_from[rows.front()].to_string()});
  if (const std::optional<Date>& filed = st.filing_date[rows.front()]) {
    v.detail.push_back({"filing_date", filed->to_string()});
  }
  v.detail.push_back(
      {"knowledge_source", std::string{to_string(st.knowledge_source[rows.front()])}});
  out.push_back(std::move(v));
}

// A period's versions, in the order they became known: each known_to must be the
// next version's known_from, and only the last may be open (ADR 0005; fafnir's
// ADR 0018).
void judge_chain(const CheckInfo& info, const Series& s, std::vector<std::size_t> versions,
                 std::vector<Violation>& out) {
  const StatementRows& st = s.statements;
  std::stable_sort(versions.begin(), versions.end(), [&st](std::size_t a, std::size_t b) {
    return std::tuple{st.known_from[a], st.version[a]} <
           std::tuple{st.known_from[b], st.version[b]};
  });
  std::vector<std::string> breaks;
  std::vector<std::string> fixes;
  SuggestedAction action;
  action.kind = "set_known_to";
  std::string listing;
  Date known = known_of(s, versions.front());
  for (std::size_t k = 0; k < versions.size(); ++k) {
    const std::size_t r = versions[k];
    const std::string name = version_text(st.version[r]);
    const std::optional<Date>& to = st.known_to[r];
    known = std::max(known, known_of(s, r));
    listing += (listing.empty() ? "" : ", ") + name + " " + st.known_from[r].to_string() + ".." +
               (to ? to->to_string() : "open");
    if (k + 1 == versions.size()) {
      if (to) {
        breaks.push_back("the last, " + name + ", is closed at " + to->to_string() +
                         ": none is in force");
        fixes.push_back("reopen " + name);
        action.fields.push_back({name, std::string{}});
      }
      continue;
    }
    const std::string next = version_text(st.version[versions[k + 1]]);
    const Date next_from = st.known_from[versions[k + 1]];
    if (!to) {
      std::string text = name;
      text += " is open, but ";
      text += next;
      text += " is known from ";
      text += next_from.to_string();
      breaks.push_back(std::move(text));
    } else if (*to != next_from) {
      breaks.push_back(link_text(name, *to, next, next_from));
    } else {
      continue;
    }
    fixes.push_back("close " + name + " at " + next_from.to_string());
    action.fields.push_back({name, next_from.to_string()});
  }
  if (breaks.empty()) {
    return;
  }
  Violation v = finding(info, s, versions.front(), false);
  v.known = known;
  v.message = "versions do not chain: " + join(breaks, "; ");
  v.detail.push_back({"versions", listing});
  action.text = join(fixes, "; ");
  v.suggested_action = std::move(action);
  out.push_back(std::move(v));
}

}  // namespace

const CheckInfo& StatementKey::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ901",
      .name = "statement-key",
      .summary =
          "two rows for one version of a statement, versions that do not chain, or a "
          "knowledge date before the period's end or apart from its filing date",
      .default_severity = Severity::kError,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void StatementKey::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  if (s.kind != SeriesKind::kStatements || s.statements.size() == 0) {
    return;
  }
  const StatementRows& st = s.statements;
  const std::vector<std::size_t> order = by_period(s);
  std::size_t begin = 0;
  while (begin < order.size()) {
    std::size_t end = begin + 1;
    while (end < order.size() && same_period(s, order[begin], order[end])) {
      ++end;
    }
    // Each version's rows, and the first row of each for the chain.
    std::vector<std::size_t> versions;
    for (std::size_t a = begin; a < end;) {
      std::size_t b = a + 1;
      while (b < end && st.version[order[b]] == st.version[order[a]]) {
        ++b;
      }
      judge_version(info(), s, std::span<const std::size_t>(order).subspan(a, b - a), out);
      versions.push_back(order[a]);
      a = b;
    }
    judge_chain(info(), s, std::move(versions), out);
    begin = end;
  }
}

const CheckInfo& ImpossibleValue::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ902",
      .name = "impossible-value",
      .summary =
          "a negative value of an item that cannot be negative, or an item above one it "
          "is at most ([statements] nonnegative and bounds)",
      .default_severity = Severity::kError,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void ImpossibleValue::run(const SeriesContext& context, std::vector<Violation>& out) const {
  const Series& s = context.series;
  const StatementSettings* settings = context.statement_settings;
  if (s.kind != SeriesKind::kStatements || settings == nullptr) {
    return;
  }
  const StatementRows& st = s.statements;
  // The series' columns of the items the settings name; an item it lacks is not
  // judged.
  std::vector<std::pair<const std::string*, const std::vector<double>*>> nonnegative;
  for (const std::string& item : settings->nonnegative) {
    if (const std::vector<double>* column = st.item(item)) {
      nonnegative.emplace_back(&item, column);
    }
  }
  struct Bound {
    const ItemBound* bound;
    const std::vector<double>* lesser;
    const std::vector<double>* greater;
  };
  std::vector<Bound> bounds;
  for (const ItemBound& bound : settings->bounds) {
    const std::vector<double>* lesser = st.item(bound.lesser);
    const std::vector<double>* greater = st.item(bound.greater);
    if (lesser != nullptr && greater != nullptr) {
      bounds.push_back({&bound, lesser, greater});
    }
  }
  for (std::size_t i = 0; i < st.size(); ++i) {
    for (const auto& [item, column] : nonnegative) {
      const double x = (*column)[i];
      if (std::isnan(x) || x >= 0.0) {
        continue;
      }
      Violation v = finding(info(), s, i, true);
      v.message = *item + " is " + amount_text(x) + ", and cannot be negative";
      v.detail.push_back({"item", *item});
      v.detail.push_back({"value", x});
      v.key.push_back({"item", *item});
      out.push_back(std::move(v));
    }
    for (const Bound& b : bounds) {
      const double lesser = (*b.lesser)[i];
      const double greater = (*b.greater)[i];
      if (std::isnan(lesser) || std::isnan(greater) ||
          lesser - greater <= kBoundSlack * std::fabs(greater)) {
        continue;
      }
      Violation v = finding(info(), s, i, true);
      v.message = b.bound->lesser + " (" + amount_text(lesser) + ") is above " + b.bound->greater +
                  " (" + amount_text(greater) + ")";
      v.detail.push_back({"item", b.bound->lesser});
      v.detail.push_back({"value", lesser});
      v.detail.push_back({"bound", b.bound->greater});
      v.detail.push_back({"bound_value", greater});
      v.key.push_back({"item", b.bound->lesser});
      v.key.push_back({"bound", b.bound->greater});
      out.push_back(std::move(v));
    }
  }
}

}  // namespace dorq
