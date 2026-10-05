#!/usr/bin/env bash
# Runs clang-tidy over dorq's own sources, using the compile database of a
# configured build directory (default: build/ci).
#
#   cmake --preset ci && cmake --build --preset ci --target dorq_generated
#   scripts/tidy.sh [build-dir]
#
# Uses $RUN_CLANG_TIDY / $CLANG_TIDY if set, else the -18 versions, else unversioned.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
build_dir="${1:-build/ci}"

if [[ ! -f "${build_dir}/compile_commands.json" ]]; then
  echo "${build_dir}/compile_commands.json not found; configure a preset first" >&2
  exit 2
fi

RUN_CLANG_TIDY="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy-18 || command -v run-clang-tidy || true)}"
CLANG_TIDY="${CLANG_TIDY:-$(command -v clang-tidy-18 || command -v clang-tidy || true)}"
if [[ -z "${RUN_CLANG_TIDY}" || -z "${CLANG_TIDY}" ]]; then
  echo "clang-tidy not found; install clang-tidy-18" >&2
  exit 2
fi

# The file filter is a Python regex over compile-database paths: our sources,
# tests and tools, and the generated build-info unit, but nothing fetched into _deps.
"${RUN_CLANG_TIDY}" -quiet -clang-tidy-binary "${CLANG_TIDY}" -p "${build_dir}" \
  '^(?!.*/_deps/).*/(src|tests|tools)/.*\.cpp$'
