# Generates a synthetic universe, checks it with dorq, and scores the result
# against the injected faults' labels and tools/synth/gates.txt, with both
# dorq-synth score and dorq-eval (which also writes report.md and report.html).
#
#   cmake -DSYNTH=... -DDORQ=... -DEVAL=... -DGATES=... -DCONFIG=... -DOUT=... -DSEED=N
#         -P synth_gate.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${SYNTH}" generate --seed "${SEED}" --out "${OUT}"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-synth generate failed (${status})")
endif()
execute_process(
  COMMAND "${DORQ}" "${OUT}/bars.csv" "${OUT}/points.csv" --config "${CONFIG}" --actions
          "${OUT}/actions.csv" --meta "${OUT}/meta.csv" --market "${OUT}/market.csv" --exit-zero
          --format csv --show-info --threads 4
  OUTPUT_FILE "${OUT}/results.csv"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq failed (${status})")
endif()
execute_process(
  COMMAND "${SYNTH}" score --labels "${OUT}/labels.csv" --results "${OUT}/results.csv" --gates
          "${GATES}" --verbose
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "the synthetic data gates failed")
endif()
execute_process(
  COMMAND "${EVAL}" --labels "${OUT}/labels.jsonl" --results "${OUT}/results.csv" --complete
          --gates "${GATES}" --markdown "${OUT}/report.md" --html "${OUT}/report.html" --title
          "dorq-synth seed ${SEED}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-eval: the synthetic data gates failed (${status})")
endif()
