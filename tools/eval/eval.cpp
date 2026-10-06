// dorq-eval: dorq's output against labels (plan section 8; tools/eval/README.md).
//
//   dorq-eval --labels L.jsonl --results R.csv [--complete] [--gates F]
//             [--markdown report.md] [--html report.html] [--title T]
//
// R.csv is `dorq check --format csv --show-info` output. Reports cover precision,
// recall and F1 per check at warn and above, precision at warn and at error,
// hard negatives, a reliability diagram of p_error with its expected calibration
// error, and precision@k.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "calibrate/labels.hpp"
#include "core/text.hpp"
#include "dorq/date.hpp"
#include "dorq/number.hpp"
#include "io/csv.hpp"

namespace {

using dorq::Date;
using dorq::Label;
using dorq::LabelClass;

constexpr int kSlack = 3;               // calendar days of slack in matching a date
constexpr std::size_t kMaxNotes = 200;  // false positives and misses listed in a report

struct Result {
  std::string series;
  Date first;
  Date last;
  std::string code;
  std::string severity;
  double p_error = 1.0;
  std::string message;
};

std::vector<std::vector<std::string>> read_csv(const std::string& path) {
  std::ifstream const in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error(path + ": cannot open the file");
  }
  std::ostringstream text;
  text << in.rdbuf();
  std::vector<std::vector<std::string>> rows;
  dorq::CsvParser parser(',');
  const dorq::CsvParser::RecordFn on_record = [&rows](std::span<const std::string_view> fields,
                                                      std::uint32_t /*line*/) {
    rows.emplace_back(fields.begin(), fields.end());
  };
  parser.feed(text.str(), on_record);
  parser.finish(on_record);
  return rows;
}

std::size_t column(const std::vector<std::string>& header, std::string_view name) {
  const auto it = std::find(header.begin(), header.end(), name);
  if (it == header.end()) {
    throw std::runtime_error("the results have no \"" + std::string{name} + "\" column");
  }
  return static_cast<std::size_t>(it - header.begin());
}

std::vector<Result> read_results(const std::string& path) {
  std::vector<Result> results;
  const auto rows = read_csv(path);
  if (rows.empty()) {
    return results;
  }
  const auto& h = rows.at(0);
  const std::size_t series = column(h, "series");
  const std::size_t date = column(h, "date");
  const std::size_t end = column(h, "end_date");
  const std::size_t code = column(h, "code");
  const std::size_t severity = column(h, "severity");
  const std::size_t p = column(h, "p_error");
  const std::size_t message = column(h, "message");
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const auto first = dorq::parse_date(row.at(date));
    if (!first) {
      continue;  // a row with no date: not something a label can cover
    }
    Result r;
    r.series = row.at(series);
    r.first = *first;
    r.last = dorq::parse_date(row.at(end)).value_or(r.first);
    r.code = row.at(code);
    r.severity = row.at(severity);
    const dorq::ParsedNumber number = dorq::parse_number(row.at(p));
    r.p_error = number.status == dorq::ParsedNumber::Status::kOk ? number.value : 1.0;
    r.message = row.at(message);
    results.push_back(std::move(r));
  }
  return results;
}

bool overlaps(const Result& r, const Label& l, int slack) {
  if (l.series != "*" && l.series != r.series) {
    return false;
  }
  if (l.series == "*" && l.label_class != LabelClass::kMarketFact && !r.series.empty()) {
    return false;  // a cross-sectional fault wants the cross-sectional row
  }
  return r.first.days() <= l.last.days() + slack && l.first.days() <= r.last.days() + slack;
}

bool is_fault(const Label& l) { return l.label_class != LabelClass::kMarketFact; }

// A label's expected codes: "DQ201" or "DQ201|DQ202".
bool expects(const Label& l, const std::string& code) {
  if (l.expect.empty()) {
    return true;
  }
  std::stringstream codes(l.expect);
  std::string item;
  while (std::getline(codes, item, '|')) {
    if (item == code) {
      return true;
    }
  }
  return false;
}

// The checks whose p_error is a probability (not a deterministic 1 or 0).
bool bayesian(const std::string& code) {
  return code.starts_with("DQ2") || code == "DQ301" || code == "DQ302" || code == "DQ303" ||
         code == "DQ304" || code.starts_with("DQ40") || code == "DQ501" ||
         (code.starts_with("DQ70") && code != "DQ705");
}

double ratio(double a, double b) { return b == 0.0 ? 1.0 : a / b; }

