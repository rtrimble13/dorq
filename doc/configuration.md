# Configuration

dorq works with no configuration. When you do want settings, there are three
layers, each overriding the one before:

1. The defaults.
2. A config file.
3. Command-line options.

`dorq config show` prints the effective settings, where they came from, and their
`config_hash`. `dorq config init` writes a starter `dorq.toml` that lists every
key at its default value.

## Where the file is found

1. `--config FILE`, if given. `--isolated` ignores every config file.
2. Otherwise dorq starts in the current directory and walks up through its
   parents, stopping at the first directory that contains `.git`. In each
   directory it looks for:
   - `dorq.toml`;
   - `pyproject.toml` with a `[tool.dorq]` table.
3. Otherwise `$XDG_CONFIG_HOME/dorq/dorq.toml`, or `~/.config/dorq/dorq.toml`.

In `dorq.toml`, keys go either at the top level or under `[tool.dorq]`. If you
use `[tool.dorq]`, every table must be under it as well: a `[columns]` header
after `[tool.dorq]` is an error, because TOML would read it as a separate
top-level table.

**An unknown key is an error** (exit status 2), and the message names the file
and line. A typo in a data-quality configuration should not be silently ignored.

## Keys

```toml
# Which checks run. Each entry is a code ("DQ101"), a code prefix ("DQ1") or a
# name ("ohlc-bounds"). As in flake8, the most specific entry wins: a check runs
# when its longest matching select entry is longer than its longest matching
# ignore entry. A name counts as the most specific match.
select = ["DQ"]
extend_select = []     # added to select (for a file that only wants to add)
ignore = []

min_severity = "warn"  # report at or above: info, warn, error
fail_on = "warn"       # exit 1 at or above: info, warn, error, never
format = "text"        # text, json, jsonl, csv, fafnir
threads = 0            # 0: one per core; output does not depend on it
kind = "auto"          # auto, ohlcv, point
input_format = "auto"  # auto, csv, tsv, jsonl, json

[columns]              # column names, when the aliases do not find them
date = "trade_date"
series = "security_id"
label = "symbol"
value = "DGS10"        # a point series' value ("close" means the same)

[integrity]            # see doc/checks/DQ102.md, DQ106.md, DQ107.md and DQ108.md
positive_point_series = false
precision_high_decimals = 5
precision_high_sig_figs = 5
precision_min_segment = 20
precision_min_contrast = 0.8
flat_bar_steps = 3    # DQ107: flat bars are ordinary where bars span this many grid steps
# bounds = [-5, 25]   # DQ108: the range values must lie in; none by default

[coverage]            # DQ3xx; see doc/checks/DQ301.md for the model
frequency = "auto"    # or daily, weekly, monthly, quarterly, annual, irregular
block_sessions = 60
outage_start = 0.0001
outage_end = 0.05
prior_density = 0.999
prior_strength = 2
trade_size = 1000
sparse_density = 0.8  # DQ302
publication_lag = 1   # DQ304
report = "run"        # DQ301: or "session"           (global only)
cohort_min_series = 3 # DQ303                          (global only)
cohort_max_tail = 1e-06                              # (global only)
confident_density = 0.95                             # (global only)

[price]               # DQ2xx; see doc/checks/DQ201.md for the model
candidate_tail_prob = 0.001  # score a bar whose return is this improbable,
floor_move = 0.5      # and every move of 50% or more either way
revert_max_bars = 5   # the longest bad print
volume_window = 40    # bars either side for volume levels
ratio_tolerance = 0.01
split_ratios = ["2:1", "3:1", "3:2", "4:1", "5:1", "5:4", "8:1", "10:1", "15:1", "20:1",
                "1:2", "1:3", "1:4", "1:5", "1:8", "1:10", "1:15", "1:20", "1:25", "1:30",
                "1:40", "1:50", "1:100"]
provisional_bars = 3  # fewer bars after one than this: provisional, at most warn
segment_gap = 60      # DQ205
volatility_discount = 0.97
tail_dof = 4
jump_prob = 0.03
jump_scale = 6
min_price = 1e-05     # prices outside [min_price, max_price] are implausible
max_price = 1000000
transform = "auto"    # point series: "log", "diff" (rates, spreads) or "auto" (log if all > 0)

[priors]              # each explanation's prior weight for a scored bar
market_move = 0.9
bad_print = 0.05
bad_close = 0.01
unreported_split = 0.02
scale_error = 0.02
tick_move = 0.01
history_segment = 0.05
explained_split = 0.9 # with --actions: a split on file between the bars explains the move
split_on_file_error = 0.06  # with --actions: a split on file being wrong (DQ701-DQ704)
stale_run = 3e-05     # DQ501: a run of repeated closes being a stale feed, per bar

[severity]            # p_error -> severity, for the probabilistic checks
info = 0.2            # below this, nothing is reported
warn = 0.6
error = 0.9

[calendar]            # see doc/calendar.md
name = "XNYS"         # or weekdays, 24x7
file = "sessions.csv" # optional reference calendar, relative to this file
exchange = "NASDAQ"   # which exchange to take from a multi-exchange file

[fafnir]
table_name = "core.daily_price"
```

