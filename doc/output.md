# Output formats

`--format` picks one of five formats. All five report the same violations, in
the same order:

1. Series in the order they first appear in the input.
2. Within a series, by date. A row with no usable date has no date to sort by, so
   it comes first, in line order.
3. Then by check code.

That order doesn't depend on `--threads` ([ADR 0001](adr/0001-stateless-deterministic-linter.md)).

The cross-sectional checks, DQ303 (cohort gaps) and DQ304 (stale feeds), can only
be judged once every series has been read. While either is enabled (the default),
dorq holds every series' violations until the input ends and then writes them all,
in the same order. The DQ303 violations belong to no single series, so they come
last. To get output as each series finishes, ignore both checks.

`--min-severity` (default `warn`) and `--since` decide which violations are
reported. `--fail-on` (default `warn`) decides the exit status: 1 if any
**reported** violation is at or above it, otherwise 0. `--exit-zero` always
exits 0.

## text (default)

One line per violation. This format is for people.

```
AAPL  2024-01-03  DQ101 error  ohlc-bounds  high 9.5 < open 10; high 9.5 < close 10  (line 3)
BBB  line 7  DQ104 error  missing-field  date "2024-13-01" is not a date; the row is skipped
```

The fields are:

- **name**: the series label (e.g. a ticker) if there is one, otherwise the series
  id; `(all series)` for a DQ303 cohort.
- **date**: the date, or `line N` when the row had no usable date.
- **code and severity**.
- **check name**.
- **message**.
- **`(line N)`**: the source line, when one row is at fault.

`--show-evidence` adds indented lines under each violation that compares
explanations (the DQ2xx price checks): the suggested action, every hypothesis
with its posterior probability, and the evidence, as each term's log Bayes factor
for the reported explanation against a real market move:

```
LIQ03  2022-01-07  DQ203 error  unreported-split  close 199.99→67 (×0.335 ≈ a 3:1 split), volume ×3.087 after; P(error) = >0.99  (line 5768)
      → add split 3:1 ex 2022-01-07
      hypotheses: unreported_split >0.999 market_move <0.001 scale_error <0.001 bad_print <0.001 bad_close <0.001
      evidence (log Bayes factor against market_move):
        prior 0.02  -3.8
        return 0.335 (nearest split 3:1)  +12.6
        next_bars 1.018 (the largest of the next 5 moves)  +0.7
        open_high_low 0.3337  +0.7
        volume_on_day 4.465 (against the median before)  +0.1
        volume_shift 3.087 (median after / before)  +1.1
```

A violation judged with too few bars after it ends its message with
`provisional` (see doc/checks/DQ201.md).

`--color auto|always|never` colours the severity. `auto` colours only when
stdout is a terminal and `NO_COLOR` is unset.

## jsonl

One JSON object per line. This is the record every machine format is built from:

```json
{"series":"AAPL","label":null,"source":"prices.csv","date":"2024-01-03","line":3,
 "code":"DQ101","check":"ohlc-bounds","severity":"error","p_error":1,
 "classification":"data_error","message":"high 9.5 < open 10; high 9.5 < close 10",
 "detail":{"open":10,"high":9.5,"low":9,"close":10},
 "hypotheses":{},"evidence":[],"suggested_action":null,"provisional":false,
 "record_key":{"trade_date":"2024-01-03"},
 "dorq":{"version":"0.6.0","config_hash":"2dbc9760e4b8882f"}}
```

A price check's record carries its model (wrapped the same way):

```json
{"series":"LIQ03","label":null,"source":"bars.csv","date":"2022-01-07","line":5768,
 "code":"DQ203","check":"unreported-split","severity":"error","p_error":0.99999999,
 "classification":"context_gap","message":"close 199.99→67 (×0.335 ≈ a 3:1 split), ...",
 "detail":{"close":67,"previous_close":199.99,"factor":0.335,"tail_probability":1.2e-09,
           "volume_ratio":3.087,"split_ratio":"3:1"},
 "hypotheses":{"market_move":1.1e-06,"tick_move":0,"bad_print":2.3e-09,...},
 "evidence":[{"feature":"prior","value":0.02,"log_bf":-3.807},
             {"feature":"return","value":0.335,"log_bf":12.6,"note":"nearest split 3:1"},
             {"feature":"volume_shift","value":3.087,"log_bf":1.1,"note":"median after / before"}],
 "suggested_action":{"kind":"add_split","ratio":"3:1","ex_date":"2022-01-07"},
 "provisional":false,"record_key":{"trade_date":"2022-01-07"},"dorq":{...}}
```

