#!/usr/bin/env bash
# release.sh -- set dorq's version. CONTRIBUTING.md, "Versioning and releasing", is
# the process; this is its mechanics.
#
#   scripts/release.sh               what is here: the version, the latest release,
#                                    and what has changed since
#   scripts/release.sh bump BUMP     in a pull request that changes the code: set
#                                    the version it releases, in CMakeLists.txt and
#                                    README.md's install steps
#   scripts/release.sh plan          for the release workflow: the version on this
#                                    commit and whether it is tagged (version=,
#                                    commit=, tagged=)
#
#   BUMP is patch, minor, major (from the version on main) or X.Y.Z.
#   --dry-run   say what would happen and change nothing
#
# Every version that reaches main is a release (ADR 0008). A pull request that
# changes the code (src/, include/, cmake/, CMakeLists.txt) raises the version above
# main's: scripts/check_version.sh, the `version` job, holds it to that. When it
# merges, .github/workflows/release.yml finds the version untagged, builds and
# tests the merge commit, checks that the binary reports the version, and tags that
# commit vX.Y.Z as it publishes the archives. Nothing here pushes or tags.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

DRY_RUN=0
COMMAND=""
BUMP=""

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }
run() {
  if (( DRY_RUN )); then
    printf '    would run: %s\n' "$*"
  else
    "$@"
  fi
}

while (( $# )); do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) die "unknown option $1 (try --help)" ;;
    *)
      if [[ -z "${COMMAND}" ]]; then
        COMMAND="$1"
      elif [[ -z "${BUMP}" ]]; then
        BUMP="$1"
      else
        die "too many arguments (try --help)"
      fi
      ;;
  esac
  shift
done

semver='^[0-9]+\.[0-9]+\.[0-9]+$'
version_lt() {  # A < B
  [[ "$1" != "$2" && "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" == "$1" ]]
}

current_version() { sed -nE 's/^  VERSION ([^ )]+)$/\1/p' CMakeLists.txt; }
version_at() { git show "$1:CMakeLists.txt" 2>/dev/null | sed -nE 's/^  VERSION ([^ )]+)$/\1/p' | head -1; }
latest_tag() { git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || true; }

CURRENT="$(current_version)"
[[ "${CURRENT}" =~ ${semver} ]] || die "CMakeLists.txt's version '${CURRENT}' is not X.Y.Z"
TAG="$(latest_tag)"
RELEASED="${TAG#v}"
[[ -n "${TAG}" ]] || RELEASED="0.0.0"

# What a bump counts from: the version on main, which is released or being
# released. Without origin/main (a fresh clone has it), the latest release.
MAIN=""
if git rev-parse -q --verify origin/main >/dev/null; then
  MAIN="$(version_at origin/main)"
fi
BASE="${MAIN:-${RELEASED}}"
if version_lt "${BASE}" "${RELEASED}"; then
  BASE="${RELEASED}"
fi

# The version BUMP names, above the version on main.
target() {
  local bump="$1" major minor patch new
  IFS=. read -r major minor patch <<< "${BASE}"
  case "${bump}" in
    patch) new="${major}.${minor}.$((patch + 1))" ;;
    minor) new="${major}.$((minor + 1)).0" ;;
    major) new="$((major + 1)).0.0" ;;
    *) new="${bump#v}" ;;
  esac
  [[ "${new}" =~ ${semver} ]] || die "'${new}' is not X.Y.Z; the tag adds the v"
  version_lt "${BASE}" "${new}" || die "${new} does not come after ${BASE}, the version on main"
  printf '%s' "${new}"
}