std::string fixed(double x, int decimals = 3) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(decimals);
  out << x;
  return out.str();
}

// A metric for a report, or a dash when nothing defines it.
std::string shown(double value, bool defined) { return defined ? fixed(value) : "–"; }

struct Gate {
  std::string code;
  std::string metric;
  double bound = 0.0;
};

std::vector<Gate> read_gates(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error(path + ": cannot open the file");
  }
  std::vector<Gate> gates;
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) {
      line.erase(hash);
    }
    std::istringstream words(line);
    Gate g;
    if (words >> g.code >> g.metric >> g.bound) {
      gates.push_back(g);
    }
  }
  return gates;
}

struct Counts {
  int reported = 0;
  int correct = 0;
  int other_fault = 0;
  int false_positive = 0;
  int unlabelled = 0;  // matching no label (without --complete)
  int faults = 0;
  int found = 0;

  [[nodiscard]] int judged() const { return correct + other_fault + false_positive; }
  [[nodiscard]] double precision() const {
    return ratio(correct, correct + other_fault + false_positive);
  }
  [[nodiscard]] double fault_precision() const {
    return ratio(correct + other_fault, correct + other_fault + false_positive);
  }
  [[nodiscard]] double recall() const { return ratio(found, faults); }
  [[nodiscard]] double f1() const {
    const double p = precision();
    const double r = recall();
    return p + r == 0.0 ? 0.0 : 2.0 * p * r / (p + r);
  }
};

struct Bin {
  double p = 0.0;
  double y = 0.0;
  int n = 0;
};

struct Evaluation {
  std::map<std::string, Counts> by_code;
  std::map<std::string, int> hard_hits;  // by hard-negative kind
  std::array<Counts, 2> by_severity{};   // warn, error
  std::vector<Bin> bins;                 // reliability, ten equal-width bins
  double ece = 0.0;
  int calibration_points = 0;
  std::vector<std::pair<int, double>> precision_at;  // k, precision
  std::vector<std::string> notes;                    // false positives and misses
};

// Whether a result is right, names a different fault, is wrong, or is unlabelled.
enum class Outcome : std::uint8_t { kCorrect, kOtherFault, kFalse, kUnlabelled };

Outcome judge(const Result& r, const std::vector<Label>& labels, bool complete) {
  bool correct = false;
  bool other = false;
  bool negative = false;
  for (const Label& l : labels) {
    if (is_fault(l) && overlaps(r, l, kSlack)) {
      (expects(l, r.code) ? correct : other) = true;
    } else if (!is_fault(l) && overlaps(r, l, 0) && r.code.starts_with(l.codes)) {
      negative = true;
    }
  }
  if (correct) {
    return Outcome::kCorrect;
  }
  if (other) {
    return Outcome::kOtherFault;
  }
  return negative || complete ? Outcome::kFalse : Outcome::kUnlabelled;
}

void count(Counts& c, Outcome outcome) {
  ++c.reported;
  switch (outcome) {
    case Outcome::kCorrect:
      ++c.correct;
      break;
    case Outcome::kOtherFault:
      ++c.other_fault;
      break;
    case Outcome::kFalse:
      ++c.false_positive;
      break;
    case Outcome::kUnlabelled:
      ++c.unlabelled;
      break;
  }
}

// A series' own report on a session a cross-sectional fault covers: part of the
// cohort (DQ301 rows folded into DQ303), so a fault for calibration.
bool in_cohort_fault(const Result& r, const std::vector<Label>& labels) {
  return !r.series.empty() && std::any_of(labels.begin(), labels.end(), [&r](const Label& l) {
    return l.series == "*" && is_fault(l) && r.first.days() <= l.last.days() + kSlack &&
           l.first.days() <= r.last.days() + kSlack;
  });
}

