#include "cli/app.hpp"

#include <ostream>
#include <span>
#include <string>

#include <CLI/CLI.hpp>

#include "dorq/build_info.hpp"
#include "dorq/exit_code.hpp"
#include "dorq/version.hpp"

namespace dorq::cli {
namespace {

constexpr const char* kDescription =
    "dorq: a Bayesian data-quality linter for financial time series.\n"
    "Reads OHLCV bars or single-value series, runs a battery of checks, and reports "
    "the observations most likely to be data errors.";

constexpr const char* kFooter =
    "Exit status: 0 nothing to report, 1 violations found, 2 usage or configuration "
    "error, 3 unreadable input.\n"
    "Documentation: https://github.com/rtrimble13/dorq";

}  // namespace

int run(std::span<const char* const> args, std::ostream& out, std::ostream& err) {
  CLI::App app{kDescription, "dorq"};
  app.footer(kFooter);
  // Bare, so a script can compare it to a tag: test "$(dorq --version)" = 0.0.1
  app.set_version_flag("--version", std::string{kVersion}, "Print the version and exit");

  // Until `check` arrives in M1 and becomes the default, a command is required.
  app.require_subcommand(1);

  CLI::App* version = app.add_subcommand("version", "Print the version and build information");
  std::string format = "text";
  version->add_option("--format", format, "Output format")
      ->check(CLI::IsMember({"text", "json"}))
      ->capture_default_str();

  try {
    app.parse(static_cast<int>(args.size()), args.data());
  } catch (const CLI::ParseError& error) {
    // --help and --version arrive here too, as "errors" CLI11 reports as success.
    const int status = app.exit(error, out, err);
    return status == 0 ? to_int(ExitCode::kOk) : to_int(ExitCode::kUsage);
  }

  if (version->parsed()) {
    if (format == "json") {
      out << to_json(build_info()) << '\n';
    } else {
      out << to_text(build_info());
    }
  }
  return to_int(ExitCode::kOk);
}

}  // namespace dorq::cli
