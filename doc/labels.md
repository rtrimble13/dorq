# Labels

A label records what a person decided about a stretch of one series. Was the value
wrong, and repaired? Was it right but missing context, such as a split that had
to be added? Or was it anomalous but true? `dorq calibrate` fits the price model
to labels ([calibration.md](calibration.md)). [dorq-eval](../tools/eval/README.md)
scores dorq's output against them. In fafnir, labels come from
`ops.operator_override`: every repair is a decision (plan section 6).

## The file

Labels are JSON Lines: one flat object per line. Blank lines are skipped.

```json
{"series":"AKR","first":"2020-03-02","last":"2020-03-04","class":"data_error","expect":"DQ202","kind":"scale_era","source":"operator_override","note":"closes stored in cents"}
{"series":"LIQ07","date":"2021-06-14","class":"context_gap","expect":"DQ203","kind":"split_missing"}
{"series":"LIQ12","date":"2020-03-16","class":"market_fact","kind":"crash"}
{"series":"*","date":"2019-08-20","class":"data_error","expect":"DQ303","kind":"failed_load"}
{"series":"752016","first":"2016-01-04","last":"2026-03-31","class":"data_error","expect":"DQ202","kind":"scale_era","decided_at":"2026-10-13T18:50:22Z","applies_until":"2026-10-13T18:50:22Z","verified":true}
{"series":"*","date":"2026-10-06","class":"market_fact","codes":"DQ601","kind":"coincidence","members":"3560,5286,19195"}
```

| Field | Meaning |
|---|---|
| `series` | The series id: the series column of the bars, or the label (a ticker) when no id matches. `*` means every series, for a cross-sectional fault (DQ303, DQ601) or a market-wide fact |
| `first`, `last` | The dates the label covers. `last` defaults to `first` |
| `date` | Shorthand for `first` and `last` being the same date |
| `class` | `data_error`: the stored value was wrong and was repaired. `context_gap`: the value was right, but context was missing, such as a split that had to be added to `core.corporate_action`. `market_fact`: anomalous but correct, so nothing should be reported |
| `expect` | For a fault: the code dorq should report, such as `DQ201`. Alternatives are separated by `\|`, as in `DQ201\|DQ204`. Empty means any code |
| `codes` | For a market fact: the code prefix that would be wrong to report. The default is `DQ2`, any price check |
| `kind` | Free text, such as `bad_print` or `hn_earnings`. dorq-eval groups hard negatives by kind |
| `source`, `note` | Free text, for people |
| `remove` | Comma-separated dates that a repair *added* to the series, such as a shifted bar's new date. With `--restore` these dates are dropped |
| `decided_at` | When the label was decided: an ISO 8601 date or timestamp (`2026-10-13T18:50:22Z`; no offset means UTC). For people, and for the order of decisions |
| `applies_until` | The label applies only to runs whose input predates this time. A repair's label describes data the repair replaced: after it, dorq's reports on the corrected bars are not about that fault. dorq-eval's `--input-time` drops the labels whose time has passed |
| `verified` | `true` when a person checked the label bar by bar; `false` for a bulk acceptance. dorq-eval reports precision on every label and on the verified ones. Absent means not verified |
| `members` | For a cohort label (`series` `*`): the members' ids, comma-separated as DQ601's `detail.members` is. The label matches a DQ601 row whose members overlap them. A market fact with `codes` DQ601 makes a coincidental cohort row false |

Other keys are ignored, so an exporter can keep extra fields for people.

### Kinds fafnir sends

`kind` is free text, but fafnir's exports use these, with the class and the
codes each implies:

| Kind | Class | `expect` or `codes` |
|---|---|---|
| `feed_gap` | data_error | DQ304, or DQ301 for a gap the feed later filled |
| `odd_lots_only` | market_fact | codes DQ304: no round-lot trade, so no bar is right |
| `distribution` | market_fact | codes DQ705 for the dividend, DQ2 for the price's drop |
| `dividend_units` | data_error | DQ705 |
| `wrong_instrument` | data_error | DQ202\|DQ205: another security's prices stored under this one |
| `split_deleted` | data_error | DQ702\|DQ704 |
| `split_redated` | data_error | DQ701 |
| `dividend_deleted` | data_error | DQ705 |
| `dividend_added` | context_gap | DQ201\|DQ203: the move was reported before the dividend reached the file | A line
that cannot be read stops the run with exit status 3, and the message names the
line. That happens when the JSON is malformed, when `series`, `first` or `class`
is missing, when a date or class is invalid, or when `last` is before `first`.

## What the price model learns from a label

`dorq calibrate` uses a label when a scored price move falls within 3 calendar
days of the label's span. The label says which explanations are acceptable:

| Label | Explanations it allows |
|---|---|
| `expect` DQ201 | bad print |
| `expect` DQ202 | scale error |
| `expect` DQ203 or DQ601, or a `context_gap` with no `expect` | unreported split |
| `expect` DQ204 | bad close |
| `expect` DQ205 | history segment |
| `expect` of several codes (`DQ202\|DQ205`) | each code's |
| `data_error` with no `expect` | any error |
| `market_fact` with `codes` DQ, DQ2 or DQ2x | market move, tick move, explained split, explained dividend, pending distribution |

A `context_gap` also allows the explanations that need context (explained split,
explained dividend, pending distribution): its repair is to put the split or the
dividend on file, and once it is, the same move is explained.

Labels about something the price model does not judge are counted but not used.
That covers gaps (DQ3xx), volume (DQ4xx), stale runs (DQ5xx) and splits on file
(DQ7xx). When a fault label and a market fact cover the same move, the fault
wins.

## Series as they stood before repair: `--restore`

fafnir repairs a fault in place. Once repaired, the bars hold the corrected value,
and the label's evidence is gone. `--restore F` rebuilds each series as it stood
*before* the repairs. It is accepted by `dorq check` and `dorq calibrate`.

- `F` is a bars file in the same format as the input. It must name its series in a
  series column, even for a single series. Each of its rows replaces the input's
  row for that series and date, or is added when the input has no such row.
- With `--labels`, the labels' `remove` dates are dropped from their series.
- The rows are then sorted by date, keeping input order among equal dates.

```bash
# before.csv: each repaired row with the values it had before the repair
# (in fafnir, exported from ops.operator_override with the labels)
dorq calibrate prices.csv --restore before.csv --labels labels.jsonl --out priors.toml
dorq check prices.csv --restore before.csv --labels labels.jsonl --format csv --show-info
```

## Actions as they stood before repair: `--restore-actions`

A repair to `core.corporate_action` (a deleted duplicate split, a re-dated one, a
dividend added or removed) changes what dorq sees as much as a repaired bar
does: after the delete, the DQ702 that prompted it has nothing to report.
`--restore-actions F`, on `dorq check` and `dorq calibrate`, puts the actions
back as they stood, over `--actions`.

- `F` is an actions file in the `--actions` format, with an optional
  `added_by_repair` column (true or false).
- For each series, ex-date and type (split or dividend) it names, its rows
  replace every `--actions` row with that key.
- A row with `added_by_repair` true stands for one the repair added and was not
  on file before it: it only takes the key's rows out. A re-dated split is its
  old row (restored) and its new one (`added_by_repair`); a split or dividend an
  operator added is one row with `added_by_repair`.
- A file of deleted rows as they stood (fafnir's `deleted_at_utc` export) works
  as it is: extra columns are ignored. A row that cannot be used is DQ109.

```bash
dorq check bars.csv --actions actions.csv --restore-actions before-actions.csv \
     --format jsonl
```
