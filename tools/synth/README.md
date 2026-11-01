# dorq-synth

Synthetic market data with labelled faults, and a scorer that measures dorq's
precision and recall against the labels (plan item DR-0308). Synthetic financial
statements (DR-0903) [follow](#statements). The tool is built with the tests, and
`ctest -R synth` runs the gate in [gates.txt](gates.txt) on two seeds.

```bash
dorq-synth generate --seed 1 --out /tmp/synth   # bars, points, labels, actions, meta, market
dorq /tmp/synth/bars.csv /tmp/synth/points.csv --config tools/synth/dorq.toml \
     --actions /tmp/synth/actions.csv --meta /tmp/synth/meta.csv \
     --market /tmp/synth/market.csv --exit-zero --format csv --show-info > /tmp/synth/results.csv
dorq-synth score --labels /tmp/synth/labels.csv --results /tmp/synth/results.csv \
                 --gates tools/synth/gates.txt --verbose
dorq-eval --labels /tmp/synth/labels.jsonl --results /tmp/synth/results.csv --complete \
          --html /tmp/synth/report.html     # the same, plus calibration (tools/eval)
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

The context files (doc/context.md):

- `actions.csv` holds splits and dividends, under fafnir's column names
  (`core.corporate_action`);
- `meta.csv` holds every series' `asset_type`, `peer_group` and `exchange`;
- `market.csv` holds the market factor as an index level (series `market`).

## Labels

`labels.csv` has a row per injected fault and a row per hard negative. For a
fault, `expect` is the code dorq should report. For a hard negative, `expect` is
empty and `codes` is the code prefix a report would be wrong under.
`labels.jsonl` holds the same labels in the schema of
[doc/labels.md](../../doc/labels.md), for `dorq calibrate` and dorq-eval. A fault
becomes a `data_error`, except a split missing from the data (DQ203, DQ601),
which becomes a `context_gap`. A hard negative becomes a `market_fact`.

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
| `family_split` | DQ203, and DQ601 (`*`) | four liquid names in peer group `FAM1` split 2:1 on the same day, none of it on file |
| `split_misdated` | DQ701 | 4 splits in the bars, on file 1-3 sessions away |
| `split_without_jump` | DQ702 | 4 splits on file that the bars never show |
| `split_ratio_mismatch` | DQ703 | 4 splits on file at the wrong ratio: inverted, or another split's |
| `split_double_applied` | DQ704 | 3 splits the bars apply at the ex-date and again 2-12 sessions later |
| `dividend_slip`, `dividend_above_price` | DQ705 | 3 dividends ×100, and 2 at four times the price (quarterly dividends on every liquid name) |
| `hn_split_on_file` | - | 8 splits on file, in the bars at their ex-date (DQ2 and DQ70) |
| `hn_split_between_bars` | - | 3 reverse splits on thin names whose ex-date is a session with no bar (DQ2 and DQ70) |
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

At M5 the gate runs dorq with all three context files, and adds DQ601 and
DQ701–DQ705. No split on file is reported, at its ex-date or between two bars.
DQ703 and DQ704 miss one fault now and then: a wrong ratio a few percent from
the one on file, or a doubled split in a volatile spell. DQ203's precision gate
is 0.85: when DQ704 misses a doubled split, the second move is reported as DQ203,
at the right bar. With the market taken out, a thin name's ambiguous move
sometimes crosses the warn line, and the gate allows three such moves.

At M4 the gate adds DQ108 and DQ403 at 1.0 and 0.9, DQ401 at precision 0.9 and
recall 0.8, and DQ501 at 0.8 and 0.6. On seeds 1-6, DQ501 misses one to three
runs a universe, all of them two to four bars long. A quiet stock repeats its
close twice now and then, so such runs are reported at info if at all. A
rate's quarter-point move that half-reverts the next day is ambiguous; one
such report is allowed.

DQ107 is held to fault precision 0.5 and recall 0.9. It reports 10-20 bars a
universe: every injected flat bar, other faults that leave a bar flat, and a few
rare flat days on low-priced names. On the penny and 1/16 series it reports 0-4
bars, where it reported over a thousand before it learned the price grid.

## Statements

`dorq-synth statements` writes a universe of financial statements, for the DQ9xx
checks (plan item DR-0903). It draws from its own random stream, so it changes
nothing in the bars universe of the same seed.

```bash
dorq-synth statements --seed 1 --out /tmp/st   # statements, items, actions, meta, labels
dorq-synth verify-statements --dir /tmp/st --config tools/synth/statements.toml
dorq /tmp/st/statements.csv --items /tmp/st/items.csv --actions /tmp/st/actions.csv \
     --meta /tmp/st/meta.csv --config tools/synth/statements.toml --exit-zero \
     --format csv --show-info > /tmp/st/results.csv
dorq-synth score --labels /tmp/st/labels.csv --results /tmp/st/results.csv \
                 --gates tools/synth/gates.txt --verbose
```

The files are in fafnir's shapes:

- `statements.csv`: every version current on fafnir's load timeline, as its export
  sends them ([doc/statements.md](../../doc/statements.md)). It has the three
  statements' quarterly and annual rows, and fafnir's 58 line items as columns.
  Values are whole multiples of $1,000, or of $1M for a company with $1B or more of
  revenue a quarter. An item off a row's statement is NULL.
- `items.csv`: fafnir's `ref.statement_line_item`, as exported.
- `actions.csv`: the splits on file.
- `meta.csv`: the fields DR-0910 reads: `currency`, `first_trade_date`,
  `last_trade_date`, `active`, `delisted_date`, `sector`, `industry` and
  `statements_expected`. It adds two series with no rows: `ETF01` expects no
  statements, and `MIS01` expects them.
- `labels.csv` and `labels.jsonl`.

[statements.toml](statements.toml) is the config the gate runs with: SF1's
candidate identities (plan §4.2), with the bounds and lists that hold on the
generated data.

### The companies

| Class | Companies | Quarterly revenue | What sets them apart |
|---|---|---|---|
| Industrials (`IND`) | 40 | $20M–$5B | fiscal years ending in December (65%), September, June or March. 12% have 52/53-week years, whose periods end on the month's last Saturday. 45% carry R&D, and the rest report 0. Two report in euros. 15% have noncontrolling interests, 8% temporary equity and 15% restricted cash |
| Banks (`BNK`) | 8 | $50M–$2B | deposits, and no current and noncurrent split and no inventory: NULL, not 0 |
| Biotechs (`PRE`) | 6 | none | 0 revenue, cost of revenue and inventory. They fund their losses by selling shares, and 40% start selling product some years in |

- **History.** Industrials start in 2001–2004 (60%) or 2005–2014. Banks start in
  2001–2006 and biotechs in 2008–2016. Every company files to 2026-06-30, except
  four industrials and a biotech, which delist in their seventh to twelfth year.
  Three industrials change their fiscal year from December to June or September,
  in 2009–2018: a short transition year, then the labels start again on the new year.
- **Flows.** Revenue grows (−3% to 15% a year) and is seasonal. Costs are shares
  of revenue.
- **Stocks.** These roll forward. Working capital follows revenue and costs, and
  property and equipment move by capex and depreciation. The cash flow moves cash,
  and equity is what is left after liabilities and the noncontrolling and temporary
  equity.
- **The identities hold by construction.** Each subtotal is computed from its
  rounded components. A year's flows are its quarters' sum, and the change in cash
  is the cash flow's net change. The structural gaps are the exceptions, on
  purpose:
  - noncontrolling interests and temporary equity sit outside total liabilities
    and parent equity;
  - from 2018 the cash flow's cash includes restricted cash, and the balance
    sheet's does not.
- **Shares.** Counts grow by stock compensation and by shares sold, and fall with
  buybacks.
  - An industrial returns cash beyond three quarters of revenue as buybacks, a tenth
    of its market cap a quarter at most.
  - The market cap is the company's scale at a multiple that wanders and reverts.
    The `close` column is the market cap over the shares, in that day's terms.
  - A company splits when its raw price passes $200, half the time: 2:1, 3:1, 3:2
    or 4:1, at most three times. A biotech below $1 may reverse split.
  - Each version stores its counts in the terms of its own `known_from`, as fafnir
    does (its ADR 0018).
- **Filing.**
  - Deadlines go by size: 40 or 45 days for a 10-Q, and 60, 75 or 90 for a 10-K.
    Each issuer files a few days to three weeks inside them.
  - A year's last quarter and the year itself are known from the 10-K.
  - Before 2004 there is no filing date: the knowledge date is estimated, 45 or 90
    days after the period end.
- **Restatements.**
  - An industrial has up to three, and the others up to one. Each restates one
    statement of one fiscal year: its four quarters and the year, as version 2.
  - A reclassification moves 3–15% between two components and holds the totals:
    SG&A and R&D, cost of revenue and SG&A, payables and deferred revenue, current
    and noncurrent debt, a bank's receivables and investments, or operating and
    investing cash.
  - Half of an industrial's restatements are of its income statement, and half of
    those move its revenue, cost and SG&A by up to 4%, and its totals with them.
  - A restatement is known from the next 10-K (35%), from the backfill day
    2026-10-20 (50%, so that one load day holds many), or from a night after it.
    Its version is `observed`, as fafnir's constraints require of every version
    after the first.
  - `loaded_at` is the backfill day for every row known by then.

### Faults and hard negatives

Faults keep their fiscal year and a quarter either side clear of each other, and
of restatements, splits and the hard negatives that change the data. Each label
names the statement, period type, version and item it covers, wherever the fault
has one.

| Kind | Expect | What was done |
|---|---|---|
| `unit_value` | DQ912 | 8 values of one row at ×1,000, ×0.001 or ×10⁶ |
| `unit_row` | DQ912 | 5 quarterly rows with every money item at ×1,000 or ×0.001 |
| `unit_era` | DQ912 | 4 runs of 4–8 quarters of one statement at ×1,000 or ×0.001 |
| `sign_flip` | DQ913 | 6 values with a definite sign negated: revenue, a cost, capex, dividends paid, or depreciation |
| `bad_value` | DQ911 | 8 values multiplied or divided by 1.5–3.5 |
| `copied_period` | DQ914 | 5 quarters of an income or cash flow statement that repeat the quarter before |
| `absent_as_zero` | DQ915 | 6 items, reported in the quarters either side, set to 0 |
| `cumulative_quarters` | DQ916 | 3 fiscal years whose Q2 to Q4 hold the year to date (flows) |
| `annual_in_quarter` | DQ916 | 3 quarters holding their fiscal year's flows |
| `identity_dropped_field`, `identity_doubled_field` | DQ903 | 4 totals missing one of their terms, and 4 counting one twice |
| `split_missing` | DQ921 | 3 splits that are not on file (a context gap) |
| `counts_wrong_terms` | DQ921 | 3 quarters, the first known after a split on file, whose share counts are left in the old terms |
| `filing_at_period_end` | DQ931 | 3 periods whose filing date, and `known_from`, is the period end |
| `amendment_date` | DQ931 | 3 10-Qs dated 20–60 days after the next quarter's filing |
| `missing_quarter` | DQ941 | 4 quarters missing from all three statements |
| `restatement_x1000` | DQ932 | 3 restatements whose new version is the old one at ×1,000 |
| `statements_missing` | DQ943 | `MIS01`, which expects statements and has none |
| `duplicate_row` | DQ901 | 3 rows written twice |
| `chain_gap`, `chain_overlap`, `chain_two_open`, `chain_closed_last` | DQ901 | 7 restated periods whose versions do not chain: the first closed before the next is known (2) or after (2), the first left open (2), and the last closed (1) |
| `known_before_period` | DQ901 | 3 periods known, and filed, 1–20 days before they end |
| `filing_date_mismatch` | DQ901 | 3 periods known 3–20 days after their filing date |
| `negative_impossible` | DQ902 | 4 values that cannot be negative (revenue, a share count, total assets) at a small negative, which is not a clean sign flip |
| `component_above_total` | DQ902 | 4 components (cash, current assets, goodwill, current liabilities) above their total |
| `hn_acquisition` | - | about a fifth of the industrials buy a company with debt: goodwill and assets step, and revenue steps 15–50% for good (DQ9) |
| `hn_writedown` | - | goodwill written down by 50–90% in a quarter, through operating expenses (DQ9) |
| `hn_fiscal_year_change` | - | the three changes of fiscal year (DQ9) |
| `hn_late_filer` | - | 4 10-Qs filed 15–40 days after their deadline (DQ9) |
| `hn_real_zero` | - | every biotech, all its rows (DQ91) |
| `hn_bank_nulls` | - | every bank, all its rows (DQ9) |
| `hn_structural_gap` | - | the noncontrolling and temporary equity outside the balance sheet's identity (`identity` `balance_sheet`), and restricted cash from 2018 (`cash_tie`) (DQ90) |
| `hn_reclassification`, `hn_restatement` | - | each restatement's year and statement (DQ9) |
| `hn_split_on_file` | - | every other split, from the last period known before it to the first after (DQ92) |
| `hn_delisted` | - | each delisted company, from its last period (DQ94) |
| `hn_no_statements_expected` | - | `ETF01` (DQ94) |

A fault that leaves a row breaking one of [statements.toml](statements.toml)'s
non-negative items or bounds is DQ902's too:

- when DQ902 reports the fault's own item, or the fault has none, its label's
  `expect` adds DQ902 (`DQ913|DQ902` for a revenue flipped negative);
- when DQ902 reports another item, that item gets a label of its own, of the same
  kind. A total at ×0.001 puts each component above it.

DQ903 and DQ904 are marked the same way:

- **DQ903.** A fault that moves a row's identity gap beyond its tolerance (the
  larger of 0.5% of its largest term and 1M) adds DQ903 to `expect`. A primary
  identity fault's `item` is the term it drops or doubles, which DQ903 names: an
  identity's name is never an item.
- **DQ904.** A fault that breaks the cash tie or a year's quarter sum in force
  (with DQ904's tolerances, restricted cash's from 2018 included) adds DQ904 where
  its label covers the row DQ904 reports. Where DQ904 reports another row (the
  cash flow after a balance sheet's cash, the year of a quarter), that row gets a
  consequence label expecting DQ904.

Restricted cash is 1–4% of the cash balance and moves a little each quarter, so
that its change stays within the 10% that DQ904 allows from 2018.

A label's `first` and `last` are fiscal dates. `labels.csv` adds the columns
`statement`, `period_type`, `version_no`, `item` and `identity`; an empty one covers
every value. `labels.jsonl` has the same fields ([doc/labels.md](../../doc/labels.md)).
`identity` names the identity or tie a structural gap breaks, for people and for
`verify-statements`.

### `verify-statements`

`dorq-synth verify-statements --dir DIR --config F` reads `DIR/statements.csv`
through dorq's own reader, with `DIR/items.csv`. On every row that no fault label
covers, it holds:

- each identity, bound and non-negative item of `F`'s `[statements]`, exactly;
- a fiscal year's quarters against the year, for every flow;
- the change in cash against the cash flow's net change, quarter to quarter.

It checks the ties in the in-force view and in the as-first-reported view. A
structural gap excuses the identity or tie it names. It prints how often each rule
was judged, held, excused and broken. It exits 1 when a rule breaks, when a rule
holds on no row, or when the input has a row dorq cannot use (DQ104, DQ109).

### The statements gate

`ctest -R synth` runs `synth.statements-seed-1` and `-2`
([tests/synth_statements_gate.cmake](../../tests/synth_statements_gate.cmake)):

1. it generates the universe twice, and holds every file byte for byte;
2. it runs `verify-statements`;
3. it runs dorq on the universe, with all three context files;
4. it scores dorq's output with `dorq-synth score` and dorq-eval against
   [gates.txt](gates.txt).

The gate holds the following, and each DQ9xx check's plan item adds its own
bounds:

- no row dorq cannot use: `DQ104 max 0`;
- no context row it cannot use: `DQ109 max 0`;
- DQ901 and DQ902 (DR-0904), which are deterministic, at precision and recall 1.0;
- DQ903 (DR-0905) at precision and recall 0.9, and DQ904 at precision 0.9. DQ904's
  recall is not gated: its consequence labels cover rows other faults break
  without its reporting each one;
- no report at warn on any hard negative.

It also runs dorq with `priors/fafnir-statements.toml`, so that file names only
items fafnir's taxonomy lists.

`max` on a code bounds that code's reports at warn and above. On a hard-negative
kind, it bounds that kind's hits.

Scoring matches a statement label as it matches a bars label, and also by
statement, period type, version and item. A field left empty on either the label or
the finding covers every value. A whole-period finding, which has no version,
matches each version's label, and an identity's finding, which has no item, matches
each item's label.

## A large input: `bench`

`dorq-synth bench --copies K [--seed N] --out FILE` writes K universes' bars to
one file. Copy *i* uses seed N + i, and its series are named `<i>_<id>`. It
writes no labels or context files. At 650 copies the file holds 150M bars
(7.7 GB), the size of the runtime target in
[doc/performance.md](../../doc/performance.md).
