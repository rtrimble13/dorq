#include "cli/app.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>

#include "calibrate/calibrate.hpp"
#include "calibrate/labels.hpp"
#include "checks/check.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "core/text.hpp"
#include "dorq/build_info.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/exit_code.hpp"
#include "dorq/version.hpp"
#include "engine/engine.hpp"
#include "io/input_error.hpp"
#include "io/reader.hpp"
#include "output/writer.hpp"

namespace dorq::cli {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDescription =
    "dorq: a Bayesian data-quality linter for financial time series.\n"
    "Reads OHLCV bars or single-value series, runs a battery of checks, and reports "
    "the observations most likely to be data errors.";

constexpr const char* kFooter =
    "Exit status: 0 nothing to report, 1 violations found, 2 usage or configuration "
    "error, 3 unreadable input.\n"
    "Documentation: https://github.com/rtrimble13/dorq";

// Inputs up to this size (in total) are read whole before checking, so their rows
// may come in any order. Larger inputs, and stdin, are checked as they are read.
constexpr std::uintmax_t kBufferLimitBytes = std::uintmax_t{256} << 20U;

constexpr std::array<std::string_view, 6> kCommands = {"check",   "calibrate", "list-checks",
                                                       "explain", "config",    "version"};

// `dorq FILE` means `dorq check FILE`: put the command in when it is left out.
std::vector<const char*> with_default_command(std::span<const char* const> args) {
  std::vector<const char*> out(args.begin(), args.end());
  if (out.empty()) {
    out.push_back("dorq");
  }
  if (out.size() > 1) {
    const std::string_view first = out[1];
    const bool known = std::find(kCommands.begin(), kCommands.end(), first) != kCommands.end();
    if (known || first == "-h" || first == "--help" || first == "--version") {
      return out;
    }
  }
  out.insert(out.begin() + 1, "check");
  return out;
}

struct CheckOptions {
  std::vector<std::string> files;
  std::string kind;
  std::string input_format;
  std::string columns;
  std::string format;
  std::vector<std::string> select;
  std::vector<std::string> extend_select;
  std::vector<std::string> ignore;
  std::string min_severity;
  bool show_info = false;
  bool show_evidence = false;
  std::string fail_on;
  bool exit_zero = false;
  std::string since;
  bool statistics = false;
  int threads = -1;
  bool buffer = false;
  std::string config;
  bool isolated = false;
  std::string color = "auto";
  std::string calendar;
  std::string calendar_file;
  std::string calendar_exchange;
  std::string as_of;
  std::string actions;
  std::string meta;
  std::string market;
  std::string items;            // --items: statements' line items (ADR 0005)
  std::string labels;           // --labels: their `remove` dates, with --restore
  std::string restore;          // --restore: bars as they stood before repairs
  std::string restore_actions;  // --restore-actions: actions as they stood before repairs
};

struct CalibrateCommand {
  CheckOptions check;  // the input, config and context options check takes
  bool clean_unlabelled = false;
  bool no_grid = false;
  bool no_isotonic = false;
  double holdout = 0.0;
  std::string name;
  std::string out;
};

class UsageError : public std::runtime_error {
 public:
  explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

Config load_effective_config(const std::string& config_path, bool isolated, const fs::path& cwd) {
  if (isolated) {
    return Config{};
  }
  if (!config_path.empty()) {
    return load_config(config_path);
  }
  if (const auto found = discover_config(cwd)) {
    return load_config(*found);
  }
  return Config{};
}

void validate_selection(const Config& config) {
  for (const auto* list : {&config.select, &config.extend_select, &config.ignore}) {
    if (const std::string error = Selection::validate(*list); !error.empty()) {
      throw ConfigError(error);
    }
  }
  for (const Profile& profile : config.profiles) {
    for (const auto* list : {&profile.select, &profile.ignore}) {
      if (const std::string error = Selection::validate(*list); !error.empty()) {
        throw ConfigError("profiles." + profile.name + ": " + error);
      }
    }
  }
}

// Command-line options override the config file. Returns whether any was given.
bool apply_overrides(const CheckOptions& o, CLI::App& cmd, Config& config) {
  bool any = false;
  // Whether an option was given; false for one this command does not take.
  const auto given = [&cmd, &any](const char* name) {
    const CLI::Option* option = cmd.get_option_no_throw(name);
    const bool was_given = option != nullptr && option->count() > 0;
    any = any || was_given;
    return was_given;
  };
  // CLI11 has already checked each value against its list; the ifs only unwrap.
  if (const auto kind = parse_kind_option(o.kind); given("--kind") && kind) {
    config.kind = *kind;
  }
  if (const auto format = parse_input_format(o.input_format); given("--input-format") && format) {
    config.input_format = *format;
  }
  if (given("--columns")) {
    ColumnOverrides overrides;
    if (const auto error = parse_column_overrides(o.columns, overrides)) {
      throw UsageError("--columns: " + *error);
    }
    // A field named on the command line replaces the config file's column for it.
    for (const auto& [field, column] : overrides) {
      std::erase_if(config.columns, [f = field](const auto& entry) { return entry.first == f; });
    }
    config.columns.insert(config.columns.end(), overrides.begin(), overrides.end());
  }
  if (const auto format = parse_output_format(o.format); given("--format") && format) {
    config.format = *format;
  }
  if (given("--select")) {
    config.select = o.select;
  }
  if (given("--extend-select")) {
    config.extend_select.insert(config.extend_select.end(), o.extend_select.begin(),
                                o.extend_select.end());
  }
  if (given("--ignore")) {
    config.ignore.insert(config.ignore.end(), o.ignore.begin(), o.ignore.end());
  }
  if (const auto severity = parse_severity(o.min_severity); given("--min-severity") && severity) {
    config.min_severity = *severity;
  }
  if (o.show_info) {
    config.min_severity = Severity::kInfo;
    any = true;
  }
  if (given("--fail-on")) {
    config.fail_on = o.fail_on == "never" ? std::nullopt : parse_severity(o.fail_on);
  }
  if (o.threads >= 0) {
    config.threads = o.threads;
    any = true;
  }
  if (const auto kind = parse_calendar_name(o.calendar); given("--calendar") && kind) {
    config.calendar = *kind;
  }
  if (given("--calendar-file")) {
    config.calendar_file = o.calendar_file;
  }
  if (given("--calendar-exchange")) {
    config.calendar_exchange = o.calendar_exchange;
  }
  return any;
}

// FNV-1a over a file's bytes, as hex: what config_hash covers in place of the
// calendar file's path. Empty when the file cannot be read (load_calendar then
// reports it).
std::string file_digest(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  std::array<char, 1 << 16> block{};
  while (in.read(block.data(), block.size()) || in.gcount() > 0) {
    for (std::streamsize i = 0; i < in.gcount(); ++i) {
      hash ^= static_cast<unsigned char>(block.at(static_cast<std::size_t>(i)));
      hash *= 0x100000001b3ULL;
    }
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

void stamp_calendar_digest(Config& config, const fs::path& cwd) {
  if (!config.calendar_file.empty()) {
    config.calendar_digest = file_digest(
        config.calendar_file.is_relative() ? cwd / config.calendar_file : config.calendar_file);
  }
}

// What config_hash covers of --items: the file's content, as for a calendar.
void stamp_items_digest(Config& config, const std::string& items, const fs::path& cwd) {
  if (!items.empty()) {
    const fs::path path(items);
    config.items_digest = file_digest(path.is_relative() ? cwd / path : path);
  }
}

// [statements] may name only items --items lists, when it is given: an item it
// does not list is a typo, or a column that is not read (exit 2).
void validate_statement_items(const Config& config, const Context& context,
                              const std::string& items) {
  if (!context.have_items) {
    return;
  }
  for (const auto& [where, item] : config.statements.named_items()) {
    if (context.item(item) == nullptr) {
      std::string message = "statements." + where;
      message += " names \"" + item + "\", which ";
      message += items;
      message += " does not list";
      throw ConfigError(message);
    }
  }
}

// The built-in calendar, with the reference file over it when one is given.
Calendar load_calendar(const Config& config, const fs::path& cwd) {
  Calendar calendar(config.calendar);
  if (config.calendar_file.empty()) {
    return calendar;
  }
  const fs::path path =
      config.calendar_file.is_relative() ? cwd / config.calendar_file : config.calendar_file;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw CalendarError(config.calendar_file.string() + ": cannot open the calendar file");
  }
  calendar.apply_reference(
      read_reference_calendar(file, config.calendar_file.string(), config.calendar_exchange));
  return calendar;
}

std::ifstream open_context(const std::string& path, const fs::path& cwd, std::string_view what) {
  const fs::path full = fs::path(path).is_relative() ? cwd / path : fs::path(path);
  std::ifstream file(full, std::ios::binary);
  if (!file) {
    throw InputError(path + ": cannot open the " + std::string{what} + " file");
  }
  return file;
}

// The context inputs: --actions, --meta and --market (doc/context.md).
Context load_context(const CheckOptions& o, const fs::path& cwd) {
  Context context;
  if (!o.actions.empty()) {
    std::ifstream file = open_context(o.actions, cwd, "actions");
    read_actions(file, o.actions, context);
  }
  if (!o.restore_actions.empty()) {
    std::ifstream file = open_context(o.restore_actions, cwd, "restore-actions");
    restore_actions(file, o.restore_actions, context);
  }
  if (!o.meta.empty()) {
    std::ifstream file = open_context(o.meta, cwd, "metadata");
    read_meta(file, o.meta, context);
  }
  if (!o.items.empty()) {
    std::ifstream file = open_context(o.items, cwd, "items");
    read_items(file, o.items, context);
  }
  if (!o.market.empty()) {
    std::ifstream file = open_context(o.market, cwd, "market");
    std::vector<Series> found;
    SeriesAssembler assembler(Grouping::kBuffer,
                              [&found](Series&& series) { found.push_back(std::move(series)); });
    const fs::path path(o.market);
    read_input(file, o.market, path.stem().string(), path.extension().string(), ReadOptions{},
               assembler);
    assembler.finish();
    if (found.size() != 1) {
      throw InputError(o.market + ": the market file must hold one series (it holds " +
                       std::to_string(found.size()) + ")");
    }
    context.market = make_market(found.front());
  }
  return context;
}

struct InputFile {
  std::string path;  // "-" for stdin
  std::string name;  // shown in messages
  std::string stem;  // the series id when there is no series column
  std::string extension;
};

std::vector<Label> load_labels(const std::string& path, const fs::path& cwd) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file = open_context(path, cwd, "labels");
  return read_labels(file, path);
}

// --restore (and the labels' `remove` dates): the series as they stood.
Restorer load_restorer(const CheckOptions& o, const std::vector<Label>& labels,
                       const fs::path& cwd) {
  std::vector<Series> restored;
  if (!o.restore.empty()) {
    std::ifstream file = open_context(o.restore, cwd, "restore");
    SeriesAssembler assembler(Grouping::kBuffer,
                              [&restored](Series&& s) { restored.push_back(std::move(s)); });
    const fs::path path(o.restore);
    read_input(file, o.restore, path.stem().string(), path.extension().string(), ReadOptions{},
               assembler);
    assembler.finish();
  }
  return {labels, std::move(restored)};
}

int run_check(const CheckOptions& options, CLI::App& cmd, Io& io) {
  Config config = load_effective_config(options.config, options.isolated, io.cwd);
  apply_overrides(options, cmd, config);
  validate_selection(config);

  std::optional<Date> since;
  if (!options.since.empty()) {
    since = parse_date(options.since);
    if (!since) {
      throw UsageError("--since: \"" + options.since + "\" is not a date (YYYY-MM-DD)");
    }
  }
  std::optional<Date> as_of;
  if (!options.as_of.empty()) {
    as_of = parse_date(options.as_of);
    if (!as_of) {
      throw UsageError("--as-of: \"" + options.as_of + "\" is not a date (YYYY-MM-DD)");
    }
  }
  const Calendar calendar = load_calendar(config, io.cwd);
  stamp_calendar_digest(config, io.cwd);
  stamp_items_digest(config, options.items, io.cwd);
  const Context context = load_context(options, io.cwd);
  validate_statement_items(config, context, options.items);
  const Restorer restorer = load_restorer(options, load_labels(options.labels, io.cwd), io.cwd);

  std::vector<InputFile> inputs;
  for (const std::string& file : options.files) {
    if (file == "-") {
      inputs.push_back({"-", "<stdin>", "stdin", ""});
    } else {
      const fs::path path(file);
      inputs.push_back({file, file, path.stem().string(), path.extension().string()});
    }
  }
  if (inputs.empty()) {
    if (io.stdin_is_tty) {
      throw UsageError("no input: name a file, or pipe data to stdin (dorq --help)");
    }
    inputs.push_back({"-", "<stdin>", "stdin", ""});
  }

  Grouping grouping = Grouping::kBuffer;
  if (!options.buffer) {
    std::uintmax_t total = 0;
    for (const InputFile& input : inputs) {
      std::error_code ec;
      const auto size = input.path == "-" ? kBufferLimitBytes + 1 : fs::file_size(input.path, ec);
      total += ec ? 0 : size;
    }
    if (total > kBufferLimitBytes) {
      grouping = Grouping::kStream;
    }
  }

  const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
  const unsigned threads = config.threads > 0 ? static_cast<unsigned>(config.threads) : hardware;

  WriterOptions writer_options;
  writer_options.color = options.color == "always" || (options.color == "auto" && io.color);
  writer_options.version = std::string{kVersion};
  writer_options.config_hash = config_hash(config);
  writer_options.fafnir_table = config.fafnir_table;
  writer_options.show_evidence = options.show_evidence;
  const std::unique_ptr<Writer> writer = make_writer(config.format, io.out, writer_options);
  Report report(*writer);
  {
    Engine engine(config,
                  {.threads = threads,
                   .min_severity = config.min_severity,
                   .since = since,
                   .as_of = as_of,
                   .calendar = &calendar,
                   .context = &context},
                  report);
    SeriesAssembler assembler(grouping, [&engine, &restorer](Series&& s) {
      restorer.apply(s);
      engine.submit(std::move(s));
    });
    ReadOptions read_options{config.input_format, config.kind, config.columns, std::nullopt};
    if (context.have_items) {
      read_options.items = context.item_names();
    }
    writer->begin();
    for (const InputFile& input : inputs) {
      if (input.path == "-") {
        read_input(io.in, input.name, input.stem, input.extension, read_options, assembler);
      } else {
        std::ifstream file(input.path, std::ios::binary);
        if (!file) {
          throw InputError(input.name + ": cannot open the file");
        }
        read_input(file, input.name, input.stem, input.extension, read_options, assembler);
      }
    }
    assembler.finish();
    engine.finish();
  }
  Summary& summary = report.summary();
  summary.inputs = inputs.size();
  writer->end(summary);

  if (options.statistics) {
    (config.format == OutputFormat::kText ? io.out : io.err) << statistics_text(summary);
  }
  if (options.exit_zero || !config.fail_on) {
    return to_int(ExitCode::kOk);
  }
  for (auto s = static_cast<std::size_t>(*config.fail_on); s < 3; ++s) {
    if (summary.by_severity.at(s) > 0) {
      return to_int(ExitCode::kViolations);
    }
  }
  return to_int(ExitCode::kOk);
}

int run_calibrate(const CalibrateCommand& command, CLI::App& cmd, Io& io) {
  const CheckOptions& options = command.check;
  Config config = load_effective_config(options.config, options.isolated, io.cwd);
  apply_overrides(options, cmd, config);
  validate_selection(config);
  const Calendar calendar = load_calendar(config, io.cwd);
  const Context context = load_context(options, io.cwd);
  if (!context.issues.empty()) {
    // check reports each as DQ109; calibrate reports no violations.
    io.err << "dorq: warning: " << context.issues.size()
           << " row(s) of the context files could not be used and were skipped; "
              "dorq check --select DQ109 lists them\n";
  }
  const std::vector<Label> labels = load_labels(options.labels, io.cwd);
  const Restorer restorer = load_restorer(options, labels, io.cwd);

  std::vector<Series> series;
  SeriesAssembler assembler(Grouping::kBuffer, [&](Series&& s) {
    restorer.apply(s);
    series.push_back(std::move(s));
  });
  const ReadOptions read_options{config.input_format, config.kind, config.columns, std::nullopt};
  if (options.files.empty()) {
    if (io.stdin_is_tty) {
      throw UsageError("no input: name the data files, or pipe them to stdin");
    }
    read_input(io.in, "<stdin>", "stdin", "", read_options, assembler);
  }
  for (const std::string& file : options.files) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      throw InputError(file + ": cannot open the file");
    }
    const fs::path path(file);
    read_input(in, file, path.stem().string(), path.extension().string(), read_options, assembler);
  }
  assembler.finish();