Lists can also be written as one comma-separated string (`ignore = "DQ106, DQ107"`).

## Profiles

A profile applies settings to the series it matches. It can match on
`kind = "ohlcv" | "point"`, on a list of series ids, and on the fields of a
`--meta` file: `asset_type`, `nav_priced`, `exchange` and `peer_group` (each a
value or a list, but `nav_priced`, which is true or false). Every key given must
hold, and a key on a metadata field never holds for a series the file does not
list. A profile with no `match` table applies to every series.

```toml
[profiles.rates]
match = { kind = "point" }
ignore = ["DQ106"]
price = { transform = "diff" }           # moves are changes: rates cross zero
integrity = { bounds = [-5, 25] }        # a yield in percent (DQ108)

[profiles.nav_funds]
match = { nav_priced = true }            # from --meta
ignore = ["DQ107", "DQ4"]                # a NAV strike is a flat bar, on no volume
integrity = { positive_point_series = true }

[profiles.etfs]
match = { asset_type = ["etf"], exchange = "ARCX" }
priors = { unreported_split = 0.05 }     # fund families split more often
```

A profile's `select` and `ignore` entries are **added** to the global lists. Its
`integrity`, `coverage`, `price` and `priors` settings replace the global ones
for the series it matches. The run-wide coverage keys (`report` and the cohort settings) can't be
set per profile. When two
profiles match the same series, they are applied in name order, so where they
disagree the later name wins.


## Command-line options

The options with the same meaning as a key above are `--select`, `--extend-select`,
`--ignore`, `--min-severity`, `--fail-on`, `--format`, `--threads`, `--kind`,
`--input-format`, `--calendar`, `--calendar-file`, `--calendar-exchange` and
`--columns date=trade_date,series=security_id`. Each one overrides the file:

- `--select` replaces the list.
- `--extend-select` and `--ignore` add to their lists.
- A `--columns` field replaces that field's column.

`--actions F`, `--meta F` and `--market F` name the context inputs, which have no
config keys; doc/context.md describes them. `--show-info` is `--min-severity info`. `--show-evidence` adds each violation's
suggested action, hypotheses and evidence to text output (doc/output.md). `--exit-zero` always exits 0. `--as-of
DATE` sets the date DQ304 judges staleness against; by default it is the latest
session with a bar anywhere in the input.

## Reading input

- **Formats.** CSV, TSV, JSON Lines and JSON arrays. The format is detected from
  the file's extension or by sniffing its content. JSON records must be flat; a
  number's precision is read from its text.
- **Columns.** Columns are matched by alias, ignoring case, spaces, underscores
  and hyphens: `Date`, `trade_date`, `Adj Close`, `Vol` and so on. When an input
  has `close` as well as `adj_close`, the raw `close` wins. A lone `symbol` column
  names the series; beside a `security_id` column it becomes the label. A FRED
  download (`observation_date,DGS10`) works as it is: the one column left over is
  the value.
- **Grouping.** Up to 256 MiB of input in total is read whole before checking, so
  rows may come in any order. Larger input, and stdin, is checked as it is read.
  That keeps memory to roughly one series per thread, but it needs every row of a
  series together, as `ORDER BY series, date` gives. A series that reappears
  later in the input is an error (exit 3). `--buffer` reads everything first
  whatever the size.
- **Empty input.** An empty input is an error (exit 3). A pipeline whose export
  step failed looks exactly like an empty input, so an empty input is not treated
  as a clean run. A header with no rows is fine.
