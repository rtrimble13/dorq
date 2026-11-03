# DR-0911's acceptance on dorq-synth: the statement models fitted on seed 1's labels
# (dorq calibrate, with a held-out share) and applied to seed 2. Seed 2 must still
# meet tools/synth/gates.txt with the fit, and dorq-eval's expected calibration
# error over the statement checks with a posterior must be at most 0.05 and no
# worse than without it. The universes are the statements gate's (fixtures).
#
#   cmake -DSYNTH=... -DDORQ=... -DEVAL=... -DGATES=... -DCONFIG=... -DTRAIN=...
#         -DTEST=... -DOUT=... -P synth_statements_calibrate.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(
  COMMAND "${DORQ}" calibrate "${TRAIN}/statements.csv" --labels "${TRAIN}/labels.jsonl"
          --items "${TRAIN}/items.csv" --actions "${TRAIN}/actions.csv" --meta
          "${TRAIN}/meta.csv" --config "${CONFIG}" --as-of 2026-10-30 --clean-unlabelled
          --holdout 0.3 --threads 4 --name seed-1 --out "${OUT}/fit.toml"
  OUTPUT_FILE "${OUT}/calibrate.txt"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq calibrate failed (${status})")
endif()
file(READ "${OUT}/fit.toml" fit)
foreach(section "[statements.priors]" "[statements.calibration]")
  string(FIND "${fit}" "${section}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "the fit has no ${section}")
  endif()
endforeach()
file(WRITE "${OUT}/fitted.toml" "include = [\"${CONFIG}\", \"${OUT}/fit.toml\"]\n")

foreach(run plain fitted)
  if(run STREQUAL "plain")
    set(config "${CONFIG}")
  else()
    set(config "${OUT}/fitted.toml")
  endif()
  execute_process(
    COMMAND "${DORQ}" "${TEST}/statements.csv" --items "${TEST}/items.csv" --actions
            "${TEST}/actions.csv" --meta "${TEST}/meta.csv" --config "${config}" --exit-zero
            --format csv --show-info --threads 4 --as-of 2026-10-30
    OUTPUT_FILE "${OUT}/results-${run}.csv"
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "dorq (${run}) failed (${status})")
  endif()
  execute_process(
    COMMAND "${EVAL}" --labels "${TEST}/labels.jsonl" --results "${OUT}/results-${run}.csv"
            --complete --markdown "${OUT}/report-${run}.md"
    OUTPUT_QUIET
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "dorq-eval (${run}) failed (${status})")
  endif()
  file(STRINGS "${OUT}/report-${run}.md" line REGEX "^Expected calibration error: \\*\\*")
  string(REGEX MATCH "[0-9]+\\.[0-9]+" ece_${run} "${line}")
  if(ece_${run} STREQUAL "")
    message(FATAL_ERROR "dorq-eval (${run}) reported no expected calibration error")
  endif()
endforeach()
execute_process(
  COMMAND "${SYNTH}" score --labels "${TEST}/labels.csv" --results "${OUT}/results-fitted.csv"
          --gates "${GATES}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "seed 2 with seed 1's fit fails the synthetic statements gates")
endif()
message(STATUS "expected calibration error: ${ece_plain} without the fit, ${ece_fitted} with it")
if(ece_fitted GREATER 0.05)
  message(FATAL_ERROR "expected calibration error ${ece_fitted} with the fit: above 0.05")
endif()
if(ece_fitted GREATER ece_plain)
  message(FATAL_ERROR "the fit raised the expected calibration error: ${ece_plain} to ${ece_fitted}")
endif()
