# dorq-eval

Scores dorq's output against labels (plan item DR-0603). It reports strict and
fault precision with their coverage, at warn and above and at error, on every
label and on the verified ones; precision, recall and F1 per check; hard
negatives; a reliability diagram of `p_error` with its expected calibration error;
and precision@k. Every estimate is weighted by the labels' `weight` and comes with
a 95% interval ([Intervals](#intervals); DE-01). With `--compare`, it judges a
second engine's flags on the same labels and compares the two engines' recall
with a paired test ([Two engines](#two-engines)). The report is written as
markdown and as a self-contained HTML page.
The tool is built with the tests, and `ctest -R synth` runs it on two dorq-synth
seeds against [tools/synth/gates.txt](../synth/gates.txt).

```bash
dorq prices.csv --config dorq.toml --exit-zero --format csv --show-info > results.csv
dorq-eval --labels labels.jsonl --results results.csv \
          --markdown report.md --html report.html [--complete] [--gates gates.txt]
# fafnir's shadow nights, as their distinct conditions
dorq-eval --labels labels.jsonl --results 2026-10-14.jsonl --results 2026-10-13.jsonl \
          --results 2026-10-12.jsonl --input-time 2026-10-15
# an audit sample's labels only, and another engine's flags on the same labels
dorq-eval --labels labels.jsonl --sample audit-2026-11 --results 2026-11-20.jsonl \
          --compare sql-flags.jsonl --compare-name sql
```

| Option | Meaning |
|---|---|
| `--labels F` | Labels in the JSONL schema of [doc/labels.md](../../doc/labels.md) |
| `--results F` | `dorq check` output as `csv`, `jsonl` or `fafnir` (told apart by the first character). Use `--show-info` to get the calibration points below warn. Give it more than once to judge several runs |
| `--input-time T` | When the runs' input was taken (an ISO 8601 date or timestamp). A label whose `applies_until` is not after it is dropped: the data it describes had been repaired |
| `--sample S` | Keep only the labels whose `sample` is `S` ([labels.md](../../doc/labels.md)): an audit sample judged on its own |
| `--complete` | Every fault is labelled, as in dorq-synth. A report that matches no label counts as a false positive. Without this flag it is "unlabelled" and left out of precision |
| `--gates F` | Bounds to enforce, in the format of `tools/synth/gates.txt`. `max` bounds a code's reports at warn and above, or a hard-negative kind's hits. The exit status is 1 when any bound fails |
| `--markdown F`, `--html F` | Where to write the report. Without `--markdown`, the markdown goes to stdout |
| `--title T` | The report's heading |
| `--name A` | The `--results` engine's name in the two-engine section. The default is `dorq` |
| `--compare F` | A second engine's output on the same inputs: `dorq check` output in any format above, or the [generic flag format](#the-generic-flag-format). Give it more than once for several runs, which are judged as their distinct conditions, as `--results` are |
| `--compare-name B` | The second engine's name. The default is the first `--compare` file's stem |

## Summary

The report opens with a summary table: for each set of reports, at warn and
above and at error, how many were reported and judged, the **coverage** (judged /
reported), the right, other-fault, false and unlabelled counts, **strict
precision** (right / judged) and **fault precision** ((right + other fault) /
judged). The sets are:

- each results file, when there are several;
- every report, or with several files their **distinct conditions**: one per
  code, series and `record_key` (a DQ601 row's key includes its ratio, and a
  statement finding's its period, version and item, from the CSV's statement
  columns as well), the last file's kept, as fafnir's one-flag-per-condition
  dedupe keeps one flag;
- the same judged against the **verified labels** only (`verified: true`).

On fafnir's three shadow nights of 2026-10-12..14 and its hand labels, dorq-eval
prints what fafnir's own `dq compare` replica (`judge.py`) prints: 0.378 strict
and 0.667 fault precision over 45 judged of 238 distinct conditions.

## Matching

The matching follows `dorq-synth score`, so the two tools agree on synthetic data.

- **Fault labels** (`data_error`, `context_gap`) match a report on the same series
  whose dates overlap the label within 3 calendar days. The report is *right* when
  its code is one the label's `expect` names (`DQ201`, or `DQ201|DQ202`), and
  *other fault* when it names a different code. A `*` fault label matches only the
  cross-sectional row: a report with no series.
- **Market facts** match a report on an overlapping date, with no slack, whose code
  starts with the label's `codes` (`DQ2` by default). Such a report is *false*.
- **Cohort labels** (`*` with `members`) match only a cross-sectional row whose
  `detail.members` overlap the label's. CSV results carry no members, so there a
  cohort label matches the cross-sectional row on its date alone.
- **Statement labels** also match by statement, period type, version and item
  ([doc/labels.md](../../doc/labels.md)). A field the label sets must equal the
  finding's, where the finding has one. So a finding on a whole period (no version)
  or on an identity (no item) matches each version's or each item's label. CSV
  results carry these fields in their statement columns. JSON results carry them
  as fields, with the item in `record_key`.
- **Recall** counts a fault label as found when a report at warn or above names
  the label's expected code within the slack. The label is credited to the first
  code in `expect`. The intervals section's recall is over every fault label, a
  label with no `expect` included: any code finds it.

## Intervals

A label drawn in a sample carries a `weight`, 1 / the probability that the sample
included it ([labels.md](../../doc/labels.md)). A label without one weighs 1.

- **A judged report carries a weight:** the largest among the labels that judge
  it (the fault labels that credit it, else those that match it, else the market
  facts that cover it). A report `--complete` makes false without a label weighs
  1.
- **Every share is a Hájek ratio** of weights. Strict precision is Σw(right) /
  Σw(judged), fault precision Σw(right + other fault) / Σw(judged), recall
  Σw(found) / Σw(fault labels). The reliability diagram's bins sum weights, and
  the expected calibration error is Σ over bins |Σw·p_error − Σw·outcome| /
  Σw. Precision@k is the weighted share of hits among the top k reports. With
  every weight 1 these are the unweighted figures, to the last digit.
- **Each interval is the two-sided 95% Wilson score interval** (z = 1.959964) on
  Kish's effective size, n = (Σw)² / Σw² over the judged reports (for precision)
  or the fault labels (for recall). With every weight 1, n is the count and the
  interval is the textbook Wilson interval.

The summary table keeps the counts. The section "Estimates with 95% intervals"
gives, for each of its sets, the judged count, their weight, the effective size
and the two precisions with their intervals, then recall over every fault label.

## Two engines

`--compare F` judges a second engine's flags on the same labels as the
`--results` (the first engine). For each engine the section "Two engines" gives
strict and fault precision at warn and above and at error, and recall over every
fault label, each with its interval. Then the two are paired on the fault labels
(the confirmed errors). For each fault label, each engine found it or not:

| | B found | B missed |
|---|---|---|
| **A found** | a | b |
| **A missed** | c | d |

The cells are sums of the labels' weights (with counts beside them), and W = a +
b + c + d.

- **The recall difference** is Δ = (b − c) / W, A's recall minus B's.
- **Its one-sided 95% lower bound** is Newcombe's hybrid score method 10 for
  paired proportions (Newcombe 1998, *Statistics in Medicine* 17:2635),
  uncorrected. Take Wilson bounds at z = 1.644854 on p1 = (a + b) / W and p2 =
  (a + c) / W, each on the effective size of the fault labels' weights, (l1, u1)
  and (l2, u2). Take φ = (ad − bc) / √((a + b)(c + d)(a + c)(b + d)), or 0 when
  that product is 0. Then L = Δ − √((p1 − l1)² − 2φ(p1 − l1)(u2 − p2) +
  (u2 − p2)²).
- **The exact McNemar test** is one-sided, on the unweighted counts of discordant
  labels nb (A only) and nc (B only): p = P(X ≥ nb) for X ~ Binomial(nb + nc,
  ½), the evidence that A finds more than B. It is 1 when no label is discordant.
  It conditions on the sample and ignores the weights; with unequal weights, read
  L.
- **"A's recall is higher at one-sided 95%"** means L > 0. When both engines find
  every fault, L is below 0: equal recall is not shown higher.

### The generic flag format

Any engine's flags can be compared, in JSON Lines with one flag a line. A line
with a `check` and no `code` is a flag; any other line is read as dorq's output.

```json
{"series":"752016","date":"2026-11-18","check":"gap","severity":"warn","codes":"DQ301|DQ303"}
{"series":"752016","first":"2026-10-01","last":"2026-11-20","check":"stale","codes":"DQ304"}
{"series":"8812","date":"2024-06-30","check":"negative_revenue","statement":"income","period_type":"quarter","item":"revenue","codes":"DQ902"}
```

| Field | Meaning |
|---|---|
| `series` | The series id, as the labels name it. Empty or absent: a cross-sectional flag, matched as a dorq row with no series |
| `date`, or `first` and `last` | The dates the flag is about. `last` (or `end_date`) defaults to the first |
| `check` | The flag's check. It stands for a code in the per-check table |
| `severity` | `info`, `warn` or `error`; `warn` when absent. Only warn and above find a fault |
| `codes` | The dorq codes or code prefixes the flag is credited with, separated by `\|` (`DQ2\|DQ601\|DQ7`). A fault label credits the flag when one of them and a code it expects overlap, one starting with the other, and a market fact covers it when one of them and the fact's `codes` overlap. Absent: any code, so any fault label it matches finds it and any market fact on it makes it false |
| `statement`, `period_type`, `version_no`, `item` | For a flag on statements: its scope, matched as a dorq finding's. They also make its condition key |
| `p_error`, `message` | Optional. A flag is never a calibration point |

## Calibration

The reliability diagram uses the reports of the checks whose `p_error` is a
posterior: DQ2xx, DQ301–DQ304, DQ401–DQ403, DQ501 and DQ701–DQ704 on bars, and
DQ903, DQ911–DQ915, DQ919, DQ921, DQ922, DQ931, DQ932 and DQ942 on statements
(since 0.14.0). It draws on
reports at every severity, info included. A point is 1 when the report is right
or is a series' own row on a session that a `*` fault covers (DQ301 rows folded
into a DQ303 cohort). A point is 0 when the report is false. A report next to a
fault that names a different code is left out, because the labels cannot say
whether it is the error it reports.

The expected calibration error is the mean |mean p_error − observed rate| over ten
equal-width bins, with each bin weighted by its share of the points. It is the same
measure `dorq calibrate` prints. The calibrate figure covers every candidate of
the models it fits (price moves, or the statement checks' sub-families), while
this one covers the reports of every check listed above.

## Precision@k

Reports at warn and above that the labels judge are ranked by `p_error`, highest
first. The ranking is stable, so ties keep file order. Precision@k is the share
of the top k that were right or another fault. It is listed for k = 10, 25, 50,
100, 200 and 500, as far as the reports go. Deterministic checks report
`p_error` 1, so they sit at the top.
