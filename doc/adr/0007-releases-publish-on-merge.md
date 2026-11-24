# ADR 0007: A release publishes when its pull request merges

- Status: Accepted
- Date: 2026-11-24
- Amends: [ADR 0006](0006-versions-and-releases.md), decision 4 (`scripts/release.sh
  tag` was the release)
- Implemented by: `.github/workflows/release.yml`, `scripts/release.sh plan`,
  `scripts/check_version.sh` (rule 5)
- Related: [CONTRIBUTING.md](../../CONTRIBUTING.md#versioning-and-releasing);
  rtrimble13/alpha-tango's release workflow, which this follows

## Context

Under ADR 0006 a release took two steps: merge the pull request that
`scripts/release.sh prepare` makes, then run `scripts/release.sh tag --push` on main
and push the tag, which started the release workflow. The second step is easy to
forget and needs someone at a machine with push access to tags. dorq 0.15.0's pull
request (#60) merged on 2026-11-24. Its tag could not be pushed from the session
that prepared it, so 0.15.0 was not published. Meanwhile the next pull request
bumped main to 0.16.0, and its `version` check then failed: README.md named 0.15.0,
which was neither the latest release nor its own version.

alpha-tango has no second step. Its release workflow runs on every push to main,
finds each version that has no tag, and tags and publishes it.

## Decision

1. **The merge is the release.** `.github/workflows/release.yml` runs on every push
   to main. A release is due when README.md's install steps name the version
   `CMakeLists.txt` carries and no GitHub release `vX.Y.Z` exists. Only
   `scripts/release.sh prepare` sets the install steps to an unreleased version
   (`scripts/check_version.sh`, rule 5), so the install steps are the sign that a
   release was meant. A bump alone is not one: main carries the next version for
   weeks (ADR 0006, decision 1).
2. **The release is the commit that prepared it.** That is the oldest commit on
   main's first-parent line from which the install steps have named the version,
   which is the merge of prepare's pull request, provided `CMakeLists.txt` said the
   same version there. Once tagged, it is the tag's commit. A pull request merged
   after it, before the workflow ran, is not part of the release: it is what the
   release's pull request was reviewed without. `scripts/release.sh plan` gives
   the version and the commit (`version=`, `commit=`, `tagged=`).
3. **Build and check, then tag.** The workflow builds and tests that commit and
   checks that the binary reports the version, all before the tag exists
   (`DORQ_RELEASE=ON`). Only then does it push an annotated tag `vX.Y.Z`, as
   `release.sh tag` makes it, and create the GitHub release. A tag that is already
   there, pushed by hand or by a run that failed after tagging, must name the
   commit built. Otherwise the run fails: a tag is never moved.
4. **Idempotent.** A version with a GitHub release is skipped. Re-running the
   workflow, or merging an unrelated change, releases nothing. A release that
   failed half-way publishes on the next push to main, or from the Actions tab.
5. **A merged, untagged release counts as released for the version rules.** Rule 5
   accepts install steps that name it. `bump` and `prepare` count from it, so the
   pull request after a release can set the next version while the workflow runs.
6. **`scripts/release.sh tag` stays, as the fallback** for when the workflow cannot
   create a tag (a tag ruleset that leaves out GitHub Actions). It tags the same
   commit the workflow would, and the workflow publishes a tag pushed by hand the
   same way. A tag the workflow pushes with `GITHUB_TOKEN` starts no second run.

## Consequences

- Releasing is one pull request. Nobody has to remember a second step, and nobody
  needs tag push access outside CI.
- 0.15.0 publishes itself on the first push to main after this lands: README.md
  has named it since #60's merge, and it has no tag. The release is that merge
  commit, 917eea6.
- Every push to main runs a short `plan` job that usually finds nothing to do.
- The workflow needs `contents: write` to push the tag. A repository ruleset that
  stops GitHub Actions creating `v*` tags would stop it. Then the fallback above
  applies, or the ruleset lets `github-actions[bot]` through.
- A release's notes stay GitHub's generated list of merged pull request titles
  (ADR 0006, recommendation 3, still applies).
