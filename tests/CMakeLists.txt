# Unit tests: in-process, through dorq_lib.
add_executable(
  dorq_tests
  unit/test_build_info.cpp
  unit/test_calendar.cpp
  unit/test_cli.cpp
  unit/test_csv_json.cpp
  unit/test_date_number.cpp
  unit/test_reader.cpp
  unit/test_stats.cpp
  unit/test_checks.cpp
  unit/test_config.cpp
  unit/test_coverage.cpp)
target_include_directories(dorq_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dorq_tests PRIVATE dorq_lib doctest::doctest_with_main)
dorq_configure_target(dorq_tests)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(dorq_tests TEST_PREFIX "unit.")

# CLI tests: the real binary, as a script or job would run it. Every check run is
# --isolated, so a config file on the machine cannot change the result.
add_test(NAME cli.version-flag COMMAND dorq --version)
set_tests_properties(cli.version-flag PROPERTIES PASS_REGULAR_EXPRESSION
                                                 "^${PROJECT_VERSION}\n$")

set(DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")

function(dorq_cli_exit_test name expected)
  list(JOIN ARGN "|" args)
  add_test(NAME cli.${name}
           COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:dorq>" "-DEXPECTED=${expected}"
                   "-DARGS=${args}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/expect_exit.cmake")
endfunction()

dorq_cli_exit_test(help 0 --help)
dorq_cli_exit_test(version-json 0 version --format json)
dorq_cli_exit_test(list-checks 0 list-checks)
dorq_cli_exit_test(explain 0 explain DQ101)
dorq_cli_exit_test(unknown-option 2 --no-such-option)
dorq_cli_exit_test(unknown-check 2 --isolated --select DQ999x ${DATA}/clean.csv)
dorq_cli_exit_test(clean 0 --isolated ${DATA}/clean.csv)
dorq_cli_exit_test(missing-file 3 --isolated ${DATA}/no-such-file.csv)
dorq_cli_exit_test(no-date-column 3 --isolated ${DATA}/no_date.csv)
dorq_cli_exit_test(exit-zero 0 --isolated --exit-zero ${DATA}/bad_bars.csv)
foreach(format text json jsonl csv fafnir)
  dorq_cli_exit_test(violations-${format} 1 --isolated --format ${format} ${DATA}/bad_bars.csv)
endforeach()
