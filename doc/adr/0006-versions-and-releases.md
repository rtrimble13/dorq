# ADR 0006: The version on main is the next release's, and only the release says so

- Status: Accepted
- Date: 2026-11-24
- Implemented by: `scripts/release.sh`, `scripts/check_version.sh`, the `version`
  job in `.github/workflows/ci.yml`, `cmake/WriteBuildInfo.cmake` (`-dev`, and
  `DORQ_RELEASE`), `.github/workflows/release.yml`; fafnir's `DORQ_MIN_VERSION`
  (`src/fafnir/dq/dorq.py`) and `test/fafnir/test_dorq_version.py`
- Related: [CONTRIBUTING.md](../../CONTRIBUTING.md#versioning-and-releasing),
  [ADR 0003](0003-check-codes.md) (what a major change is), fafnir's ADR 0011 (its
  own version)

## Context

A review on 2026-11-24, while the efficacy plan's (DE) items were landing, of how
dorq and its caller, fafnir, number and release what they ship.

**dorq had** one version, in `project()` in `CMakeLists.txt`, which the binary and
every output record report; a release workflow that refuses a tag whose binary
reports another version, or that was built from a dirty tree; and a written rule
(while `0.y.z`, a change to check codes, names or output fields is a minor bump).
Releasing was by hand: edit the version in a pull request, merge, tag the merge
commit, push the tag.

**What went wrong, or could:**

1. **Unreleased builds claimed a release's number.** DE-01 set 0.15.0 on
   2026-11-23, and DE-02 and DE-13 landed under it. No 0.15.0 was tagged. Every
   binary built from main in that time said `0.15.0`, and so did every record it
   wrote. fafnir stores that string with every flag and audit cycle
   (`dorq_version`). A shadow night run on a build of main, and the eventual 0.15.0
   release, would have been indistinguishable, though they report differently.
   The efficacy plan's baseline (fafnir's DE-05) is defined as "dorq 0.14.0 as
   deployed": that definition needs a version that names one build.
2. **Nothing held the version to the tags before a tag.** The release workflow is
   the only check, and it runs after the tag is pushed. A pull request could lower
   the version, change the code without a bump, or change the bars output (the
   golden hashes) under a patch number, and CI said nothing.
3. **The install steps drift.** README.md pins the release to install
   (`version=`, `git checkout v`). Someone has to remember to move both at each
   release. fafnir's `doc/dorq.md` had drifted: it pinned dorq 0.8.0 and said "fafnir
   needs 0.8.0 or later" long after its statements run needed 0.13.0 and the host
   ran 0.14.0.
4. **No caller states what it needs.** The efficacy plan's decision 5 ships each
   change to dorq's input or record keys behind a minor version, "with fafnir's
   change in lockstep". fafnir did not record which dorq its code is written
   against, and ran any binary it was given.

**fafnir's own versioning is sound** (its ADR 0011): one constant in source,
re-exported, read by the packaging, `scripts/release.sh` that rewrites, checks,
commits and tags with refusals, and a tag workflow that installs the tagged commit
and compares. Its gaps are the dorq side (above) and that releases leave no
notes (below).

## Decision

1. **Main carries the number of the release it is becoming.** The first pull
   request that changes the code (`src/`, `include/`, `cmake/`, `CMakeLists.txt`)
   after a release sets the next version, with `scripts/release.sh bump
   patch|minor|major`. This is what dorq already did (DE-01 set 0.15.0); it is now
   a rule CI enforces.
