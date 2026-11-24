#!/usr/bin/env bash
# release.sh -- set dorq's version, and cut a release. CONTRIBUTING.md, "Versioning
# and releasing", is the process; this is its mechanics.
#
#   scripts/release.sh                     what is here: the version, the latest
#                                          release, and what has changed since
#   scripts/release.sh bump BUMP           set the version the code is becoming, in
#                                          the pull request that first changes it
#   scripts/release.sh prepare [BUMP]      on a branch: the release's pull request.
#                                          Sets the version (omitted: the one
#                                          CMakeLists.txt carries) and README.md's
#                                          install steps, checks, commits
#   scripts/release.sh tag [--push]        on main, once that pull request has
#                                          merged: tag the merge commit vX.Y.Z
#
#   BUMP is patch, minor, major (from the latest release) or X.Y.Z.
#   --dry-run   say what would happen and change nothing
#
# The version is written once, in project() in CMakeLists.txt. Pushing the tag runs
# .github/workflows/release.yml, which builds, tests, checks that the binary reports
# the tag's version from a clean tree, and publishes the archives. Nothing is
# pushed unless you ask; a pushed tag is awkward to take back.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

DRY_RUN=0
PUSH=0
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
    --push) PUSH=1 ;;
    -h|--help) sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
latest_tag() { git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' HEAD 2>/dev/null || true; }

CURRENT="$(current_version)"
[[ "${CURRENT}" =~ ${semver} ]] || die "CMakeLists.txt's version '${CURRENT}' is not X.Y.Z"
TAG="$(latest_tag)"
RELEASED="${TAG#v}"
[[ -n "${TAG}" ]] || RELEASED="0.0.0"

# The version BUMP names: from the latest release, never below what main carries.
target() {
  local bump="$1" major minor patch new
  IFS=. read -r major minor patch <<< "${RELEASED}"
  case "${bump}" in
    patch) new="${major}.${minor}.$((patch + 1))" ;;
    minor) new="${major}.$((minor + 1)).0" ;;
    major) new="$((major + 1)).0.0" ;;
    *) new="${bump#v}" ;;
  esac
  [[ "${new}" =~ ${semver} ]] || die "'${new}' is not X.Y.Z; the tag adds the v"
  version_lt "${RELEASED}" "${new}" || die "${new} does not come after the latest release ${TAG}"
  if version_lt "${new}" "${CURRENT}"; then
    die "${new} is below ${CURRENT}, which CMakeLists.txt already carries: the changes" \
        "since ${TAG} already call for ${CURRENT}. Versions only go up."
  fi
  printf '%s' "${new}"
}

set_version() {
  local new="$1"
  if [[ "${new}" == "${CURRENT}" ]]; then
    return
  fi
  note "--> CMakeLists.txt: ${CURRENT} -> ${new}"
  run sed -i.bak -E "s/^  VERSION ${CURRENT//./\\.}\$/  VERSION ${new}/" CMakeLists.txt
  run rm -f CMakeLists.txt.bak
  if (( ! DRY_RUN )); then
    [[ "$(current_version)" == "${new}" ]] || die "the rewrite of CMakeLists.txt did not take"
  fi
}

status() {
  note "version          : ${CURRENT}"
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
    note "Main is the release ${TAG}. The first pull request that changes the code sets"
    note "the next version:  scripts/release.sh bump patch|minor|major"
  else
    note "${CURRENT} is unreleased. To release it, on a branch:"
    note "    scripts/release.sh prepare        # then open and merge its pull request"
    note "    scripts/release.sh tag --push     # on main, after the merge"
  fi
  note ""
  note "Merged since ${TAG}:"
  git log --first-parent --format='  %s' "${TAG}..HEAD" | sed -n '1,40p'
}

require_clean() {
  [[ -z "$(git status --porcelain)" ]] || die "the working tree has uncommitted changes; commit or stash first"
}

