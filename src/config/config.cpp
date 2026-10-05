#include "config/config.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "core/text.hpp"
#include "dorq/number.hpp"

namespace dorq {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& where, const toml::node& node, const std::string& what) {
  const auto& begin = node.source().begin;
  std::string message = where;
  if (begin.line > 0) {
    message += ":" + std::to_string(begin.line);
  }
  throw ConfigError(message + ": " + what);
}

std::string key_path(const std::string& prefix, std::string_view key) {
  return prefix.empty() ? std::string{key} : prefix + "." + std::string{key};
}

std::vector<std::string> read_string_list(const toml::node& node, const std::string& where,
                                          const std::string& key) {
  std::vector<std::string> out;
  if (const auto* text = node.as_string()) {  // "DQ1,DQ2" is allowed too
    std::string_view rest = text->get();
    while (!rest.empty()) {
      const auto comma = rest.find(',');
      const std::string_view item = trim(rest.substr(0, comma));
      if (!item.empty()) {
        out.emplace_back(item);
      }
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return out;
  }
  const auto* array = node.as_array();
  if (array == nullptr) {
    fail(where, node, "\"" + key + "\" must be a list of strings");
  }
  for (const auto& item : *array) {
    const auto* text = item.as_string();
    if (text == nullptr) {
      fail(where, item, "\"" + key + "\" must be a list of strings");
    }
    out.emplace_back(text->get());
  }
  return out;
}

std::string read_string(const toml::node& node, const std::string& where, const std::string& key) {
  const auto* text = node.as_string();
  if (text == nullptr) {
    fail(where, node, "\"" + key + "\" must be a string");
  }
  return text->get();
}

bool read_bool(const toml::node& node, const std::string& where, const std::string& key) {
  const auto* value = node.as_boolean();
  if (value == nullptr) {
    fail(where, node, "\"" + key + "\" must be true or false");
  }
  return value->get();
}

int read_int(const toml::node& node, const std::string& where, const std::string& key, int min,
             int max) {
  const auto* value = node.as_integer();
  if (value == nullptr || value->get() < min || value->get() > max) {
    fail(where, node,
         "\"" + key + "\" must be a whole number from " + std::to_string(min) + " to " +
             std::to_string(max));
  }
  return static_cast<int>(value->get());
}

double read_double(const toml::node& node, const std::string& where, const std::string& key,
                   double min, double max) {
  std::optional<double> value;
  if (const auto* f = node.as_floating_point()) {
    value = f->get();
  } else if (const auto* i = node.as_integer()) {
    value = static_cast<double>(i->get());
  }
  if (!value || *value < min || *value > max) {
    fail(where, node,
         "\"" + key + "\" must be a number from " + format_number(min) + " to " +
             format_number(max));
  }
  return *value;
}

const toml::table& read_table(const toml::node& node, const std::string& where,
                              const std::string& key) {
  const auto* table = node.as_table();
  if (table == nullptr) {
    fail(where, node, "\"" + key + "\" must be a table");
  }
  return *table;
}

IntegrityPatch read_integrity(const toml::table& table, const std::string& where,
                              const std::string& prefix) {
  IntegrityPatch patch;
  for (const auto& [key_node, node] : table) {
    const std::string_view key = key_node.str();
    const std::string path = key_path(prefix, key);
    if (key == "positive_point_series") {
      patch.positive_point_series = read_bool(node, where, path);
    } else if (key == "precision_high_decimals") {
      patch.precision_high_decimals = read_int(node, where, path, 1, 20);
    } else if (key == "precision_high_sig_figs") {
      patch.precision_high_sig_figs = read_int(node, where, path, 1, 20);
    } else if (key == "precision_min_segment") {
      patch.precision_min_segment = read_int(node, where, path, 2, 1000000);
    } else if (key == "precision_min_contrast") {
      patch.precision_min_contrast = read_double(node, where, path, 0.0, 1.0);
    } else {
      fail(where, node, "unknown key \"" + path + "\"");
    }
  }
  return patch;
}

// Reads [coverage] keys. The run-report and cohort keys are global; a profile
// passing `global` = nullptr may not set them.
CoveragePatch read_coverage(const toml::table& table, const std::string& where,
                            const std::string& prefix, Config* global) {
  CoveragePatch patch;
  for (const auto& [key_node, node] : table) {
    const std::string_view key = key_node.str();
    const std::string path = key_path(prefix, key);
    if (key == "frequency") {
      const auto frequency = parse_frequency(read_string(node, where, path));
      if (!frequency) {
        fail(where, node,
             "\"" + path +
                 "\" must be auto, daily, weekly, monthly, quarterly, annual or irregular");
      }
      patch.frequency = frequency;
    } else if (key == "block_sessions") {
      patch.block_sessions = read_int(node, where, path, 10, 10000);
    } else if (key == "outage_start") {
      patch.outage_start = read_double(node, where, path, 1e-12, 0.5);
    } else if (key == "outage_end") {
      patch.outage_end = read_double(node, where, path, 1e-6, 1.0);
    } else if (key == "prior_density") {
      patch.prior_density = read_double(node, where, path, 0.0, 1.0);
    } else if (key == "prior_strength") {
      patch.prior_strength = read_double(node, where, path, 0.0, 1e6);
    } else if (key == "trade_size") {
      patch.trade_size = read_double(node, where, path, 1e-9, 1e12);
    } else if (key == "sparse_density") {
      patch.sparse_density = read_double(node, where, path, 0.0, 1.0);
    } else if (key == "publication_lag") {
      patch.publication_lag = read_int(node, where, path, 0, 100);
    } else if (global != nullptr && key == "report") {
      const std::string report = read_string(node, where, path);
      if (report == "run") {
        global->gap_report = GapReport::kRun;
      } else if (report == "session") {
        global->gap_report = GapReport::kSession;
      } else {
        fail(where, node, "\"" + path + R"(" must be "run" or "session")");
      }
    } else if (global != nullptr && key == "cohort_min_series") {
      global->cohort.min_series = read_int(node, where, path, 2, 1000000);
    } else if (global != nullptr && key == "cohort_max_tail") {
      global->cohort.max_tail = read_double(node, where, path, 0.0, 1.0);
    } else if (global != nullptr && key == "confident_density") {
      global->cohort.confident_density = read_double(node, where, path, 0.0, 1.0);
    } else {
      fail(where, node, "unknown key \"" + path + "\"");
    }
  }
  return patch;
}

Profile read_profile(std::string name, const toml::table& table, const std::string& where) {
  Profile profile;
  profile.name = std::move(name);
  const std::string prefix = "profiles." + profile.name;
  for (const auto& [key_node, node] : table) {
    const std::string_view key = key_node.str();
    const std::string path = key_path(prefix, key);
    if (key == "match") {
      for (const auto& [match_key_node, match_node] : read_table(node, where, path)) {
        const std::string_view match_key = match_key_node.str();
        const std::string match_path = key_path(path, match_key);
        if (match_key == "kind") {
          const std::string kind = read_string(match_node, where, match_path);
          if (kind == "ohlcv") {
            profile.match_kind = SeriesKind::kOhlcv;
          } else if (kind == "point") {
            profile.match_kind = SeriesKind::kPoint;
          } else {
            fail(where, match_node, "\"" + match_path + R"(" must be "ohlcv" or "point")");
          }
        } else if (match_key == "series") {
          profile.match_series = read_string_list(match_node, where, match_path);
        } else {
          fail(where, match_node,
               "unknown key \"" + match_path + "\" (a profile matches on kind or series)");
        }
      }
    } else if (key == "select") {
      profile.select = read_string_list(node, where, path);
    } else if (key == "ignore") {
      profile.ignore = read_string_list(node, where, path);
    } else if (key == "integrity") {
      profile.integrity = read_integrity(read_table(node, where, path), where, path);
    } else if (key == "coverage") {
      profile.coverage = read_coverage(read_table(node, where, path), where, path, nullptr);
    } else {
      fail(where, node, "unknown key \"" + path + "\"");
    }
  }
  return profile;
}

void read_severity(const toml::table& table, const toml::node& node, const std::string& where,
                   SeverityThresholds& severity) {
  for (const auto& [sub_node, value] : table) {
    const std::string path = key_path("severity", sub_node.str());
    if (sub_node.str() == "info") {
      severity.info = read_double(value, where, path, 0.0, 1.0);
    } else if (sub_node.str() == "warn") {
      severity.warn = read_double(value, where, path, 0.0, 1.0);
    } else if (sub_node.str() == "error") {
      severity.error = read_double(value, where, path, 0.0, 1.0);
    } else {
      fail(where, value, "unknown key \"" + path + "\"");
    }
  }
  if (severity.info > severity.warn || severity.warn > severity.error) {
    fail(where, node, "[severity] needs info <= warn <= error");
  }
}

void read_calendar(const toml::table& table, const std::string& where, Config& config) {
  for (const auto& [sub_node, value] : table) {
    const std::string path = key_path("calendar", sub_node.str());
    if (sub_node.str() == "name") {
      const auto kind = parse_calendar_name(read_string(value, where, path));
      if (!kind) {
        fail(where, value, "\"" + path + R"(" must be "XNYS", "weekdays" or "24x7")");
      }
      config.calendar = *kind;
    } else if (sub_node.str() == "file") {
      config.calendar_file = read_string(value, where, path);
    } else if (sub_node.str() == "exchange") {
      config.calendar_exchange = read_string(value, where, path);
    } else {
      fail(where, value, "unknown key \"" + path + "\"");
    }
  }
}

void read_root(const toml::table& root, const std::string& where, Config& config) {
  for (const auto& [key_node, node] : root) {
    const std::string key{key_node.str()};
    if (key == "select") {
      config.select = read_string_list(node, where, key);
    } else if (key == "extend_select") {
      config.extend_select = read_string_list(node, where, key);
    } else if (key == "ignore") {
      config.ignore = read_string_list(node, where, key);
    } else if (key == "min_severity") {
      const auto severity = parse_severity(read_string(node, where, key));
      if (!severity) {
        fail(where, node, R"("min_severity" must be "info", "warn" or "error")");
      }
      config.min_severity = *severity;
    } else if (key == "fail_on") {
      const std::string text = read_string(node, where, key);
      if (text == "never") {
        config.fail_on.reset();
      } else if (const auto severity = parse_severity(text)) {
        config.fail_on = severity;
      } else {
        fail(where, node, R"("fail_on" must be "info", "warn", "error" or "never")");
      }
    } else if (key == "format") {
      const auto format = parse_output_format(read_string(node, where, key));
      if (!format) {
        fail(where, node, "\"format\" must be text, json, jsonl, csv or fafnir");
      }
      config.format = *format;
    } else if (key == "threads") {
      config.threads = read_int(node, where, key, 0, 1024);
    } else if (key == "kind") {
      if (const auto kind = parse_kind_option(read_string(node, where, key))) {
        config.kind = *kind;
      } else {
        fail(where, node, R"("kind" must be "auto", "ohlcv" or "point")");
      }
    } else if (key == "input_format") {
      const auto format = parse_input_format(read_string(node, where, key));
      if (!format) {
        fail(where, node, "\"input_format\" must be auto, csv, tsv, jsonl or json");
      }
      config.input_format = *format;
    } else if (key == "columns") {
      for (const auto& [field_node, column_node] : read_table(node, where, key)) {
        const std::string path = key_path(key, field_node.str());
        const auto field = parse_field_name(field_node.str());
        if (!field) {
          fail(where, column_node,
               "unknown field \"" + path +
                   "\" (fields: series, label, date, open, high, low, close, value, volume, vwap)");
        }
        config.columns.emplace_back(*field, read_string(column_node, where, path));
      }
    } else if (key == "integrity") {
      read_integrity(read_table(node, where, key), where, key).apply_to(config.integrity);
    } else if (key == "coverage") {
      read_coverage(read_table(node, where, key), where, key, &config).apply_to(config.coverage);
    } else if (key == "severity") {
      read_severity(read_table(node, where, key), node, where, config.severity);
    } else if (key == "calendar") {
      read_calendar(read_table(node, where, key), where, config);
    } else if (key == "fafnir") {
      for (const auto& [sub_node, value] : read_table(node, where, key)) {
        const std::string path = key_path(key, sub_node.str());
        if (sub_node.str() == "table_name") {
          config.fafnir_table = read_string(value, where, path);
        } else {
          fail(where, value, "unknown key \"" + path + "\"");
        }
      }
    } else if (key == "profiles") {
      for (const auto& [name_node, profile_node] : read_table(node, where, key)) {
        const std::string name{name_node.str()};
        config.profiles.push_back(
            read_profile(name, read_table(profile_node, where, key_path(key, name)), where));
      }
      std::sort(config.profiles.begin(), config.profiles.end(),
                [](const Profile& a, const Profile& b) { return a.name < b.name; });
    } else {
      fail(where, node, "unknown key \"" + key + "\"");
    }
  }
}

toml::table parse_toml(std::string_view text, const std::string& name) {
  try {
    return toml::parse(text, name);
  } catch (const toml::parse_error& error) {
    throw ConfigError(name + ":" + std::to_string(error.source().begin.line) + ": " +
                      std::string{error.description()});
  }
}

std::string read_file(const fs::path& path) {
  std::ifstream const in(path, std::ios::binary);
  if (!in) {
    throw ConfigError(path.string() + ": cannot read the file");
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool pyproject_has_dorq(const fs::path& path) {
  const toml::table doc = parse_toml(read_file(path), path.string());
  return doc["tool"]["dorq"].as_table() != nullptr;
}

// TOML basic strings take the same escapes JSON strings do.
void append_toml_string(std::string& out, std::string_view text) { append_json_string(out, text); }

void append_toml_list(std::string& out, const std::vector<std::string>& items) {
  out += '[';
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    append_toml_string(out, items[i]);
  }
  out += ']';
}

std::string_view kind_name(KindOption kind) {
  switch (kind) {
    case KindOption::kAuto:
      return "auto";
    case KindOption::kOhlcv:
      return "ohlcv";
    case KindOption::kPoint:
      return "point";
  }
  return "auto";
}

std::string toml_key(std::string_view name) {
  const bool bare = !name.empty() && std::all_of(name.begin(), name.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
  });
  if (bare) {
    return std::string{name};
  }
  std::string out;
  append_toml_string(out, name);
  return out;
}

void append_integrity_patch(std::string& out, const IntegrityPatch& patch) {
  if (patch.positive_point_series) {
    out += "positive_point_series = ";
    out += *patch.positive_point_series ? "true\n" : "false\n";
  }
  if (patch.precision_high_decimals) {
    out += "precision_high_decimals = " + std::to_string(*patch.precision_high_decimals) + "\n";
  }
  if (patch.precision_high_sig_figs) {
    out += "precision_high_sig_figs = " + std::to_string(*patch.precision_high_sig_figs) + "\n";
  }
  if (patch.precision_min_segment) {
    out += "precision_min_segment = " + std::to_string(*patch.precision_min_segment) + "\n";
  }
  if (patch.precision_min_contrast) {
    out += "precision_min_contrast = " + format_number(*patch.precision_min_contrast) + "\n";
  }
}

IntegrityPatch full_patch(const IntegritySettings& settings) {
  IntegrityPatch full;
  full.positive_point_series = settings.positive_point_series;
  full.precision_high_decimals = settings.precision_high_decimals;
  full.precision_high_sig_figs = settings.precision_high_sig_figs;
  full.precision_min_segment = settings.precision_min_segment;
  full.precision_min_contrast = settings.precision_min_contrast;
  return full;
}

CoveragePatch full_patch(const CoverageSettings& settings) {
  CoveragePatch full;
  full.frequency = settings.frequency;
  full.block_sessions = settings.block_sessions;
  full.outage_start = settings.outage_start;
  full.outage_end = settings.outage_end;
  full.prior_density = settings.prior_density;
  full.prior_strength = settings.prior_strength;
  full.trade_size = settings.trade_size;
  full.sparse_density = settings.sparse_density;
  full.publication_lag = settings.publication_lag;
  return full;
}

void append_coverage_patch(std::string& out, const CoveragePatch& patch) {
  const auto number = [&out](const char* key, const std::optional<double>& value) {
    if (value) {
      out += key;
      out += " = ";
      out += format_number(*value);
      out += '\n';
    }
  };
  if (patch.frequency) {
    out += "frequency = \"";
    out += to_string(*patch.frequency);
    out += "\"\n";
  }
  if (patch.block_sessions) {
    out += "block_sessions = " + std::to_string(*patch.block_sessions) + "\n";
  }
  number("outage_start", patch.outage_start);
  number("outage_end", patch.outage_end);
  number("prior_density", patch.prior_density);
  number("prior_strength", patch.prior_strength);
  number("trade_size", patch.trade_size);
  number("sparse_density", patch.sparse_density);
  if (patch.publication_lag) {
    out += "publication_lag = " + std::to_string(*patch.publication_lag) + "\n";
  }
}

// The settings that decide what is reported (config_hash): the effective
// configuration as TOML, with the keys that only change presentation, speed or
// the exit status held at their defaults.
std::string canonical(const Config& config) {
  Config copy = config;
  copy.format = OutputFormat::kText;
  copy.threads = 0;
  copy.fail_on = Severity::kWarn;
  copy.input_format = InputFormat::kAuto;
  copy.source.clear();
  return to_toml(copy);
}

}  // namespace

