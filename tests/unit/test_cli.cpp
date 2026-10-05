#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "cli/app.hpp"
#include "dorq/exit_code.hpp"
#include "dorq/version.hpp"

namespace {

struct Result {
  int status;
  std::string out;
  std::string err;
};

Result run(std::initializer_list<const char*> args) {
  std::vector<const char*> argv{"dorq"};
  argv.insert(argv.end(), args);
  std::ostringstream out;
  std::ostringstream err;
  const int status = dorq::cli::run(argv, out, err);
  return {status, out.str(), err.str()};
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("exit codes are a fixed contract") {
  CHECK(dorq::to_int(dorq::ExitCode::kOk) == 0);
  CHECK(dorq::to_int(dorq::ExitCode::kViolations) == 1);
  CHECK(dorq::to_int(dorq::ExitCode::kUsage) == 2);
  CHECK(dorq::to_int(dorq::ExitCode::kInput) == 3);
}

TEST_CASE("--version prints the bare version on stdout") {
  const Result result = run({"--version"});
  CHECK(result.status == 0);
  CHECK(result.out == std::string{dorq::kVersion} + "\n");
  CHECK(result.err.empty());
}

TEST_CASE("--help succeeds and documents the commands and exit codes") {
  const Result result = run({"--help"});
  CHECK(result.status == 0);
  CHECK(contains(result.out, "version"));
  CHECK(contains(result.out, "Exit status"));
}

TEST_CASE("no command is a usage error") {
  const Result result = run({});
  CHECK(result.status == dorq::to_int(dorq::ExitCode::kUsage));
  CHECK(result.out.empty());
  CHECK_FALSE(result.err.empty());
}

TEST_CASE("an unknown option or command is a usage error") {
  CHECK(run({"--no-such-option"}).status == dorq::to_int(dorq::ExitCode::kUsage));
  CHECK(run({"no-such-command"}).status == dorq::to_int(dorq::ExitCode::kUsage));
}

TEST_CASE("version prints build information as text by default") {
  const Result result = run({"version"});
  CHECK(result.status == 0);
  CHECK(result.out.rfind("dorq " + std::string{dorq::kVersion} + "\n", 0) == 0);
  CHECK(result.err.empty());
}

TEST_CASE("version --format json prints one JSON object") {
  const Result result = run({"version", "--format", "json"});
  CHECK(result.status == 0);
  REQUIRE(result.out.size() > 2);
  CHECK(result.out.front() == '{');
  CHECK(result.out.substr(result.out.size() - 2) == "}\n");
  CHECK(contains(result.out, R"("version":")" + std::string{dorq::kVersion} + R"(")"));
}

TEST_CASE("version rejects an unknown format") {
  const Result result = run({"version", "--format", "xml"});
  CHECK(result.status == dorq::to_int(dorq::ExitCode::kUsage));
  CHECK(result.out.empty());
  CHECK_FALSE(result.err.empty());
}