  CalibrateOptions calibrate_options;
  calibrate_options.clean_unlabelled = command.clean_unlabelled;
  calibrate_options.grid = !command.no_grid;
  calibrate_options.isotonic = !command.no_isotonic;
  calibrate_options.holdout = command.holdout;
  const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
  calibrate_options.threads = config.threads > 0 ? static_cast<unsigned>(config.threads) : hardware;
  calibrate_options.version =
      command.name.empty() ? fs::path(options.labels).stem().string() : command.name;
  const CalibrationReport report =
      calibrate(series, labels, config, calendar, context, calibrate_options);
  const std::string toml = priors_toml(report);
  if (command.out.empty()) {
    io.out << toml;
    io.err << calibration_summary(report);
  } else {
    fs::path path(command.out);
    if (path.is_relative()) {
      path = io.cwd / path;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw UsageError(command.out + ": cannot write the file");
    }
    file << toml;
    io.out << calibration_summary(report) << "wrote " << command.out << "\n";
  }
  return to_int(ExitCode::kOk);
}

int run_list_checks(const std::string& format, Io& io) {
  std::string out;
  if (format == "json") {
    out += '[';
    bool first = true;
    for (const Check* check : all_checks()) {
      const CheckInfo& info = check->info();
      out += first ? "" : ",";
      first = false;
      out += R"({"code":")" + std::string{info.code} + R"(","name":")" + std::string{info.name} +
             R"(","severity":")" + std::string{to_string(info.default_severity)} +
             R"(","applies":")" + std::string{to_string(info.applies)} + R"(","summary":)";
      append_json_string(out, info.summary);
      out += '}';
    }
    out += "]\n";
  } else {
    for (const Check* check : all_checks()) {
      const CheckInfo& info = check->info();
      std::string name{info.name};
      name.resize(std::max<std::size_t>(name.size(), 24), ' ');
      std::string severity{to_string(info.default_severity)};
      severity.resize(7, ' ');
      out += info.code;
      out += "  ";
      out += name;
      out += severity;
      std::string applies{to_string(info.applies)};
      applies.resize(std::max<std::size_t>(applies.size() + 2, 13), ' ');
      out += applies;
      out += info.summary;
      out += '\n';
    }
  }
  io.out << out;
  return to_int(ExitCode::kOk);
}