std::optional<OutputFormat> parse_output_format(std::string_view text) noexcept {
  if (iequals(text, "text")) {
    return OutputFormat::kText;
  }
  if (iequals(text, "json")) {
    return OutputFormat::kJson;
  }
  if (iequals(text, "jsonl") || iequals(text, "ndjson")) {
    return OutputFormat::kJsonl;
  }
  if (iequals(text, "csv")) {
    return OutputFormat::kCsv;
  }
  if (iequals(text, "fafnir")) {
    return OutputFormat::kFafnir;
  }
  return std::nullopt;
}

std::string_view to_string(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::kText:
      return "text";
    case OutputFormat::kJson:
      return "json";
    case OutputFormat::kJsonl:
      return "jsonl";
    case OutputFormat::kCsv:
      return "csv";
    case OutputFormat::kFafnir:
      return "fafnir";
  }
  return "text";
}

void IntegrityPatch::apply_to(IntegritySettings& settings) const {
  if (positive_point_series) {
    settings.positive_point_series = *positive_point_series;
  }
  if (precision_high_decimals) {
    settings.precision_high_decimals = *precision_high_decimals;
  }
  if (precision_high_sig_figs) {
    settings.precision_high_sig_figs = *precision_high_sig_figs;
  }
  if (precision_min_segment) {
    settings.precision_min_segment = *precision_min_segment;
  }
  if (precision_min_contrast) {
    settings.precision_min_contrast = *precision_min_contrast;
  }
}

