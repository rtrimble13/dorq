# The bars output on a dorq-synth universe, held to the hashes in EXPECTED: work
# on another series kind (the statements of M9, DR-0901) must not change what a
# bars run reports. The JSON Lines output is hashed with dorq's version blanked
# and the input's directory removed; its config_hash stays in, so a release that
# changes the hash of a bars run's settings fails here too. The text output, with
# evidence and info, is hashed as it is.
#
# A change that is meant to move bars output (a price model fix) updates EXPECTED
# with the hashes this prints, and says so in its pull request.
#
#   cmake -DSYNTH=... -DDORQ=... -DCONFIG=... -DOUT=... -DEXPECTED=... -P bars_golden.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${SYNTH}" generate --seed 1 --out "${OUT}" OUTPUT_QUIET
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-synth generate failed (${status})")
endif()

set(inputs "${OUT}/bars.csv" "${OUT}/points.csv" --config "${CONFIG}" --actions
           "${OUT}/actions.csv" --meta "${OUT}/meta.csv" --market "${OUT}/market.csv")
execute_process(COMMAND "${DORQ}" ${inputs} --exit-zero --format jsonl --show-info --threads 4
                OUTPUT_FILE "${OUT}/results.jsonl" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq --format jsonl failed (${status})")
endif()
execute_process(
  COMMAND "${DORQ}" ${inputs} --exit-zero --format text --show-info --show-evidence --threads 4
  OUTPUT_FILE "${OUT}/results.txt" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq --format text failed (${status})")
endif()

file(READ "${OUT}/results.jsonl" jsonl)
string(REPLACE "${OUT}/" "" jsonl "${jsonl}")
string(REGEX REPLACE "\"dorq\":{\"version\":\"[^\"]*\"" "\"dorq\":{\"version\":\"\"" jsonl
                     "${jsonl}")
string(SHA256 jsonl_hash "${jsonl}")
file(SHA256 "${OUT}/results.txt" text_hash)

file(STRINGS "${EXPECTED}" expected REGEX "^(jsonl|text) ")
set(failed FALSE)
foreach(line IN LISTS expected)
  string(REGEX MATCH "^(jsonl|text) ([0-9a-f]+)" _ "${line}")
  set(format "${CMAKE_MATCH_1}")
  set(want "${CMAKE_MATCH_2}")
  if(NOT "${${format}_hash}" STREQUAL "${want}")
    message(SEVERE_WARNING "bars ${format} output changed: ${${format}_hash}, expected ${want}")
    set(failed TRUE)
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR
          "the bars output on dorq-synth seed 1 is not what it was (${OUT}). If the change "
          "is meant, put these in ${EXPECTED}:\njsonl ${jsonl_hash}\ntext ${text_hash}")
endif()
