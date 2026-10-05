#include "output/writer.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

#include "core/text.hpp"
#include "dorq/number.hpp"

namespace dorq {
namespace {

void append_json_number(std::string& out, double value) {
  if (std::isfinite(value)) {
    out += format_number(value);
  } else {
    out += "null";
  }
}

void append_detail_value(std::string& out, const DetailValue& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    append_json_string(out, *text);
  } else if (const auto* number = std::get_if<double>(&value)) {
    append_json_number(out, *number);
  } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    out += std::to_string(*integer);
  } else if (const auto* flag = std::get_if<bool>(&value)) {
    out += *flag ? "true" : "false";
  }
}

void append_record_key(std::string& out, const Violation& v) {
  if (v.date) {
    out += R"({"trade_date":")" + v.date->to_string() + "\"}";
  } else {
    out += R"({"line":)" + std::to_string(v.line) + "}";
  }
}

// One violation as the JSON object jsonl, json and fafnir all use.
void append_record(std::string& out, const SeriesResult& series, const Violation& v,
                   const WriterOptions& options) {
  out += R"({"series":)";
  if (series.cross_sectional) {
    out += "null";
  } else {
    append_json_string(out, series.id);
  }
  out += R"(,"label":)";
  if (series.label.empty()) {
    out += "null";
  } else {
    append_json_string(out, series.label);
  }
  out += R"(,"source":)";
  append_json_string(out, series.source);
  out += R"(,"date":)";
  if (v.date) {
    out += "\"" + v.date->to_string() + "\"";
  } else {
    out += "null";
  }
  if (v.end_date) {
    out += R"(,"end_date":")" + v.end_date->to_string() + "\"";
  }
  out += R"(,"line":)";
  if (v.line > 0) {
    out += std::to_string(v.line);
  } else {
    out += "null";
  }
  out += R"(,"code":")" + std::string{v.check->code} + "\"";
  out += R"(,"check":")" + std::string{v.check->name} + "\"";
  out += R"(,"severity":")" + std::string{to_string(v.severity)} + "\"";
  out += R"(,"p_error":)";
  append_json_number(out, v.p_error);
  out += R"(,"classification":")" + std::string{to_string(v.classification)} + "\"";
  out += R"(,"message":)";
  append_json_string(out, v.message);
  out += R"(,"detail":{)";
  for (std::size_t i = 0; i < v.detail.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    append_json_string(out, v.detail[i].key);
    out += ':';
    append_detail_value(out, v.detail[i].value);
  }
  out += R"(},"record_key":)";
  append_record_key(out, v);
  out += R"(,"dorq":{"version":)";
  append_json_string(out, options.version);
  out += R"(,"config_hash":)";
  append_json_string(out, options.config_hash);
  out += "}}";
}

bool is_integer_id(std::string_view id) {
  if (id.empty() || id.size() > 18) {
    return false;
  }
  std::size_t const start = id.front() == '-' ? 1 : 0;
  if (start == id.size() || (id[start] == '0' && id.size() > start + 1)) {
    return false;
  }
  for (std::size_t i = start; i < id.size(); ++i) {
    if (id[i] < '0' || id[i] > '9') {
      return false;
    }
  }
  return true;
}

void append_summary(std::string& out, const Summary& summary, const WriterOptions& options) {
  out += R"({"inputs":)" + std::to_string(summary.inputs);
  out += R"(,"series":)" + std::to_string(summary.series);
  out += R"(,"rows":)" + std::to_string(summary.rows);
  out += R"(,"violations":)" + std::to_string(summary.violations);
  out += R"(,"by_severity":{"error":)" + std::to_string(summary.by_severity[2]);
  out += R"(,"warn":)" + std::to_string(summary.by_severity[1]);
  out += R"(,"info":)" + std::to_string(summary.by_severity[0]) + "}";
  out += R"(,"by_code":{)";
  bool first = true;
  for (const auto& [code, count] : summary.by_code) {
    out += first ? "" : ",";
    first = false;
    append_json_string(out, code);
    out += ":" + std::to_string(count);
  }
  out += R"(},"dorq":{"version":)";
  append_json_string(out, options.version);
  out += R"(,"config_hash":)";
  append_json_string(out, options.config_hash);
  out += "}}";
}

