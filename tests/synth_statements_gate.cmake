# Generates dorq-synth's statements universe twice and holds the files byte for
# byte; holds its identities on the rows with no fault (dorq-synth
# verify-statements); checks it with dorq; and scores the result against the
# labels and tools/synth/gates.txt with both dorq-synth score and dorq-eval. fafnir's
# statements configuration must name only items fafnir's taxonomy lists.
#
#   cmake -DSYNTH=... -DDORQ=... -DEVAL=... -DGATES=... -DCONFIG=... -DFAFNIR_CONFIG=...
#         -DOUT=... -DSEED=N -P synth_statements_gate.cmake
file(REMOVE_RECURSE "${OUT}" "${OUT}-again")
foreach(dir "${OUT}" "${OUT}-again")
  execute_process(COMMAND "${SYNTH}" statements --seed "${SEED}" --out "${dir}"
                  RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "dorq-synth statements failed (${status})")
  endif()
endforeach()
foreach(name statements.csv items.csv actions.csv meta.csv labels.csv labels.jsonl)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}/${name}"
                          "${OUT}-again/${name}" RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "seed ${SEED} gave a different ${name} the second time")
  endif()
endforeach()
execute_process(COMMAND "${SYNTH}" verify-statements --dir "${OUT}" --config "${CONFIG}"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "the statements universe breaks a rule on a row with no fault")
endif()
execute_process(
  COMMAND "${DORQ}" "${OUT}/statements.csv" --items "${OUT}/items.csv" --actions
          "${OUT}/actions.csv" --meta "${OUT}/meta.csv" --config "${CONFIG}"
          --exit-zero --format csv --show-info --threads 4
  OUTPUT_FILE "${OUT}/results.csv"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq failed (${status})")
endif()
execute_process(
  COMMAND "${DORQ}" "${OUT}/statements.csv" --items "${OUT}/items.csv" --config
          "${FAFNIR_CONFIG}" --exit-zero --format fafnir
  OUTPUT_QUIET
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq with fafnir's statements configuration failed (${status})")
endif()
execute_process(
  COMMAND "${SYNTH}" score --labels "${OUT}/labels.csv" --results "${OUT}/results.csv" --gates
          "${GATES}" --verbose
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "the synthetic statements gates failed")
endif()
execute_process(
  COMMAND "${EVAL}" --labels "${OUT}/labels.jsonl" --results "${OUT}/results.csv" --complete
          --gates "${GATES}" --markdown "${OUT}/report.md" --html "${OUT}/report.html" --title
          "dorq-synth statements, seed ${SEED}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-eval: the synthetic statements gates failed (${status})")
endif()
