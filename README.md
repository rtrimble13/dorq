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

**Pre-alpha: milestone M1 (reading input, output formats, integrity checks).** dorq
reads CSV, TSV, JSON Lines and JSON arrays, and reports in five formats. It runs
the deterministic integrity checks (DQ1xx). The Bayesian checks, which are dorq's
reason to exist, arrive from M2 onward; the
[development plan](doc/plans/dorq-development-plan.md) sets out what comes when.

```console
$ dorq tests/data/bad_bars.csv
AAA  2024-01-03  DQ101 error  ohlc-bounds  high 9.5 < open 10; high 9.5 < close 10  (line 3)
AAA  2024-01-04  DQ101 error  ohlc-bounds  low 9 > close 0  (line 4)
AAA  2024-01-04  DQ102 error  non-positive  close is 0  (line 4)
AAA  2024-01-05  DQ104 error  missing-field  close is empty  (line 5)
AAA  2024-01-06  DQ104 warn  missing-field  volume is empty  (line 6)
BBB  line 7  DQ104 error  missing-field  date "2024-13-01" is not a date; the row is skipped
$ echo $?
1
```

## Usage

```bash
dorq prices.csv                              # check a file ("check" is the default command)
psql -c "COPY (...) TO STDOUT CSV HEADER" | dorq --format fafnir -   # or stdin
dorq --format jsonl --select DQ1 --ignore DQ106 a.csv b.jsonl
dorq --show-info --statistics prices.csv     # info too, then counts per check
dorq list-checks                             # every check, its severity and what it needs
dorq explain DQ106                           # a check's full documentation
dorq config show                             # the effective settings and their hash
dorq config init                             # write a starter dorq.toml
```

Input is long format, one row per series and date. Columns are found by name,
ignoring case and punctuation: `date`/`trade_date`, `open`, `high`, `low`,
`close`, `volume`, `series`/`security_id`, `symbol`, `value`, and so on. Bars
need open, high, low and close; a series with only a value column is a point
series. When the names don't match, use `--columns date=asof,value=DGS10`.
[doc/configuration.md](doc/configuration.md) covers columns, the config file and
profiles. [doc/output.md](doc/output.md) covers each output format, including the
`fafnir` format that maps onto `ops.data_quality_flag`.

| Check | Name | Reports |
|---|---|---|
| [DQ101](doc/checks/DQ101.md) | ohlc-bounds | a high below open/low/close, or a low above open/close |
| [DQ102](doc/checks/DQ102.md) | non-positive | a price at or below zero, or a negative volume |
| [DQ103](doc/checks/DQ103.md) | duplicate-date | more than one row for a date |
| [DQ104](doc/checks/DQ104.md) | missing-field | an empty or unparseable value |
| [DQ106](doc/checks/DQ106.md) | precision-shift | computed (e.g. back-adjusted) prices among quoted ones |
| [DQ107](doc/checks/DQ107.md) | zero-range-with-volume | a flat bar on the series' typical volume |

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
version=0.1.0
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
git checkout v0.1.0                        # or stay on main for the latest
cmake --workflow --preset release          # configure, build, run the tests
sudo cmake --install build/release --prefix /opt/dorq
/opt/dorq/bin/dorq version                 # names the commit it was built from
```

The configure step downloads four pinned dependencies from GitHub: CLI11,
fast_float, toml++ and doctest (see `cmake/Deps.cmake`). On a host without network access, clone each
dependency at the commit pinned there. Then point CMake at those checkouts:

```bash
cmake --preset release \
  -DFETCHCONTENT_SOURCE_DIR_CLI11=/src/CLI11 \
  -DFETCHCONTENT_SOURCE_DIR_FAST_FLOAT=/src/fast_float \
  -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=/src/tomlplusplus \
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
| `fuzz` | libFuzzer targets with ASan + UBSan (Clang; see `fuzz/`) |

```bash
cmake --workflow --preset dev        # configure + build + test
ctest --preset dev -R cli.           # just the command-line contract tests
scripts/format.sh                    # clang-format 18, in place (--check to verify)
scripts/tidy.sh build/dev            # clang-tidy 18 over our sources
```

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## Documentation

- [Configuration](doc/configuration.md): the config file, profiles, columns, input
- [Output formats](doc/output.md): text, json, jsonl, csv, fafnir
- [Checks](doc/checks/): one page per check, also printed by `dorq explain`
- [Development plan](doc/plans/dorq-development-plan.md): goals, checks, models, milestones
- [Architecture decisions](doc/adr/)
- [Contributing](CONTRIBUTING.md): conventions, tests, dependencies, releasing

## License

MIT. See [LICENSE](LICENSE).
