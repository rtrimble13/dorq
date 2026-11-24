# Contributing to dorq

## Workflow

1. Work on a branch and open a pull request against `main`. CI must be green
   before a merge: build and test on GCC 13 and Clang 18 (Linux), sanitizers,
   clang-format and clang-tidy.
2. Each PR names the plan item it implements (`DR-xxxx` in
   [the development plan](doc/plans/dorq-development-plan.md)), and updates that
   item's status in the same PR.
3. Commit subjects are imperative and say what changed ("Add the CSV reader", not
   "CSV stuff"). The body says why.

## Docs move with the code

A change and its documentation land in the same PR. Leaving docs for a follow-up
is how they go stale. The PR template has the checklist:

| You changed | Update |
|---|---|
| A command, option, output format, exit code, or the install steps | `README.md` |
| A check: added, renamed, default changed, behaviour changed | `doc/checks/DQxxx.md` |
| A decision someone will later ask "why" about | a new ADR in `doc/adr/`, or a correction note on an existing one |
| Scope, order or status of planned work | `doc/plans/dorq-development-plan.md` |

## Code

- **C++20**, built with CMake presets (see `README.md`).
- **Formatting:** `scripts/format.sh` runs clang-format **18** (`.clang-format`). CI
  checks with the same version, because output changes between releases. Bump the
  version on purpose, in a commit that also reformats.
- **Static analysis:** `scripts/tidy.sh <build-dir>` runs clang-tidy 18
  (`.clang-tidy`) with every finding an error. If you disable a check, write the
  reason in the file next to it. `scripts/tidy.sh <build-dir> --changed-since
  origin/main` checks only the translation units your branch changed or that
  include a header it changed. CI does that on a pull request and checks the
  whole tree on a push to main.
- **Warnings:** our targets build with a strict warning set (`cmake/DorqTargetOptions.cmake`),
  and the `ci` and `asan` presets make every warning an error.
- **Naming** (enforced by clang-tidy): `snake_case` functions, variables and
  namespaces; `CamelCase` types; `kCamelCase` constants and enumerators; private
  members end in `_`.
- **Layout:** public headers in `include/dorq/`, everything else in `src/`. All code
  except `main()` goes in `dorq_lib`, so tests exercise exactly what ships.
- **The CLI writes to the streams it is given**, never straight to `std::cout`. That
  keeps the whole command-line contract testable in-process.

### Determinism rules

For a given binary, input and configuration, dorq's output is fixed
([ADR 0001](doc/adr/0001-stateless-deterministic-linter.md)). In practice:

- No randomness. If an algorithm needs it, the seed belongs in the configuration.
- No output that depends on the clock, the locale, the environment, thread
  scheduling, or iteration over an unordered container.
- Never add `-ffast-math` or anything that turns on floating-point contraction.
  The build sets `-ffp-contract=off` on purpose.
- Reductions over many series happen in a fixed order, whatever the thread count.

## Tests

- **Every change comes with tests.** Unit tests use doctest and live in
  `tests/unit/`. Command-line contract tests run the real binary and live in
  `tests/CMakeLists.txt`; use them for anything a script would depend on, such as
  exit codes and machine-readable output.
- **A Bayesian check is held to precision and recall** on synthetic data:
  `ctest -R synth` runs [dorq-synth](tools/synth/README.md)'s bars and statements
  universes and the gate in `tools/synth/gates.txt`. A model change that moves a number there should say so
  in the pull request; a new Bayesian check adds its faults to the generator and
  its bounds to the gate.
- Run at least `cmake --workflow --preset dev` and `cmake --workflow --preset asan`
  before pushing. The sanitizer preset needs the compiler's sanitizer runtime: GCC
  ships it; for Clang on Ubuntu, install `libclang-rt-18-dev`.

## Adding a check

1. Implement it as a `Check` (`src/checks/`), with a `CheckInfo` that gives its
   code, name, summary, default severity and what it applies to. Take the next
   free code in its family ([ADR 0003](doc/adr/0003-check-codes.md)). A code is
   never reused.
2. Register it in `all_checks()` (`src/checks/registry.cpp`), in code order.
3. Write `doc/checks/DQxxx.md`, starting `# DQxxx name`. The build compiles it into
   the binary for `dorq explain`, and a test fails if a registered check has no
   page.
4. Test what it reports *and* what it must not report: the near misses are the
   point of a data-quality check.
5. Add any settings to `Config` and `IntegritySettings` (or the family's
   equivalent). Settings are parsed in `src/config/config.cpp` and documented in
   `doc/configuration.md`; anything that changes what is reported belongs in
   `config_hash`.

## Fuzzing

The readers (and every check behind them), the config parser and the reference
calendar reader have libFuzzer targets in `fuzz/`. CI runs each one for a minute on every pull request. Locally,
with Clang and its runtime (`libclang-rt-18-dev` on Ubuntu):

