# Plan: fundamentals in dorq (milestone M9)

- Status: **proposed** (2026-10-27). The questions in §11 need answers before work starts; each
  has a recommended default. The work items in §9 are drafted as issues in
  [dorq-fundamentals-issues.md](dorq-fundamentals-issues.md).
- Scope: this repository, plus three items of integration work in `rtrimble13/fafnir`
  (DR-0912 to DR-0914), as M7 had.
- Builds on: the [development plan](dorq-development-plan.md), whose principles (§1.3), engine
  (§5) and calibration (§6) hold unchanged. On fafnir's side: ADR 0018 (bitemporal
  fundamentals), ADR 0019 (derived metrics, not vendor metrics), `doc/plans/fundamentals-sf1.md`
  §6.5 to §6.7 (the fundamentals DQ checks, fafnir #102), and the fafnir-dba skill's playbooks
  for the fundamentals flags.
- Status legend: ⬜ planned · 🔄 in progress · ✅ done (PR #) · ⏭ carried over · ✖ dropped (reason)

---

## 1. Why dorq, and why now

fafnir now loads Sharadar SF1's statements: three tables (`core.income_statement`,
`core.balance_sheet`, `core.cash_flow_statement`), 58 line items, and every first-reported and
restated version on a knowledge timeline (ADR 0018). That is about 6M rows. `fafnir dq run`
judges them with fourteen SQL checks. They have the same problem the price checks had, in a new
place: fixed thresholds cannot tell SF1 getting a figure wrong from a filing that really says
it. The playbooks already do that sorting by hand:

- **`balance_sheet_identity`.** The playbook says "a gap in every period is the company's
  structure" (temporary equity, redeemable NCI) and "a gap in one period alone, unexplained by
  the filing, is a vendor error". The check flags both the same way.
- **`cash_balance_tie`.** This fires on restricted cash, which SF1's balance leaves out and US
  GAAP's cash flow has included since 2018.
- **`fundamentals_suspicious_zero`.** SF1 writes 0 for an item the filing does not carry, so the
  check flags absent values and real zeros alike. The playbook says to "check the filing".
- **`fundamentals_restatement`.** One flag per security and run. The operator reads its moves
  by eye, and the playbook says "an implausible one (a factor of 1,000, a sign) is a vendor
  error".
- **The identity pass rate.** operations.md says "a drop after a load is a load problem, not a
  hundred companies". No check says so.

The faults most common in statement data have no check at all:

- a value, a whole period, or an era in the wrong units (thousands stored as units, millions
  stored as thousands);
- one bad value in a series that is otherwise smooth;
- a sign flipped;
- a period copied from the one before;
- **share counts in the wrong share terms.** ADR 0018 records that "a split missing from
  Sharadar's ACTIONS misstates every earlier period's share counts". Through ADR 0019's
  conversion, a wrong count reaches every market cap and per-share factor in the panel,
  plausibly and silently;
- subtotals that do not add up: current plus noncurrent, gross profit, EBIT, the net income
  chain. Only the balance sheet and the cash flow are tied today;
- a restatement whose new version is itself the error.

The SQL checks are also brittle in the way M7 met. On 2026-10-27 `fundamentals_ytd_suspect`
produced two detections on one key. The open-condition index refused the insert, and the whole
`dq run` rolled back, price checks included (fafnir #217). dorq's ingest already merges two rows
for one condition (fafnir's doc/dorq.md, *Ingest*).

**The goal.** dorq becomes the only engine that detects data-quality problems in fafnir's
statements, as it is becoming for prices. For each suspicious value it reports the probability
of a data error, the competing explanations, the evidence and the repair. fafnir keeps the
queue: deduplication, acceptance and resolution (ADR 0001). It also keeps the few flags that
are not judgements of stored data (§3.7).

### 1.1 What counts as a true positive

The three classes of the development plan's §1.1 hold. For statements they read:

| Class | For statements | fafnir's disposition | dorq default |
|---|---|---|---|
| **Data error** | SF1's stored figure or date is not the filing's: units, sign, a dropped or doubled field, a copied period, a wrong filing date | report to Sharadar; the fix arrives as a vendor correction or a restatement; then resolve | **reported** |
| **Market fact** (here, a *filing fact*) | Anomalous, and it is what the filing says: an acquisition, a write-down, temporary equity outside both liabilities and equity, a change of fiscal year, a late filer | `dq accept`, naming the filing | info |
| **Context gap** | The figures are right, but context is missing: a split not in `core.corporate_action`, so the share counts step | `actions add` | reported, with the missing context named |

The `Classification` enum does not change.

---

## 2. Design

### 2.1 Statements are a third input kind

`SeriesKind::kStatements` joins OHLCV and point. An input row is one version of one statement of
one period: one row per (series, statement, period type, fiscal date, version). fafnir sends
every version current on the load timeline (`superseded_at IS NULL`).

| Column | Also accepted | Required | Meaning |
|---|---|---|---|
| `series` | `security_id` | yes, or the file stem | the security |
| `label` | `symbol`, `ticker` | no | for people |
| `statement` | `table_name` | yes | `income`, `balance_sheet` or `cash_flow` (fafnir's table names are read too) |
| `period_type` | | yes | `quarter` or `annual` |
| `fiscal_date` | `reportperiod` | yes | the period's end: the observation timeline |
| `fiscal_year`, `fiscal_period` | | yes | SF1's labels (`2025`, `Q3` or `FY`), not derived from the date |
| `version_no` | | no (default 1) | 1 is as first reported |
| `known_from`, `known_to` | | `known_from` | the knowledge interval, half-open; an empty `known_to` is in force |
| `knowledge_source` | | no | `filed`, `observed` or `estimated` |
| `filing_date` | | no | |
| `loaded_on` | | no | the day fafnir loaded the row (DQ951) |
| `reported_currency` | | no | |
| `close` | | no | the raw close on or before `known_from`: evidence for share terms (DQ92x) |
| any line item | | | the item's value. Empty is NULL, and is kept apart from 0 |

- **NULL is not 0.** An empty field is NULL, "does not apply". `0` is a zero: either SF1's
  "not carried" or a real zero. The reader keeps them apart, as the warehouse does.
- **Grouped by series**, as bars are: fafnir's export is ordered by `security_id`. The kind is
  detected from the columns (`statement`, `period_type` and `fiscal_date`) or set with
  `--kind statements`. Each input file holds one kind.
- **A field that does not parse** is DQ104, with its meaning unchanged. The row's other fields
  are kept.
- **Size.** About 6M rows: a few hundred per security, small beside 150M bars.

### 2.2 Which view each check reads

fafnir #102 settled which versions each SQL check reads. dorq builds the same views from its
rows, plus one:

| View | Rows | Read by |
|---|---|---|
| Every version | every row | DQ90x, DQ93x |
| In force at the as-of date | `known_from ≤ as_of < known_to` | DQ91x, DQ92x, DQ94x |
| As first reported | version 1 of each period | DQ91x, DQ92x |

`--as-of` defaults to the latest `known_from` in the input. Judging the first-reported view is
the departure from #102. A backtest dated before a restatement reads version 1, so a ×1,000 in
version 1 is still an error a researcher meets after a restatement fixes it. A finding on a
value both views share is one report, keyed on its version.

### 2.3 Line items and identities

- **`--items F` is the taxonomy.** It takes fafnir's `ref.statement_line_item` as it is:
  `column`, `statement`, `item_class` (flow, stock, share count), `sign_convention` and
  `is_subtotal`. It is a context file (doc/context.md), so dorq stays vendor-neutral and
  fafnir's map stays the only source. Its content goes into `config_hash`, as a reference
  calendar's does. Without it, the checks that need an item's class or sign are skipped and
  listed in the run summary. A row that cannot be used is DQ109.
- **`[statements]` in the config holds the judgements**, as opposed to the taxonomy:
  - the identities (§4.2);
  - the items that are never negative and the items that are never absent;
  - the tolerances;
  - the coverage floor.

  dorq ships SF1's in `priors/fafnir-statements.toml`.
- **An identity is a named equation over items**, such as `balance_sheet = "total_assets =
  total_liabilities + total_equity_parent"`. The parser takes sums and differences of item
  names and nothing else. An identity with a NULL term is not judged, so a bank's missing
  current and noncurrent split is never a break.

### 2.4 Codes: DQ9xx (ADR 0005)

ADR 0003 holds DQ9xx back "rather than given a meaning before one is needed". It is needed now.
Statements fill dorq's last free family, and the tens digit gives them sub-families. `--select
DQ9` picks every statement check and `--select DQ91` the value model. Prefix selection already
works at any length (`src/checks/registry.cpp`). ADR 0005 records the decision and amends the
table in ADR 0003.

| Codes | Sub-family |
|---|---|
| DQ90x | integrity and accounting identities |
| DQ91x | values over time: the statement analogue of DQ2xx |
| DQ92x | share counts and share terms |
| DQ93x | the knowledge timeline and revisions |
| DQ94x | coverage and freshness |
| DQ95x | cross-sectional |

Generic codes keep their meaning where it is the same: DQ104 (a value that does not parse) and
DQ109 (a context row that cannot be used). fafnir check names are derived as ever:
`dorq_identity_break`.

### 2.5 Output

- **A finding's date is its `fiscal_date`.** A run of periods, such as a unit era, has an
  `end_date`.
- **`record_key`** is `{"statement", "period_type", "fiscal_date", "version_no"}`, plus `item`
  or `identity` where the condition is per item. That is the shape fafnir #102 settled: a
  restated version that still fails is a new condition, and conditions on two tables or two
  items do not merge in `ux_dq_flag_open_condition`.
- **fafnir's `table_name`** is set per finding, from its statement (`core.balance_sheet` and so
  on). `[fafnir] table_name` stays the default for bars.
- **The newest period** of a series has no period after it, so it is provisional and capped at
  warn, as the newest bars are.
- **Text output** names the period and the version:

  ```
  AAPL  2020-06-27 Q3 FY2020 income v1  DQ912 error  unit-error  revenue 59,685 is ×0.001 the level either side, and every amount in the row moved with it; P(error) = >0.99
  ```

### 2.6 Bars runs do not change

- **A statements run is a separate invocation, with its own config file** (fafnir: `[dq]
  dorq_statements_config`). The bars run's `config_hash` therefore stays the same, and this
  work cannot restart the price shadow period (DR-0707).
- **Every M9 release proves it.** A test checks that the bars output on dorq-synth's universe
  is byte-identical to the previous release's, version fields aside.
- **`--since D`** on statements keeps a finding when any version it names has a `known_from` or
  `loaded_on` of D or later. **`--as-of`** is the knowledge date of the in-force view, and
  DQ942's reference date.

### 2.7 Public-safe

As in Q5 of the development plan: SF1's rows are licensed, so dorq's tests use synthetic
statements only. Real-data cases live in fafnir's tests, as they do for bars.

---

## 3. The checks

The numbers are provisional until each check ships (ADR 0003 §8). **B** checks are Bayesian and
report `p_error`; **D** checks are deterministic. *Succeeds* names the fafnir SQL check the dorq
check replaces at cutover.

### DQ90x: integrity and identities

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ901 | `statement-key` | D | | Two rows for one (statement, period, version). A period whose versions do not chain: each `known_to` must equal the next `known_from`, with only the last one open. A `known_from` before its `fiscal_date`, or a `filed` version whose `known_from` is not its `filing_date` | `fundamentals_version_chain` |
| DQ902 | `impossible-value` | D | `--items`, config | A negative where none can be (revenue, total assets, share counts). A component above its total among non-negative items (current assets, cash or goodwill above total assets) | `fundamentals_negative_impossible` |
| DQ903 | `identity-break` | B | config | A configured identity fails beyond tolerance. The model tells a structural gap (NCI or temporary equity: persistent, at a stable share) and a reclassification (one version only) from an error (one period alone; the gap equals one term, a term times a power of ten, or twice a term) | `balance_sheet_identity`, `cash_flow_tie` |
| DQ904 | `period-tie` | B | `--items` | A tie across periods fails: the change in cash against the cash flow's net change, or a fiscal year's four quarters against its annual period (flows) | `cash_balance_tie` |

### DQ91x: values over time

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ911 | `bad-value` | B | | One period's value off a series that is otherwise smooth, which the next periods revert | |
| DQ912 | `unit-error` | B | | A value, a whole row or an era stored at a power of ten (×10³, ×10⁶) | |
| DQ913 | `sign-error` | B | `--items` | A sign flipped against the item's convention and its own history | |
| DQ914 | `copied-period` | B | | A period identical to the one before it on many items, flows included | |
| DQ915 | `absent-as-zero` | B | | A zero where the company reports the item in the periods either side | `fundamentals_suspicious_zero` |
| DQ916 | `cumulative-quarters` | B | `--items` | Quarters stored year to date, or an annual figure stored as a quarter | `fundamentals_ytd_suspect` |
| DQ919 | `large-change` | B, info | | A change that is probably real (an acquisition, a write-down), kept for review: the successor of a threshold that would have flagged it | |

### DQ92x: share counts and share terms

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ921 | `share-terms-step` | B | `--actions`; `close` sharpens it | Share counts, put into one share terms with the splits on file, step by a clean split ratio: a split missing from the actions (a context gap), or a count stored in the wrong terms | |
| DQ922 | `share-count-mismatch` | B | | A period's weighted-average and outstanding shares disagree by a clean split ratio or a power of ten | |

### DQ93x: the knowledge timeline and revisions

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ931 | `filing-date-implausible` | B | | A filing date judged against the issuer's own lags and the filing deadlines. Too early (the period end recorded as the filing date: a look-ahead), or an amendment's date taken for the original's | `fundamentals_implausible_filing_lag` |
| DQ932 | `restatement-implausible` | B | | A new version that differs from the one before it by a power of ten, a sign, or two fields swapped. Either the new version is the error, or the old one was | the judging half of `fundamentals_restatement` |

### DQ94x: coverage and freshness

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ941 | `period-gap` | B | `--meta` (the trading span) | A missing period in the quarterly or annual sequence while the security traded, told apart from a change of fiscal year and from a mislabelled quarter | `fundamentals_gap`, `fundamentals_ttm_incomplete` |
| DQ942 | `statements-stale` | B | `--meta`, `--as-of` | No newer period by the as-of date, judged by the issuer's own cadence and filing lag rather than a fixed 200 days | `fundamentals_stale` |
| DQ943 | `statements-missing` | D | `--meta` | A series the metadata expects statements for, with none in the input | `fundamentals_coverage` |
| DQ944 | `reporting-currency` | D, info | `--meta` | Statements in a currency other than the one the security trades in | `fundamentals_currency_mismatch` |

### DQ95x: cross-sectional

| Code | Name | | Needs | Detects | Succeeds |
|---|---|---|---|---|---|
| DQ951 | `revision-cohort` | B | more than one series | Many series revised on one date, or carrying the same finding in versions loaded the same day, beyond the base rate. That is a vendor batch or a load fault, not many separate events. Members' reports are downgraded and linked | |

### 3.7 What stays in fafnir

| fafnir flag | Why it stays |
|---|---|
| `fundamentals_unmapped_fields` | It is about SF1's file layout, not a stored value, and dorq never sees `extra`. It moves into the loader, which meets the column first |
| the loader's quarantines (`fundamentals_unlinked`, `_missing_period_label`, `_filing_before_period`, `_value_out_of_range`, `_duplicate_period`) | The row was never stored, so there is nothing to export |
| `fundamentals_restatement`, `fundamentals_vendor_correction`, `fundamentals_estimated_knowledge_date` | They are the loader's record of what happened, not a judgement. At cutover the nightly restatement flag drops to `info`, because DQ932 judges which restatements matter |

The other thirteen SQL checks retire at cutover (DR-0914).

---

## 4. The models

As in the development plan's §4, every model is closed-form or online, costs O(n) per series,
and samples nothing.

### 4.1 The statement value model (DQ911 to DQ916, DQ919)

- **The scale.** Each item is read as `z = asinh(v / s)`, with `s` the median of `|v|` over the
  series. This is close to a log for large values, and it is safe for zeros and signs. A ×1,000
  error is a shift of `log 1000` on either side of zero.
- **The expected value.** A quarter is predicted from the adjacent quarters and from the same
  quarter a year before and after (seasonality). A year is predicted from the adjacent years.
  Each item has a discounted robust scale, like DQ2xx's NIG. A quarterly series has only four
  points a year, so the class prior carries more weight than it does for bars, and `dorq
  calibrate` fits it.
- **Screening.** A value becomes a candidate on any of these:
  - a predictive tail probability below 1e-3;
  - a ratio of 5 or more against both sides;
  - a sign change on a non-zero item;
  - a zero between non-zeros;
  - a row that equals the previous row on `copy_min_items` items.
- **The hypotheses**, scored with the period's other items and the periods either side:

  | H | Evidence that favours it |
  |---|---|
  | `real_change` | the level holds in the later periods; related items move with it (revenue with assets, goodwill with an acquisition) |
  | `bad_value` | the next periods revert; one item moves alone; the period's identities fail, and the value's repair restores them |
  | `unit_error` | the ratio is a power of ten. Every money item in the row moving by the same factor is decisive. An item at the ratio for an era, and back, is an era |
  | `sign_error` | the ratio is −1 within tolerance, against the item's convention and its own history; an identity that fails holds again with the sign flipped |
  | `cumulative_quarter` | quarter *k* equals the sum of quarters 1 to *k*; Q4 equals the year; the four quarters sum to about two and a half years |
  | `annual_in_quarter` | a quarter equals its fiscal year |
  | `copied_period` | the row equals the previous row to the cent on many items, flows included |
  | `absent_as_zero` | a 0 where both neighbours report the item. The prior depends on the item: revenue is never absent for a company that reports it, and R&D often is |

- **Cross-field evidence makes this tractable on few periods.** An identity that fails with the
  candidate value and holds with its repair is strong evidence, and so is a quarter-sum tie that
  holds once the candidate is rescaled. The bars model has dozens of neighbours on each side;
  this one has a few, and the identities make up for them.
- **`p_error`** is the sum of the error hypotheses, and the reported code is the most probable
  error, as in DQ2xx. When `real_change` wins, the value is DQ919 at info, or nothing.

### 4.2 Identity gaps (DQ903, DQ904)

- **The tolerance** is #102's: the larger of 0.5% of the scale and 1M of the reporting currency,
  or 2% of the balance for cash.
- **The hypotheses for a gap beyond it:**

  | H | Evidence that favours it | Class |
  |---|---|---|
  | `structural` | the gap persists across the periods at a stable share of the scale; for the balance sheet, NCI income on the income statement; for the cash tie, periods from 2018 (restricted cash) | filing fact |
  | `reclassification` | the gap is in one version of the period and not the other, with the totals preserved | filing fact |
  | `field_error` | the gap equals one term, a term times (10^k − 1), or twice a term. The report names that field | data error |
  | `unexplained` | a gap in one period alone that no term explains | data error |

- **Candidate identities for SF1.** Each is confirmed against SF1's definitions and measured on
  the host before it ships at warn. An identity that holds on 100% of rows holds by
  construction: it judges nothing and is not shipped. One that holds on less than 99% is a
  model question before it is a check.
  - Balance sheet:
    - total assets = total liabilities + parent equity, with the structural gap;
    - total assets = current + noncurrent assets;
    - total liabilities = current + noncurrent liabilities;
    - total debt = current + noncurrent debt;
    - total investments = current + noncurrent investments.
  - Income statement:
    - gross profit = revenue − cost of revenue;
    - operating income = gross profit − operating expenses;
    - EBIT = income before tax + interest expense;
    - net income = consolidated income − NCI income;
    - net income to common = net income − preferred dividends.
  - Cash flow: net change in cash = operating + investing + financing + FX.
  - Across periods (DQ904):
    - the change in cash = the net change in cash;
    - the sum of four quarters = the year, for flows.

### 4.3 Share terms (DQ921, DQ922)

ADR 0018 stores a share count in the terms of its `known_from`. ADR 0019 converts it to a date's
terms with the warehouse's splits in `(known_from, t]`. dorq does the same conversion with
`--actions`, into the terms of the latest version. The converted series should then move only
by issuance and buybacks. A step by a clean ratio between two periods (DQ203's ratio mixture,
priced by level) has these explanations:

| H | Evidence that favours it | Class |
|---|---|---|
| `unreported_split` | no split on file between the two periods; with `close`, the price steps by the inverse ratio and the implied market cap stays level | context gap |
| `terms_error` | a split on file between them, with the step equal to its inverse or its square; the implied market cap steps | data error |
| `unit_error` | the step is ×1,000 | data error |
| `real_issuance` | the step is not clean, or the market cap moves with it (a merger, an offering) | filing fact |

DQ922 compares a period's weighted-average and outstanding shares. A split on file between the
period's start and the filing date explains a ratio equal to that split.

### 4.4 The knowledge timeline (DQ931, DQ932)

- **Filing lags.** For each series and period type, the lags of its filed versions give a robust
  centre and spread. The SEC's deadlines give the prior: 40 to 45 days for a 10-Q, and 60 to 90
  days for a 10-K, by filer size. The hypotheses:

  | H | Evidence that favours it | Severity at most |
  |---|---|---|
  | `ordinary` | the lag sits with the issuer's other lags | |
  | `late_filing` | one long lag, then the issuer's usual lag again: a filing fact | info |
  | `misdated_early` | below any filer's lag, or far below this issuer's own: the period end or a press release's date. Every backtest would know the figures too soon | error |
  | `misdated_amendment` | an amendment's date: the period becomes known after the next period does | warn |

- **Restatements.** For each item the ratio of new to old is taken. An ordinary restatement
  moves a few components and keeps the totals, or moves values by a few per cent. The
  hypotheses:
  - `restatement`: a filing fact;
  - `new_version_error`: a power of ten, a sign, two fields swapped, or a total that moved while
    its components held;
  - `old_version_error`: the new version agrees with the neighbouring periods and the old one
    did not. The old version was then wrong for every point-in-time read in its interval, and
    the report names the old version.

### 4.5 The period sequence (DQ941 to DQ943)

- **The sequence** is read from the fiscal labels and the spacing of period ends: quarters 56 to
  126 days apart, years 300 to 430 (#102's ranges). The hypotheses for a break:
  - `missing_period`: a data error;
  - `fiscal_year_change`: a transition period with one odd spacing, after which the labels
    rebase. A filing fact;
  - `mislabel`: the labels skip, but the spacing is one period;
  - `not_yet_filed`: the newest period, which is provisional.
- **Only while the security traded.** The meta's first and last trading dates, or its listing
  and delisting dates, bound the span, and so does the coverage floor (1998, fafnir ADR 0016).
- **Staleness** is the expected next period end plus the issuer's usual lag. The finding is
  keyed on the latest `fiscal_date` and reported for as long as it holds, as DQ304 is since
  v0.10.0.
- **DQ943** needs dorq to see series that appear only in `--meta`. Once the input ends, a meta
  series with `statements_expected` and no rows is reported.

### 4.6 The revision cohort (DQ951)

For each `known_from` date of observed versions, and each `loaded_on` date, dorq counts the
series with a new version and the series with each finding. A Poisson tail against the base
rate (1e-4 or less, as for DQ601) makes a cohort. The cohort is reported once, and its members'
DQ932 and DQ903 reports are downgraded and linked (`detail.members`). A backfill, which dates
every restatement it finds to one day, is the canonical cohort.

---

## 5. Integration with fafnir (DR-0912 to DR-0914)

1. **Export.** The three tables' current versions, as one stream ordered by `security_id`,
   statement, period type, fiscal date and version, go into `dorq check --kind statements`.
   Beside them go `ref.statement_line_item` (`--items`), `core.corporate_action` (`--actions`,
   as for bars) and the security master (`--meta`). The meta gains the currency, the trading
   span, the active and delisted state, the sector, and `statements_expected` (from the vendor
   master's `first_quarter`). `close` comes from a lateral join on `core.daily_price`.
2. **Window.** Statements are small, so the default is a full run each night if it stays under
   the runtime target (measured on the host). Otherwise the run takes the whole history of each
   security with a row loaded since the previous run. A finding can appear on an old period
   when a new one arrives (a revert confirms a bad value). That is when the finding becomes
   knowable.
3. **Ingest.** As for bars, with the `table_name` read from each row.
4. **Shadow.** Rows go to `<dorq_shadow_dir>/statements/<as-of>.jsonl`. It has to be a
   directory of its own, because `dq compare` takes the last `*.jsonl` in the shadow directory
   as the newest night of bars.
5. **Compare.** The successors of the thirteen SQL checks (§3) are matched by security, period
   and statement, not by a date slack.
6. **Recheck.** Rerun and negate, over the securities with open dorq statement flags. DQ951 is
   excluded as a cross-sectional check. DQ901 goes in `NEVER_AUTO_RESOLVE`: its repair is made on
   the host, as the `fundamentals_version_chain` playbook already says.
7. **Labels.** Vendor corrections are this warehouse's repairs of statements. When SF1 corrects
   a row, the new row supersedes the old, and fafnir keeps the old one:
   - a correction that moved a value materially is a `data_error` label on the superseded row,
     and `--restore` puts that row back, as it does for bars;
   - a fundamentals flag accepted with a filing named is a filing fact;
   - a flag resolved because the vendor fixed the value is a data error;
   - `dq label add` gains kinds for statements and their keys (period type, fiscal date,
     version, statement, item).
8. **Cutover.** The thirteen SQL checks retire, and `fundamentals_unmapped_fields` moves into
   the loader (§3.7).

---

## 6. Measuring success

**dorq-synth statements** (DR-0903) generates companies whose identities hold, with:
- seasonality and growth;
- acquisitions (a step that holds) and write-downs;
- splits, with counts in their filing's terms;
- restatements by reclassification;
- changes of fiscal year, and late filers;
- banks with no current and noncurrent split, and pre-revenue companies.

It injects labelled faults:
- unit errors in one value, a whole row, and an era;
- sign flips, bad values, copied periods and absent-as-zero values;
- quarters stored year to date, and an annual figure stored as a quarter;
- identity breaks from a dropped or doubled field;
- a split missing from the actions, and counts recovered in the wrong terms;
- a filing date at the period end, and an amendment's date;
- a missing quarter;
- a restatement at ×1,000.

The CTest gate holds each check's precision and recall on seeds 1 and 2, as `tools/synth/gates.txt`
does for bars.

**Acceptance criteria for the statements cutover:**

| Metric | Target |
|---|---|
| Precision at warn and above | **≥ 0.85**, and ≥ 0.95 at error. Judged on an EDGAR-verified sample of a full-history run (at least 200 reports, stratified so that each code has at least 10) and on every correction label |
| Recall | **≥** the SQL checks' recall, on the conditions they flagged that closure confirmed as errors |
| Queue | the standing count at warn and above after the first full run is at most 30% of the SQL checks'; new flags per night are at least 70% fewer |
| Calibration | expected calibration error of 0.05 or less, once there are 100 labels |
| Runtime | the full statements history in 2 minutes or less on the host, end to end |
| Determinism | as in the development plan's §8 |
| Stability | 20 nightly sessions in shadow, with nothing new that the sample did not cover |

---

## 7. Sequencing

- **Alongside the price cutover, not after it.** Nothing in M9 changes a bars run (§2.6), so
  the work does not wait for DR-0707. fafnir's statements shadow can start with v0.11.0,
  whatever state the price shadow is in.
- **dorq-synth first.** DR-0903 lands before the first Bayesian check (DR-0905), so the
  synthetic faults shape the models, as DR-0308's did in M3.
- **Successors before new checks.** v0.11.0 covers five of the thirteen SQL checks, so the
  shadow has something to compare from its first night.

## 8. Releases

| Release | Items | What fafnir can do with it |
|---|---|---|
| v0.11.0 | DR-0901 to DR-0905 | shadow the integrity and identity checks: five SQL successors |
| v0.12.0 | DR-0906 to DR-0908 | the value model and share terms |
| v0.13.0 | DR-0909, DR-0910 | the timeline, coverage and the cohort: every successor |
| v0.14.0 | DR-0911 | a calibration on fafnir's labels; the go/no-go and cutover (DR-0914) |

## 9. Work items

Sizes as in the development plan: S ≈ ½ day, M ≈ 1 to 2 days, L ≈ 3 to 5 days.

| ID | Repo | Item | Size | Depends on | Status |
|---|---|---|---|---|---|
| DR-0900 | dorq | Tracking issue for M9 | | | ⬜ |
| DR-0901 | dorq | ADR 0005; the statements input kind: reader, data model, `--items`, `[statements]` config, engine path, `--since` and `--as-of` | L | | ⬜ |
| DR-0902 | dorq | Statement findings in every output format: keys, `record_key`, per-finding `table_name`, text lines | M | 0901 | ⬜ |
| DR-0903 | dorq | dorq-synth statements: companies, faults, hard negatives, gate | L | 0901 | ⬜ |
| DR-0904 | dorq | DQ901 statement-key, DQ902 impossible-value | M | 0902 | ⬜ |
| DR-0905 | dorq | DQ903 identity-break, DQ904 period-tie; SF1's identities in `priors/fafnir-statements.toml`; v0.11.0 | L | 0903, 0904 | ⬜ |
| DR-0906 | dorq | The statement value model: DQ911 bad-value, DQ912 unit-error, DQ913 sign-error, DQ919 large-change | L | 0905 | ⬜ |
| DR-0907 | dorq | Quarterly structure: DQ914 copied-period, DQ915 absent-as-zero, DQ916 cumulative-quarters | M | 0906 | ⬜ |
| DR-0908 | dorq | Share terms: DQ921 share-terms-step, DQ922 share-count-mismatch; v0.12.0 | M | 0906 | ⬜ |
| DR-0909 | dorq | The knowledge timeline and revisions: DQ931, DQ932, DQ951 | L | 0906 | ⬜ |
| DR-0910 | dorq | Coverage and freshness: DQ941 to DQ944, the new `--meta` fields, series known only from the meta; v0.13.0 | L | 0902 | ⬜ |
| DR-0911 | dorq | Labels, calibration and dorq-eval for statements; fit on fafnir's labels; v0.14.0 | M | 0906 to 0910, 0913 | ⬜ |
| DR-0912 | fafnir | Export, run and ingest statements; shadow; `dq compare`; recheck | L | 0902 (v0.11.0) | ⬜ |
| DR-0913 | fafnir | Fundamentals labels (corrections, closures, stated); playbooks, tiers, `NEVER_AUTO_RESOLVE` | M | 0912 | ⬜ |
| DR-0914 | fafnir | Statements shadow period, go/no-go, cutover; the SQL checks retire | M | 0911, 0912, 0913 | ⬜ |

## 10. Deferred, not planned

- **Vendor corrections as a check.** Superseded rows as an input, so that a correction can be
  judged as a restatement is. In M9 they are labels only (§5.7).
- **The factor panel's outliers.** The panel is derived, so its errors are its inputs' errors
  (ADR 0019). dorq checks the inputs.
- **Sector and industry priors** beyond what profiles can match on.
- **SF1's vendor metrics as evidence** (`eps`, `sharefactor`). They are not read, by ADR 0019's
  rule.
- **Cross-vendor statements** (DQ801 for statements). No second vendor writes statements.
- Segments, and SF2 and SF3 (insiders, holdings).

## 11. Questions for sign-off

| # | Question | Recommended default |
|---|---|---|
| F1 | Give DQ9xx to statements, with sub-families by the tens digit (ADR 0005)? | Yes |
| F2 | Judge the first-reported view as well as the in-force view (§2.2)? | Yes: the first-reported view is what a backtest reads |
| F3 | Take the taxonomy from fafnir's table as a context file (`--items`), and keep the identities in dorq's config? | Yes |
| F4 | Run the full statements history every night, if it takes under 2 minutes? | Yes; otherwise the securities with a row loaded since the previous run |
| F5 | Judge the cutover on an EDGAR-verified sample of a full run, rather than on 20 nightly sessions' labels alone (§6)? | Yes: SF1's delta is about 1,100 rows over four days, too few to judge by |
| F6 | At cutover, move `fundamentals_unmapped_fields` into the loader, and drop the nightly `fundamentals_restatement` flag to info? | Yes |
| F7 | Run M9 alongside the price cutover (DR-0707) rather than after it? | Yes: §2.6 keeps the bars runs unchanged |
