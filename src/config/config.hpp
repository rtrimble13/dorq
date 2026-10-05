#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dorq/series.hpp"
#include "dorq/violation.hpp"
#include "io/columns.hpp"
#include "io/reader.hpp"

namespace dorq {

enum class OutputFormat : std::uint8_t { kText, kJson, kJsonl, kCsv, kFafnir };

[[nodiscard]] std::optional<OutputFormat> parse_output_format(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(OutputFormat format) noexcept;

// Settings for the DQ1xx checks.
struct IntegritySettings {
  // DQ102 on point series. Off by default: rates and spreads can be negative.
  bool positive_point_series = false;
  // DQ106: a close is "high precision" when it needs at least this many decimal
  // places and significant figures (see doc/checks/DQ106.md).
  int precision_high_decimals = 5;
  int precision_high_sig_figs = 5;
  int precision_min_segment = 20;       // rows on each side of a regime change
  double precision_min_contrast = 0.8;  // difference in the high-precision share
};

// A partial IntegritySettings: what one config table sets.
struct IntegrityPatch {
  std::optional<bool> positive_point_series;
  std::optional<int> precision_high_decimals;
  std::optional<int> precision_high_sig_figs;
  std::optional<int> precision_min_segment;
  std::optional<double> precision_min_contrast;

  void apply_to(IntegritySettings& settings) const;
};

// Settings that apply to the series a profile matches. Profiles are applied in
// name order, so where two match and disagree the later name wins.
struct Profile {
  std::string name;
  std::optional<SeriesKind> match_kind;
  std::vector<std::string> match_series;  // series ids; empty matches any
  std::vector<std::string> select;        // added to the selection
  std::vector<std::string> ignore;        // added to the ignores
  IntegrityPatch integrity;

  [[nodiscard]] bool matches(const Series& series) const;
};

struct Config {
  std::vector<std::string> select = {"DQ"};
  std::vector<std::string> extend_select;
  std::vector<std::string> ignore;
  Severity min_severity = Severity::kWarn;
  std::optional<Severity> fail_on = Severity::kWarn;  // nullopt: never fail
  OutputFormat format = OutputFormat::kText;
  int threads = 0;  // 0: one per core
  KindOption kind = KindOption::kAuto;
  InputFormat input_format = InputFormat::kAuto;
  ColumnOverrides columns;
  IntegritySettings integrity;
  std::vector<Profile> profiles;  // sorted by name
  std::string fafnir_table = "core.daily_price";

  // Where the settings came from; empty when nothing was read.
  std::filesystem::path source;
};

// A config file that cannot be used: unreadable, invalid TOML, an unknown key, a
// wrong type. Maps to exit status 2.
class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

// Finds the config file for `start`: the first dorq.toml, or pyproject.toml with
// a [tool.dorq] table, in `start` or a parent directory, stopping at a directory
// that holds .git; then $XDG_CONFIG_HOME/dorq/dorq.toml (or ~/.config/dorq/...).
[[nodiscard]] std::optional<std::filesystem::path> discover_config(
    const std::filesystem::path& start);

// Reads a config file over the defaults. Throws ConfigError.
[[nodiscard]] Config load_config(const std::filesystem::path& path);

// Parses config text (TOML) over the defaults; `name` is used in errors.
[[nodiscard]] Config parse_config(std::string_view text, const std::string& name,
                                  bool is_pyproject);

// The effective configuration as TOML, every key present.
[[nodiscard]] std::string to_toml(const Config& config);

// A short, stable hash of the settings that can change what is reported:
// selection, severity threshold, input interpretation, check settings, profiles.
// Not the output format, thread count or exit-code policy.
[[nodiscard]] std::string config_hash(const Config& config);

// The starter file `dorq config init` writes.
[[nodiscard]] std::string starter_config();

}  // namespace dorq