```bash
CC=clang-18 CXX=clang++-18 cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p /tmp/corpus && build/fuzz/fuzz/fuzz_reader -max_total_time=300 /tmp/corpus fuzz/corpus/reader
```

When the fuzzer finds a crash, fix it, then add the crashing input to
`fuzz/corpus/` so it stays fixed.

## Dependencies

Dependencies are fetched by `cmake/Deps.cmake`, each pinned to a **commit**, with
the tag in a comment. A new dependency:

- must be header-only or small, and permissively licensed;
- is added in the PR that first uses it, not ahead of time;
- needs a reason in the PR description, including why the standard library won't
  do.

## Versioning and releasing

dorq uses [Semantic Versioning](https://semver.org/). The version is written in
one place, the `project()` call in `CMakeLists.txt`, and `scripts/release.sh` is
how it changes. While the version is `0.y.z`, a change to check codes, names or
output fields bumps the minor version. From `1.0.0` on, renaming or renumbering a
check is a major change ([ADR 0003](doc/adr/0003-check-codes.md)).

**Main carries the number of the release it is becoming.** The first pull request
that changes the code after a release sets the next version, with the change:

```bash
scripts/release.sh                 # the version, the latest release, what changed since
scripts/release.sh bump minor      # or patch, major, X.Y.Z: from the latest release
```

A binary reports the version as tagged (`0.15.0`) only when it is built from a
clean tree at the tag `v0.15.0`, or from a source tree without git (a release
archive). Any other build reports `0.15.0-dev`: `--version`, `dorq version` (whose
JSON says `"release": false`, and names the commit) and every output record's
`dorq.version`. A record from a build of main is never mistaken for the release.
`-DDORQ_RELEASE=ON` or `OFF` (or the environment variable `DORQ_RELEASE`)
overrides the guess; the release workflow sets `ON`.

**CI holds the version to the tags** (`scripts/check_version.sh`, the `version`
job, on every pull request and push to main):

1. It is `X.Y.Z`, and never below the latest release (the newest tag `vX.Y.Z` in
   the history).
2. Once `src/`, `include/`, `cmake/` or `CMakeLists.txt` differs from the latest
   release, it is above it.
3. A change to the bars output since the latest release
   (`tests/data/bars-golden.sha256`) is at least a minor bump.
4. `README.md`'s install steps name the latest release, or this version (the
   release's own pull request), or a release whose pull request has merged and
   that the release workflow has yet to tag.

**A release publishes when its pull request merges**
([ADR 0007](doc/adr/0007-releases-publish-on-merge.md)):

1. On a branch, `scripts/release.sh prepare` sets `README.md`'s install steps to
   the version `CMakeLists.txt` carries (or `prepare minor` and so on, to choose
   another), checks the rules, and commits `Release vX.Y.Z`. Open a pull request
   and merge it.
2. The merge runs `.github/workflows/release.yml`. It finds the release that is
   due (`scripts/release.sh plan`: the install steps name the version
   `CMakeLists.txt` carries, and no GitHub release `vX.Y.Z` exists), and the commit
   where the install steps first named it: the merge. It builds and tests that
   commit on Linux x86-64, checks that the binary reports the version from a clean
   tree, tags the commit `vX.Y.Z`, and publishes the archives and `SHA256SUMS` as a
   GitHub release. The release notes are generated from the merged PR titles.
3. Every other push to main finds nothing due, in a few seconds. A release that
   failed half-way publishes on the next push to main, or from the Actions tab
   (`release`, "Run workflow", on main). A tag already there must name the
   commit built.

`scripts/release.sh plan` shows what the next run would release. `scripts/release.sh
tag --push` is for when the workflow cannot tag: it tags the commit the workflow
would, from main at `origin/main`, and the pushed tag runs the same workflow. The
workflow cannot tag a commit whose `.github/workflows` differ from main's (GitHub
would need a `workflows` permission that its token cannot have): a release tagged
late, after a pull request that changed a workflow. Its plan job then fails at
once and names this command. It also cannot tag past a tag ruleset that does not
let GitHub Actions create tags. Tagging by hand needs credentials that may touch
workflow files: an SSH key, or a token with the `workflow` scope.

The workflow only picks a commit whose `CMakeLists.txt` says the release's
version, so its version check fails only for a tag pushed by hand on another
commit: delete that tag (`git push --delete origin vX.Y.Z`) and let the workflow
tag the release. Never move a tag once a release is published: a checkout that
already has it will not see the move.

**Callers move in lockstep.** fafnir runs dorq and reads its output. A release
whose output fafnir's code needs is paired with fafnir's change, which raises
`DORQ_MIN_VERSION` (`src/fafnir/dq/dorq.py` there) and its install steps
(`doc/dorq.md`) to it. fafnir refuses an older binary.
