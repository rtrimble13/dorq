# Generates the translation unit behind dorq::build_info().
#
# Two halves. The version is fixed at configure time, into dorq/version.hpp, because
# it comes from project() and only changes when CMakeLists.txt does. The commit is
# recorded at BUILD time by WriteBuildInfo.cmake, which runs on every build: a
# `git pull && cmake --build` that does not re-run the configure step must still
# produce a binary that names the commit it was built from. configure_file only
# rewrites its output when the content changes, so an unchanged tree recompiles
# nothing.

set(DORQ_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
set(DORQ_GENERATED_INCLUDE_DIR "${DORQ_GENERATED_DIR}/include")
set(DORQ_BUILD_INFO_CPP "${DORQ_GENERATED_DIR}/src/build_info_data.cpp")

configure_file("${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
               "${DORQ_GENERATED_INCLUDE_DIR}/dorq/version.hpp" @ONLY)

add_custom_target(
  dorq_build_info
  COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DINPUT=${PROJECT_SOURCE_DIR}/cmake/build_info_data.cpp.in" "-DOUTPUT=${DORQ_BUILD_INFO_CPP}"
    "-DDORQ_GIT_COMMIT=${DORQ_GIT_COMMIT}" "-DDORQ_BUILD_TYPE=$<CONFIG>"
    "-DDORQ_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    "-DDORQ_SYSTEM=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}" -P
    "${PROJECT_SOURCE_DIR}/cmake/WriteBuildInfo.cmake"
  BYPRODUCTS "${DORQ_BUILD_INFO_CPP}"
  COMMENT "Recording build information"
  VERBATIM)

