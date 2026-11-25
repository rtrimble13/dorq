// DQ921 share-terms-step and DQ922 share-count-mismatch: the share terms model's
// findings (statements/shares.cpp; doc/checks/DQ921.md, DQ922.md).

#include "statements/shares.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "checks/statement_rows.hpp"
#include "checks/statements.hpp"
#include "dorq/series.hpp"

namespace dorq {
namespace {

std::string power_text(int power) {
  if (power >= 0) {
    return "x1" + std::string(static_cast<std::size_t>(power), '0');
  }
  return "x0." + std::string(static_cast<std::size_t>(-power - 1), '0') + "1";
}

std::string factor_text(const ShareFinding& f) {
  if (f.ratio && f.power == 0) {
    return f.ratio->to_string();
  }
  if (f.power != 0) {
    return power_text(f.power);
  }
  return amount_text(std::round(f.factor * 1000.0) / 1000.0);
}

std::string views_text(const ShareFinding& f) {
  if (f.in_force && f.first_reported) {
    return "both";
  }
  return f.in_force ? "in_force" : "first_reported";
}

Violation step_violation(const CheckInfo& info, const SeriesContext& context, const ShareFinding& f,
                         Severity severity) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  const std::string& item = st.item_names[f.item];
  Violation v = statement_finding(info, s, f.row, true);
  v.severity = severity;
  v.p_error = f.p_not_real();
  v.provisional = f.provisional;
  const std::string step = factor_text(f);
  const std::string from = s.date[f.before].to_string();
  const std::string to = s.date[f.row].to_string();
  switch (f.explanation()) {
    case ShareHypothesis::kUnreportedSplit: {
      v.classification = Classification::kContextGap;
      const std::string split = f.split ? f.split->to_string() : step;
      v.message = item + " steps by " + step + " from " + from + " to " + to +
                  ", with no split on file: a " + split +
                  " split missing from the actions, its ex-date after " +
                  f.window_from.to_string() + " and on or before " + f.window_to.to_string();
      if (f.explains > 0) {
        v.message += "; it explains " + std::to_string(f.explains) + " other step" +
                     (f.explains == 1 ? "" : "s") + " in the counts";
      }
      SuggestedAction action{"add_split", {}, {}};
      action.fields.push_back({"ratio", split});
      action.fields.push_back({"ex_date_after", f.window_from.to_string()});
      action.fields.push_back({"ex_date_on_or_before", f.window_to.to_string()});
      action.text = "add a " + split + " split, its ex-date after " + f.window_from.to_string() +
                    " and on or before " + f.window_to.to_string() + ", to the actions";
      v.suggested_action = std::move(action);
      break;
    }
    case ShareHypothesis::kTermsError:
    case ShareHypothesis::kUnitError: {
      v.classification = Classification::kDataError;
      const bool terms = f.explanation() == ShareHypothesis::kTermsError;
      v.message = item + " steps by " + step + " from " + from + " to " + to +
                  (terms ? ", against the split on file (" + f.split_on_file +
                               "): a count stored in the wrong share terms"
                         : ": a count stored at a power of ten");
      if (f.last_row != f.row) {
        v.end_date = s.date[f.last_row];
        v.message += ", to " + s.date[f.last_row].to_string();
      }
      SuggestedAction action{"check_share_terms", {}, {}};
      action.fields.push_back({"item", item});
      action.fields.push_back({"fiscal_date", to});
      if (!f.split_on_file.empty()) {
        action.fields.push_back({"split", f.split_on_file});
      }
      action.text = "check " + item + " for " + to + " against its filing's share terms";
      v.suggested_action = std::move(action);
      break;
    }
    case ShareHypothesis::kRealIssuance:
      break;
  }
  v.key.push_back({"item", item});
  v.detail.push_back({"item", item});
  v.detail.push_back({"step", f.factor});
  v.detail.push_back({"ratio", step});
  v.detail.push_back({"previous_fiscal_date", from});
  v.detail.push_back({"window_from", f.window_from.to_string()});
  v.detail.push_back({"window_to", f.window_to.to_string()});
  if (f.cap_step) {
    v.detail.push_back({"market_cap_step", *f.cap_step});
  }
  v.detail.push_back({"views", views_text(f)});
  for (std::size_t h = 0; h < f.posterior.size(); ++h) {
    v.hypotheses.push_back({kShareHypothesisNames.at(h), f.posterior.at(h)});
  }
  v.evidence = f.evidence;
  return v;
}

Violation mismatch_violation(const CheckInfo& info, const SeriesContext& context,
                             const ShareFinding& f, Severity severity) {
  const Series& s = context.series;
  const StatementRows& st = s.statements;
  const std::string& item = st.item_names[f.item];
  const std::string& other = st.item_names[f.other];
  Violation v = statement_finding(info, s, f.row, true);
  v.severity = severity;
  v.p_error = f.p_not_real();
  const std::string by = factor_text(f);
  v.message = item + " (" + amount_text(f.value) + ") and " + other + " (" +
              amount_text(f.other_value) + ") disagree by " + by +
              ", which no split on file explains";
  v.key.push_back({"item", item});
  v.detail.push_back({"item", item});
  v.detail.push_back({"value", f.value});
  v.detail.push_back({"other", other});
  v.detail.push_back({"other_value", f.other_value});
  v.detail.push_back({"ratio", by});
  v.detail.push_back({"views", views_text(f)});
  v.hypotheses = {
      {"real", f.posterior[0]}, {"mismatch", f.posterior[1]}, {"unreported_split", f.posterior[2]}};
  v.evidence = f.evidence;
  SuggestedAction action{"check_share_terms", {}, {}};
  action.fields.push_back({"item", item});
  action.fields.push_back({"fiscal_date", s.date[f.row].to_string()});
  action.text = "check " + item + " and " + other + " for " + s.date[f.row].to_string() +
                " against the filing";
  v.suggested_action = std::move(action);
  return v;
}

// Each finding of `kind` reported, with its row.
std::vector<RowReport> report(const CheckInfo& info, const SeriesContext& context,
                              ShareFinding::Kind kind) {
  std::vector<RowReport> out;
  if (context.series.kind != SeriesKind::kStatements || context.shares == nullptr) {
    return out;
  }
  for (const ShareFinding& f : context.shares->findings) {
    if (f.kind != kind) {
      continue;
    }
    if (kind == ShareFinding::Kind::kStep && f.explanation() == ShareHypothesis::kRealIssuance) {
      continue;
    }
    const double p_error = context.statement_config().calibration.apply(info.code, f.p_not_real());
    std::optional<Severity> severity = context.thresholds.for_probability(p_error);
    if (!severity) {
      continue;
    }
    if (f.provisional && *severity == Severity::kError) {
      severity = Severity::kWarn;
    }
    Violation v = kind == ShareFinding::Kind::kStep
                      ? step_violation(info, context, f, *severity)
                      : mismatch_violation(info, context, f, *severity);
    v.p_error = p_error;
    // A mismatch's condition is its pair of counts, exactly.
    const StatementRows& st = context.series.statements;
    std::string pair = st.item_names[f.item] + "=" + amount_text(f.value);
    if (kind == ShareFinding::Kind::kMismatch) {
      pair += "/" + st.item_names[f.other] + "=" + amount_text(f.other_value);
    }
    out.push_back({std::move(v), f.row, std::move(pair), 0.0, 0.0});
  }
  return out;
}

// A text field of a report's detail, or empty.
std::string text_of(const Violation& v, std::string_view key) {
  for (const DetailField& d : v.detail) {
    if (const auto* text = std::get_if<std::string>(&d.value); text != nullptr && d.key == key) {
      return *text;
    }
  }
  return {};
}

// DQ922 once per period (DE-12): a period's weighted-average counts, basic and
// diluted, against its outstanding counts, are one condition. One report on the
// income statement's version, keyed with no item: the strongest pair's model, the
// others in its message and detail.items. A later version that repeats the counts
// folds into the first.
std::vector<Violation> fold_counts(const Series& s, std::vector<RowReport> found) {
  std::map<std::size_t, std::vector<std::size_t>> by_row;
  for (std::size_t k = 0; k < found.size(); ++k) {
    by_row[found[k].row].push_back(k);
  }
  std::vector<RowReport> reports;
  reports.reserve(by_row.size());
  for (const auto& [row, members] : by_row) {
    std::size_t best = members.front();
    std::vector<const Violation*> folded;
    std::vector<std::string> pairs;
    for (const std::size_t k : members) {
      if (found[k].v.p_error > found[best].v.p_error) {
        best = k;
      }
      folded.push_back(&found[k].v);
      pairs.push_back(found[k].condition);
    }
    std::vector<std::string> items;
    std::vector<std::string> others;
    for (const std::size_t k : members) {
      const Violation& v = found[k].v;
      const std::string item = text_of(v, "item");
      if (std::find(items.begin(), items.end(), item) == items.end()) {
        items.push_back(item);
      }
      if (k != best) {
        others.push_back(item + " and " + text_of(v, "other") + " by " + text_of(v, "ratio"));
      }
    }
    std::sort(pairs.begin(), pairs.end());
    const std::uint32_t covers = covers_of(folded, found[best].v.known);
    Violation v = std::move(found[best].v);
    v.covers = covers;
    std::erase_if(v.key, [](const DetailField& d) { return d.key == "item"; });
    if (members.size() > 1) {
      v.message += "; so do " + and_list(others);
      v.detail.push_back({"items", join(items, ",")});
      v.detail.push_back({"pairs", static_cast<std::int64_t>(members.size())});
    }
    reports.push_back({std::move(v), row, join(pairs, ";"), 0.0, 0.0});
  }
  return fold_versions(s, std::move(reports), "counts");
}

}  // namespace

const CheckInfo& ShareTermsStep::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ921",
      .name = "share-terms-step",
      .summary =
          "share counts, put into one share terms with the splits on file, that step by "
          "a clean split ratio: a split missing from the actions, or a count stored in "
          "the wrong terms (--actions, --items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void ShareTermsStep::run(const SeriesContext& context, std::vector<Violation>& out) const {
  for (RowReport& r : report(info(), context, ShareFinding::Kind::kStep)) {
    out.push_back(std::move(r.v));
  }
}

const CheckInfo& ShareCountMismatch::info() const noexcept {
  static const CheckInfo kInfo{
      .code = "DQ922",
      .name = "share-count-mismatch",
      .summary =
          "a period's weighted-average and outstanding shares apart by a clean split "
          "ratio or a power of ten, which no split on file explains (--items)",
      .default_severity = Severity::kWarn,
      .applies = Applies::kStatements,
  };
  return kInfo;
}

void ShareCountMismatch::run(const SeriesContext& context, std::vector<Violation>& out) const {
  for (Violation& v :
       fold_counts(context.series, report(info(), context, ShareFinding::Kind::kMismatch))) {
    out.push_back(std::move(v));
  }
}

}  // namespace dorq
