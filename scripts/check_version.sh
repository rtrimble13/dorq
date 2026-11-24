#!/usr/bin/env bash
# check_version.sh -- the version rules, checked on every pull request and every
# push to main (the `version` job in .github/workflows/ci.yml). CONTRIBUTING.md,
# "Versioning and releasing", says why each one holds.
#
#   scripts/check_version.sh
#
# 1. project(VERSION) in CMakeLists.txt is X.Y.Z.
# 2. It is not below the latest release: the newest tag vX.Y.Z in HEAD's history.
# 3. Once the code (src/, include/, cmake/, CMakeLists.txt) differs from that
#    release's, the version is above it. Main always carries the number of the
#    release it is becoming, and a binary built from it reports X.Y.Z-dev.
# 4. A change to the bars output since that release (tests/data/bars-golden.sha256)
#    is at least a minor bump: a change to what a check reports is not a patch.
# 5. README.md's install steps name the latest release, or this version (in the
#    pull request that prepares it: scripts/release.sh prepare).
#
# It needs the tags. CI checks out with fetch-depth 0; a shallow clone fails here
# with the command that fetches them.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

failures=0
fail() {
  printf '::error::%s\n' "$*" >&2
  failures=$((failures + 1))
}

# A < B for X.Y.Z versions.
version_lt() {
  [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" == "$1" ]]
}

mapfile -t found < <(sed -nE 's/^  VERSION ([^ )]+)$/\1/p' CMakeLists.txt)
if [[ ${#found[@]} -ne 1 ]]; then
  echo "::error::expected one 'VERSION X.Y.Z' line in CMakeLists.txt's project(), found ${#found[@]}" >&2
  exit 1
fi
version="${found[0]}"
semver='^[0-9]+\.[0-9]+\.[0-9]+$'
if [[ ! "${version}" =~ ${semver} ]]; then
  echo "::error::CMakeLists.txt's version '${version}' is not X.Y.Z" >&2
  exit 1
fi

tag="$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || true)"
if [[ -z "${tag}" ]]; then
  if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
    echo "::error::a shallow clone has no tags to check against: git fetch --tags --unshallow" >&2
    exit 1
  fi
  echo "version ${version}: no release tag in this history yet; nothing to check against"
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

# 3. Code past a release carries the next release's number.
code_changed=false
if ! git diff --quiet "${tag}" HEAD -- src include cmake CMakeLists.txt; then
  code_changed=true
fi
if [[ "${code_changed}" == true && "${version}" == "${released}" ]]; then
  fail "the code has changed since ${tag}, and CMakeLists.txt still says ${released}." \
       "Set the version of the release this becomes: scripts/release.sh bump patch|minor|major."
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

# 5. README's install steps name a release that exists, or the one being prepared.
mapfile -t pins < <(grep -oE '^version=[0-9]+\.[0-9]+\.[0-9]+|git checkout v[0-9]+\.[0-9]+\.[0-9]+' README.md |
                    sed -E 's/^version=//; s/^git checkout v//')
if [[ ${#pins[@]} -eq 0 ]]; then
  fail "README.md's install steps name no version (version=X.Y.Z, git checkout vX.Y.Z)"
fi
for pin in "${pins[@]}"; do
  if [[ "${pin}" != "${released}" && "${pin}" != "${version}" ]]; then
    fail "README.md's install steps name ${pin}: neither the latest release (${released})" \
         "nor this version (${version}). scripts/release.sh prepare sets them."
  fi
done

if (( failures > 0 )); then
  exit 1
fi
state="unreleased"
[[ "${version}" == "${released}" ]] && state="released"
echo "version ${version} (${state}; latest release ${tag}, ${ahead} commit(s) since," \
     "code $([[ "${code_changed}" == true ]] && echo changed || echo unchanged)): ok"
