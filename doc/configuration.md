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

[integrity]            # see doc/checks/DQ102.md and DQ106.md
positive_point_series = false
precision_high_decimals = 5
precision_high_sig_figs = 5
precision_min_segment = 20
precision_min_contrast = 0.8

[fafnir]
table_name = "core.daily_price"
```

Lists can also be written as one comma-separated string (`ignore = "DQ106, DQ107"`).

## Profiles

A profile applies settings to the series it matches. It can match on
`kind = "ohlcv" | "point"`, on a list of series ids, or on both. A profile with
no `match` table applies to every series.

```toml
[profiles.rates]
match = { kind = "point" }
ignore = ["DQ106"]

[profiles.nav_funds]
match = { series = ["VFIAX", "TDEAX"] }
ignore = ["DQ107"]                       # a NAV strike is a flat bar
integrity = { positive_point_series = true }
```

A profile's `select` and `ignore` entries are **added** to the global lists. Its
`integrity` settings replace the global ones for the series it matches. When two
profiles match the same series, they are applied in name order, so where they
disagree the later name wins.

Matching on fields from a metadata file (`asset_type`, `nav_priced`) arrives with
`--meta` in M5.

## Command-line options

The options with the same meaning as a key above are `--select`, `--extend-select`,
`--ignore`, `--min-severity`, `--fail-on`, `--format`, `--threads`, `--kind`,
`--input-format` and `--columns date=trade_date,series=security_id`. Each one
overrides the file:

- `--select` replaces the list.
- `--extend-select` and `--ignore` add to their lists.
- A `--columns` field replaces that field's column.

`--show-info` is `--min-severity info`. `--exit-zero` always exits 0.

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
