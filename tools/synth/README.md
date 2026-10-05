# dorq-synth

Synthetic market data with labelled faults, and a scorer that measures dorq's
precision and recall against the labels (plan item DR-0308). It is built with the
tests, and `ctest -R synth` runs the gate in [gates.txt](gates.txt) on two seeds.

```bash
dorq-synth generate --seed 1 --out /tmp/synth   # bars.csv, points.csv and labels.csv
dorq /tmp/synth/bars.csv /tmp/synth/points.csv --config tools/synth/dorq.toml \
     --exit-zero --format csv > /tmp/synth/results.csv
dorq-synth score --labels /tmp/synth/labels.csv --results /tmp/synth/results.csv \
                 --gates tools/synth/gates.txt --verbose
```

The generator draws from its own PRNG (xoshiro256**) and its own normal and t
samplers, so a seed gives the same data with any C++ library.

## The market

2,000 XNYS sessions from 2015, and 600-750 from 1998 for the 1/16-tick names.

| Class | Series | Price | Daily volatility | Volume a day |
|---|---|---|---|---|
| Liquid (`LIQ`) | 60 | $20-200 | 1.2-2.5% | 0.5-5M |
| Mid (`MID`) | 30 | $5-40 | 2.5-4% | 30-300k |
| Thin (`THN`) | 20 | $1-10 | 4-6% | 300-3,000, no bar on 30% of sessions, no trade on 15% |
| Penny (`PNY`) | 10 | $0.03-0.09 on a cent grid | 3-6% | 0.1-1M |
| Sixteenths (`SXT`) | 5 | $0.15-0.50 on a 1/16 grid, 1998-2000 | 3-5% | 10-100k |
| Rates (`RAT`, in `points.csv`) | 8 | a level of 0.2 (three of them) or 1-6, quoted to 0.01 | 0.03 a day, Student t | - |

Returns are GARCH(1,1) with Student t (4 degrees of freedom) innovations, plus a
market factor with one crash day (a fall of 10-15%, then a volatile aftermath).
Volume follows an AR(1) level, rises with the size of each day's move, and surges
3-10 times on earnings days. Liquid and mid names have two to four earnings
reactions of 8-35%. Bars open between the previous close and the close, with
highs and lows around them, rounded to the price grid. A traded day's prints land
on both sides of the spread, so the range spans at least a tick on most days; on
a coarse grid that is most of the range.

The rates are point series that revert slowly to their level. They take
quarter-point policy moves, at most one every 30 sessions, and the three near
0.2 cross zero. [dorq.toml](dorq.toml) is the config the gate runs with: the
defaults, plus a `rates` profile that puts point series on the difference scale
(`transform = "diff"`) with bounds of [−5, 25].

## Labels

`labels.csv` has a row per injected fault (`expect` is the code dorq should
report) and per hard negative (`expect` empty; `codes` is the code prefix a report
would be wrong under).

| Kind | Expect | What was done |
|---|---|---|
| `bad_print` | DQ201 | 30 blocks of 1-3 bars multiplied by a decimal slip (×10, ×0.1, ×100, ×0.01) or a plausible wrong factor (×1.35-2.5 either way), on ordinary or low volume |
| `bad_print_newest` | DQ201 or DQ202 | 4 last bars ×100 or ×0.01 (with nothing after them, the two look alike) |
| `bad_close` | DQ204 | 12 closes multiplied by such a factor, the rest of the bar left alone |
| `unreported_split` | DQ203 | 16 splits: forward (2:1, 3:1, 3:2, 4:1) above $25, reverse (1:5, 1:10, 1:20) below $8; prices divided and volume multiplied from the ex-date |
| `scale_era`, `scale_to_end` | DQ202 | 12 eras of 20-250 bars, or to the end, at ×100, ×0.01, ×1000 or ×0.001 |
| `history_segment` | DQ205 | 6 histories that stop for 100-300 sessions and resume as another security at $10, $20 or $25 |
| `history_gap` | DQ301 | the gap before each of those |
| `outage` | DQ301 | 8 runs of 1-8 sessions missing from liquid names |
| `failed_load` | DQ303 | one session missing from 70% of the liquid names |
| `flat_bar` | DQ107 | 8 liquid bars with the close copied into open, high and low, on 1.2-3 times the median volume |
| `series_ended` | DQ304 | the 1/16 names, which stop in 2000 |
| `volume_units` | DQ401 | 6 eras of 60-200 bars on liquid names, every third to the end, with volume ×100, ×0.01 or ×1000 |
| `zero_volume_move` | DQ403 | 6 liquid bars whose close moved, with their volume set to zero |
| `stale_feed` | DQ501 | 8 runs of 2-5 bars repeating the close before them (half of them the whole bar), volume still arriving |
| `rate_bad_print` | DQ201 | a rate value ×100 (two series), and a value of 999 (one) |
| `rate_out_of_bounds` | DQ108 | those values, where they fall outside [−5, 25] |
| `rate_decimal_era` | DQ202 | 30-150 values stored ×0.01 (two series) |
| `hn_earnings` | - | each earnings reaction |
| `hn_crash` | - | the crash day, every series |
| `hn_tick` | - | every penny and 1/16 series, all its bars |
| `hn_thin` | - | every thin series, all its bars (DQ20x only) |
| `hn_rate` | - | every rate series, all its values (DQ2xx only) |

Faults keep 60 sessions clear of each other and of earnings days, so each label
is unambiguous.

## Scoring

Only violations at warn and above count. A violation matches a label when its
date range overlaps the label's, give or take 3 calendar days (a cross-sectional
label matches only a cross-sectional row). Per code:

- **precision**: the share of its violations that match a fault expecting that
  code;
- **fault precision**: the share that match any fault;
- **recall**: the share of faults expecting that code that it reports.

A violation that matches no fault but overlaps a hard negative counts as a hit
for that kind. `--verbose` lists the false positives, hard-negative hits and
misses for the codes the gates judge.

## The gate

[gates.txt](gates.txt) holds the minimum precision and recall per check and the
most hard-negative hits per kind. At M3 the bar is the plan's: DQ201 and DQ203 at
precision 0.9 or better, and no earnings gap, crash day or tick move reported at
warn. Across seeds 1-14 the gates hold on 12. Seeds 8 and 9 report three thin
names' spike-and-revert trades (two are allowed), and seed 9 brings DQ201's
precision to 0.88. Thin names' spike-and-revert trades are genuinely ambiguous.

At M4 the gate adds DQ108 and DQ403 at 1.0 and 0.9, DQ401 at precision 0.9 and
recall 0.8, and DQ501 at 0.8 and 0.6. On seeds 1-6, DQ501 misses one to three
runs a universe, all of them two or three bars long. A quiet stock repeats its
close twice now and then, so such runs are reported at info if at all. A
rate's quarter-point move that half-reverts the next day is ambiguous; one
such report is allowed.

DQ107 is held to fault precision 0.5 and recall 0.9. It reports 10-20 bars a
universe: every injected flat bar, other faults that leave a bar flat, and a few
rare flat days on low-priced names. On the penny and 1/16 series it reports 0-4
bars, where it reported over a thousand before it learned the price grid.
