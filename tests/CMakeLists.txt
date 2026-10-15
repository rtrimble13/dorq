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
  unit/test_coverage.cpp
  unit/test_price.cpp
  unit/test_volume_stale.cpp
  unit/test_context.cpp
  unit/test_calibrate.cpp)
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
# The configuration shipped for fafnir (priors/fafnir.toml) must stay valid.
dorq_cli_exit_test(fafnir-config 0 config show --config
                   "${PROJECT_SOURCE_DIR}/priors/fafnir.toml")
# config show takes a check command line as it is (fafnir's, here), to print the
# config_hash that run reports. It reads no input or context file.
dorq_cli_exit_test(
  config-show-check-options 0 config show --config "${PROJECT_SOURCE_DIR}/priors/fafnir.toml" -
  --format fafnir --exit-zero --calendar-exchange NASDAQ --actions actions.csv --meta meta.csv
  --restore-actions before-actions.csv --as-of 2026-10-09 --since 2026-10-02)
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

# dorq-eval on fafnir-format and jsonl results of two nights: distinct conditions
# (DQ601's record_key ratio keeps two cohorts apart), a cohort label matched by its
# members, applies_until against --input-time, and verified labels. Within an
# expected line, "/" stands for the table's "|".
function(dorq_eval_test name args expect)
  add_test(NAME cli.${name}
           COMMAND "${CMAKE_COMMAND}" "-DEVAL=$<TARGET_FILE:dorq-eval>" "-DARGS=${args}"
                   "-DEXPECT=${expect}" -P "${CMAKE_CURRENT_SOURCE_DIR}/eval_test.cmake")
endfunction()
set(EVAL_ARGS
    "--labels|${DATA}/eval/labels.jsonl|--results|${DATA}/eval/night-1.jsonl|--results|${DATA}/eval/night-2.jsonl"
)
dorq_eval_test(
  eval-nights "${EVAL_ARGS}"
  "night-1.jsonl / warn and above / 5 / 4 / 0.800 / 1 / 1 / 2 / 1 / 0.250 / 0.500|distinct conditions / warn and above / 6 / 5 / 0.833 / 2 / 1 / 2 / 1 / 0.400 / 0.600|distinct conditions / error / 4 / 4 / 1.000 / 2 / 1 / 1 / 0 / 0.500 / 0.750|verified labels (3) / warn and above / 6 / 4 / 0.667 / 1 / 1 / 2 / 2 / 0.250 / 0.500"
)
dorq_eval_test(
  eval-input-time "${EVAL_ARGS}|--input-time|2026-10-14"
  "distinct conditions / warn and above / 6 / 4 / 0.667 / 1 / 1 / 2 / 2 / 0.250 / 0.500")

# The synthetic-data gate (DR-0308): precision and recall per check on dorq-synth
# universes, against tools/synth/gates.txt.
foreach(seed 1 2)
  add_test(
    NAME synth.gate-seed-${seed}
    COMMAND
      "${CMAKE_COMMAND}" "-DSYNTH=$<TARGET_FILE:dorq-synth>" "-DDORQ=$<TARGET_FILE:dorq>"
      "-DEVAL=$<TARGET_FILE:dorq-eval>"
      "-DGATES=${PROJECT_SOURCE_DIR}/tools/synth/gates.txt"
      "-DCONFIG=${PROJECT_SOURCE_DIR}/tools/synth/dorq.toml"
      "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/synth-${seed}" "-DSEED=${seed}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/synth_gate.cmake")
endforeach()
