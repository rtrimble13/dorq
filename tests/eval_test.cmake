# Runs dorq-eval and checks that its output holds every expected line.
#
#   cmake -DEVAL=... -DARGS="a|b|c" -DEXPECT="line 1|line 2" -P eval_test.cmake
string(REPLACE "|" ";" args "${ARGS}")
execute_process(COMMAND "${EVAL}" ${args} OUTPUT_VARIABLE out ERROR_VARIABLE err
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "dorq-eval exited ${status}: ${err}")
endif()
string(REPLACE "|" ";" expected "${EXPECT}")
foreach(line IN LISTS expected)
  string(REPLACE "/" "|" line "${line}")
  string(FIND "${out}" "${line}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "expected \"${line}\" in:\n${out}")
  endif()
endforeach()