Evaluation evaluate(const std::vector<Label>& labels, const std::vector<Result>& results,
                    bool complete) {
  Evaluation e;
  std::vector<std::pair<double, int>> ranked;  // p_error, correct (warn and above)
  e.bins.assign(10, Bin{});
  for (const Result& r : results) {
    const Outcome outcome = judge(r, labels, complete);
    // Calibration: every Bayesian report, at any severity, that the labels judge.
    // A report near a fault that names another code is left out: the fault is
    // real, but whether it is the thing reported is not known.
    const bool cohort = outcome == Outcome::kFalse && in_cohort_fault(r, labels);
    if (bayesian(r.code) &&
        (outcome == Outcome::kCorrect || outcome == Outcome::kFalse || cohort)) {
      const double y = outcome == Outcome::kCorrect || cohort ? 1.0 : 0.0;
      const auto b = std::min<std::size_t>(9, static_cast<std::size_t>(r.p_error * 10.0));
      e.bins[b].p += r.p_error;
      e.bins[b].y += y;
      ++e.bins[b].n;
      ++e.calibration_points;
    }
    if (r.severity == "info") {
      continue;
    }
    Counts& c = e.by_code[r.code];
    Counts& s = e.by_severity.at(r.severity == "error" ? 1 : 0);
    count(c, outcome);
    count(s, outcome);
    if (outcome == Outcome::kFalse) {
      bool hard = false;
      for (const Label& l : labels) {
        if (!is_fault(l) && overlaps(r, l, 0) && r.code.starts_with(l.codes)) {
          e.hard_hits[l.kind] += hard ? 0 : 1;
          hard = true;
        }
      }
      e.notes.push_back(std::string{hard ? "hard negative: " : "false positive: "} + r.series +
                        " " + r.first.to_string() + " " + r.code + " " + r.message);
    }
    if (outcome != Outcome::kUnlabelled) {
      ranked.emplace_back(r.p_error, outcome == Outcome::kFalse ? 0 : 1);
    }
  }
  for (const Label& l : labels) {
    if (!is_fault(l)) {
      e.hard_hits.try_emplace(l.kind, 0);
      continue;
    }
    const std::string code = l.expect.substr(0, l.expect.find('|'));
    if (code.empty()) {
      continue;
    }
    Counts& c = e.by_code[code];
    ++c.faults;
    const bool found = std::any_of(results.begin(), results.end(), [&](const Result& r) {
      return r.severity != "info" && expects(l, r.code) && overlaps(r, l, kSlack);
    });
    c.found += found ? 1 : 0;
    if (!found) {
      e.notes.push_back("missed (" + l.kind + "): " + l.series + " " + l.first.to_string() + ".." +
                        l.last.to_string() + " " + l.expect);
    }
  }
  for (const Bin& b : e.bins) {
    if (b.n > 0 && e.calibration_points > 0) {
      e.ece += std::fabs(b.p - b.y) / e.calibration_points;
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const int k : {10, 25, 50, 100, 200, 500}) {
    if (static_cast<std::size_t>(k) > ranked.size()) {
      break;
    }
    int hits = 0;
    for (int i = 0; i < k; ++i) {
      hits += ranked.at(static_cast<std::size_t>(i)).second;
    }
    e.precision_at.emplace_back(k, static_cast<double>(hits) / k);
  }
  return e;
}

std::optional<double> metric(Evaluation& e, const Gate& gate) {
  if (gate.metric == "max") {
    return e.hard_hits[gate.code];
  }
  const Counts& c = e.by_code[gate.code];
  if (gate.metric == "precision") {
    return c.precision();
  }
  if (gate.metric == "fault_precision") {
    return c.fault_precision();
  }
  if (gate.metric == "recall") {
    return c.recall();
  }
  if (gate.metric == "f1") {
    return c.f1();
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Reports

std::string markdown(const Evaluation& e, const std::string& title, bool complete) {
  std::string out = "# " + title + "\n\n";
  out += complete ? "Every fault is labelled: a report matching no label is a false positive.\n\n"
                  : "Reports matching no label are left out of precision (\"unlabelled\").\n\n";
  out += "## Per check, at warn and above\n\n";
  out +=
      "| Check | Reported | Right | Other fault | False | Unlabelled | Precision | "
      "Fault precision | Faults | Found | Recall | F1 |\n";
  out += "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& [code, c] : e.by_code) {
    out += "| " + code + " | " + std::to_string(c.reported) + " | " + std::to_string(c.correct) +
           " | " + std::to_string(c.other_fault) + " | " + std::to_string(c.false_positive) +
           " | " + std::to_string(c.unlabelled) + " | " + shown(c.precision(), c.judged() > 0) +
           " | " + shown(c.fault_precision(), c.judged() > 0) + " | " + std::to_string(c.faults) +
           " | " + std::to_string(c.found) + " | " + shown(c.recall(), c.faults > 0) + " | " +
           shown(c.f1(), c.judged() > 0 && c.faults > 0) + " |\n";
  }
  out +=
      "\n## By severity\n\n| Severity | Reported | Precision | Fault precision "
      "|\n|---|---|---|---|\n";
  const std::array<const char*, 2> names = {"warn", "error"};
  for (std::size_t s = 0; s < 2; ++s) {
    const Counts& c = e.by_severity.at(s);
    out += std::string{"| "} + names.at(s) + " | " + std::to_string(c.reported) + " | " +
           shown(c.precision(), c.judged() > 0) + " | " +
           shown(c.fault_precision(), c.judged() > 0) + " |\n";
  }
  out += "\n## Hard negatives reported at warn or above\n\n| Kind | Reports |\n|---|---|\n";
  for (const auto& [kind, hits] : e.hard_hits) {
    out += "| " + kind + " | " + std::to_string(hits) + " |\n";
  }
  out += "\n## Calibration of p_error\n\nExpected calibration error: **" + fixed(e.ece) +
         "** over " + std::to_string(e.calibration_points) +
         " labelled reports of the Bayesian checks, at any severity.\n\n"
         "| p_error | Reports | Mean p_error | Share that were faults |\n|---|---|---|---|\n";
  for (std::size_t b = 0; b < e.bins.size(); ++b) {
    const Bin& bin = e.bins[b];
    const std::string range =
        fixed(static_cast<double>(b) / 10.0, 1) + "–" + fixed(static_cast<double>(b + 1) / 10.0, 1);
    out += "| " + range + " | " + std::to_string(bin.n) + " | " +
           (bin.n > 0 ? fixed(bin.p / bin.n) : "–") + " | " +
           (bin.n > 0 ? fixed(bin.y / bin.n) : "–") + " |\n";
  }
  out +=
      "\n## Precision at k\n\nReports at warn and above, by p_error, highest first.\n\n"
      "| k | Precision |\n|---|---|\n";
  for (const auto& [k, p] : e.precision_at) {
    out += "| " + std::to_string(k) + " | " + fixed(p) + " |\n";
  }
  if (!e.notes.empty()) {
    out += "\n## False positives and misses\n\n";
    for (std::size_t i = 0; i < std::min(kMaxNotes, e.notes.size()); ++i) {
      out += "- " + e.notes[i] + "\n";
    }
    if (e.notes.size() > kMaxNotes) {
      out += "- and " + std::to_string(e.notes.size() - kMaxNotes) + " more\n";
    }
  }
  return out;
}

std::string escape(std::string_view text) {
  std::string out;
  for (const char ch : text) {
    switch (ch) {
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '&':
        out += "&amp;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += ch;
    }
  }
  return out;
}

// The reliability diagram: observed fault rate against mean p_error per bin.
std::string reliability_svg(const Evaluation& e) {
  constexpr double kSize = 260.0;
  constexpr double kPad = 48.0;
  const auto x = [&](double v) { return kPad + v * kSize; };
  const auto y = [&](double v) { return kPad + (1.0 - v) * kSize; };
  std::string out = "<svg viewBox=\"0 0 " + fixed(kSize + 2 * kPad, 0) + " " +
                    fixed(kSize + 2 * kPad, 0) +
                    R"(" role="img" aria-label="Reliability diagram" class="chart">)";
  out += "<rect x=\"" + fixed(x(0), 1) + "\" y=\"" + fixed(y(1), 1) + "\" width=\"" +
         fixed(kSize, 1) + "\" height=\"" + fixed(kSize, 1) + R"(" class="frame"/>)";
  for (int t = 0; t <= 4; ++t) {
    const double v = t / 4.0;
    out += "<text x=\"" + fixed(x(v), 1) + "\" y=\"" + fixed(y(0) + 16, 1) +
           R"(" class="tick" text-anchor="middle">)" + fixed(v, 2) + "</text>";
    out += "<text x=\"" + fixed(x(0) - 6, 1) + "\" y=\"" + fixed(y(v) + 4, 1) +
           R"(" class="tick" text-anchor="end">)" + fixed(v, 2) + "</text>";
  }
  out += "<line x1=\"" + fixed(x(0), 1) + "\" y1=\"" + fixed(y(0), 1) + "\" x2=\"" +
         fixed(x(1), 1) + "\" y2=\"" + fixed(y(1), 1) + R"(" class="diagonal"/>)";
  std::string path;
  for (const Bin& b : e.bins) {
    if (b.n == 0) {
      continue;
    }
    const double px = b.p / b.n;
    const double py = b.y / b.n;
    path += (path.empty() ? "M" : " L") + fixed(x(px), 1) + " " + fixed(y(py), 1);
    const double r = 3.0 + std::min(6.0, std::log1p(b.n));
    out += "<circle cx=\"" + fixed(x(px), 1) + "\" cy=\"" + fixed(y(py), 1) + "\" r=\"" +
           fixed(r, 1) + R"(" class="point"><title>p )" + fixed(px) + ", observed " + fixed(py) +
           ", " + std::to_string(b.n) + " reports</title></circle>";
  }
  out += "<path d=\"" + path + R"(" class="curve"/>)";
  out += "<text x=\"" + fixed(x(0.5), 1) + "\" y=\"" + fixed(y(0) + 32, 1) +
         R"(" class="axis" text-anchor="middle">mean p_error</text>)";
  out += R"(<text x="12" y=")" + fixed(y(0.5), 1) +
         "\" class=\"axis\" text-anchor=\"middle\" "
         "transform=\"rotate(-90 12 " +
         fixed(y(0.5), 1) + ")\">share that were faults</text>";
  out += "</svg>";
  return out;
}

