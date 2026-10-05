# libFuzzer targets (Clang only). Built by the `fuzz` preset; CI runs each one for
# a short time on every pull request. Seeds: tests/data and fuzz/corpus.
foreach(name fuzz_reader fuzz_config fuzz_calendar)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE dorq_lib)
  dorq_configure_target(${name})
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endforeach()
