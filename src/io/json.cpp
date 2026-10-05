#include "io/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include "io/input_error.hpp"

namespace dorq {
namespace {

class Cursor {
 public:
  Cursor(std::string_view text, std::size_t& pos, const std::string& where) noexcept
      : text_(text), pos_(pos), where_(where) {}

  [[noreturn]] void fail(const std::string& what) const {
    throw InputError("invalid JSON at " + where_ + ": " + what);
  }

  void skip_ws() noexcept { skip_json_whitespace(text_, pos_); }

  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

  [[nodiscard]] char peek() const {
    if (at_end()) {
      fail("unexpected end of input");
    }
    return text_[pos_];
  }

  void expect(char ch) {
    skip_ws();
    if (peek() != ch) {
      fail(std::string("expected '") + ch + "'");
    }
    ++pos_;
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      const char ch = peek();
      ++pos_;
      if (ch == '"') {
        return out;
      }
      if (static_cast<unsigned char>(ch) < 0x20U) {
        fail("control character in string");
      }
      if (ch != '\\') {
        out.push_back(ch);
        continue;
      }
      const char esc = peek();
      ++pos_;
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          append_utf8(out, parse_unicode_escape());
          break;
        default:
          fail("invalid escape in string");
      }
    }
  }

  // A number, true, false or null, returned as its raw text.
  std::string_view parse_bare_token() {
    const std::size_t start = pos_;
    while (!at_end()) {
      const char ch = text_[pos_];
      const bool token_char = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                              (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '+' || ch == '.';
      if (!token_char) {
        break;
      }
      ++pos_;
    }
    if (pos_ == start) {
      fail("expected a value");
    }
    return text_.substr(start, pos_ - start);
  }

 private:
  std::uint32_t parse_hex4() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = peek();
      ++pos_;
      value <<= 4U;
      if (ch >= '0' && ch <= '9') {
        value |= static_cast<std::uint32_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        value |= static_cast<std::uint32_t>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        value |= static_cast<std::uint32_t>(ch - 'A' + 10);
      } else {
        fail("invalid \\u escape");
      }
    }
    return value;
  }

  std::uint32_t parse_unicode_escape() {
    const std::uint32_t first = parse_hex4();
    if (first >= 0xD800U && first <= 0xDBFFU) {  // high surrogate: needs a low one
      if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
        pos_ += 2;
        const std::uint32_t second = parse_hex4();
        if (second >= 0xDC00U && second <= 0xDFFFU) {
          return 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
        }
      }
      return 0xFFFDU;  // unpaired: the replacement character
    }
    if (first >= 0xDC00U && first <= 0xDFFFU) {
      return 0xFFFDU;
    }
    return first;
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80U) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
      out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
      out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
  }

  std::string_view text_;
  std::size_t& pos_;
  const std::string& where_;
};

}  // namespace

void skip_json_whitespace(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
    ++pos;
  }
}

JsonRecord parse_json_record(std::string_view text, std::size_t& pos, const std::string& where) {
  Cursor cursor(text, pos, where);
  JsonRecord record;
  cursor.expect('{');
  cursor.skip_ws();
  if (cursor.peek() == '}') {
    cursor.expect('}');
    return record;
  }
  while (true) {
    JsonMember member;
    member.key = cursor.parse_string();
    cursor.expect(':');
    cursor.skip_ws();
    const char ch = cursor.peek();
    if (ch == '"') {
      member.text = cursor.parse_string();
    } else if (ch == '{' || ch == '[') {
      cursor.fail("nested value for key \"" + member.key + "\"; dorq reads flat records");
    } else {
      const std::string_view token = cursor.parse_bare_token();
      if (token == "null") {
        member.is_null = true;
      } else {
        member.text = std::string{token};
      }
    }
    record.push_back(std::move(member));
    cursor.skip_ws();
    if (cursor.peek() == ',') {
      cursor.expect(',');
      continue;
    }
    cursor.expect('}');
    return record;
  }
}

}  // namespace dorq