void CoveragePatch::apply_to(CoverageSettings& settings) const {
  if (frequency) {
    settings.frequency = *frequency;
  }
  if (block_sessions) {
    settings.block_sessions = *block_sessions;
  }
  if (outage_start) {
    settings.outage_start = *outage_start;
  }
  if (outage_end) {
    settings.outage_end = *outage_end;
  }
  if (prior_density) {
    settings.prior_density = *prior_density;
  }
  if (prior_strength) {
    settings.prior_strength = *prior_strength;
  }
  if (trade_size) {
    settings.trade_size = *trade_size;
  }
  if (sparse_density) {
    settings.sparse_density = *sparse_density;
  }
  if (publication_lag) {
    settings.publication_lag = *publication_lag;
  }
}

std::optional<Severity> SeverityThresholds::for_probability(double p) const noexcept {
  if (p >= error) {
    return Severity::kError;
  }
  if (p >= warn) {
    return Severity::kWarn;
  }
  if (p >= info) {
    return Severity::kInfo;
  }
  return std::nullopt;
}

bool Profile::matches(const Series& series) const {
  if (match_kind && *match_kind != series.kind) {
    return false;
  }
  return match_series.empty() ||
         std::find(match_series.begin(), match_series.end(), series.id) != match_series.end();
}

