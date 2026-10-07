# dorq-eval

Scores dorq's output against labels (plan item DR-0603). It reports precision,
recall and F1 per check, precision at warn and at error, hard negatives, a
reliability diagram of `p_error` with its expected calibration error, and
precision@k. The report is written as markdown and as a self-contained HTML page.
The tool is built with the tests, and `ctest -R synth` runs it on two dorq-synth
seeds against [tools/synth/gates.txt](../synth/gates.txt).

```bash
dorq prices.csv --config dorq.toml --exit-zero --format csv --show-info > results.csv
dorq-eval --labels labels.jsonl --results results.csv \
          --markdown report.md --html report.html [--complete] [--gates gates.txt]
```

| Option | Meaning |
|---|---|
| `--labels F` | Labels in the JSONL schema of [doc/labels.md](../../doc/labels.md) |
| `--results F` | `dorq check --format csv` output. Use `--show-info` to get the calibration points below warn |
| `--complete` | Every fault is labelled, as in dorq-synth. A report that matches no label counts as a false positive. Without this flag it is "unlabelled" and left out of precision |
| `--gates F` | Bounds to enforce, in the format of `tools/synth/gates.txt`. The exit status is 1 when any bound fails |
| `--markdown F`, `--html F` | Where to write the report. Without `--markdown`, the markdown goes to stdout |
| `--title T` | The report's heading |

## Matching

The matching follows `dorq-synth score`, so the two tools agree on synthetic data.

- **Fault labels** (`data_error`, `context_gap`) match a report on the same series
  whose dates overlap the label within 3 calendar days. The report is *right* when
  its code is one the label's `expect` names (`DQ201`, or `DQ201|DQ202`), and
  *other fault* when it names a different code. A `*` fault label matches only the
  cross-sectional row: a report with no series.
- **Market facts** match a report on an overlapping date, with no slack, whose code
  starts with the label's `codes` (`DQ2` by default). Such a report is *false*.
- **Recall** counts a fault label as found when a report at warn or above names
  the label's expected code within the slack. The label is credited to the first
  code in `expect`.

## Calibration

The reliability diagram uses the reports of the checks whose `p_error` is a
posterior: DQ2xx, DQ301–DQ304, DQ401–DQ403, DQ501 and DQ701–DQ704. It draws on
reports at every severity, info included. A point is 1 when the report is right
or is a series' own row on a session that a `*` fault covers (DQ301 rows folded
into a DQ303 cohort). A point is 0 when the report is false. A report next to a
fault that names a different code is left out, because the labels cannot say
whether it is the error it reports.

The expected calibration error is the mean |mean p_error − observed rate| over ten
equal-width bins, with each bin weighted by its share of the points. It is the same
measure `dorq calibrate` prints. The calibrate figure covers only price moves,
though, while this one covers every check listed above.

## Precision@k

Reports at warn and above that the labels judge are ranked by `p_error`, highest
first. The ranking is stable, so ties keep file order. Precision@k is the share
of the top k that were right or another fault. It is listed for k = 10, 25, 50,
100, 200 and 500, as far as the reports go. Deterministic checks report
`p_error` 1, so they sit at the top.
