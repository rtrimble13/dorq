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
copy_min_items = 5            # DQ914: a copy repeats at least this many items
nonnegative = ["revenue", "total_assets"]          # items that cannot be negative
never_absent = ["revenue", "net_income"]           # items a reporting company always has
null_is_zero = ["fx_effect_on_cash"]               # items an identity reads as 0 where NULL
bounds = ["total_current_assets <= total_assets"]  # one item at most another
cash_tie = ["cash_and_equivalents", "net_change_in_cash"]  # DQ904: cash, and its change
restricted_cash_from = "2018-01-01"  # from here the cash flow includes restricted cash,
restricted_cash_share = 0.10         # up to this share of the balance

[statements.identities]       # named equations over items
balance_sheet = "total_assets = total_liabilities + total_equity_parent"
cash_flow = "net_change_in_cash = net_cash_operating + net_cash_investing + net_cash_financing"

[statements.explained_by]     # DQ903: items that explain an identity's gap
balance_sheet = ["net_income_noncontrolling"]

[statements.priors]           # the value model's defaults (DQ911 to DQ919), for DR-0911 to fit
bad_value = 0.001             # the prior that any one value is wrong in each way
unit_error = 0.0002
sign_error = 0.0002
flow_scale = 0.08             # the class prior on a period's move (asinh scale): flows
stock_scale = 0.05            # and stocks
prior_weight = 6              # in periods, against the item's own residuals
```

- An identity is item names joined by `+` and `-`, with one `=` between the two
  sides. A bound is `item <= item`.
- A file that includes another (`include`) adds identities, and `explained_by`
  entries, to the ones it includes, or replaces one of the same name. The lists
  replace the included file's lists, as every list in the config does.
- `null_is_zero` names items an identity reads as 0 where NULL, as fafnir's cash
  flow tie reads the exchange-rate effect most companies leave out. Any other
  NULL term leaves the identity unjudged on that row.
- `explained_by` names, for an identity, the items whose presence in a period
  explains its gap: noncontrolling interests, which SF1's balance sheet leaves out
  of `total_equity_parent`. Each entry must name an identity the table defines
  (exit status 2 otherwise).
- `cash_tie` is two items or none: the balance sheet's cash, and the cash flow's
  net change in it. From `restricted_cash_from` (ASU 2016-18), the cash flow
  reconciles cash and restricted cash, so the tie allows up to
  `restricted_cash_share` of the balance, where it otherwise allows
  `cash_tie_share`.
- With `--items`, every item the table names must be one it lists: another is a
  configuration error (exit status 2). Without `--items` nothing is checked.
- The lists and identities are empty by default, so dorq names no vendor's items.
  fafnir's are in `priors/fafnir-statements.toml`.
- The table is global: a profile does not change it.
- `config_hash` covers the table only where it differs from its defaults, so the
  release that added it changed no existing hash.

## What runs on statements

| Check | On statements |
|---|---|
| DQ901 | every version: rows that collide, versions that do not chain, knowledge dates that cannot be right |
| DQ902 | every version: the `nonnegative` items and `bounds` above |
| DQ903 | every version: each identity, and why it fails where it does |
| DQ904 | the periods in force: the change in cash (`cash_tie`), and a year's quarters against the year (`--items`' flows) |
| DQ911, DQ912, DQ913, DQ919 | both views, in force and as first reported: each money item over time (the value model, below) |
| DQ914, DQ915, DQ916 | both views: copied rows, zeros where the item is reported either side, and quarters to date (`--items`' flows) |
| DQ921, DQ922 | both views: the share counts `--items` lists, in one share terms with `--actions`' splits (share counts, below) |
| DQ931, DQ932 | every version: each period's filing date against the issuer's own lags, and each version against the one before it (the knowledge timeline, below) |
| DQ941, DQ942, DQ943, DQ944 | the period sequence, with `--meta`'s trading life: a period missing, the series stale by `--as-of`, statements missing, another currency (coverage and freshness, below) |
| DQ951 | the whole input, once every series is in: revisions and findings by the day they became known |
| the other DQ9xx | only on statements, as the plan adds them |
| DQ104 | on every kind: a field that cannot be used |
| DQ109 | on every context file, `--items` included |
| every other check | not on statements: DQ103 would read a restatement as a duplicate date, and DQ105 a fiscal date on a Sunday as a bar on a closed day |

`dorq list-checks` shows the kinds each check applies to. Each DQ9xx check is held to
its precision and recall on dorq-synth's statements universe
([tools/synth/README.md](../tools/synth/README.md#statements)), whose faults and hard
negatives follow the plan's §6.

## Identities and ties

An identity holds within the larger of `identity_share` of its largest term and
`tie_floor`: fafnir's tolerance. DQ903 judges each version's identities, and
weighs a gap beyond it as the company's structure (the same gap in the periods
around it, or on one side of it; the identity failing in every period around it;
or an `explained_by` item reported), a reclassification (another
version of the period holds, with the same total), a field error (the gap is a
term, twice one, or one at a thousand or a million times), or unexplained. The
first two are filing facts, at info; a field error names the field
([DQ903](checks/DQ903.md)).

DQ904 ties what an identity within one row cannot: the change in cash between
two balance sheets in force, within `quarter_spacing` or `year_spacing`, against
the later period's cash flow; and, for each flow item `--items` lists, a fiscal
year's four quarters against its one year. It names the row out of line: one
balance sheet's cash where two ties break and cancel, a quarter, or the year
([DQ904](checks/DQ904.md)).

## The value model

DQ911 bad-value, DQ912 unit-error, DQ913 sign-error and DQ919 large-change read
each money item (a flow or a stock in `--items`; every item without it) of each
statement over time, quarters and years apart, in both views. A value both views
hold alike is one finding; a version 1 that a restatement put right is judged
in the first-reported view, and reported on version 1.

- **The scale.** An item is read as `z = asinh(v / s)`, `s` the median magnitude
  of its non-zero values in the line: close to a log for large values, and safe
  for zeros and signs.
- **The prediction.** A quarterly flow is predicted from the same quarter a year
  before and after, moved by the change since in the adjacent quarters
  (seasonality); any other line from the two periods either side. Each prediction
  leaves out the values the last pass found far from theirs, so that one wrong
  value does not make its neighbours look wrong.
- **The scale of a move.** The spread of the item's residuals over the twelve
  periods either side, shrunk to the class prior (`flow_scale`, `stock_scale`,
  half again for years) by `prior_weight`, and never below it.
- **Rows and eras first.** A step at a power of ten between two periods that most
  of a row's money items take, and a step back, is a row or a run of rows stored
  at that power: reported once (DQ912), and put right before anything else is
  judged.
- **Screening.** A value is a candidate when its predictive tail probability is
  below 1e-3, when it is five times or a fifth of both neighbours, or, for an item
  with a sign, when its sign differs from both. A zero, and a value next to one,
  are left to DQ915 (DR-0907).
- **The hypotheses.** `real_change`, `bad_value`, `unit_error` and `sign_error`.
  A real change is mostly small, and as large as an error as often as the item's
  own values stray that far; an error's repair (the value divided by its power of
  ten, negated, or the value an identity needs) is judged by the same real-change
  model. The priors are `[statements.priors]`.
- **The evidence.** Identities that hold as stored or once repaired; the year's
  quarters against the year; the row's other items moving with it, or by the same
  factor; a subtotal moving by the same amount; the level holding, or going back;
  the item's sign history ([DQ911](checks/DQ911.md) lists them).
- **The code** is the likeliest error's, at the severity its `p_error` gives;
  where a real change is likeliest, DQ919 at info for a change large for its item,
  or nothing. The newest period of a line is provisional, and at most warn.

On 6M synthetic rows, fafnir's history's size, the model adds about 17 seconds to
a run on four threads ([performance.md](performance.md#statements-dr-0906)).

## The quarterly structure

DQ914 copied-period, DQ915 absent-as-zero and DQ916 cumulative-quarters are the
value model's row and zero hypotheses (fundamentals plan §4.1), judged on the same
lines, predictions and identities:

- **DQ914.** A row that repeats the period before it to the unit on at least
  `copy_min_items` items, four in five of those both report and two of them flows,
  against the chance of that many repeats from each item's own repeats in the
  series.
- **DQ915.** A 0 where the item is reported either side, weighed against a real 0:
  as likely as the item's own zeros, and its chance of falling to 0 under the value
  model. Items in `never_absent` are rarely absent for real. The evidence is the
  row's identities, the year's quarter sum and its subtotals, which a value the
  filing did not carry leaves as filed.
- **DQ916.** A fiscal year's quarters stored year to date, or one quarter holding
  the year, on the flows `--items` lists, for a year with four quarters and one
  year in force.

The rows DQ914 and DQ916 explain are not reported again by DQ911 or DQ919, and
DQ911 leaves zeros, and values next to one, to DQ915.

## Share counts

DQ921 share-terms-step and DQ922 share-count-mismatch judge the counts `--items`
lists as `share_count`: the income statement's weighted averages and the balance
sheet's outstanding shares (fundamentals plan §4.3).

- **One terms.** A count is stored in the terms of its version's `known_from`
  (fafnir's ADR 0018). dorq multiplies each count by the splits on file with
  ex-dates after its `known_from`, up to the latest knowledge date in the series.
  After that, a count moves only by issuance and buybacks, and the periods' two
  counts can be compared.
- **DQ921.** A count that still steps by a clean ratio or a power of ten is weighed
  as four things:
  - a real issuance;
  - a split missing from the actions, its ex-date between the two counts'
    knowledge dates (a context gap, with `add_split`);
  - a count stored in the wrong terms against a split on file;
  - a count stored at a power of ten.

  The evidence: the step net of the count's trend; where the period's weighted
  average sits between its outstanding counts; the market cap the count implies
  with `close`; and the price before the step. Each missing split is one report,
  on its narrowest step, with the steps it explains folded in. That includes the
  ones a restatement filed years later leaves.
- **DQ922.** A period whose weighted average and outstanding count are a clean
  ratio or a power of ten apart, which no split on file explains.

DQ921 needs `--actions`: without it there are no splits to put the counts into one
terms, and it judges nothing. DQ922 runs with `--items` alone, and then weighs a
missing split wherever the two counts were known on different dates.

## The knowledge timeline

DQ931 filing-date-implausible, DQ932 restatement-implausible and DQ951
revision-cohort judge when a version became known (fundamentals plan §4.4, §4.6):

- **DQ931.** Each period's first filed version, its lag from the period end
  against the issuer's own lags of the same form (10-Q, or the 10-K for a year and
  its last quarter) and the SEC's deadlines. A date at the period end, or far
  before any filer's, is a look-ahead and can reach error. A date after the next
  period's is an amendment's, at warn. A long lag otherwise is a late filing, a
  filing fact, at info.
- **DQ932.** Each pair of a period's consecutive versions. The changed items'
  pattern (a power of ten, a sign, two values swapped, a total alone, or an amount
  moved between two items) and how each version sits with the periods around it
  tell a restatement from a new version stored wrong, or from an old version that a
  restatement corrected. An old version wrong is reported on the old version,
  which every point-in-time read in its interval saw.
- **DQ951.** Across the input: many series revised on one day (a backfill, at
  info), or carrying one finding in rows known the same day (a load fault, at
  warn), beyond the base rate. A load fault's DQ932 and DQ903 reports go to info,
  linked to it. While DQ951 runs, results are held until every series is in.

## Coverage and freshness

DQ941 period-gap, DQ942 statements-stale, DQ943 statements-missing and DQ944
reporting-currency succeed fafnir's `fundamentals_gap`, `fundamentals_ttm_incomplete`,
`fundamentals_stale`, `fundamentals_coverage` and `fundamentals_currency_mismatch`
(fundamentals plan §4.5). Where those used fixed spans, these go by the issuer's own
cadence and lags, and by `--meta`'s trading life:

- **DQ941.** Each period type's sequence, by spacing and labels: a missing period
  while the security traded, a change of fiscal year (info), a mislabelled quarter,
  or a period not yet filed (info).
- **DQ942.** With `--as-of`: no newer period by then, against the next period's end
  plus the issuer's usual lag. A security no longer trading is not stale.
- **DQ943.** A security `--meta` expects statements for, with none. After the
  input, dorq visits each meta series it did not have, in the meta file's order.
- **DQ944.** Statements in another currency than the one the security trades in,
  at info.

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