status() {
  note "version          : ${CURRENT}"
  [[ -n "${MAIN}" ]] && note "on main          : ${MAIN}"
  if [[ -z "${TAG}" ]]; then
    note "latest release   : (none yet)"
    return
  fi
  local ahead
  ahead="$(git rev-list --count "${TAG}..HEAD")"
  note "latest release   : ${TAG} (${ahead} commit(s) since)"
  note "this checkout    : $(git describe --tags --always --dirty 2>/dev/null)"
  if ! git diff --quiet "${TAG}" HEAD -- src include cmake CMakeLists.txt; then
    note "code             : changed since ${TAG}"
  else
    note "code             : as released"
  fi
  if ! git diff --quiet "${TAG}" HEAD -- tests/data/bars-golden.sha256; then
    note "bars output      : changed since ${TAG} (a minor release at least)"
  fi
  note ""
  if [[ "${CURRENT}" == "${RELEASED}" ]]; then
    note "This is the release ${TAG}. A pull request that changes the code raises the"
    note "version, and merging it publishes that release:"
    note "    scripts/release.sh bump patch|minor|major"
  elif [[ "${CURRENT}" == "${BASE}" ]]; then
    note "${CURRENT} is on main and not yet tagged: the release workflow publishes it"
    note "(Actions, release), or did and this clone has not fetched the tag."
  else
    note "${CURRENT} is this branch's version: merging it publishes v${CURRENT}."
  fi
  note ""
  note "Merged since ${TAG}:"
  git log --first-parent --format='  %s' "${TAG}..HEAD" | sed -n '1,40p'
}

case "${COMMAND}" in
  ""|status)
    status
    ;;

  bump)
    [[ -n "${BUMP}" ]] || die "bump what? patch, minor, major or X.Y.Z"
    NEW="$(target "${BUMP}")"
    if [[ "${NEW}" != "${CURRENT}" ]]; then
      note "--> CMakeLists.txt: ${CURRENT} -> ${NEW}"
      run sed -i.bak -E "s/^  VERSION ${CURRENT//./\\.}\$/  VERSION ${NEW}/" CMakeLists.txt
      run rm -f CMakeLists.txt.bak
      if (( ! DRY_RUN )); then
        [[ "$(current_version)" == "${NEW}" ]] || die "the rewrite of CMakeLists.txt did not take"
      fi
    else
      note "CMakeLists.txt already says ${NEW}."
    fi
    note "--> README.md: the install steps name ${NEW}"
    run sed -i.bak -E "s/^version=[0-9]+\.[0-9]+\.[0-9]+\$/version=${NEW}/; s/git checkout v[0-9]+\.[0-9]+\.[0-9]+/git checkout v${NEW}/" README.md
    run rm -f README.md.bak
    note ""
    note "Commit both with the change that needs them. Merging the pull request"
    note "publishes v${NEW}. A build of it reports ${NEW}-dev until then."
    note "If fafnir needs this release's output, its doc/dorq.md install steps and"
    note "DORQ_MIN_VERSION (src/fafnir/dq/dorq.py) move with it, in lockstep."
    ;;

  plan)
    # The version on this commit, as committed, and whether it is tagged. The
    # release workflow publishes it if GitHub has no release of it yet: at its tag,
    # or at this commit.
    VERSION="$(version_at HEAD)"
    [[ "${VERSION}" =~ ${semver} ]] || die "CMakeLists.txt's committed version '${VERSION}' is not X.Y.Z"
    if COMMIT="$(git rev-parse -q --verify "refs/tags/v${VERSION}^{commit}")"; then
      TAGGED=true
    else
      COMMIT="$(git rev-parse HEAD)"
      TAGGED=false
    fi
    printf 'version=%s\ncommit=%s\ntagged=%s\n' "${VERSION}" "${COMMIT}" "${TAGGED}"
    if [[ "${TAGGED}" == true ]]; then
      note "v${VERSION} is tagged, at $(git rev-parse --short "${COMMIT}")." >&2
    else
      note "v${VERSION} is not tagged: the release workflow publishes it from $(git rev-parse --short HEAD)." >&2
    fi
    ;;

  *)
    die "unknown command '${COMMAND}' (try --help)"
    ;;
esac