case "${COMMAND}" in
  ""|status)
    status
    ;;

  bump)
    [[ -n "${BUMP}" ]] || die "bump what? patch, minor, major or X.Y.Z"
    NEW="$(target "${BUMP}")"
    if [[ "${NEW}" == "${CURRENT}" ]]; then
      note "CMakeLists.txt already says ${CURRENT}."
      exit 0
    fi
    set_version "${NEW}"
    note "Commit it with the change that needs it. A build of it reports ${NEW}-dev until"
    note "the release is tagged."
    ;;

  prepare)
    require_clean
    BRANCH="$(git rev-parse --abbrev-ref HEAD)"
    [[ "${BRANCH}" != "main" ]] || die "prepare on a branch: a release goes through a pull request like any change"
    if [[ -n "${BUMP}" ]]; then
      NEW="$(target "${BUMP}")"
    else
      NEW="${CURRENT}"
      version_lt "${RELEASED}" "${NEW}" ||
        die "CMakeLists.txt says ${CURRENT}, the latest release's; give a bump (patch, minor, major)"
    fi
    note "==> preparing ${NEW} (latest release ${TAG:-none})"
    set_version "${NEW}"
    note "--> README.md: the install steps name ${NEW}"
    run sed -i.bak -E "s/^version=[0-9]+\.[0-9]+\.[0-9]+\$/version=${NEW}/; s/git checkout v[0-9]+\.[0-9]+\.[0-9]+/git checkout v${NEW}/" README.md
    run rm -f README.md.bak
    note "--> checking the version rules"
    run scripts/check_version.sh
    note "--> committing"
    run git add CMakeLists.txt README.md
    run git commit -m "Release v${NEW}" -m "Sets the version and README.md's install steps to ${NEW} (scripts/release.sh prepare)."
    note ""
    note "Open a pull request for ${BRANCH}, and merge it once CI is green. Then, on main:"
    note "    scripts/release.sh tag --push"
    note "If fafnir needs this release's output, its doc/dorq.md install steps and"
    note "DORQ_MIN_VERSION (src/fafnir/dq/dorq.py) move with it, in lockstep."
    ;;

  tag)
    require_clean
    [[ "$(git rev-parse --abbrev-ref HEAD)" == "main" ]] || die "tag on main, once the release's pull request has merged"
    run git fetch -q origin main --tags
    if (( ! DRY_RUN )) && [[ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]]; then
      die "main is not origin/main: git pull first, so the tag names the merged commit everyone has"
    fi
    TAG="$(latest_tag)"
    RELEASED="${TAG#v}"
    [[ -n "${TAG}" ]] || RELEASED="0.0.0"
    NEW="${CURRENT}"
    version_lt "${RELEASED}" "${NEW}" ||
      die "CMakeLists.txt says ${CURRENT}, which is already released (${TAG}); prepare the next one first"
    grep -qE "^version=${NEW//./\\.}\$" README.md ||
      die "README.md's install steps do not name ${NEW}: run scripts/release.sh prepare, and merge it, first"
    if git rev-parse -q --verify "refs/tags/v${NEW}" >/dev/null ||
       [[ -n "$(git ls-remote --tags origin "refs/tags/v${NEW}" 2>/dev/null)" ]]; then
      die "tag v${NEW} already exists. Never move a pushed tag: a checkout that has it will not update"
    fi
    scripts/check_version.sh
    note "==> tagging $(git rev-parse --short HEAD) v${NEW}"
    run git tag -a "v${NEW}" -m "dorq ${NEW}"
    if (( PUSH )); then
      run git push origin "v${NEW}"
      note ""
      note "Pushed v${NEW}: .github/workflows/release.yml builds and publishes it."
    elif (( ! DRY_RUN )); then
      note ""
      note "Tagged locally. To publish:  git push origin v${NEW}"
      note "To undo before pushing:      git tag -d v${NEW}"
    fi
    ;;

  *)
    die "unknown command '${COMMAND}' (try --help)"
    ;;
esac
