# The newest-bar gate (DE-13): dorq-synth's newest universe as of the night its
# events are on the last bars (end.csv), and ten sessions on (later.csv), each
# scored against its own gates (tools/synth/gates-newest-end.txt, -later.txt).
#
#   cmake -DSYNTH=... -DDORQ=... -DGATES_DIR=... -DCONFIG=... -DOUT=... -DSEED=N
#         -P synth_newest_gate.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${SYNTH}" newest --seed "${SEED}" --out "${OUT}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-synth newest failed (${status})")
endif()
foreach(view end later)
  execute_process(
    COMMAND "${DORQ}" "${OUT}/${view}.csv" --config "${CONFIG}" --exit-zero --format csv
            --show-info --threads 4
    OUTPUT_FILE "${OUT}/${view}-results.csv"
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "dorq failed on ${view}.csv (${status})")
  endif()
  message(STATUS "newest universe, ${view}:")
  execute_process(
    COMMAND "${SYNTH}" score --labels "${OUT}/labels.csv" --results "${OUT}/${view}-results.csv"
            --gates "${GATES_DIR}/gates-newest-${view}.txt" --verbose
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "the newest-bar gates (${view}) failed")
  endif()
endforeach()
