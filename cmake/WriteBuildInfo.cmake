# Run at build time (cmake -P) by the dorq_build_info target. See BuildInfo.cmake.
#
# Inputs: SOURCE_DIR, INPUT, OUTPUT, DORQ_GIT_COMMIT, DORQ_BUILD_TYPE, DORQ_COMPILER,
# DORQ_SYSTEM.

set(commit "${DORQ_GIT_COMMIT}")
set(dirty "false")

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

configure_file("${INPUT}" "${OUTPUT}" @ONLY)