2. **Only the release says the release's number.** A binary reports `X.Y.Z` when
   it is built from a clean tree at the tag `vX.Y.Z`, or from a source tree with
   no git (a release archive, a packager's `DORQ_GIT_COMMIT`). Anything else
   reports `X.Y.Z-dev`: `--version`, `dorq version` (whose JSON adds
   `"release": false`), every record's `dorq.version`, and the header
   `dorq calibrate` writes. `X.Y.Z-dev` is SemVer's pre-release of `X.Y.Z`, which
   is what a build of main is. The build decides, at build time, from
   `git describe --exact-match`; `DORQ_RELEASE=ON|OFF` overrides it, and the
   release workflow sets `ON`, where the tag check that follows makes sure the tag
   and the version agree.
3. **CI holds the version to the tags on every pull request** (the `version` job,
   `scripts/check_version.sh`):
   - `X.Y.Z`, never below the latest release (the newest `vX.Y.Z` in the history);
   - above it once the code differs from the release's;
   - a minor bump at least once the bars output differs
     (`tests/data/bars-golden.sha256`): the hashes are the one machine-checkable
     sign that what a check reports has changed;
   - README.md's install steps name the latest release, or this version.
4. **`scripts/release.sh` is the release.** `prepare` (on a branch) moves the
   install steps to the version, checks the rules and commits `Release vX.Y.Z`,
   for a pull request like any other. `tag` (on main, at `origin/main`, clean,
   prepared, the tag new) tags the merge commit and, with `--push`, pushes it. It
   refuses rather than guesses, as fafnir's does.
5. **fafnir states the oldest dorq it runs**: `DORQ_MIN_VERSION` (0.13.0), which a
   run checks before it exports a row. An older binary is refused; a `-dev` build
   whose own version meets the minimum runs, with a warning, since its flags name a
   version no archive has; `fafnir status` marks either. The minimum moves in the
   fafnir pull request that first reads a newer dorq's output, and
   `test_dorq_version.py` holds `doc/dorq.md`'s install steps at or above it.

## Consequences

- A record, a flag or an audit cycle names one build: `0.15.0` is the release,
  `0.15.0-dev` is not, and `dorq version` gives the commit for the latter.
- A pull request that changes the code right after a release fails `version` until
  it bumps. That is one line, and `scripts/release.sh` says which.
- The golden-hash rule is narrow on purpose. A change to the statements' output,
  which has no golden file, or to a check's documentation, is still the author's
  call, as before.
- The release build no longer trusts git to say it is the release (`DORQ_RELEASE`
  is set); the tag check is what keeps it honest, as it always was.
- fafnir with an old dorq fails loudly, at the start of a run, instead of failing
  oddly in the middle of one (a missing field, an unknown option).

## Not done: recommendations

In order of value:

1. **Release 0.15.0 soon, and release per plan phase.** DE-01, DE-02 and DE-13 are
   on main and untagged. fafnir's next audit cycle (DE-05 and the phase
   re-measures) should run a published version; the plan's "re-measure after each
   phase" maps naturally onto one minor release per phase.
2. **A record should carry the commit.** `dorq.version` plus `dorq.commit` in every
   record would make a `-dev` record fully traceable without the run's logs. It is
   an output field, so it waits for a minor release with fafnir's ingest in
   lockstep, and the golden test blanks it as it blanks the version.
3. **A changelog of caller-visible changes.** GitHub's generated notes list PR
   titles. A short `CHANGELOG.md` section per release, of the changes a caller
   acts on (new or renamed detail fields, record keys, `config_hash` changes,
   defaults, settings), would make the lockstep explicit. `release.sh prepare`
   could refuse an empty one.
4. **fafnir: publish a GitHub release per tag**, with generated notes, from its
   `version` workflow once the tag check passes. Deploys are `git pull`, so this is
   for the operator reading what a deploy brings; doc/releasing.md's hand-kept
   "Upgrade notes" would link to it rather than be the only record.
5. **fafnir: a release cadence.** fafnir cut 12 patch releases from v3.1.0 to
   v3.1.12, most of them one per merged pull request. Since the operator deploys
   by `git pull` and reads the tag only to know what runs, a release per deploy
   rather than per merge would keep the tags meaningful.
6. **Statements output under test, like bars.** A golden hash for a statements run
   on dorq-synth's universe would let `check_version.sh` see a change to the
   statements' output too, the domain the efficacy plan's Phase 1 changes most.