std::string html(const Evaluation& e, const std::string& title, bool complete) {
  std::string out = R"(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>)" + escape(title) +
                    R"(</title>
<style>
:root { --bg: #ffffff; --fg: #1d2330; --muted: #5d6675; --line: #d8dde6; --accent: #2f6fde;
        --good: #1f8a4c; --bad: #c2412d; --panel: #f5f7fa; }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { --bg: #12151b; --fg: #e5e9f0; --muted: #9aa3b2;
    --line: #2b313c; --accent: #6ea0ff; --good: #4cc27e; --bad: #f07a63; --panel: #1a1f27; }
}
:root[data-theme="dark"] { --bg: #12151b; --fg: #e5e9f0; --muted: #9aa3b2; --line: #2b313c;
  --accent: #6ea0ff; --good: #4cc27e; --bad: #f07a63; --panel: #1a1f27; }
body { background: var(--bg); color: var(--fg); margin: 0 auto; max-width: 1100px;
       padding: 24px 16px; font: 15px/1.5 system-ui, sans-serif; }
h1 { font-size: 1.6rem; margin: 0 0 4px; } h2 { font-size: 1.15rem; margin-top: 32px; }
p.lead { color: var(--muted); margin-top: 0; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 12px; }
.tile { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 12px; }
.tile .v { font-size: 1.5rem; font-variant-numeric: tabular-nums; }
.tile .k { color: var(--muted); font-size: .85rem; }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; font-variant-numeric: tabular-nums; width: 100%; }
th, td { border-bottom: 1px solid var(--line); padding: 4px 8px; text-align: right; white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--muted); font-weight: 600; }
td.low { color: var(--bad); }
.chart { max-width: 340px; width: 100%; }
.chart .frame { fill: none; stroke: var(--line); }
.chart .diagonal { stroke: var(--muted); stroke-dasharray: 4 4; }
.chart .curve { fill: none; stroke: var(--accent); stroke-width: 2; }
.chart .point { fill: var(--accent); }
.chart .tick, .chart .axis { fill: var(--muted); font-size: 11px; }
ul.notes { color: var(--muted); font-size: .9rem; }
</style></head><body>
<h1>)" + escape(title) +
                    R"(</h1>
