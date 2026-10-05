# Run at build time (cmake -P) to compile doc/checks/*.md into the binary, so
# `dorq explain` prints exactly the page in the repository.
#
# Inputs: DOC_DIR (doc/checks), OUTPUT (the .cpp to write).

file(GLOB pages "${DOC_DIR}/DQ*.md")
list(SORT pages)

set(entries "")
set(count 0)
foreach(page IN LISTS pages)
  get_filename_component(code "${page}" NAME_WE)
  file(READ "${page}" text)
  string(FIND "${text}" ")dorqdoc\"" clash)
  if(NOT clash EQUAL -1)
    message(FATAL_ERROR "${page} contains the raw-string delimiter )dorqdoc\"")
  endif()
  string(APPEND entries "    Doc{\"${code}\", R\"dorqdoc(${text})dorqdoc\"},\n")
  math(EXPR count "${count} + 1")
endforeach()

set(content "// Generated at build time from doc/checks/*.md by cmake/EmbedCheckDocs.cmake.
// Do not edit: edit the pages.
#include <array>
#include <string_view>

#include \"checks/check.hpp\"

namespace dorq {
namespace {

struct Doc {
  std::string_view code;
  std::string_view text;
};

constexpr std::array<Doc, ${count}> kDocs = {
${entries}};

}  // namespace

std::string_view check_doc(std::string_view code) {
  for (const Doc& doc : kDocs) {
    if (doc.code == code) {
      return doc.text;
    }
  }
  return {};
}

}  // namespace dorq
")

# Only touch the file when it changes, so an unchanged tree rebuilds nothing.
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" previous)
  if(previous STREQUAL content)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
