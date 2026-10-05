#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dorq {

// One member of a flat JSON object. Numbers keep their raw text, so the precision
// they were written with survives (DQ106 reads it). Strings are unescaped.
struct JsonMember {
  std::string key;
  std::string text;
  bool is_null = false;
};

using JsonRecord = std::vector<JsonMember>;

// Parses one flat JSON object -- members whose values are strings, numbers,
// booleans or null -- starting at `pos` (leading whitespace allowed) and advances
// `pos` past it. A nested object or array, or malformed JSON, is an InputError
// naming `where` (e.g. "line 12").
JsonRecord parse_json_record(std::string_view text, std::size_t& pos, const std::string& where);

// Skips JSON whitespace from `pos`.
void skip_json_whitespace(std::string_view text, std::size_t& pos) noexcept;

}  // namespace dorq
