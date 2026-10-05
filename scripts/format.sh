#!/usr/bin/env bash
# Formats every tracked C++ source with clang-format, or with --check reports the
# files that are not formatted and exits non-zero (what CI runs).
#
#   scripts/format.sh           # rewrite in place
#   scripts/format.sh --check   # verify only
#
# Uses $CLANG_FORMAT if set, else clang-format-18, else clang-format.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

REQUIRED_MAJOR=18
CLANG_FORMAT="${CLANG_FORMAT:-$(command -v clang-format-${REQUIRED_MAJOR} || command -v clang-format || true)}"
if [[ -z "${CLANG_FORMAT}" ]]; then
  echo "clang-format not found; install clang-format-${REQUIRED_MAJOR}" >&2
  exit 2
fi
major="$("${CLANG_FORMAT}" --version | sed -E 's/.*version ([0-9]+).*/\1/')"
if [[ "${major}" != "${REQUIRED_MAJOR}" ]]; then
  echo "warning: ${CLANG_FORMAT} is version ${major}; CI uses ${REQUIRED_MAJOR}," \
       "so its output may differ" >&2
fi

mapfile -t files < <(git ls-files -- '*.cpp' '*.hpp')
if [[ ${#files[@]} -eq 0 ]]; then
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  "${CLANG_FORMAT}" --dry-run --Werror "${files[@]}"
else
  "${CLANG_FORMAT}" -i "${files[@]}"
fi