(This example is wrapped here; each record is one line in the output.)

| Field | Meaning |
|---|---|
| `series` | The series id: the series column, or the file's stem when there is none. `null` for a cross-sectional violation (DQ303, DQ601, DQ602) |
| `label` | The label column (e.g. a ticker), or `null` |
| `source` | The input file, or `<stdin>` |
| `date` | The date the violation is about; `null` for a row with no usable date |
| `end_date` | Present only on a violation that spans dates |
| `line` | The source line (CSV) or record (JSON) when one row is at fault, else `null` |
| `code`, `check` | The check's code and name ([ADR 0003](adr/0003-check-codes.md)) |
| `severity` | `info`, `warn` or `error` |
| `p_error` | The probability that this is a data error. Deterministic checks report 1 |
| `classification` | `data_error`, `market_fact` or `context_gap` |
| `message` | What the text format prints |
| `detail` | The check's own fields; see `doc/checks/` |
| `hypotheses` | For a check that compares explanations: each one considered, with its posterior probability. `{}` otherwise |
| `evidence` | The evidence terms: `feature`, `value`, `log_bf` (the log Bayes factor for the reported explanation against a real market move) and an optional `note`. `[]` otherwise |
| `suggested_action` | What would fix it: `kind` (`delete_bars`, `refetch_bar`, `add_split`, `rescale`, `rescale_volume`, `split_history`, `redate_split`, `check_split`, `fix_split_ratio`, `fix_dividend`) and its fields, or `null`. dorq never changes data |
| `provisional` | `true` when the violation was judged with fewer bars after it than the model needs; a later run may change it. Its severity is at most `warn` |
| `record_key` | `{"trade_date": "..."}`, or `{"line": N}` when there is no date |
| `dorq` | The version, and the hash of the settings that can change what is reported (`dorq config show` prints it) |

Numbers are written in their shortest exact form. A value that is not a finite
number is written as `null`.

## json

One object: the same records in a `violations` array, followed by a `summary`.

```json
{"violations":[
{...},
{...}
],"summary":{"inputs":1,"series":2,"rows":6,"violations":6,
 "by_severity":{"error":5,"warn":1,"info":0},"by_code":{"DQ101":2,"DQ102":1,"DQ104":3},
 "dorq":{"version":"0.6.0","config_hash":"2dbc9760e4b8882f"}}}
```

`rows` counts every data row read, including rows skipped for an unusable date.

## csv

A header row, then one row per violation:

```
series,label,source,date,end_date,line,code,check,severity,p_error,classification,message
```

`detail` and the model's fields are not included; use `jsonl` when you need them.

## fafnir

One JSON object per line, shaped as a row of fafnir's `ops.data_quality_flag`:

```json
{"security_id":12345,"table_name":"core.daily_price","record_key":{"trade_date":"2024-01-03"},
 "check_name":"dorq_ohlc_bounds","severity":"error","detail":{ ...the jsonl record... }}
```

- `security_id` is the series id. It is written as a JSON number when it is an
  integer, as a string otherwise, and as `null` for a DQ303 cohort, which belongs
  to no one security (fafnir's column is nullable).
- `table_name` comes from `fafnir.table_name` in the config (default `core.daily_price`).
- `check_name` is `dorq_` followed by the check name, with `-` turned into `_`.
- `record_key` keeps fafnir's `{"trade_date": ...}` shape, so fafnir's
  once-per-condition deduplication works unchanged.
- `detail` is the whole `jsonl` record.

## --statistics

`--statistics` prints one line per code after the violations, as `flake8
--statistics` does:

```
2     DQ101 ohlc-bounds
1     DQ102 non-positive
3     DQ104 missing-field
```

The lines go to stdout with `--format text`, and to stderr with the machine
formats, so they never corrupt the machine-readable output.