constexpr std::string_view kAllSeries = "(all series)";

class TextWriter final : public Writer {
 public:
  TextWriter(std::ostream& out, WriterOptions options) : out_(out), options_(std::move(options)) {}

  void series(const SeriesResult& result) override {
    std::string name{result.label.empty() ? result.id : result.label};
    if (result.cross_sectional) {
      name = kAllSeries;
    }
    std::string line;
    for (const Violation& v : result.violations) {
      line.clear();
      line += name.empty() ? std::string{"(blank)"} : name;
      line += "  ";
      line += v.date ? v.date->to_string() : "line " + std::to_string(v.line);
      if (v.end_date) {
        line += ".." + v.end_date->to_string();
      }
      line += "  ";
      line += v.check->code;
      line += ' ';
      line += colored(v.severity);
      line += "  ";
      line += v.check->name;
      line += "  ";
      line += v.message;
      if (v.date && v.line > 0) {
        line += "  (line " + std::to_string(v.line) + ")";
      }
      line += '\n';
      out_ << line;
    }
  }

 private:
  [[nodiscard]] std::string colored(Severity severity) const {
    std::string word{to_string(severity)};
    if (!options_.color) {
      return word;
    }
    std::string out;
    switch (severity) {
      case Severity::kError:
        out = "\x1b[31m";  // red
        break;
      case Severity::kWarn:
        out = "\x1b[33m";  // yellow
        break;
      case Severity::kInfo:
        out = "\x1b[36m";  // cyan
        break;
    }
    out += word;
    out += "\x1b[0m";
    return out;
  }

  std::ostream& out_;
  WriterOptions options_;
};

class JsonlWriter final : public Writer {
 public:
  JsonlWriter(std::ostream& out, WriterOptions options) : out_(out), options_(std::move(options)) {}

  void series(const SeriesResult& result) override {
    for (const Violation& v : result.violations) {
      buffer_.clear();
      append_record(buffer_, result, v, options_);
      buffer_ += '\n';
      out_ << buffer_;
    }
  }

 private:
  std::ostream& out_;
  WriterOptions options_;
  std::string buffer_;
};

class JsonWriter final : public Writer {
 public:
  JsonWriter(std::ostream& out, WriterOptions options) : out_(out), options_(std::move(options)) {}

  void begin() override { out_ << R"({"violations":[)"; }

  void series(const SeriesResult& result) override {
    for (const Violation& v : result.violations) {
      buffer_.clear();
      buffer_ += first_ ? "\n" : ",\n";
      first_ = false;
      append_record(buffer_, result, v, options_);
      out_ << buffer_;
    }
  }

  void end(const Summary& summary) override {
    buffer_.clear();
    buffer_ += first_ ? "" : "\n";
    buffer_ += R"(],"summary":)";
    append_summary(buffer_, summary, options_);
    buffer_ += "}\n";
    out_ << buffer_;
  }

 private:
  std::ostream& out_;
  WriterOptions options_;
  std::string buffer_;
  bool first_ = true;
};

class FafnirWriter final : public Writer {
 public:
  FafnirWriter(std::ostream& out, WriterOptions options)
      : out_(out), options_(std::move(options)) {}

