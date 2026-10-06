#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dorq {

[[nodiscard]] constexpr char ascii_lower(char ch) noexcept {
  return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

// A column name for matching: trimmed, lower case, without spaces, '_' or '-'
// ("Trade_Date" -> "tradedate").
[[nodiscard]] std::string normalize_name(std::string_view name);

// The first column whose normalized name is one of `aliases` (tried in order),
// or -1.
[[nodiscard]] int find_column(const std::vector<std::string>& names,
                              std::span<const std::string_view> aliases);

// true/t/1/yes/y or false/f/0/no/n, any case. Returns false for anything else.
[[nodiscard]] bool parse_bool(std::string_view text, bool& out);

// Appends `text` as a JSON string literal, quotes included (RFC 8259, section 7).
void append_json_string(std::string& out, std::string_view text);

// 1234567 -> "1,234,567".
[[nodiscard]] std::string with_commas(long long value);
// A count or volume, rounded, with commas; one beyond 10^18 (or not finite) is
// written as format_number writes it.
[[nodiscard]] std::string whole_number(double value);

// 0.2618 -> "26.2%".
[[nodiscard]] std::string percent(double share);

// Shortens `text` to at most `max_chars` bytes, marking a cut with "...".
[[nodiscard]] std::string abbreviate(std::string_view text, std::size_t max_chars);

}  // namespace dorq
