# The check pages, compiled in for `dorq explain` (cmake/EmbedCheckDocs.cmake). The
# command lives here, beside the target that compiles its output.
set(DORQ_CHECK_DOCS_CPP "${DORQ_GENERATED_DIR}/src/check_docs.cpp")
file(GLOB DORQ_CHECK_DOC_PAGES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/doc/checks/DQ*.md")
add_custom_command(
  OUTPUT "${DORQ_CHECK_DOCS_CPP}"
  COMMAND "${CMAKE_COMMAND}" "-DDOC_DIR=${PROJECT_SOURCE_DIR}/doc/checks"
          "-DOUTPUT=${DORQ_CHECK_DOCS_CPP}" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedCheckDocs.cmake"
  DEPENDS ${DORQ_CHECK_DOC_PAGES} "${PROJECT_SOURCE_DIR}/cmake/EmbedCheckDocs.cmake"
  COMMENT "Embedding check documentation"
  VERBATIM)

# Every generated source, without compiling anything: what clang-tidy needs before
# it can read the compile database (scripts/tidy.sh, the CI lint job).
add_custom_target(dorq_generated DEPENDS "${DORQ_CHECK_DOCS_CPP}")
add_dependencies(dorq_generated dorq_build_info)

# Everything but main() lives in dorq_lib, so the tests link exactly the code the
# binary runs.
add_library(
  dorq_lib STATIC
  checks/coverage.cpp
  checks/coverage_model.cpp
  checks/integrity.cpp
  checks/registry.cpp
  cli/app.cpp
  config/config.cpp
  core/build_info.cpp
  core/calendar.cpp
  core/date.cpp
  core/frequency.cpp
  core/number.cpp
  core/series.cpp
  core/text.cpp
  core/violation.cpp
  io/columns.cpp
  io/csv.cpp
  io/json.cpp
  io/reader.cpp
  engine/cross_section.cpp
  engine/engine.cpp
  stats/hmm2.cpp
  stats/special.cpp
  output/writer.cpp
  "${DORQ_BUILD_INFO_CPP}"
  "${DORQ_CHECK_DOCS_CPP}")
add_dependencies(dorq_lib dorq_build_info)
target_include_directories(
  dorq_lib
  PUBLIC "${PROJECT_SOURCE_DIR}/include" "${DORQ_GENERATED_INCLUDE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(dorq_lib PRIVATE CLI11::CLI11 FastFloat::fast_float
                                       tomlplusplus::tomlplusplus Threads::Threads)
dorq_configure_target(dorq_lib)

add_executable(dorq cli/main.cpp)
target_include_directories(dorq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dorq PRIVATE dorq_lib)
dorq_configure_target(dorq)

# A release binary that carries its own C++ runtime runs on any Linux whose glibc is
# at least as new as the build host's, whatever libstdc++ that host has installed.
if(DORQ_STATIC_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(dorq PRIVATE -static-libstdc++ -static-libgcc)
endif()
