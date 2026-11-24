# ADR 0008: Every version on main is a release

- Status: Accepted
- Date: 2026-11-24
- Supersedes: [ADR 0007](0007-releases-publish-on-merge.md); decisions 1, 3 (the
  install-steps rule) and 4 of [ADR 0006](0006-versions-and-releases.md)
- Implemented by: `.github/workflows/release.yml`, `scripts/release.sh` (`bump`,
  `plan`), `scripts/check_version.sh` (`--base`, and rules 5 and 6), the `version`
  job in `.github/workflows/ci.yml`
- Related: [CONTRIBUTING.md](../../CONTRIBUTING.md#versioning-and-releasing);
  rtrimble13/alpha-tango's `docs/versioning-and-releases.md` and release workflow,
  the model this adopts

## Context

ADR 0006 had main carry the next release's number for as long as it took. A
separate pull request (`release.sh prepare`) marked a release, and someone then
tagged the merge by hand. ADR 0007 automated the tag: on each push to main, the
workflow found the commit where the release pull request had merged, and tagged
it.

That commit is not always the one just pushed. 0.15.0's release pull request (#60)
merged before ADR 0007 landed. The first run after #61, which changed the
workflows, built and tested 917eea6. Then GitHub refused the tag push: "refusing
to allow a GitHub App to create or update workflow `.github/workflows/ci.yml`
without `workflows` permission". A tag from `GITHUB_TOKEN` on a commit whose
workflow files differ from main's counts as creating workflows, and that token
cannot have the permission.

rtrimble13/alpha-tango has released this way without a step by hand, and without
that failure. Its rule is simpler: a version on main with no release is released,
at the commit just pushed. A pull request that changes a skill must raise the
skill's version above the base branch's, so the merge is the release.

## Decision

1. **Every version that reaches main is a release.** A pull request that changes
   the code (`src/`, `include/`, `cmake/`, `CMakeLists.txt`) raises the version
   above its base branch's. `check_version.sh --base`, run by the `version` job on
   pull requests, holds it to that. One that changes nothing a binary does (docs,
   tests, tooling) keeps the version, and its merge releases nothing.
2. **`scripts/release.sh bump` sets the version and README.md's install steps
   together**, counting from the version on main. The install steps always name the
   version in `CMakeLists.txt` (rule 5). Merged, it is the release, minutes away.
   `prepare` and `tag` are gone: there is no release pull request, and no tag by
   hand.
3. **The release workflow releases the version on the commit pushed.** On each
   push to main it reads the committed version (`release.sh plan`). If GitHub has
   no release of it, the workflow builds and tests that commit and checks that the
   binary reports the version, all before a tag exists (`DORQ_RELEASE=ON`). Then
   `gh release create vX.Y.Z --target <commit>` creates the tag and the release
   together, as alpha-tango's does. The commit is main's head when the run starts,
   so its workflow files are main's. A tag already there, pushed by hand, is used
   as it is.
4. **Idempotent.** A version with a release is skipped, in seconds. A run that
   failed can be run again from the Actions tab, or it runs again on the next push
   to main. Runs never overlap or cancel half-way. A run started by hand publishes
   from main only.
5. **Kept from ADR 0006:** the version is written once, in `project()`; a build
   reports `X.Y.Z` only from a clean tree at the tag (or a source tree without
   git), and `X.Y.Z-dev` otherwise; versions only go up; a change to the bars
   output is at least a minor bump; fafnir's `DORQ_MIN_VERSION` moves in lockstep.

## Consequences

- Releasing is merging. No second pull request, no tag by hand, and no credentials
  outside CI.
- dorq releases more often: each pull request that changes the code is a release,
  where before several were batched under one number. A run of fixes is a run of
  patch releases. A pull request that should not release yet waits on its branch.
- The `-dev` builds mean less than before. Main is a release, or minutes from one.
  A build of a branch, or of main past its release (a docs change), still reports
  `-dev`.
- Release notes stay GitHub's generated list of merged pull request titles. Unlike
  alpha-tango, dorq has no CHANGELOG.md gate. ADR 0006's recommendation 3, a
  changelog of what a caller acts on, still applies.
- 0.15.0 was never published by the workflow. Its commit predates the workflow
  changes, so only a tag pushed by hand can publish it, and that tag runs the
  release workflow at that commit (ADR 0006's, which publishes a pushed tag). The
  merge of this change publishes 0.16.0, which contains all of 0.15.0. If 0.15.0
  is published as well, publish it first, so that the GitHub release marked
  latest is 0.16.0.