<p class="lead">)" +
                    (complete ? std::string{"Every fault is labelled: a report matching no label "
                                            "is a false positive."}
                              : std::string{"Reports matching no label are left out of "
                                            "precision."}) +
                    R"(</p>
<div class="tiles">)";
  const auto tile = [&out](const std::string& value, const std::string& key) {
    out += R"(<div class="tile"><div class="v">)" + value + "</div><div class=\"k\">" + key +
           "</div></div>";
  };
  tile(shown(e.by_severity.at(0).precision(), e.by_severity.at(0).judged() > 0),
       "precision at warn");
  tile(shown(e.by_severity.at(1).precision(), e.by_severity.at(1).judged() > 0),
       "precision at error");
  tile(fixed(e.ece), "expected calibration error");
  int faults = 0;
  int found = 0;
  for (const auto& [code, c] : e.by_code) {
    faults += c.faults;
    found += c.found;
  }
  tile(shown(ratio(found, faults), faults > 0), "recall, all labelled faults");
  out +=
      "</div>\n<h2>Per check, at warn and above</h2><div class=\"scroll\"><table><tr><th>Check</th>"
      "<th>Reported</th><th>Right</th><th>Other fault</th><th>False</th><th>Unlabelled</th>"
      "<th>Precision</th><th>Recall</th><th>F1</th></tr>";
  for (const auto& [code, c] : e.by_code) {
    const auto cell = [](double v, bool defined) {
      return std::string{defined && v < 0.8 ? "<td class=\"low\">" : "<td>"} + shown(v, defined) +
             "</td>";
    };
    out += "<tr><td>" + code + "</td><td>" + std::to_string(c.reported) + "</td><td>" +
           std::to_string(c.correct) + "</td><td>" + std::to_string(c.other_fault) + "</td><td>" +
           std::to_string(c.false_positive) + "</td><td>" + std::to_string(c.unlabelled) + "</td>" +
           cell(c.precision(), c.judged() > 0) + cell(c.recall(), c.faults > 0) +
           cell(c.f1(), c.judged() > 0 && c.faults > 0) + "</tr>";
  }
  out += "</table></div>\n<h2>Calibration of p_error</h2>" + reliability_svg(e) +
         "<p class=\"lead\">Each point is a bin of reports by p_error; on the dashed line, "
         "p_error is the share that were faults. " +
         std::to_string(e.calibration_points) + " labelled reports.</p>";
  out +=
      "<h2>Precision at k</h2><div class=\"scroll\"><table><tr><th>k</th><th>Precision</th></tr>";
  for (const auto& [k, p] : e.precision_at) {
    out += "<tr><td>" + std::to_string(k) + "</td><td>" + fixed(p) + "</td></tr>";
  }
  out +=
      "</table></div><h2>Hard negatives reported at warn or above</h2><div class=\"scroll\">"
      "<table><tr><th>Kind</th><th>Reports</th></tr>";
  for (const auto& [kind, hits] : e.hard_hits) {
    out += "<tr><td>" + escape(kind) + "</td><td>" + std::to_string(hits) + "</td></tr>";
  }
  out += "</table></div>";
  if (!e.notes.empty()) {
    out += "<h2>False positives and misses</h2><ul class=\"notes\">";
    for (std::size_t i = 0; i < std::min(kMaxNotes, e.notes.size()); ++i) {
      out += "<li>" + escape(e.notes[i]) + "</li>";
    }
    if (e.notes.size() > kMaxNotes) {
      out += "<li>and " + std::to_string(e.notes.size() - kMaxNotes) + " more</li>";
    }
    out += "</ul>";
  }
  out += "</body></html>\n";
  return out;
}

