#pragma once

#include <string>
#include <string_view>

namespace dorq {

// What this binary is and how it was built. `dorq version` prints it, and fafnir
// records it beside the flags a run writes, so a flag can always be traced to the
// code that raised it.
struct BuildInfo {
  // From project() in CMakeLists.txt, with "-dev" after it for a build that is not
  // the release: anything but a clean tree at the tag v<version> (or a source tree
  // without git, a release archive). What `dorq --version` and every record report.
  std::string_view version;
  bool release;                 // the version as tagged: no "-dev"
  std::string_view commit;      // short git commit at build time, or "unknown"
  bool dirty;                   // the source tree differed from `commit`
  std::string_view build_type;  // Release, Debug, ...
  std::string_view compiler;    // e.g. "GNU 13.3.0"
  std::string_view system;      // e.g. "Linux x86_64"
};

// Defined in a translation unit generated at build time (cmake/BuildInfo.cmake).
[[nodiscard]] const BuildInfo& build_info() noexcept;

// Multi-line, for people.
[[nodiscard]] std::string to_text(const BuildInfo& info);

// One JSON object on one line, for programs.
[[nodiscard]] std::string to_json(const BuildInfo& info);

}  // namespace dorq