std::optional<fs::path> discover_config(const fs::path& start) {
  std::error_code ec;
  fs::path dir = fs::absolute(start, ec);
  if (ec) {
    dir = start;
  }
  while (true) {
    if (fs::is_regular_file(dir / "dorq.toml", ec)) {
      return dir / "dorq.toml";
    }
    if (fs::is_regular_file(dir / "pyproject.toml", ec) &&
        pyproject_has_dorq(dir / "pyproject.toml")) {
      return dir / "pyproject.toml";
    }
    if (fs::exists(dir / ".git", ec) || dir == dir.parent_path() || dir.empty()) {
      break;
    }
    dir = dir.parent_path();
  }
  // Read on the main thread before any worker starts.
  const char* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
  const char* home = std::getenv("HOME");            // NOLINT(concurrency-mt-unsafe)
  fs::path user_dir;
  if (xdg != nullptr && *xdg != '\0') {
    user_dir = fs::path(xdg);
  } else if (home != nullptr && *home != '\0') {
    user_dir = fs::path(home) / ".config";
  }
  if (!user_dir.empty() && fs::is_regular_file(user_dir / "dorq" / "dorq.toml", ec)) {
    return user_dir / "dorq" / "dorq.toml";
  }
  return std::nullopt;
}

Config parse_config(std::string_view text, const std::string& name, bool is_pyproject) {
  const toml::table doc = parse_toml(text, name);
  Config config;
  if (const auto* tool = doc["tool"]["dorq"].as_table()) {
    if (!is_pyproject) {
      // In a dorq.toml with a [tool.dorq] table, a later [columns] header is a
      // top-level table, not part of the settings. Say so rather than ignore it.
      for (const auto& [key, node] : doc) {
        if (key.str() != "tool") {
          fail(name, node,
               "\"" + std::string{key.str()} + "\" is outside [tool.dorq]; write [tool.dorq." +
                   std::string{key.str()} + "], or drop the [tool.dorq] header");
        }
      }
    }
    read_root(*tool, name, config);
  } else if (!is_pyproject) {
    read_root(doc, name, config);
  }
  return config;
}

