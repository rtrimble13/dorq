cmake_minimum_required(VERSION 3.25)

# The version is written here and nowhere else. The binary reports it
# (`dorq --version`), every output record will carry it, and the release
# workflow refuses a tag that disagrees with it. See CONTRIBUTING.md.
project(
  dorq
  VERSION 0.0.1
  DESCRIPTION "Bayesian data-quality linter for financial time series"
  HOMEPAGE_URL "https://github.com/rtrimble13/dorq"
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset dev")
endif()

option(DORQ_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(DORQ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DORQ_COVERAGE "Instrument for gcov-style coverage (GCC/Clang)" OFF)
option(DORQ_STATIC_RUNTIME "Link libstdc++ and libgcc statically (Linux only)" OFF)
set(DORQ_SANITIZERS
    ""
    CACHE STRING "Sanitizers to build with, as a list: address;undefined")
set(DORQ_GIT_COMMIT
    ""
    CACHE STRING "Commit to record in the binary; empty asks git at build time")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_property(_dorq_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _dorq_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(DorqTargetOptions)
include(Deps)
include(BuildInfo)

add_subdirectory(src)

if(DORQ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS dorq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
