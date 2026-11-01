# Statements

Financial statements are dorq's third input kind, beside OHLCV bars and point
series ([ADR 0005](adr/0005-statements.md)). They are checked by the DQ9xx
family, which the [fundamentals plan](plans/dorq-fundamentals-plan.md) (M9) adds
check by check. This page is the input contract.

```bash
dorq --items items.csv statements.csv               # detected from the columns
psql -c "COPY (...) TO STDOUT CSV HEADER" | dorq --kind statements --items items.csv -
```

## The input

A row is **one version of one statement of one period**, keyed by its statement,
period type, fiscal date and version. fafnir sends every version current on its
load timeline (`superseded_at IS NULL`) of `core.income_statement`,
`core.balance_sheet` and `core.cash_flow_statement`, as one stream ordered by
`security_id`. Columns are matched by name, ignoring case, spaces, `_`, `-` and `.`.

| Column | Also accepted | Required | Meaning |
|---|---|---|---|
| `series` | `security_id`, `id`, `symbol`, `ticker` | no: the file's stem | the security |
| `label` | `symbol`, `ticker`, `name` | no | for people |
| `statement` | `statement_type`, `table_name`, `table` | yes | `income`, `balance_sheet` or `cash_flow`; fafnir's table names (`core.income_statement`) are read too |
| `period_type` | | yes | `quarter` or `annual` |
| `fiscal_date` | `reportperiod`, `period_end` | yes | the period's end |
| `fiscal_year` | | no | the fiscal year as the vendor labels it (`2025`) |
| `fiscal_period` | | no | `Q1` to `Q4`, or `FY` |
| `version_no` | `version` | no: 1 | 1 is as first reported; each restatement adds one |
| `known_from` | | yes | the first day the market could know this version |
| `known_to` | | no | the next version's `known_from`; empty while this version is in force |
| `knowledge_source` | | no | `filed`, `observed` or `estimated` (fafnir's ADR 0018) |
| `filing_date` | | no | the filing's date |
| `loaded_on` | `loaded_at` | no | the day the row was loaded; a timestamp is read as its day |
| `reported_currency` | `currency` | no | the reporting currency |
| `close` | `close_price`, `raw_close` | no | the raw close on or before `known_from`, for share-terms checks |
| any other column | | | a line item: see below |

The kind is detected when a file has a statement, a period type and a fiscal date
column; `--kind statements` (or `kind = "statements"`) names it. A file without
the four required columns is exit status 3. `--columns` names the fields by the
names in the table: `--columns fiscal_date=period,known_from=kf`.

A series is all the rows of one security, and is read as bars are: whole when the
input is small, and streamed, with every row of a security together, when it is
large or comes on stdin (`--buffer` reads everything first). A series' rows are
sorted by fiscal date, then statement, period type and version. One input file
holds one kind, and a series is one kind throughout.

### Line items

- **Which columns.** With `--items`, the columns it lists, and no others. Without
  it, every column that is not one of the fields above, except the bookkeeping
  columns fafnir's tables carry: `vendor_key`, `calendar_date`, `period_start_date`,
  `content_hash`, `extra`, `source`, `ingestion_run_id`, `superseded_at` and
  `dimension`.
- **NULL is not 0.** An empty value, or a missing-value marker (`NA`, `null`, JSON
  `null`), is NULL: the item does not apply to the row. `0` is a zero. SF1 writes 0
  for an item a filing does not carry, so a zero can mean "absent"; the check that
  judges that (DQ915, DR-0907) needs the two kept apart, and the reader keeps them apart.
- **Across files.** A security's rows can come from several files with different
  items. Each item is NULL on the rows of a file that does not have it.

### What cannot be used

As for bars, a row that cannot be used is a finding, not a failed run
([ADR 0004](adr/0004-bad-rows-are-findings.md)). DQ104 reports it:

| Problem | Then | Severity |
|---|---|---|
| A statement, period type, fiscal date or `known_from` that is empty or cannot be read; a `version_no` below 1, or not a whole number; a `known_to` that is not a date | the row is skipped | error |
| A line item that is not a number | the item is NULL on that row | error |
| A `fiscal_year`, `fiscal_period`, `knowledge_source`, `filing_date`, `loaded_on` or `close` that cannot be read | the field is left unset | warn |

## The views

A period is a statement, a period type and a fiscal date. Checks read three views
of a series' rows:

| View | Rows |
|---|---|
| Every version | every row |
| In force | each period's version with `known_from` on or before `--as-of`, and `known_to` after it or empty. With no `--as-of`, each period's open version: on an input of every current version, the view at its latest knowledge date |
| As first reported | each period's earliest version, normally version 1: what a backtest dated before a restatement reads |

Where versions do not chain and more than one is in force, the view takes the
highest. A period with no version known by `--as-of` is not in the in-force view.

## `--items`

The taxonomy: what each line item is. It is a CSV or TSV file in the shape of
fafnir's `ref.statement_line_item`, exported as it is:

| Column | Also accepted | Meaning |
|---|---|---|
| `column` | `column_name`, `item`, `item_name` | the input column |
| `statement` | | `income`, `balance_sheet` or `cash_flow` |
| `item_class` | `class` | `flow`, `stock`, `share_count`, `per_share` or `memo` |
| `sign_convention` | `sign` | `positive_is_inflow`, `positive_is_outflow` or `signed` |
| `is_subtotal` | `subtotal` | `true` or `false` (default `false`) |

```csv
column,statement,item_class,sign_convention,is_subtotal
revenue,income,flow,positive_is_inflow,false
cost_of_revenue,income,flow,positive_is_outflow,false
total_assets,balance_sheet,stock,signed,true
```

A file without the `column`, `statement`, `item_class` and `sign_convention`
columns is exit status 3. A row whose statement, class or sign cannot be read is
skipped, and its column is then not read as an item (DQ109, error). A column
listed twice keeps its first row (DQ109, warn). The file's content is part of
`config_hash`, as a reference calendar's is: the same items from another
directory are the same settings.

## `[statements]`

The judgements the statement checks make, as opposed to what the items are:

```toml
[statements]
identity_share = 0.005        # an identity holds within this share of its scale
tie_floor = 1e6               # or within this much of the reporting currency
cash_tie_share = 0.02         # the change in cash, within this share of the balance
quarter_spacing = [56, 126]   # days between consecutive quarter ends
year_spacing = [300, 430]     # and year ends
coverage_floor = "1998-01-01" # no period is expected before this
nonnegative = ["revenue", "total_assets"]          # items that cannot be negative
never_absent = ["revenue", "net_income"]           # items a reporting company always has
bounds = ["total_current_assets <= total_assets"]  # one item at most another

[statements.identities]       # named equations over items
balance_sheet = "total_assets = total_liabilities + total_equity_parent"
cash_flow = "net_change_in_cash = net_cash_operating + net_cash_investing + net_cash_financing"
```

- An identity is item names joined by `+` and `-`, with one `=` between the two
  sides. A bound is `item <= item`.
- A file that includes another (`include`) adds identities to the ones it
  includes, or replaces one of the same name. The lists replace the included
  file's lists, as every list in the config does.
- With `--items`, every item the table names must be one it lists: another is a
  configuration error (exit status 2). Without `--items` nothing is checked.
- The lists and identities are empty by default, so dorq names no vendor's items.
  fafnir's come with the checks that use them (DR-0905).
- The table is global: a profile does not change it.
- `config_hash` covers the table only where it differs from its defaults, so the
  release that added it changed no existing hash.

## What runs on statements

| Check | On statements |
|---|---|
| DQ9xx | only on statements (added from DR-0904) |
| DQ104 | on every kind: a field that cannot be used |
| DQ109 | on every context file, `--items` included |
| every other check | not on statements: DQ103 would read a restatement as a duplicate date, and DQ105 a fiscal date on a Sunday as a bar on a closed day |

`dorq list-checks` shows the kinds each check applies to.

## Output

A statement finding names its period: in text as `2024-06-30 Q2 FY2024 income
v1`, in the JSON formats as `statement`, `period_type`, `fiscal_year`,
`fiscal_period` and `version_no` fields, and in `record_key` as
`{"statement", "period_type", "fiscal_date", "version_no"}` plus an `item` or
`identity` where the condition is per item. The `fafnir` format files it under its
statement's table (`[fafnir.tables]`). [output.md](output.md) has each format.

## `--since` and `--as-of`

- **`--since D`** keeps a statement finding unless every version it names was
  known, and loaded, before D: it goes by the newer of `known_from` and
  `loaded_on`, not by the fiscal date. A restatement of a 2015 period loaded last
  night is new. A finding on a row with no `known_from` is always kept.
- **`--as-of D`** is the date of the in-force view.