Config load_config(const fs::path& path) {
  Config config = parse_config(read_file(path), path.string(), path.filename() == "pyproject.toml");
  config.source = path;
  // A calendar file named in a config file is relative to that file.
  if (!config.calendar_file.empty() && config.calendar_file.is_relative()) {
    config.calendar_file = path.parent_path() / config.calendar_file;
  }
  return config;
}

std::string to_toml(const Config& config) {
  std::string out;
  out += "select = ";
  append_toml_list(out, config.select);
  out += "\nextend_select = ";
  append_toml_list(out, config.extend_select);
  out += "\nignore = ";
  append_toml_list(out, config.ignore);
  out += "\nmin_severity = \"" + std::string{to_string(config.min_severity)} + "\"";
  out += "\nfail_on = \"" +
         std::string{config.fail_on ? to_string(*config.fail_on) : std::string_view{"never"}} +
         "\"";
  out += "\nformat = \"" + std::string{to_string(config.format)} + "\"";
  out += "\nthreads = " + std::to_string(config.threads);
  out += "\nkind = \"" + std::string{kind_name(config.kind)} + "\"";
  out += "\ninput_format = \"" + std::string{to_string(config.input_format)} + "\"\n";

  out += "\n[columns]\n";
  for (const auto& [field, column] : config.columns) {
    out += std::string{field_name(field, SeriesKind::kOhlcv)} + " = ";
    append_toml_string(out, column);
    out += "\n";
  }

  out += "\n[integrity]\n";
  append_integrity_patch(out, full_patch(config.integrity));

  out += "\n[coverage]\n";
  append_coverage_patch(out, full_patch(config.coverage));
  out += "report = \"";
  out += config.gap_report == GapReport::kRun ? "run" : "session";
  out += "\"\ncohort_min_series = " + std::to_string(config.cohort.min_series);
  out += "\ncohort_max_tail = " + format_number(config.cohort.max_tail);
  out += "\nconfident_density = " + format_number(config.cohort.confident_density) + "\n";

  out += "\n[severity]\ninfo = " + format_number(config.severity.info);
  out += "\nwarn = " + format_number(config.severity.warn);
  out += "\nerror = " + format_number(config.severity.error) + "\n";

  out += "\n[calendar]\nname = \"";
  out += to_string(config.calendar);
  out += "\"\n";
  if (!config.calendar_file.empty()) {
    out += "file = ";
    append_toml_string(out, config.calendar_file.string());
    out += "\n";
  }
  if (!config.calendar_exchange.empty()) {
    out += "exchange = ";
    append_toml_string(out, config.calendar_exchange);
    out += "\n";
  }

  out += "\n[fafnir]\ntable_name = ";
  append_toml_string(out, config.fafnir_table);
  out += "\n";

  for (const Profile& profile : config.profiles) {
    const std::string section = "profiles." + toml_key(profile.name);
    out += "\n[" + section + "]\n";
    out += "select = ";
    append_toml_list(out, profile.select);
    out += "\nignore = ";
    append_toml_list(out, profile.ignore);
    out += "\n";
    out += "\n[" + section + ".match]\n";
    if (profile.match_kind) {
      out += "kind = \"" + std::string{to_string(*profile.match_kind)} + "\"\n";
    }
    if (!profile.match_series.empty()) {
      out += "series = ";
      append_toml_list(out, profile.match_series);
      out += "\n";
    }
    std::string integrity;
    append_integrity_patch(integrity, profile.integrity);
    if (!integrity.empty()) {
      out += "\n[";
      out += section;
      out += ".integrity]\n";
      out += integrity;
    }
    std::string coverage;
    append_coverage_patch(coverage, profile.coverage);
    if (!coverage.empty()) {
      out += "\n[";
      out += section;
      out += ".coverage]\n";
      out += coverage;
    }
  }
  return out;
}