int run_explain(const std::string& what, Io& io) {
  const Check* check = find_check(what);
  if (check == nullptr) {
    io.err << "dorq: error: unknown check \"" << what << "\" (see dorq list-checks)\n";
    return to_int(ExitCode::kUsage);
  }
  io.out << check_doc(check->info().code);
  return to_int(ExitCode::kOk);
}

// The settings a check run with these options uses, and the config_hash it reports.
int run_config_show(const CheckOptions& options, CLI::App& cmd, Io& io) {
  Config config = load_effective_config(options.config, options.isolated, io.cwd);
  const bool overridden = apply_overrides(options, cmd, config);
  validate_selection(config);
  stamp_calendar_digest(config, io.cwd);
  stamp_items_digest(config, options.items, io.cwd);
  io.out << "# source: " << (config.source.empty() ? "defaults" : config.source.string())
         << (overridden ? ", with command-line options" : "") << "\n"
         << "# config_hash: " << config_hash(config) << "\n\n"
         << to_toml(config);
  return to_int(ExitCode::kOk);
}

int run_config_init(const std::string& path_text, bool force, Io& io) {
  fs::path path(path_text);
  if (path.is_relative()) {
    path = io.cwd / path;
  }
  std::error_code ec;
  if (fs::exists(path, ec) && !force) {
    throw UsageError(path.string() + " already exists (pass --force to overwrite it)");
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw UsageError(path.string() + ": cannot write the file");
  }
  file << starter_config();
  io.out << "wrote " << path.string() << "\n";
  return to_int(ExitCode::kOk);
}

