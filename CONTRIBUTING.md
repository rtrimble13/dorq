# Contributing to dorq

## Workflow

1. Work on a branch and open a pull request against `main`. CI must be green
   before a merge: build and test on GCC 13, Clang 18 and AppleClang, sanitizers,
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
| A check: added, renamed, default changed, behaviour changed | `doc/checks/DQxxx.md` (from M1) |
| A decision someone will later ask "why" about | a new ADR in `doc/adr/`, or a correction note on an existing one |
| Scope, order or status of planned work | `doc/plans/dorq-development-plan.md` |

## Code

- **C++20**, built with CMake presets (see `README.md`).
- **Formatting:** `scripts/format.sh` runs clang-format **18** (`.clang-format`). CI
  checks with the same version, because output changes between releases. Bump the
  version on purpose, in a commit that also reformats.
- **Static analysis:** `scripts/tidy.sh <build-dir>` runs clang-tidy 18
  (`.clang-tidy`) with every finding an error. If you disable a check, write the
  reason in the file next to it.
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
- Run at least `cmake --workflow --preset dev` and `cmake --workflow --preset asan`
  before pushing. The sanitizer preset needs the compiler's sanitizer runtime: GCC
  ships it; for Clang on Ubuntu, install `libclang-rt-18-dev`.

## Dependencies

Dependencies are fetched by `cmake/Deps.cmake`, each pinned to a **commit**, with
the tag in a comment. A new dependency:

- must be header-only or small, and permissively licensed;
- is added in the PR that first uses it, not ahead of time;
- needs a reason in the PR description, including why the standard library won't
  do.

## Versioning and releasing

dorq uses [Semantic Versioning](https://semver.org/). The version is written in
one place, the `project()` call in `CMakeLists.txt`. The binary reports it, and the
release workflow refuses a tag that disagrees with it.

While the version is `0.y.z`, a change to check codes, names or output fields bumps
the minor version. From `1.0.0` on, renaming or renumbering a check is a major
change ([ADR 0003](doc/adr/0003-check-codes.md)).

To release:

1. In a PR, set the new version in `CMakeLists.txt` and merge it to `main`.
2. Tag the merge commit and push the tag:
   ```bash
   git checkout main && git pull
   git tag -a v0.1.0 -m "dorq 0.1.0"
   git push origin v0.1.0
   ```
3. `.github/workflows/release.yml` builds and tests on Linux x86-64 and macOS
   arm64. It checks that the binary reports the tag's version from a clean tree,
   then publishes the archives and `SHA256SUMS` as a GitHub release. The release
   notes are generated from the merged PR titles.

If the tag check fails, delete the tag (`git push --delete origin vX.Y.Z`), fix the
version, and tag again.
