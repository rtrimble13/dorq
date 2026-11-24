#!/usr/bin/env bash
# check_version.sh -- the version rules, checked on every pull request and every
# push to main (the `version` job in .github/workflows/ci.yml). CONTRIBUTING.md,
# "Versioning and releasing", says why each one holds.
#
#   scripts/check_version.sh [--base REF]
#
# Every version that reaches main is a release (ADR 0008):
#
# 1. project(VERSION) in CMakeLists.txt is X.Y.Z.
# 2. It is not below the latest release: the newest tag vX.Y.Z in HEAD's history.
# 3. Once the code (src/, include/, cmake/, CMakeLists.txt) differs from that
#    release's, the version is above it.
# 4. A change to the bars output since that release (tests/data/bars-golden.sha256)
#    is at least a minor bump: a change to what a check reports is not a patch.
# 5. README.md's install steps name this version: once merged, it is the release.
# 6. With --base (a pull request: the branch it merges into), a change to the code
#    since the base raises the version above the base's. The merge publishes it.
#
# It needs the history and the tags. CI checks out with fetch-depth 0; a shallow
# clone fails here with the command that fetches them.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

BASE_REF=""
while (( $# )); do
  case "$1" in
    --base) BASE_REF="${2:?--base needs a ref}"; shift ;;
    *) echo "::error::unknown argument $1 (usage: check_version.sh [--base REF])" >&2; exit 2 ;;
  esac
  shift
done

failures=0
fail() {
  printf '::error::%s\n' "$*" >&2
  failures=$((failures + 1))
}

# A < B for X.Y.Z versions.
version_lt() {
  [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" == "$1" ]]
}

CODE=(src include cmake CMakeLists.txt)
semver='^[0-9]+\.[0-9]+\.[0-9]+$'

mapfile -t found < <(sed -nE 's/^  VERSION ([^ )]+)$/\1/p' CMakeLists.txt)
if [[ ${#found[@]} -ne 1 ]]; then
  echo "::error::expected one 'VERSION X.Y.Z' line in CMakeLists.txt's project(), found ${#found[@]}" >&2
  exit 1
fi
version="${found[0]}"
if [[ ! "${version}" =~ ${semver} ]]; then
  echo "::error::CMakeLists.txt's version '${version}' is not X.Y.Z" >&2
  exit 1
fi
if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
  echo "::error::a shallow clone cannot be checked against the tags: git fetch --tags --unshallow" >&2
  exit 1
fi

# 5. README's install steps name the version that merging this publishes.
mapfile -t pins < <(grep -oE '^version=[0-9]+\.[0-9]+\.[0-9]+|git checkout v[0-9]+\.[0-9]+\.[0-9]+' README.md |
                    sed -E 's/^version=//; s/^git checkout v//')
if [[ ${#pins[@]} -eq 0 ]]; then
  fail "README.md's install steps name no version (version=X.Y.Z, git checkout vX.Y.Z)"
fi
for pin in "${pins[@]}"; do
  if [[ "${pin}" != "${version}" ]]; then
    fail "README.md's install steps name ${pin}, and CMakeLists.txt says ${version}." \
         "Every version on main is a release: scripts/release.sh bump sets both."
  fi
done

# 6. A pull request that changes the code raises the version above its base's.
base_note=""
if [[ -n "${BASE_REF}" ]]; then
  git rev-parse -q --verify "${BASE_REF}^{commit}" >/dev/null ||
    { echo "::error::no base ${BASE_REF}: fetch it (actions/checkout fetch-depth: 0)" >&2; exit 1; }
  base_version="$(git show "${BASE_REF}:CMakeLists.txt" | sed -nE 's/^  VERSION ([^ )]+)$/\1/p' | head -1)"
  fork="$(git merge-base "${BASE_REF}" HEAD)"
  if ! git diff --quiet "${fork}" HEAD -- "${CODE[@]}"; then
    if ! version_lt "${base_version}" "${version}"; then
      fail "this pull request changes the code, and the version (${version}) is not above" \
           "${BASE_REF}'s (${base_version}). Merging it publishes a release:" \
           "scripts/release.sh bump patch|minor|major."
    fi
    base_note="; code changed since ${BASE_REF} (${base_version})"
  else
    base_note="; code as on ${BASE_REF} (${base_version})"
  fi
fi

tag="$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || true)"
if [[ -z "${tag}" ]]; then
  (( failures == 0 )) || exit 1
  echo "version ${version}: no release tag in this history yet${base_note}: ok"
  exit 0
fi
released="${tag#v}"
if [[ ! "${released}" =~ ${semver} ]]; then
  echo "::error::the latest release tag '${tag}' is not vX.Y.Z" >&2
  exit 1
fi
ahead="$(git rev-list --count "${tag}..HEAD")"

# 2. Versions only go up.
if version_lt "${version}" "${released}"; then
  fail "CMakeLists.txt says ${version}, below the latest release ${tag}. Versions only go up."
fi

# 3. Code past a release carries a new version.
code_changed=false
if ! git diff --quiet "${tag}" HEAD -- "${CODE[@]}"; then
  code_changed=true
fi
if [[ "${code_changed}" == true && "${version}" == "${released}" ]]; then
  fail "the code has changed since ${tag}, and CMakeLists.txt still says ${released}." \
       "Set the version this releases: scripts/release.sh bump patch|minor|major."
fi

# 4. A change to the bars output is a minor bump at least.
if version_lt "${released}" "${version}" &&
   ! git diff --quiet "${tag}" HEAD -- tests/data/bars-golden.sha256; then
  IFS=. read -r rmaj rmin _ <<< "${released}"
  IFS=. read -r vmaj vmin _ <<< "${version}"
  if (( vmaj == rmaj && vmin == rmin )); then
    fail "the bars output has changed since ${tag} (tests/data/bars-golden.sha256), and" \
         "${version} is a patch release of it. A change to what a check reports is at least" \
         "a minor one: scripts/release.sh bump minor."
  fi
fi

if (( failures > 0 )); then
  exit 1
fi
state="unreleased: merging it publishes it"
[[ "${version}" == "${released}" ]] && state="released"
echo "version ${version} (${state}; latest release ${tag}, ${ahead} commit(s) since," \
     "code $([[ "${code_changed}" == true ]] && echo changed || echo unchanged)${base_note}): ok"