// The options `check` takes. `config show` takes them too, so that a check command
// line shows the settings, and the config_hash, that the run reports.
void add_check_options(CLI::App& cmd, CheckOptions& o) {
  cmd.add_option("files", o.files, "Input files; '-' or none reads stdin");
  cmd.add_option("--kind", o.kind, "Series kind")
      ->check(CLI::IsMember({"auto", "ohlcv", "point", "statements"}));
  cmd.add_option("--input-format", o.input_format, "Input format")
      ->check(CLI::IsMember({"auto", "csv", "tsv", "jsonl", "json"}));
  cmd.add_option("--columns", o.columns, "Column names, e.g. date=trade_date,series=security_id");
  cmd.add_option("--format", o.format, "Output format")
      ->check(CLI::IsMember({"text", "json", "jsonl", "csv", "fafnir"}));
  cmd.add_option("--select", o.select, "Checks to run (codes, prefixes, names)")->delimiter(',');
  cmd.add_option("--extend-select", o.extend_select, "Checks to add")->delimiter(',');
  cmd.add_option("--ignore", o.ignore, "Checks to skip")->delimiter(',');
  cmd.add_option("--min-severity", o.min_severity, "Lowest severity to report")
      ->check(CLI::IsMember({"info", "warn", "error"}));
  cmd.add_flag("--show-info", o.show_info, "Report info too (--min-severity info)");
  cmd.add_flag("--show-evidence", o.show_evidence,
               "In text output, show each violation's suggested action, hypotheses and "
               "evidence");
  cmd.add_option("--fail-on", o.fail_on, "Lowest severity that fails the run")
      ->check(CLI::IsMember({"info", "warn", "error", "never"}));
  cmd.add_flag("--exit-zero", o.exit_zero, "Exit 0 even when violations are found");
  cmd.add_option("--since", o.since,
                 "Report only violations active on or after (dated, or ending, then)");
  cmd.add_flag("--statistics", o.statistics, "Print counts per check");
  cmd.add_option("--threads", o.threads, "Worker threads (0: one per core)")
      ->check(CLI::Range(0, 1024));
  cmd.add_flag("--buffer", o.buffer,
               "Read all input before checking (for large input not grouped by series)");
  cmd.add_option("--config", o.config, "Config file to use");
  cmd.add_flag("--isolated", o.isolated, "Ignore all config files");
  cmd.add_option("--calendar", o.calendar, "Built-in calendar")
      ->check(
          CLI::IsMember({"XNYS", "NYSE", "XNAS", "NASDAQ", "weekdays", "24x7"}, CLI::ignore_case));
  cmd.add_option("--calendar-file", o.calendar_file,
                 "Reference calendar (CSV: date[, is_open][, exchange]); overrides the "
                 "built-in calendar within its span");
  cmd.add_option("--calendar-exchange", o.calendar_exchange,
                 "The exchange to take from a multi-exchange calendar file");
  cmd.add_option("--as-of", o.as_of,
                 "Date DQ304 judges staleness against (default: the latest bar), and "
                 "statements' versions are in force at");
  cmd.add_option("--actions", o.actions,
                 "Corporate actions (CSV: series, ex_date, type, numerator, denominator, "
                 "amount); enables DQ7xx and explains splits on file");
  cmd.add_option("--meta", o.meta,
                 "Series metadata (CSV: series, asset_type, nav_priced, tick_size, "
                 "peer_group, exchange); profiles match on it");
  cmd.add_option("--market", o.market,
                 "A market reference series (e.g. SPY); moves are judged net of it");
  cmd.add_option("--items", o.items,
                 "Statements' line items (CSV: column, statement, item_class, "
                 "sign_convention, is_subtotal); only these columns are read as items");
  cmd.add_option("--restore", o.restore,
                 "Bars as they stood before repairs, over the input (doc/labels.md)");
  cmd.add_option("--restore-actions", o.restore_actions,
                 "Corporate actions as they stood before repairs, over --actions "
                 "(doc/labels.md)");
  cmd.add_option("--labels", o.labels,
                 "Labels (JSON Lines) whose `remove` dates are dropped, with --restore");
  cmd.add_option("--color", o.color, "Colour text output")
      ->check(CLI::IsMember({"auto", "always", "never"}))
      ->capture_default_str();
}

}  // namespace

