# Third-party dependencies, fetched at configure time.
#
# Each one is pinned to a commit rather than a tag: a tag can be moved, a commit
# cannot. The tag the commit was taken from is in the comment beside it. A new
# dependency belongs here only when the code that uses it lands, and must be
# header-only or small (doc/plans/dorq-development-plan.md, section 5.1).
#
# Building without network access: clone each repository at the pinned commit and
# pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=/path/to/checkout (NAME in upper case, e.g.
# FETCHCONTENT_SOURCE_DIR_CLI11). See README.md.

include(FetchContent)

# CLI11: command-line parsing. Header-only; its tests, examples, docs and install
# rules stay off because it is not the top-level project.
set(CLI11_BUILD_DOCS OFF)
set(CLI11_PRECOMPILED OFF)
FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG cbd58a3696887b34c70949aef21a71735a0c2ad5 # v2.7.2
  SYSTEM)
FetchContent_MakeAvailable(cli11)

if(DORQ_BUILD_TESTS)
  # doctest: the unit-test framework.
  set(DOCTEST_NO_INSTALL ON)
  set(DOCTEST_WITH_TESTS OFF)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG 2d0a9359a60c51affe2a9bebb1be1dca47868151 # v2.5.3
    SYSTEM)
  FetchContent_MakeAvailable(doctest)
endif()
