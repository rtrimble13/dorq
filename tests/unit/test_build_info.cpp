#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "dorq/build_info.hpp"
#include "dorq/version.hpp"

namespace {

bool is_digits(std::string_view text) {
  return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
}

}  // namespace

TEST_CASE("the version is MAJOR.MINOR.PATCH and agrees with its components") {
  const std::string expected = std::to_string(dorq::kVersionMajor) + "." +
                               std::to_string(dorq::kVersionMinor) + "." +
                               std::to_string(dorq::kVersionPatch);
  CHECK(dorq::kVersion == expected);

  const auto first = dorq::kVersion.find('.');
  const auto last = dorq::kVersion.rfind('.');
  REQUIRE(first != std::string_view::npos);
  REQUIRE(last != first);
  CHECK(is_digits(dorq::kVersion.substr(0, first)));
  CHECK(is_digits(dorq::kVersion.substr(first + 1, last - first - 1)));
  CHECK(is_digits(dorq::kVersion.substr(last + 1)));
}

TEST_CASE("build_info describes this binary") {
  const dorq::BuildInfo& info = dorq::build_info();
  CHECK(info.version == dorq::kVersion);
  CHECK_FALSE(info.commit.empty());
  CHECK_FALSE(info.compiler.empty());
  CHECK_FALSE(info.system.empty());
}

TEST_CASE("to_text leads with the name and version") {
  const std::string text = dorq::to_text(dorq::build_info());
  CHECK(text.rfind("dorq " + std::string{dorq::kVersion} + "\n", 0) == 0);
  CHECK(text.find("\ncommit ") != std::string::npos);
  CHECK(text.back() == '\n');
}

TEST_CASE("to_text marks a dirty build") {
  dorq::BuildInfo info = dorq::build_info();
  info.commit = "abc123";
  info.dirty = true;
  CHECK(dorq::to_text(info).find("abc123+dirty") != std::string::npos);
}

TEST_CASE("to_json is one object on one line, fields in a fixed order") {
  const dorq::BuildInfo info{
      .version = "1.2.3",
      .commit = "abc123",
      .dirty = false,
      .build_type = "Release",
      .compiler = "GNU 13.3.0",
      .system = "Linux x86_64",
  };
  CHECK(dorq::to_json(info) ==
        R"({"version":"1.2.3","commit":"abc123","dirty":false,"build_type":"Release",)"
        R"("compiler":"GNU 13.3.0","system":"Linux x86_64"})");
}

TEST_CASE("to_json escapes what JSON requires") {
  dorq::BuildInfo info = dorq::build_info();
  info.compiler = "a\"b\\c\nd\te\x01";
  info.dirty = true;
  const std::string json = dorq::to_json(info);
  CHECK(json.find(R"("compiler":"a\"b\\c\nd\te\u0001")") != std::string::npos);
  CHECK(json.find(R"("dirty":true)") != std::string::npos);
  CHECK(json.find('\n') == std::string::npos);
}
