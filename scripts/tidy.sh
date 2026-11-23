#!/usr/bin/env bash
# Runs clang-tidy over dorq's own sources, using the compile database of a
# configured build directory (default: build/ci).
#
#   cmake --preset ci && cmake --build --preset ci --target dorq_generated
#   scripts/tidy.sh [build-dir]                          # every source
#   scripts/tidy.sh [build-dir] --changed-since REF      # what changed since REF
#
# In GitHub Actions on a pull request (GITHUB_EVENT_NAME pull_request), it checks
# what the pull request changed against its base branch, fetched here, unless
# TIDY_ALL=1. A push to main checks every source.
#
# --changed-since checks only the translation units a change can affect (the
# whole tree takes about 8 minutes in CI):
#   - each .cpp changed since REF (git diff REF...HEAD);
#   - for each header changed, its own .cpp and every .cpp that includes it by
#     name ("dorq/series.hpp", "config/config.hpp"; a test's or tool's header by
#     its file name).
# A unit that reaches a changed header only through another header is not
# checked; the full run on main catches what that misses. A change to
# .clang-tidy, this script or the build's CMake files checks everything.
#
# Uses $RUN_CLANG_TIDY / $CLANG_TIDY if set, else the -18 versions, else unversioned.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
build_dir="build/ci"
since=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --changed-since)
      since="${2:?--changed-since needs a git ref}"
      shift 2
      ;;
    *)
      build_dir="$1"
      shift
      ;;
  esac
done

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  echo "${build_dir}/compile_commands.json not found; configure a preset first" >&2
  exit 2
fi

# A pull request in GitHub Actions: its base branch's tip. The checkout is the
# pull request's merge commit, one commit deep, so the diff is taken tree to tree.
if [[ -z "${since}" && "${GITHUB_EVENT_NAME:-}" == "pull_request" && -n "${GITHUB_BASE_REF:-}" &&
  "${TIDY_ALL:-}" != "1" ]]; then
  git fetch --quiet --no-tags --depth=1 origin \
    "+refs/heads/${GITHUB_BASE_REF}:refs/remotes/origin/${GITHUB_BASE_REF}"
  since="origin/${GITHUB_BASE_REF}"
fi

RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy-18 || command -v run-clang-tidy || true)}"
CLANG_TIDY="${CLANG_TIDY:-$(command -v clang-tidy-18 || command -v clang-tidy || true)}"
if [[ -z "${RUN_CLANG_TIDY}" || -z "${CLANG_TIDY}" ]]; then
  echo "clang-tidy not found; install clang-tidy-18" >&2
  exit 2
fi

# The file filter is a Python regex over compile-database paths: our sources,
# tests and tools, and the generated build-info unit, but nothing fetched into _deps.
filter='^(?!.*/_deps/).*/(src|tests|tools)/.*\.cpp$'

if [[ -n "${since}" ]]; then
  # From the merge base where there is one (a branch); else tree to tree (CI's
  # shallow merge commit, whose tree is the base's plus the pull request's).
  if git merge-base "${since}" HEAD >/dev/null 2>&1; then
    range=("${since}...HEAD")
  else
    range=("${since}" "HEAD")
  fi
  mapfile -t changed < <(git diff --name-only --diff-filter=d "${range[@]}")
  everything=false
  for f in "${changed[@]}"; do
    case "${f}" in
      .clang-tidy | scripts/tidy.sh | CMakeLists.txt | */CMakeLists.txt | CMakePresets.json | cmake/*)
        everything=true
        ;;
    esac
  done
  if [[ "${everything}" == true ]]; then
    echo "clang-tidy: the tidy or build configuration changed since ${since}; checking everything"
  else
    declare -A units=()
    for f in "${changed[@]}"; do
      case "${f}" in
        src/*.cpp | tests/*.cpp | tools/*.cpp)
          units["${f}"]=1
          ;;
        include/*.hpp | src/*.hpp | tests/*.hpp | tools/*.hpp)
          own="${f%.hpp}.cpp"
          if [[ -f "${own}" ]]; then
            units["${own}"]=1
          fi
          # The header as an #include names it.
          case "${f}" in
            include/*) name="${f#include/}" ;;
            src/*) name="${f#src/}" ;;
            *) name="${f##*/}" ;;
          esac
          while IFS= read -r unit; do
            units["${unit}"]=1
          done < <(git grep -l -F "#include \"${name}\"" -- 'src/*.cpp' 'tests/*.cpp' 'tools/*.cpp' || true)
          ;;
      esac
    done
    if [[ ${#units[@]} -eq 0 ]]; then
      echo "clang-tidy: no C++ source or header changed since ${since}; nothing to check"
      exit 0
    fi
    echo "clang-tidy: ${#units[@]} translation units changed since ${since}, or include a changed header:"
    printf '  %s\n' "${!units[@]}" | sort
    # One alternation of the units' paths, regex metacharacters escaped.
    alternation="$(printf '%s\n' "${!units[@]}" | sort | sed 's/[].^$*+?(){}|[\\]/\\&/g' | paste -sd '|' -)"
    filter="^(?!.*/_deps/).*/(${alternation})\$"
  fi
fi

"${RUN_CLANG_TIDY}" -quiet -clang-tidy-binary "${CLANG_TIDY}" -p "${build_dir}" "${filter}"
