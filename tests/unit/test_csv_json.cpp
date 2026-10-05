#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "io/csv.hpp"
#include "io/input_error.hpp"
#include "io/json.hpp"

namespace {

struct Record {
  std::vector<std::string> fields;
  std::uint32_t line;
};

std::vector<Record> parse_csv(std::string_view text, std::size_t chunk = 0, char delimiter = ',') {
  std::vector<Record> out;
  dorq::CsvParser parser(delimiter);
  const dorq::CsvParser::RecordFn fn = [&](std::span<const std::string_view> fields,
                                           std::uint32_t line) {
    out.push_back({std::vector<std::string>(fields.begin(), fields.end()), line});
  };
  if (chunk == 0) {
    parser.feed(text, fn);
  } else {
    for (std::size_t i = 0; i < text.size(); i += chunk) {
      parser.feed(text.substr(i, chunk), fn);
    }
  }
  parser.finish(fn);
  return out;
}

}  // namespace

TEST_CASE("csv: plain records, CRLF, blank lines, no final newline") {
  const auto records = parse_csv("a,b,c\r\n1,2,3\r\n\r\n4,,6");
  REQUIRE(records.size() == 3);
  CHECK(records[0].fields == std::vector<std::string>{"a", "b", "c"});
  CHECK(records[1].fields == std::vector<std::string>{"1", "2", "3"});
  CHECK(records[2].fields == std::vector<std::string>{"4", "", "6"});
  CHECK(records[2].line == 4);
}

TEST_CASE("csv: quoted fields with delimiters, quotes and newlines") {
  const auto records = parse_csv("x,y\n\"a,b\",\"say \"\"hi\"\"\"\n\"two\nlines\",z\nlast,1\n");
  REQUIRE(records.size() == 4);
  CHECK(records[1].fields == std::vector<std::string>{"a,b", "say \"hi\""});
  CHECK(records[2].fields == std::vector<std::string>{"two\nlines", "z"});
  CHECK(records[2].line == 3);
  CHECK(records[3].line == 5);
}

TEST_CASE("csv: the result does not depend on how the input is chunked") {
  const std::string text = "d,v\n\"q,\"\"x\"\"\",1\r\n2024-01-02,\"3\"\n\n5,6";
  const auto whole = parse_csv(text);
  for (std::size_t chunk = 1; chunk <= 7; ++chunk) {
    const auto pieces = parse_csv(text, chunk);
    REQUIRE(pieces.size() == whole.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
      CHECK(pieces[i].fields == whole[i].fields);
      CHECK(pieces[i].line == whole[i].line);
    }
  }
}

TEST_CASE("csv: tab-delimited, and a trailing empty field") {
  const auto records = parse_csv("a\tb\n1\t\n", 0, '\t');
  REQUIRE(records.size() == 2);
  CHECK(records[1].fields == std::vector<std::string>{"1", ""});
}

TEST_CASE("csv: an unterminated quote is an input error") {
  CHECK_THROWS_AS(parse_csv("a,b\n\"open,1\n2,3\n"), dorq::InputError);
}

TEST_CASE("json: a flat record keeps raw number text") {
  const std::string text = R"( {"date": "2024-01-02", "close": 10.500, "v": null, "ok": true,
                              "s": "a\"b\\cé😀"} )";
  std::size_t pos = 0;
  const auto record = dorq::parse_json_record(text, pos, "test");
  REQUIRE(record.size() == 5);
  CHECK(record[0].key == "date");
  CHECK(record[0].text == "2024-01-02");
  CHECK(record[1].text == "10.500");
  CHECK(record[2].is_null);
  CHECK(record[3].text == "true");
  CHECK(record[4].text == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80");
}

TEST_CASE("json: nested and malformed records are input errors") {
  for (const char* text : {R"({"a": {"b": 1}})", R"({"a": [1]})", R"({"a" 1})", R"({"a": 1,})",
                           R"({"a": "x)", R"({"a": "\q"})", "{", "[]"}) {
    CAPTURE(text);
    std::size_t pos = 0;
    CHECK_THROWS_AS(dorq::parse_json_record(text, pos, "test"), dorq::InputError);
  }
  std::size_t pos = 0;
  CHECK(dorq::parse_json_record("{}", pos, "test").empty());
}
