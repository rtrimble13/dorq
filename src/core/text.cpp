#include "core/text.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

#include "dorq/number.hpp"

namespace dorq {

void append_json_string(std::string& out, std::string_view text) {
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  out.push_back('"');
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out += R"(\")";
        break;
      case '\\':
        out += R"(\\)";
        break;
      case '\n':
        out += R"(\n)";
        break;
      case '\r':
        out += R"(\r)";
        break;
      case '\t':
        out += R"(\t)";
        break;
      default: {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20U) {
          out += R"(\u00)";
          out.push_back(kHex.at(byte >> 4U));
          out.push_back(kHex.at(byte & 0x0FU));
        } else {
          out.push_back(ch);
        }
      }
    }
  }
  out.push_back('"');
}

std::string with_commas(long long value) {
  std::string digits = std::to_string(value < 0 ? -value : value);
  for (auto i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
    digits.insert(static_cast<std::size_t>(i), ",");
  }
  return value < 0 ? "-" + digits : digits;
}

std::string percent(double share) { return format_number(std::round(share * 1000.0) / 10.0) + "%"; }

std::string abbreviate(std::string_view text, std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return std::string{text};
  }
  if (max_chars <= 3) {
    return {std::string(max_chars, '.')};
  }
  // Do not cut a UTF-8 sequence in half.
  std::size_t cut = max_chars - 3;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  std::string out{text.substr(0, cut)};
  out += "...";
  return out;
}

}  // namespace dorq