int run(std::span<const char* const> args, Io& io) {
  const std::vector<const char*> argv = with_default_command(args);

  CLI::App app{kDescription, "dorq"};
  app.footer(kFooter);
  // Bare, so a script can compare it to a tag: test "$(dorq --version)" = 0.0.1
  app.set_version_flag("--version", std::string{kVersion}, "Print the version and exit");
  app.require_subcommand(1);

  // check
  CheckOptions check_options;
  CLI::App* check = app.add_subcommand("check", "Check time series (the default command)");
  add_check_options(*check, check_options);

  // calibrate
  CalibrateCommand calibrate_command;
  CheckOptions& co = calibrate_command.check;
  CLI::App* calibrate_cmd = app.add_subcommand(
      "calibrate", "Fit the price model's priors to labelled history (doc/calibration.md)");
  calibrate_cmd->add_option("files", co.files, "Data files; '-' or none reads stdin");
  calibrate_cmd->add_option("--labels", co.labels, "Labels (JSON Lines; doc/labels.md)")
      ->required();
  calibrate_cmd->add_option("--restore", co.restore, "Bars as they stood before repairs");
  calibrate_cmd->add_option("--restore-actions", co.restore_actions,
                            "Corporate actions as they stood before repairs (over --actions)");
  calibrate_cmd->add_option("--out", calibrate_command.out,
                            "Write the fitted settings here (default: standard output)");
  calibrate_cmd->add_option("--name", calibrate_command.name,
                            "[calibration] version (default: the labels file's stem)");
  calibrate_cmd
      ->add_option("--holdout", calibrate_command.holdout,
                   "Share of series held out to report on (by id)")
      ->check(CLI::Range(0.0, 0.9));
  calibrate_cmd->add_flag("--clean-unlabelled", calibrate_command.clean_unlabelled,
                          "Count scored moves that match no label as market facts");
  calibrate_cmd->add_flag("--no-grid", calibrate_command.no_grid,
                          "Keep jump_prob, jump_scale and ratio_tolerance as configured");
  calibrate_cmd->add_flag("--no-isotonic", calibrate_command.no_isotonic, "Write no p_error map");
  calibrate_cmd->add_option("--config", co.config, "Config file to use");
  calibrate_cmd->add_flag("--isolated", co.isolated, "Ignore all config files");
  calibrate_cmd->add_option("--kind", co.kind, "Series kind")
      ->check(CLI::IsMember({"auto", "ohlcv", "point"}));
  calibrate_cmd->add_option("--input-format", co.input_format, "Input format")
      ->check(CLI::IsMember({"auto", "csv", "tsv", "jsonl", "json"}));
  calibrate_cmd->add_option("--columns", co.columns, "Column names, e.g. date=trade_date");
  calibrate_cmd->add_option("--threads", co.threads, "Worker threads (0: one per core)")
      ->check(CLI::Range(0, 1024));
  calibrate_cmd->add_option("--calendar", co.calendar, "Built-in calendar")
      ->check(
          CLI::IsMember({"XNYS", "NYSE", "XNAS", "NASDAQ", "weekdays", "24x7"}, CLI::ignore_case));
  calibrate_cmd->add_option("--calendar-file", co.calendar_file, "Reference calendar");
  calibrate_cmd->add_option("--calendar-exchange", co.calendar_exchange,
                            "The exchange to take from a multi-exchange calendar file");
  calibrate_cmd->add_option("--actions", co.actions, "Corporate actions (as for check)");
  calibrate_cmd->add_option("--meta", co.meta, "Series metadata (as for check)");
  calibrate_cmd->add_option("--market", co.market, "A market reference series (as for check)");

  // list-checks
  CLI::App* list = app.add_subcommand("list-checks", "List the checks");
  std::string list_format = "text";
  list->add_option("--format", list_format, "Output format")
      ->check(CLI::IsMember({"text", "json"}))
      ->capture_default_str();

  // explain
  CLI::App* explain = app.add_subcommand("explain", "Describe a check");
  std::string explain_what;
  explain->add_option("check", explain_what, "A code (DQ101) or name (ohlc-bounds)")->required();

  // config
  CLI::App* config = app.add_subcommand("config", "Show or create the configuration");
  config->require_subcommand(1);
  CLI::App* show = config->add_subcommand(
      "show", "Print the effective configuration and its config_hash; takes check's options");
  CheckOptions show_options;
  add_check_options(*show, show_options);
  // Taken so that a check command line can be passed as it is; they change no setting.
  for (const char* name : {"files", "--show-evidence", "--exit-zero", "--since", "--statistics",
                           "--buffer", "--as-of", "--actions", "--meta", "--market", "--restore",
                           "--restore-actions", "--labels", "--color"}) {
    show->get_option(name)->group("Check options that change no setting");
  }
  show->get_option("files")->description("Input files (read by check, not here)");
  CLI::App* init = config->add_subcommand("init", "Write a starter dorq.toml");
  std::string init_path = "dorq.toml";
  bool init_force = false;
  init->add_option("path", init_path, "Where to write it")->capture_default_str();
  init->add_flag("--force", init_force, "Overwrite an existing file");

  // version
  CLI::App* version = app.add_subcommand("version", "Print the version and build information");
  std::string version_format = "text";
  version->add_option("--format", version_format, "Output format")
      ->check(CLI::IsMember({"text", "json"}))
      ->capture_default_str();

  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& error) {
    // --help and --version arrive here too, as "errors" CLI11 reports as success.
    const int status = app.exit(error, io.out, io.err);
    return status == 0 ? to_int(ExitCode::kOk) : to_int(ExitCode::kUsage);
  }

  try {
    if (check->parsed()) {
      return run_check(check_options, *check, io);
    }
    if (calibrate_cmd->parsed()) {
      return run_calibrate(calibrate_command, *calibrate_cmd, io);
    }
    if (list->parsed()) {
      return run_list_checks(list_format, io);
    }
    if (explain->parsed()) {
      return run_explain(explain_what, io);
    }
    if (show->parsed()) {
      return run_config_show(show_options, *show, io);
    }
    if (init->parsed()) {
      return run_config_init(init_path, init_force, io);
    }
    if (version->parsed()) {
      if (version_format == "json") {
        io.out << to_json(build_info()) << '\n';
      } else {
        io.out << to_text(build_info());
      }
    }
    return to_int(ExitCode::kOk);
  } catch (const UsageError& error) {
    io.err << "dorq: error: " << error.what() << "\n";
    return to_int(ExitCode::kUsage);
  } catch (const ConfigError& error) {
    io.err << "dorq: error: " << error.what() << "\n";
    return to_int(ExitCode::kUsage);
  } catch (const CalendarError& error) {
    io.err << "dorq: error: " << error.what() << "\n";
    return to_int(ExitCode::kUsage);
  } catch (const InputError& error) {
    io.out.flush();
    io.err << "dorq: error: " << error.what() << "\n";
    return to_int(ExitCode::kInput);
  }
}

}  // namespace dorq::cli
