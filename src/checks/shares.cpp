// DQ921 share-terms-step and DQ922 share-count-mismatch: the share terms model's
// findings (statements/shares.cpp; doc/checks/DQ921.md, DQ922.md).

#include "statements/shares.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

void report(const CheckInfo& info, const SeriesContext& context, ShareFinding::Kind kind,
            std::vector<Violation>& out) {
  if (context.series.kind != SeriesKind::kStatements || context.shares == nullptr) {
    return;
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
    out.push_back(std::move(v));
  }
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
  report(info(), context, ShareFinding::Kind::kStep, out);
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
  report(info(), context, ShareFinding::Kind::kMismatch, out);
}

}  // namespace dorq
