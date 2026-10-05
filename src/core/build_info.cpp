#include "dorq/build_info.hpp"

#include <string>
#include <string_view>

#include "core/text.hpp"

namespace dorq {
namespace {

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
