#include "dorq/build_info.hpp"

#include <array>
#include <string>
#include <string_view>

namespace dorq {
namespace {

// Appends `text` as a JSON string literal, quotes included (RFC 8259, section 7).
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

std::string commit_label(const BuildInfo& info) {
  std::string label{info.commit};
  if (info.dirty) {
    label += "+dirty";
  }
  return label;
}

}  // namespace

std::string to_text(const BuildInfo& info) {
  std::string out;
  out += "dorq ";
  out += info.version;
  out += "\ncommit    ";
  out += commit_label(info);
  out += "\nbuild     ";
  out += info.build_type;
  out += "\ncompiler  ";
  out += info.compiler;
  out += "\nsystem    ";
  out += info.system;
  out += '\n';
  return out;
}

std::string to_json(const BuildInfo& info) {
  std::string out;
  out += R"({"version":)";
  append_json_string(out, info.version);
  out += R"(,"commit":)";
  append_json_string(out, info.commit);
  out += R"(,"dirty":)";
  out += info.dirty ? "true" : "false";
  out += R"(,"build_type":)";
  append_json_string(out, info.build_type);
  out += R"(,"compiler":)";
  append_json_string(out, info.compiler);
  out += R"(,"system":)";
  append_json_string(out, info.system);
  out += '}';
  return out;
}

}  // namespace dorq
