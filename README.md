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

**Pre-alpha: milestone M5 (context inputs).** dorq reads CSV, TSV, JSON
Lines and JSON arrays, and reports in five formats. It runs:

- the deterministic integrity checks (DQ1xx);
- the Bayesian price action checks (DQ2xx): for each suspicious bar, a bad print,
  a scale error, an unreported split, a bad close field, a new history after a
  gap, or a real move;
- the date-shift check (DQ206);
- the Bayesian coverage checks (DQ3xx), which judge a missing session by how often
  *this* series trades and report a failed load once rather than once per series;
- the volume checks (DQ4xx): volume in other units, spikes with no move, moves on
  no volume;
- the stale-value checks (DQ5xx): a close repeated on traded bars more often than
  the series' moves allow;
- point series (rates, spreads, index levels) on the log or the difference scale,
  with configurable bounds (DQ108);
- with context files ([doc/context.md](doc/context.md)): corporate actions
  (`--actions`), which explain splits on file and check them against the bars
  (DQ7xx); series metadata (`--meta`), which profiles match on and which lets a
  fund family's shared split count as evidence (DQ601); and a market reference
  (`--market`), net of which every move is judged (DQ602).

Calibration from fafnir's own decisions comes in M6. The
[development plan](doc/plans/dorq-development-plan.md) sets out what comes when.

```console
$ dorq tests/data/bad_bars.csv
AAA  2024-01-03  DQ101 error  ohlc-bounds  high 9.5 < open 10; high 9.5 < close 10  (line 3)
AAA  2024-01-04  DQ101 error  ohlc-bounds  low 9 > close 0  (line 4)
AAA  2024-01-04  DQ102 error  non-positive  close is 0  (line 4)
AAA  2024-01-05  DQ104 error  missing-field  close is empty  (line 5)
AAA  2024-01-08  DQ104 warn  missing-field  volume is empty  (line 6)
BBB  line 7  DQ104 error  missing-field  date "2024-13-01" is not a date; the row is skipped
$ echo $?
1
```

On a year of 30 liquid names and 10 thin ones, with a failed load on 2023-06-15
that 20 of the liquid names miss:

```console
$ dorq universe.csv
L25  2023-03-14  DQ301 warn  missing-run  1 session with no bar; nearby the series has a bar on 99.2% of sessions and trades a median 1,014,993 a day; P(feed outage) = 0.83
T0  2023-07-27..2023-12-11  DQ301 error  missing-run  96 sessions with no bar; nearby the series has a bar on 13.9% of sessions; P(feed outage) = >0.99
(all series)  2023-06-15  DQ303 error  cohort-gap  20 of 30 series that have a bar on nearly every session have none on this one, where healthy feeds would explain about 0.1; a failed load, not 20 separate gaps
```

The thin names' ordinary gaps aren't reported (`--show-info` shows them as
DQ302 sparse-series notes). The 20 members of the failed load are downgraded to
info, pointing at the cohort.

On [dorq-synth](tools/synth/README.md)'s synthetic market, with faults injected
among earnings gaps, a crash day, penny stocks moving a tick at a time and thin
names:

```console
$ dorq --select DQ2 bars.csv
LIQ01  2019-06-26  DQ201 error  bad-print  close 341.36 between 151.4 and 150.2 (×2.255, then back on the next bar); P(error) = >0.99  (line 1129)
LIQ03  2022-01-07  DQ203 error  unreported-split  close 199.99→67 (×0.335 ≈ a 3:1 split), volume ×3.087 after; P(error) = >0.99  (line 5768)
LIQ06  2020-08-24  DQ204 error  ohlc-close-mismatch  close 2.1 outside the bar's range 20.95..21.47, whose open 20.99 held the prior close 20.86; P(error) = >0.99  (line 11414)
LIQ13  2020-04-30..2021-02-08  DQ202 error  scale-shift  closes 2020-04-30..2021-02-08 are ×0.0009734 the level either side (×0.001): an era at the wrong scale; P(error) = >0.99  (line 25323)
THN02  2018-06-01  DQ205 error  history-segment  after 250 sessions without a bar, close 4.26→25 (×5.869): another security's history may continue here; P(error) = >0.99  (line 181756)
...
```

`--show-evidence` adds the suggested repair, the competing explanations and the
evidence for each: see [doc/output.md](doc/output.md).

## Usage

