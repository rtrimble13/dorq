# Unit tests: in-process, through dorq_lib.
add_executable(dorq_tests unit/test_build_info.cpp unit/test_cli.cpp)
target_include_directories(dorq_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dorq_tests PRIVATE dorq_lib doctest::doctest_with_main)
dorq_configure_target(dorq_tests)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(dorq_tests TEST_PREFIX "unit.")

# CLI tests: the real binary, as a script or job would run it.
add_test(NAME cli.version-flag COMMAND dorq --version)
set_tests_properties(cli.version-flag PROPERTIES PASS_REGULAR_EXPRESSION
                                                 "^${PROJECT_VERSION}\n$")

function(dorq_cli_exit_test name expected)
  list(JOIN ARGN "|" args)
  add_test(NAME cli.${name}
           COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:dorq>" "-DEXPECTED=${expected}"
                   "-DARGS=${args}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/expect_exit.cmake")
endfunction()

dorq_cli_exit_test(help 0 --help)
dorq_cli_exit_test(version-json 0 version --format json)
dorq_cli_exit_test(no-command 2)
dorq_cli_exit_test(unknown-option 2 --no-such-option)
dorq_cli_exit_test(bad-format 2 version --format xml)
