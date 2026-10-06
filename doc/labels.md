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

Other keys are ignored, so an exporter can keep extra fields for people. A line
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
| `data_error` with no `expect` | any error |
| `market_fact` with `codes` DQ, DQ2 or DQ2x | market move, tick move, explained split |

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