```bash
dorq prices.csv                              # check a file ("check" is the default command)
psql -c "COPY (...) TO STDOUT CSV HEADER" | dorq --format fafnir -   # or stdin
dorq --format jsonl --select DQ1 --ignore DQ106 a.csv b.jsonl
dorq --show-info --statistics prices.csv     # info too, then counts per check
dorq --calendar-file sessions.csv prices.csv # a reference calendar (default: built-in XNYS)
dorq --as-of 2026-10-05 prices.csv           # judge staleness (DQ304) against a date
dorq --show-evidence prices.csv              # repairs, hypotheses and evidence too
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
profiles. [doc/calendar.md](doc/calendar.md) covers the built-in calendars and
reference files. [doc/context.md](doc/context.md) covers the actions, metadata and market files. [doc/output.md](doc/output.md) covers each output format,
including the `fafnir` format that maps onto `ops.data_quality_flag`.

| Check | Name | Reports |
|---|---|---|
| [DQ101](doc/checks/DQ101.md) | ohlc-bounds | a high below open/low/close, or a low above open/close |
| [DQ102](doc/checks/DQ102.md) | non-positive | a price at or below zero, or a negative volume |
| [DQ103](doc/checks/DQ103.md) | duplicate-date | more than one row for a date |
| [DQ104](doc/checks/DQ104.md) | missing-field | an empty or unparseable value |
| [DQ105](doc/checks/DQ105.md) | non-session-bar | a bar on a day the calendar has no session |
| [DQ106](doc/checks/DQ106.md) | precision-shift | computed (e.g. back-adjusted) prices among quoted ones |
| [DQ107](doc/checks/DQ107.md) | zero-range-with-volume | a flat bar on the series' typical volume |
| [DQ108](doc/checks/DQ108.md) | out-of-bounds | a value outside the configured bounds |
| [DQ201](doc/checks/DQ201.md) | bad-print | a wrong bar, or block of up to five, that the series reverts from |
| [DQ202](doc/checks/DQ202.md) | scale-shift | a level change by a power of ten (an era at the wrong scale) |
| [DQ203](doc/checks/DQ203.md) | unreported-split | a level change by a split ratio, with volume moving inversely |
| [DQ204](doc/checks/DQ204.md) | ohlc-close-mismatch | a wrong close in a bar whose open, high and low held the level |
| [DQ205](doc/checks/DQ205.md) | history-segment | after a long gap, another security's history continuing |
| [DQ206](doc/checks/DQ206.md) | date-shift | a history dated a day early or late |
| [DQ209](doc/checks/DQ209.md) | large-move | info: a move that is probably real |
| [DQ301](doc/checks/DQ301.md) | missing-run | sessions with no bar that look like a feed outage, given how the series trades |
| [DQ302](doc/checks/DQ302.md) | sparse-series | info: a series that trades on few of its sessions |
| [DQ303](doc/checks/DQ303.md) | cohort-gap | many series missing the same session: a failed load |
| [DQ304](doc/checks/DQ304.md) | stale-feed | a series that stops before the as-of date |
| [DQ305](doc/checks/DQ305.md) | frequency-gap | missing periods in a weekly, monthly, quarterly or annual series |
| [DQ401](doc/checks/DQ401.md) | volume-scale-shift | volume stepping by a clean ratio (×100, ×1000) with no price change |
| [DQ402](doc/checks/DQ402.md) | volume-spike-no-move | info: volume ten times its usual level on a day the price did not move |
| [DQ403](doc/checks/DQ403.md) | move-on-zero-volume | a price change on a bar with no volume, where the series rarely has one |
| [DQ501](doc/checks/DQ501.md) | repeated-price | the same close on traded bars running: a stale feed |
| [DQ502](doc/checks/DQ502.md) | carry-bar | info: untraded bars that carry the last close |
| [DQ601](doc/checks/DQ601.md) | cohort-move | several series moving by the same split ratio on the same date |
| [DQ602](doc/checks/DQ602.md) | market-day | info: a day the market reference moved far beyond its range (`--market`) |
| [DQ701](doc/checks/DQ701.md) | split-misdated | a split on file the bars show a few sessions away (`--actions`) |
| [DQ702](doc/checks/DQ702.md) | split-without-jump | a split on file the bars do not show: not real, or already applied |
| [DQ703](doc/checks/DQ703.md) | split-ratio-mismatch | a split on file at the wrong ratio (often inverted) |
| [DQ704](doc/checks/DQ704.md) | split-double-applied | a split on file the bars apply twice |
| [DQ705](doc/checks/DQ705.md) | dividend-implausible | a dividend at or above the price, or ten times off the others |

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
version=0.5.0
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
git checkout v0.5.0                        # or stay on main for the latest
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
ctest --preset dev -R synth          # precision and recall on synthetic data
```

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

## Documentation

- [Configuration](doc/configuration.md): the config file, profiles, columns, input
- [Calendars](doc/calendar.md): built-in calendars and reference calendar files
- [Output formats](doc/output.md): text, json, jsonl, csv, fafnir
- [Checks](doc/checks/): one page per check, also printed by `dorq explain`;
  [DQ201](doc/checks/DQ201.md) describes the price model
- [dorq-synth](tools/synth/README.md): synthetic data with labelled faults, and the
  precision and recall gate
- [Development plan](doc/plans/dorq-development-plan.md): goals, checks, models, milestones
- [Architecture decisions](doc/adr/)
- [Contributing](CONTRIBUTING.md): conventions, tests, dependencies, releasing

## License

MIT. See [LICENSE](LICENSE).
