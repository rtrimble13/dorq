# Run at build time (cmake -P) by the dorq_build_info target. See BuildInfo.cmake.
#
# Inputs: SOURCE_DIR, INPUT, OUTPUT, DORQ_GIT_COMMIT, DORQ_BUILD_TYPE, DORQ_COMPILER,
# DORQ_SYSTEM, VERSION (project()'s), DORQ_RELEASE (ON, OFF, or empty: ask git).

set(commit "${DORQ_GIT_COMMIT}")
set(dirty "false")
set(from_git FALSE)

# A packager building from a tarball can pass DORQ_GIT_COMMIT; otherwise ask git.
if(commit STREQUAL "")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE commit
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      set(commit "")
    else()
      set(from_git TRUE)
      # Modified and new files both count: a source file not yet committed is
      # still compiled in. Ignored files (the build directory) do not.
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      if(NOT status STREQUAL "")
        set(dirty "true")
      endif()
    endif()
  endif()
endif()

if(commit STREQUAL "")
  set(commit "unknown")
endif()

# The version the binary reports. A build of the release is one of a clean tree at
# the tag v<VERSION>; one of a source tree without git (a release archive, or a
# packager's DORQ_GIT_COMMIT) is taken at its word. Anything else is <VERSION>-dev:
# between releases, CMakeLists.txt already carries the next release's number
# (scripts/check_version.sh), and a binary built from main must not claim it.
set(release "${DORQ_RELEASE}")
if(release STREQUAL "")
  set(release ON)
  if(from_git)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "v${VERSION}" HEAD
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_QUIET ERROR_QUIET
      RESULT_VARIABLE tagged)
    if(NOT tagged EQUAL 0 OR dirty)
      set(release OFF)
    endif()
  endif()
endif()
if(release)
  set(reported_version "${VERSION}")
  set(release "true")
else()
  set(reported_version "${VERSION}-dev")
  set(release "false")
endif()

configure_file("${INPUT}" "${OUTPUT}" @ONLY)