int usage() {
  std::cerr << "usage: dorq-eval --labels L.jsonl --results R.csv [--complete] [--gates F]\n"
               "                 [--markdown F] [--html F] [--title T]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> argument_span(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string> args(argument_span.begin(), argument_span.end());
  const auto value = [&args](std::string_view flag) -> std::optional<std::string> {
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
      if (args[i] == flag) {
        return args[i + 1];
      }
    }
    return std::nullopt;
  };
  const auto flag = [&args](std::string_view name) {
    return std::find(args.begin(), args.end(), name) != args.end();
  };
  const auto labels_path = value("--labels");
  const auto results_path = value("--results");
  if (!labels_path || !results_path) {
    return usage();
  }
  try {
    std::ifstream labels_file(*labels_path, std::ios::binary);
    if (!labels_file) {
      throw std::runtime_error(*labels_path + ": cannot open the file");
    }
    const std::vector<Label> labels = dorq::read_labels(labels_file, *labels_path);
    const std::vector<Result> results = read_results(*results_path);
    const bool complete = flag("--complete");
    Evaluation e = evaluate(labels, results, complete);
    const std::string title = value("--title").value_or("dorq evaluation");
    const std::string report = markdown(e, title, complete);
    if (const auto path = value("--markdown")) {
      std::ofstream(*path, std::ios::binary) << report;
    }
    if (const auto path = value("--html")) {
      std::ofstream(*path, std::ios::binary) << html(e, title, complete);
    }
    if (!value("--markdown")) {
      std::cout << report;
    }
    int failed = 0;
    if (const auto gates_path = value("--gates")) {
      for (const Gate& gate : read_gates(*gates_path)) {
        const std::optional<double> v = metric(e, gate);
        if (!v) {
          std::cerr << "unknown metric \"" << gate.metric << "\" in the gates\n";
          return 2;
        }
        const bool ok = gate.metric == "max" ? *v <= gate.bound : *v >= gate.bound;
        if (!ok) {
          std::cout << "GATE FAILED: " << gate.code << " " << gate.metric << " "
                    << dorq::format_number(gate.bound) << " (got " << fixed(*v) << ")\n";
          ++failed;
        }
      }
      if (failed == 0) {
        std::cout << "all gates passed\n";
      }
    }
    return failed == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "dorq-eval: " << error.what() << "\n";
    return 2;
  }
}