std::string config_hash(const Config& config) {
  // FNV-1a, 64-bit: an identity for a set of settings, not a security measure.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char ch : canonical(config)) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 0x100000001b3ULL;
  }
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kHex.at(std::size_t{hash & 0xFU});
    hash >>= 4U;
  }
  return out;
}

std::string starter_config() {
  return R"(# dorq configuration. Every key is optional; the values shown are the defaults.
# dorq reads this file from the current directory or the nearest parent (stopping at
# a .git directory), or [tool.dorq] in pyproject.toml. See doc/configuration.md.

# Which checks run: codes ("DQ101"), code prefixes ("DQ1") or names ("ohlc-bounds").
select = ["DQ"]
extend_select = []
ignore = []

# Report violations at or above this severity: "info", "warn" or "error".
min_severity = "warn"
# Exit with status 1 when a reported violation is at or above this: or "never".
fail_on = "warn"

# Output: "text", "json", "jsonl", "csv" or "fafnir".
format = "text"
# Worker threads; 0 means one per core. Output does not depend on it.
threads = 0

# How to read input: kind is "auto", "ohlcv" or "point"; input_format is "auto",
# "csv", "tsv", "jsonl" or "json".
kind = "auto"
input_format = "auto"

# Column names, when the defaults ("date", "trade_date", "close", ...) do not find them.
[columns]
# date = "trade_date"
# series = "security_id"
# label = "symbol"

[integrity]
# DQ102 also applies to point series (off: rates can be negative).
positive_point_series = false
# DQ106: a close is high-precision when it needs this many decimals and significant figures.
precision_high_decimals = 5
precision_high_sig_figs = 5
precision_min_segment = 20
precision_min_contrast = 0.8

[coverage]
# DQ3xx: how missing sessions are judged (doc/checks/DQ301.md).
frequency = "auto"            # or daily, weekly, monthly, quarterly, annual, irregular
block_sessions = 60           # sessions per block of the local density estimate
outage_start = 0.0001         # P(a feed outage starts on a given session)
outage_end = 0.05             # P(an outage ends on a given session)
prior_density = 0.999         # prior on the density estimated from the counts
prior_strength = 2            # its weight, in sessions
trade_size = 1000             # volume per trade: median volume / trade_size is trades a day
sparse_density = 0.8          # DQ302 below this
publication_lag = 1           # DQ304: the latest sessions not yet expected
report = "run"                # DQ301: one violation per run, or per "session"
cohort_min_series = 3         # DQ303: fewest missing series that make a cohort
cohort_max_tail = 1e-06       # DQ303: Poisson tail probability at or below which
confident_density = 0.95      # DQ303: a series counts as expected above this density

[severity]
# p_error at or above which a probabilistic check reports info, warn or error.
info = 0.2
warn = 0.6
error = 0.9

[calendar]
# Built in: "XNYS" (NYSE holidays and closures), "weekdays" or "24x7". A reference
# file (date column, optional is_open and exchange columns) overrides it within the
# file's span; a relative path is relative to this file.
name = "XNYS"
# file = "trading_calendar.csv"
# exchange = "NASDAQ"

[fafnir]
# The table_name written into --format fafnir records.
table_name = "core.daily_price"

# Profiles apply settings to the series they match, in name order.
# [profiles.rates]
# match = { kind = "point" }
# ignore = ["DQ106"]
)";
}

}  // namespace dorq
