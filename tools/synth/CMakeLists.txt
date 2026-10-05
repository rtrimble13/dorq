# dorq-synth: synthetic market data with labelled faults, and a scorer
# (tools/synth/README.md). Built with the tests; not installed.
add_executable(dorq-synth synth.cpp)
target_include_directories(dorq-synth PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dorq-synth PRIVATE dorq_lib)
dorq_configure_target(dorq-synth)
