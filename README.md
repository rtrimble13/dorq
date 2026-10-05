# dorq

[![ci](https://github.com/rtrimble13/dorq/actions/workflows/ci.yml/badge.svg)](https://github.com/rtrimble13/dorq/actions/workflows/ci.yml)

A Bayesian data-quality linter for financial time series.

dorq works the way `flake8` does. It reads OHLCV bars or single-value series, runs
a set of built-in checks, and reports violations. Its checks ask a different question
from a fixed threshold. A threshold asks "is this move unusual?". dorq asks "is this
observation a *data error*?". For each suspicious observation it weighs the competing
explanations: a bad print, a price stored at the wrong scale, an unreported split, a
feed outage, or a genuine market move. It then reports the probability that the
observation is an error, the evidence behind that probability, and a suggested repair.

dorq is stateless and never modifies its input. Given the same input and the same
configuration, the same binary always produces the same output
([ADR 0001](doc/adr/0001-stateless-deterministic-linter.md)). It is being built to
replace the threshold checks in [fafnir](https://github.com/rtrimble13/fafnir)'s
data-quality process.

## Status

**Pre-alpha: milestone M0 (foundations).** The binary builds, tests and releases,
and reports its version. No checks exist yet. The checks and the order they arrive
in are set out in the [development plan](doc/plans/dorq-development-plan.md).

```console
$ dorq --version
0.0.1
$ dorq version
dorq 0.0.1
commit    6dab3ed4b97f
build     Release
compiler  GNU 13.3.0
system    Linux x86_64
$ dorq version --format json
{"version":"0.0.1","commit":"6dab3ed4b97f","dirty":false,"build_type":"Release","compiler":"GNU 13.3.0","system":"Linux x86_64"}
```

What usage will look like once the checks land (planned for M1–M3, not yet working):

```console
$ dorq prices.csv
AAPL  2020-08-31  DQ203 error  unreported-split  close 499.23→129.04 (×0.2585 ≈ 1:4)  P(error)=0.97
$ dorq --format jsonl --calendar-file sessions.csv --actions actions.csv - < bars.csv
```

### Exit status

The exit codes are fixed. Scripts can rely on them.

| Code | Meaning |
|---|---|
| 0 | Ran, and found nothing at or above `--fail-on` |
| 1 | Ran, and found violations at or above `--fail-on` |
| 2 | Usage or configuration error; nothing was checked |
| 3 | The input could not be read or parsed |

## Install

### Release binaries

Each [release](https://github.com/rtrimble13/dorq/releases) publishes the following
archives, plus a `SHA256SUMS` file:

| Archive | Runs on |
|---|---|
| `dorq-<version>-linux-x86_64.tar.gz` | Linux x86-64 with glibc 2.39 or newer (Ubuntu 24.04+). The C++ runtime is linked in |
| `dorq-<version>-macos-arm64.tar.gz` | macOS on Apple silicon |

```bash
version=0.0.1
curl -LO "https://github.com/rtrimble13/dorq/releases/download/v${version}/dorq-${version}-linux-x86_64.tar.gz"
curl -LO "https://github.com/rtrimble13/dorq/releases/download/v${version}/SHA256SUMS"
sha256sum --check --ignore-missing SHA256SUMS
sudo tar -xzf "dorq-${version}-linux-x86_64.tar.gz" -C /opt
sudo ln -sfn "/opt/dorq-${version}-linux-x86_64" /opt/dorq     # /opt/dorq/bin/dorq
/opt/dorq/bin/dorq --version
```

Upgrading means unpacking the new version beside the old one and moving the
symlink. Rolling back means moving it back.

### Build from source

These steps are for Ubuntu 24.04, which is also how to install on the fafnir
host. You need CMake 3.25 or newer, Ninja, and a C++20 compiler (GCC 13+ or
Clang 18+).

```bash
sudo apt-get install -y build-essential cmake ninja-build git
git clone https://github.com/rtrimble13/dorq.git && cd dorq
git checkout v0.0.1                        # or stay on main for the latest
cmake --workflow --preset release          # configure, build, run the tests
sudo cmake --install build/release --prefix /opt/dorq
/opt/dorq/bin/dorq version                 # names the commit it was built from
```

The configure step downloads two pinned dependencies from GitHub: CLI11 and
doctest (see `cmake/Deps.cmake`). On a host without network access, clone each
dependency at the commit pinned there. Then point CMake at those checkouts:

```bash
cmake --preset release \
  -DFETCHCONTENT_SOURCE_DIR_CLI11=/src/CLI11 \
  -DFETCHCONTENT_SOURCE_DIR_DOCTEST=/src/doctest
```

## Development

| Preset | Use |
|---|---|
| `dev` | Debug build for day-to-day work |
| `release` | What ships. Also what to build on a server |
| `ci` | Optimised with debug info; warnings are errors |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `coverage` | gcov instrumentation |

```bash
cmake --workflow --preset dev        # configure + build + test
ctest --preset dev -R cli.           # just the command-line contract tests
scripts/format.sh                    # clang-format 18, in place (--check to verify)
scripts/tidy.sh build/dev            # clang-tidy 18 over our sources
```

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## Documentation

- [Development plan](doc/plans/dorq-development-plan.md): goals, checks, models, milestones
- [Architecture decisions](doc/adr/)
- [Contributing](CONTRIBUTING.md): conventions, tests, dependencies, releasing

## License

MIT. See [LICENSE](LICENSE).
