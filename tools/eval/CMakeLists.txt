# dorq-eval: dorq's output against labels (tools/eval/README.md). Built with the
# tests; not installed.
add_executable(dorq-eval eval.cpp)
target_include_directories(dorq-eval PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dorq-eval PRIVATE dorq_lib)
dorq_configure_target(dorq-eval)