  void series(const SeriesResult& result) override {
    for (const Violation& v : result.violations) {
      buffer_.clear();
      buffer_ += R"({"security_id":)";
      if (result.cross_sectional) {
        buffer_ += "null";
      } else if (is_integer_id(result.id)) {
        buffer_ += result.id;
      } else {
        append_json_string(buffer_, result.id);
      }
      buffer_ += R"(,"table_name":)";
      append_json_string(buffer_, options_.fafnir_table);
      buffer_ += R"(,"record_key":)";
      append_record_key(buffer_, v);
      std::string check_name = "dorq_" + std::string{v.check->name};
      for (char& ch : check_name) {
        ch = ch == '-' ? '_' : ch;
      }
      buffer_ += R"(,"check_name":")" + check_name + "\"";
      buffer_ += R"(,"severity":")" + std::string{to_string(v.severity)} + "\"";
      buffer_ += R"(,"detail":)";
      append_record(buffer_, result, v, options_);
      buffer_ += "}\n";
      out_ << buffer_;
    }
  }

 private:
  std::ostream& out_;
  WriterOptions options_;
  std::string buffer_;
};

void append_csv_field(std::string& out, std::string_view text) {
  if (text.find_first_of(",\"\n\r") == std::string_view::npos) {
    out += text;
    return;
  }
  out += '"';
  for (const char ch : text) {
    out += ch;
    if (ch == '"') {
      out += '"';
    }
  }
  out += '"';
}

class CsvWriter final : public Writer {
 public:
  explicit CsvWriter(std::ostream& out) : out_(out) {}

  void begin() override {
    out_ << "series,label,source,date,end_date,line,code,check,severity,p_error,classification,"
            "message\n";
  }

  void series(const SeriesResult& result) override {
    for (const Violation& v : result.violations) {
      buffer_.clear();
      append_csv_field(buffer_, result.id);
      buffer_ += ',';
      append_csv_field(buffer_, result.label);
      buffer_ += ',';
      append_csv_field(buffer_, result.source);
      buffer_ += ',';
      buffer_ += v.date ? v.date->to_string() : "";
      buffer_ += ',';
      buffer_ += v.end_date ? v.end_date->to_string() : "";
      buffer_ += ',';
      buffer_ += v.line > 0 ? std::to_string(v.line) : "";
      buffer_ += ',';
      buffer_ += v.check->code;
      buffer_ += ',';
      buffer_ += v.check->name;
      buffer_ += ',';
      buffer_ += to_string(v.severity);
      buffer_ += ',';
      buffer_ += format_number(v.p_error);
      buffer_ += ',';
      buffer_ += to_string(v.classification);
      buffer_ += ',';
      append_csv_field(buffer_, v.message);
      buffer_ += '\n';
      out_ << buffer_;
    }
  }

 private:
  std::ostream& out_;
  std::string buffer_;
};

}  // namespace

std::unique_ptr<Writer> make_writer(OutputFormat format, std::ostream& out, WriterOptions options) {
  switch (format) {
    case OutputFormat::kText:
      return std::make_unique<TextWriter>(out, std::move(options));
    case OutputFormat::kJson:
      return std::make_unique<JsonWriter>(out, std::move(options));
    case OutputFormat::kJsonl:
      return std::make_unique<JsonlWriter>(out, std::move(options));
    case OutputFormat::kCsv:
      return std::make_unique<CsvWriter>(out);
    case OutputFormat::kFafnir:
      return std::make_unique<FafnirWriter>(out, std::move(options));
  }
  return std::make_unique<TextWriter>(out, std::move(options));
}

std::string statistics_text(const Summary& summary) {
  std::string out;
  for (const auto& [code, count] : summary.by_code) {
    std::string number = std::to_string(count);
    if (number.size() < 6) {
      number.resize(6, ' ');
    }
    out += number + code;
    if (const auto it = summary.names.find(code); it != summary.names.end()) {
      out += " " + it->second;
    }
    out += '\n';
  }
  return out;
}

// The report owns the result; the writer only reads it, so nothing is moved.
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
void Report::series_done(SeriesResult&& result) {
  if (!result.cross_sectional) {
    ++summary_.series;
    summary_.rows += result.rows;
  }
  for (const Violation& v : result.violations) {
    ++summary_.violations;
    ++summary_.by_severity.at(static_cast<std::size_t>(v.severity));
    const std::string code{v.check->code};
    ++summary_.by_code[code];
    summary_.names.try_emplace(code, v.check->name);
  }
  writer_.series(result);
}

}  // namespace dorq
