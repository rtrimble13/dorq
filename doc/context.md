# Context inputs

dorq judges a series by its own bars. Three optional files add context:

| Option | What it holds | What it does |
|---|---|---|
| `--actions F` | splits and dividends on file | explains moves at a split's ex-date; enables DQ701–DQ705 |
| `--meta F` | per-series metadata | profiles match on it; `tick_size` replaces the inferred tick; `peer_group` feeds DQ601 |
| `--market F` | a market reference series (an index, SPY) | moves are judged net of the market; enables DQ602 |

fafnir passes exports of `core.corporate_action` and `core.security`, and dorq
accepts their column names as they are. Each file is CSV or TSV: the delimiter is
whichever of tab and comma the header line has more of. Column names are matched
ignoring case, spaces, `_` and `-`.

A file that cannot be read as a whole stops the run with exit status 3, naming
the file: one that cannot be opened, is empty, lacks a required column, or has an
unterminated quote. A row that makes no sense (a split with no ratio, a dividend
with no amount, an unknown action type, a date that is not one) is skipped and
reported as DQ109, and the run goes on: one bad row must not cost every other
check ([ADR 0004](adr/0004-bad-rows-are-findings.md)). In a metadata file, a field
that cannot be read is left unset, and a series listed twice keeps its first row.

Series are matched by id: the series column of the bars. When a series has no row
under its id, its label (a ticker, from `--columns label=...`) is tried.

## Corporate actions: `--actions`

| Column | Also accepted | Meaning |
|---|---|---|
| `series` | `security_id`, `id`, `symbol`, `ticker` | the series |
| `ex_date` | `date`, `effective_date`, `trade_date` | the ex-date |
| `type` | `action_type`, `kind` | `split` or `dividend` |
| `numerator` | `split_numerator` | new shares (a 2-for-1 split is 2:1) |
| `denominator` | `split_denominator` | old shares |
| `amount` | `dividend_amount`, `dividend` | the cash dividend per share |

```csv
security_id,ex_date,action_type,split_numerator,split_denominator,dividend_amount
LIQ02,2022-01-24,split,2,1,
LIQ48,2015-05-27,dividend,,,0.26
```

A split multiplies the price by `denominator / numerator` from its ex-date on. dorq
expects **raw** bars, not adjusted ones (plan Q3): only raw bars with the actions
beside them can show a split that is misdated, not real, at the wrong ratio, or
applied twice.

With `--actions`:

- **A split on file explains the move it spans.** When a split's ex-date falls
  after one bar and on or before the next, the price model weighs a further
  explanation, `explained_split`: the move is the split's ratio plus an ordinary
  move (doc/checks/DQ201.md). The move is then reported as DQ209 (info), "the 2:1
  split on file". This holds when the ex-date is a session with no bar, as on a
  thin name: the split is between two stored bars. A zero-volume bar that moves by
  a split's ratio across its ex-date is not a DQ403 move.
- **Every split on file is judged against the bars** (DQ701–DQ704).
- **Dividends are checked** against the price and the series' other dividends
  (DQ705). A zero or negative amount is read and reported there, not refused:
  fafnir's `core.corporate_action` allows a zero.
- A split the bars show but the file does not have is still DQ203.

## Metadata: `--meta`

| Column | Also accepted | Meaning |
|---|---|---|
| `series` | `security_id`, `id`, `symbol`, `ticker` | the series |
| `asset_type` | `type` | `equity`, `etf`, `fund`, `rate`, ... (matched ignoring case) |
| `nav_priced` | `nav` | `true` or `false` |
| `tick_size` | `tick` | the exchange tick |
| `peer_group` | `family` | series that move together (a fund family) |
| `exchange` | `exchange_code`, `mic` | the listing exchange |

Every column but `series` is optional, and a series may appear only once.

- **Profiles** can match `asset_type`, `nav_priced`, `exchange` and `peer_group`
  (doc/configuration.md). A profile that matches on one of these never matches a
  series the file does not list.
- **`tick_size`** replaces the tick dorq infers from the date and price, in the
  price model's grid (DQ2xx), DQ107 and DQ403. The grid is still at least the
  decimals the prices are written to.
- **`peer_group`**: siblings in a group that move by the same split ratio on the
  same day are evidence that each split (DQ601).

## Market reference: `--market`

A file holding one series, bars or points, read like the main input (CSV, TSV,
JSON Lines or JSON; columns found by their usual names). Its close, or value, is
the market's level.

With `--market`, for each series on the log scale:

- **The market's move is taken out.** Each bar's return is reduced by beta times
  the market's return over the same span (from the market's level on or before
  each bar's date). Beta is a discounted regression of the series' returns on the
  market's either side of the bar (about 100 returns each way), shrunk toward the
  series' own beta, which is shrunk toward 1 by twenty market-sized returns.
  Returns spanning more than five sessions, or more than five robust standard
  deviations, do not count toward beta: a split or a bad print says nothing about
  it.
- **The volatility estimate and every hypothesis then see the residual move**: a
  crash day's fall is the market's, not the series'.
- **A beta error widens the ordinary move**: the residual of a market move m
  carries an error of about 0.5 × m. On a quiet day that is nothing; on a crash
  day it widens every series' ordinary move, so correlated moves stop producing
  reports.
- **DQ602** reports, at info, the days the market moved five or more standard
  deviations of its own recent moves.

Point series on the difference scale (rates) are not adjusted.
