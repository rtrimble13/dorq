# Output formats

`--format` picks one of five formats. All five report the same violations, in
the same order:

1. Series in the order they first appear in the input.
2. Within a series, by date. A row with no usable date has no date to sort by, so
   it comes first, in line order.
3. Then by check code.

That order doesn't depend on `--threads` ([ADR 0001](adr/0001-stateless-deterministic-linter.md)).

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

- **name**: the series label (e.g. a ticker) if there is one, otherwise the series id.
- **date**: the date, or `line N` when the row had no usable date.
- **code and severity**.
- **check name**.
- **message**.
- **`(line N)`**: the source line, when one row is at fault.

`--color auto|always|never` colours the severity. `auto` colours only when
stdout is a terminal and `NO_COLOR` is unset.

## jsonl

One JSON object per line. This is the record every machine format is built from:

```json
{"series":"AAPL","label":null,"source":"prices.csv","date":"2024-01-03","line":3,
 "code":"DQ101","check":"ohlc-bounds","severity":"error","p_error":1,
 "classification":"data_error","message":"high 9.5 < open 10; high 9.5 < close 10",
 "detail":{"open":10,"high":9.5,"low":9,"close":10},
 "record_key":{"trade_date":"2024-01-03"},
 "dorq":{"version":"0.1.0","config_hash":"1610b258f86d7891"}}
```

(This example is wrapped here; each record is one line in the output.)

| Field | Meaning |
|---|---|
| `series` | The series id: the series column, or the file's stem when there is none |
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
 "dorq":{"version":"0.1.0","config_hash":"1610b258f86d7891"}}}
```

`rows` counts every data row read, including rows skipped for an unusable date.

## csv

A header row, then one row per violation:

```
series,label,source,date,end_date,line,code,check,severity,p_error,classification,message
```

`detail` is not included; use `jsonl` when you need it.

## fafnir

One JSON object per line, shaped as a row of fafnir's `ops.data_quality_flag`:

```json
{"security_id":12345,"table_name":"core.daily_price","record_key":{"trade_date":"2024-01-03"},
 "check_name":"dorq_ohlc_bounds","severity":"error","detail":{ ...the jsonl record... }}
```

- `security_id` is the series id. It is written as a JSON number when it is an
  integer, and as a string otherwise.
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
