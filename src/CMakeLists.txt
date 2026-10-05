# Everything but main() lives in dorq_lib, so the tests link exactly the code the
# binary runs.
add_library(dorq_lib STATIC cli/app.cpp core/build_info.cpp "${DORQ_BUILD_INFO_CPP}")
add_dependencies(dorq_lib dorq_build_info)
target_include_directories(
  dorq_lib
  PUBLIC "${PROJECT_SOURCE_DIR}/include" "${DORQ_GENERATED_INCLUDE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dorq_lib PRIVATE CLI11::CLI11)
dorq_configure_target(dorq_lib)

add_executable(dorq cli/main.cpp)
target_include_directories(dorq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dorq PRIVATE dorq_lib)
dorq_configure_target(dorq)

# A release binary that carries its own C++ runtime runs on any Linux whose glibc is
# at least as new as the build host's, whatever libstdc++ that host has installed.
if(DORQ_STATIC_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(dorq PRIVATE -static-libstdc++ -static-libgcc)
endif()
