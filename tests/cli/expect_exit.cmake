# Runs the real binary and checks its exit status, which CTest alone cannot do for a
# status other than zero. Invoked by tests/CMakeLists.txt as
#   cmake -DEXE=<dorq> -DEXPECTED=<status> -DARGS=<a|b|c> -P expect_exit.cmake
# ARGS is '|'-separated so it survives being one -D value.

string(REPLACE "|" ";" args "${ARGS}")
execute_process(
  COMMAND "${EXE}" ${args}
  RESULT_VARIABLE status
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT status STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "dorq ${args}: expected exit ${EXPECTED}, got ${status}\n"
                      "stdout:\n${out}\nstderr:\n${err}")
endif()
