# Issue drafts: fundamentals in dorq (M9)

These are the GitHub issues for [dorq-fundamentals-plan.md](dorq-fundamentals-plan.md) §9, one
per work item. Each starts at a level-2 heading, which is its title. The line after the heading
gives its repository and labels. Everything after that is its body. Once the issues exist, the
plan's §9 links them, and this file can be deleted.

Cross-references in the bodies read `DR-09xx`. When the issues are created, those become links
to the issues.

---

## [DR-0900] Fundamentals in dorq (M9): tracking

Repo: rtrimble13/dorq · Labels: fundamentals, tracking

**Plan:** `doc/plans/dorq-fundamentals-plan.md` · **Milestone:** M9

#### Goal

dorq becomes the only engine that detects data-quality problems in fafnir's statements (Sharadar
SF1, rtrimble13/fafnir ADR 0018), as it is becoming for prices. For each suspicious value it
reports the probability of a data error, the competing explanations, the evidence and the
repair. fafnir keeps the queue (dorq ADR 0001).

At cutover, thirteen of fafnir's fourteen SQL fundamentals checks (rtrimble13/fafnir#211, #217)
retire in favour of DQ9xx. `fundamentals_unmapped_fields` moves into the loader.

#### Before DR-0901 starts

Answer the plan's §11 questions F1 to F7, each of which has a recommended default.

#### Work items

| Release | Items |
|---|---|
| v0.11.0 | DR-0901 statements input · DR-0902 output · DR-0903 dorq-synth statements · DR-0904 DQ901/DQ902 · DR-0905 DQ903/DQ904 |
| v0.12.0 | DR-0906 value model · DR-0907 quarterly structure · DR-0908 share terms |
| v0.13.0 | DR-0909 knowledge timeline and revisions · DR-0910 coverage and freshness |
| v0.14.0 | DR-0911 labels and calibration |
| fafnir | DR-0912 export, run, ingest, shadow · DR-0913 labels and playbooks · DR-0914 shadow period and cutover |

- [ ] DR-0901
- [ ] DR-0902
- [ ] DR-0903
- [ ] DR-0904
- [ ] DR-0905 (v0.11.0)
- [ ] DR-0906
- [ ] DR-0907
- [ ] DR-0908 (v0.12.0)
- [ ] DR-0909
- [ ] DR-0910 (v0.13.0)
- [ ] DR-0911 (v0.14.0)
- [ ] DR-0912 (fafnir)
- [ ] DR-0913 (fafnir)
- [ ] DR-0914 (fafnir)

#### Rules that hold for every item

- **A bars run does not change.** Every release keeps the bars output on dorq-synth's universe
  byte-identical to the previous release's, version fields aside (plan §2.6). The price shadow
  period (DR-0707) must not restart because of this work.
- **Public-safe.** Tests and fixtures are synthetic. No SF1 row is committed (development plan,
  Q5).
- **Docs move with the code** (CONTRIBUTING.md).

---

## [DR-0901] Statements as a third input kind: ADR 0005, reader, `--items`, `[statements]` config

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0901` · **Size:** L · **Release:** v0.11.0 · **Depends on:** none · **Plan:** §2.1 to §2.4, §2.6 · **Tracking:** DR-0900

#### Context

fafnir's statements are panel data on three timelines (rtrimble13/fafnir ADR 0018): a period
(`fiscal_date`), versions of it on a knowledge timeline (`known_from`/`known_to`), and
corrections on a load timeline. dorq reads bars and point series, one value per date. Every
statement check needs this input first.

#### Scope

- **ADR 0005.** It records three decisions:
  - statements are a third series kind;
  - DQ9xx is their family, with sub-families by the tens digit (plan §2.4);
  - checks read three views: every version, in force at the as-of date, and as first reported
    (§2.2).

  ADR 0003 gets a correction note on its family table.
- **The data model.** `SeriesKind::kStatements`. Each row holds:
  - its key: statement, period type, fiscal date, fiscal year and period, version;
  - its knowledge fields: `known_from`, `known_to`, `knowledge_source`, `filing_date`,
    `loaded_on`;
  - `reported_currency` and `close`;
  - the item values, column-major. **NULL is kept apart from 0.**
- **The reader.** It reads CSV, TSV, JSON Lines and JSON arrays.
  - Columns are matched by the aliases in §2.1. The kind is detected from `statement`,
    `period_type` and `fiscal_date`, or set with `--kind statements`.
  - Rows are streamed grouped by series, with the same rule as bars: a series that reappears
    is exit 3 unless `--buffer` is given.
  - A value that does not parse is DQ104. A row without a usable key or `known_from` is DQ104
    and is skipped.
- **The views.** They are built once per series and shared by the checks, as the price and
  coverage analyses are today.
- **`--items F`.** A context file in the shape of fafnir's `ref.statement_line_item`: `column`,
  `statement`, `item_class`, `sign_convention`, `is_subtotal`.
  - A row that cannot be used is DQ109.
  - The file's content is hashed into `config_hash`, as a reference calendar's is.
- **`[statements]` config:**
  - `identities`, parsed as sums and differences of item names. When `--items` is given, an
    unknown item is a configuration error (exit 2);
  - `nonnegative`, `never_absent` and `bounds`;
  - the tolerances `identity_share` (0.005), `tie_floor` (1e6) and `cash_tie_share` (0.02);
  - period spacing: quarters 56 to 126 days apart, years 300 to 430;
  - `coverage_floor` (1998-01-01).

  Unknown keys are rejected. `config show` and `config init` include the section.
- **The engine.** Statement series run statement checks only, and bars checks skip them
  (`Applies` gains a statements value). The cross-sectional hooks DQ951 will use are added
  here. Results are delivered in input order.
- **`--since` and `--as-of` for statements**, as in plan §2.6.
- **`list-checks`** shows which kinds each check applies to.

No DQ9xx check ships in this item. DQ104 and DQ109 run on statements.

#### Acceptance criteria

- [ ] Unit tests on a synthetic statements file:
  - NULL is kept apart from 0;
  - each period's versions are read in order;
  - the three views hold what §2.2 says;
  - an unparseable value is DQ104, and a bad items row is DQ109.
- [ ] Streaming and `--buffer` behave as they do for bars.
- [ ] The reader and context fuzzers gain corpora for a statements file and an items file.
- [ ] **A golden test: the bars output on dorq-synth's universe is byte-identical to v0.10.0's,
  version fields aside.** It stays for every M9 release.
- [ ] `--threads 1` and `--threads 16` give byte-identical statement output.
- [ ] Reading 6M synthetic statement rows is timed and recorded in doc/performance.md.

#### Docs move with the code

- [ ] `doc/adr/0005-statements.md`, and a correction note on ADR 0003
- [ ] `doc/statements.md` (new): the input contract, the views, `--items`, `[statements]`
- [ ] `doc/configuration.md`, `doc/context.md`, `README.md` (input kinds, a usage line)
- [ ] The plan's status for DR-0901

---

## [DR-0902] Statement findings in every output format

Repo: rtrimble13/dorq · Labels: fundamentals, size:M

**Key:** `DR-0902` · **Size:** M · **Release:** v0.11.0 · **Depends on:** DR-0901 · **Plan:** §2.5 · **Tracking:** DR-0900

#### Context

fafnir's queue keys a condition on `(check_name, security_id, record_key)`. Its fundamentals
checks settled the record keys for statements (rtrimble13/fafnir#211, plan §6.7 there). A key
names the period and version, plus the statement where a check reads more than one table. If
the keys are wrong, conditions merge or the open-condition index refuses the insert: on
2026-10-27 that rolled back a whole `dq run` (rtrimble13/fafnir#217).

#### Scope

- **Statement findings carry their period.** Each one holds its statement, period type, fiscal
  date, fiscal year and period, and version, plus an `item` or `identity` where the condition is
  per item. `Violation::key` holds the extra `record_key` fields.
- **`record_key`** is `{"statement", "period_type", "fiscal_date", "version_no"}`, plus `item`
  or `identity`. Cross-sectional rows are keyed on their date and the finding's kind.
- **The `fafnir` format** sets `table_name` per finding, from `[fafnir.tables]`. The defaults
  are `core.income_statement`, `core.balance_sheet` and `core.cash_flow_statement`. Bars keep
  `[fafnir] table_name`.
- **Text output:** `AAPL  2020-06-27 Q3 FY2020 income v1  DQ912 error  unit-error  ...`.
- **json, jsonl and csv.** The csv format appends its new columns (statement, period type,
  fiscal period, version, item) after the existing ones, so those keep their positions.
- **Sort order:** series, fiscal date, statement, version, code.

#### Acceptance criteria

- [ ] Writer unit tests for each format on statement findings, `table_name` included.
- [ ] Two items, or two identities, of one period give different `record_key`s, and so do two
  versions of one period.
- [ ] `--statistics` counts statement codes as it counts the others.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/output.md`: the statement keys, the `table_name` mapping, the text line
- [ ] `README.md`: an example
- [ ] The plan's status for DR-0902

---

## [DR-0903] dorq-synth statements: synthetic companies, faults, hard negatives and the gate

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0903` · **Size:** L · **Release:** v0.11.0 · **Depends on:** DR-0901 · **Plan:** §6 · **Tracking:** DR-0900

#### Context

dorq-synth's faults shaped every price model (DR-0308), and its gate holds each check's precision
and recall (CONTRIBUTING.md). The statement checks need the same, and real SF1 rows cannot be
committed (development plan, Q5). This item lands before the first Bayesian statement check.

#### Scope

- **`dorq-synth` writes a statements universe**: `statements.csv`, `items.csv`, `actions.csv`
  (splits), `meta.csv` (trading span, currency, `statements_expected`) and `labels.jsonl`, all
  determined by the seed.
- **Companies**, by size and kind: industrials, banks (NULL current and noncurrent splits), and
  pre-revenue companies (real zeros).
  - Flows are seasonal and grow; stocks roll forward. **The identities hold by construction**,
    apart from the structural gaps the generator adds on purpose: NCI, temporary equity, and
    restricted cash from 2018.
  - Share counts move with issuance and buybacks, and with splits. Counts are stored in their
    filing's terms, and a `close` column comes from a price path consistent with the market cap.
  - Filing lags are drawn per issuer, for 10-Qs and 10-Ks, with late filers among them.
  - Restatements reclassify (components move, totals hold) or move values by a few per cent.
    They are known from a load day, and one backfill day holds many of them.
  - Some companies change fiscal year, and some delist.
- **Faults**, each labelled: plan §6's list.
- **Hard negatives**, which must produce no warn:
  - acquisitions and write-downs;
  - changes of fiscal year, and late filers;
  - real zeros, and banks' NULLs;
  - structural gaps;
  - reclassifying restatements;
  - splits on file.
- **Labels.** The label schema gains `period_type`, `version_no`, `statement` and `item`, with
  `first`/`last` read as fiscal dates. dorq-synth's `score` and dorq-eval match statement labels
  by series, period, version and item.
- **The gate.** `ctest -R synth` runs the statements universe on seeds 1 and 2. Each check's
  item adds its own bounds to `tools/synth/gates.txt`.

#### Acceptance criteria

- [ ] The same seed gives byte-identical files.
- [ ] A test holds every identity on the rows with no fault injected.
- [ ] The universe reads through DR-0901's reader with no DQ104 or DQ109.
- [ ] dorq-eval reports precision and recall on statement labels.

#### Docs move with the code

- [ ] `tools/synth/README.md`, `tools/eval/README.md`, `doc/labels.md` (the statement fields)
- [ ] The plan's status for DR-0903

---

## [DR-0904] DQ901 statement-key and DQ902 impossible-value

Repo: rtrimble13/dorq · Labels: fundamentals, size:M

**Key:** `DR-0904` · **Size:** M · **Release:** v0.11.0 · **Depends on:** DR-0902 · **Plan:** §3 (DQ90x) · **Tracking:** DR-0900

#### Context

These are the deterministic successors of `fundamentals_version_chain` and
`fundamentals_negative_impossible` (rtrimble13/fafnir `src/fafnir/dq/checks.py`). dorq is
vendor-neutral, so DQ901 also checks what fafnir's constraints already make unstorable.

#### Scope

- **DQ901 `statement-key`**, at error:
  - two rows for one (statement, period, version);
  - versions of a period that do not chain: a gap, an overlap, two open, or an open version
    that is not the last;
  - a `known_from` before its `fiscal_date`;
  - a `filed` version whose `known_from` is not its `filing_date`.

  Its suggested action names the versions and the interval to close or reopen.
- **DQ902 `impossible-value`**, at error:
  - a negative value of an item in `[statements] nonnegative`. For SF1: revenue, basic and
    diluted weighted shares, total assets, shares outstanding;
  - a `[statements] bounds` relation that fails beyond rounding. For SF1: current assets, cash,
    and goodwill and intangibles at most total assets; current liabilities at most total
    liabilities.

  Keyed per version and item.

#### Acceptance criteria

- [ ] A unit test for each case, including a period with one version and an unordered input.
- [ ] Synthetic replicas of the shapes in fafnir's `test_dq_fundamentals.py` give the same
  conditions as the SQL checks.
- [ ] On dorq-synth, precision and recall are 1.0, and the gate holds them.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ901.md`, `doc/checks/DQ902.md`
- [ ] The `README.md` check table
- [ ] The plan's status for DR-0904

---

## [DR-0905] DQ903 identity-break and DQ904 period-tie; SF1's identities; v0.11.0

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0905` · **Size:** L · **Release:** v0.11.0 · **Depends on:** DR-0903, DR-0904 · **Plan:** §3, §4.2 · **Tracking:** DR-0900

#### Context

`balance_sheet_identity`, `cash_flow_tie` and `cash_balance_tie` flag every gap beyond a
tolerance. fafnir's playbook then sorts the gaps by hand: "a gap in every period is the
company's structure… a gap in one period alone, unexplained by the filing, is a vendor error"
(operations.md › *Investigating a balance-sheet-identity failure*). This item puts that sorting
into a model. It also ties the subtotals that nothing checks today.

#### Scope

- **DQ903 `identity-break`.** Each configured identity is judged on each version, with #102's
  tolerance. A gap beyond it is weighed as `structural`, `reclassification`, `field_error` or
  `unexplained` (plan §4.2), with this evidence:
  - the same identity's gap in the neighbouring periods, and in the period's other versions;
  - an identity's configured `explained_by` items (NCI income for the balance sheet);
  - whether the gap equals one term, a term times (10^k − 1), or twice a term. A `field_error`
    names the field.

  Structural gaps and reclassifications are info (filing facts).
- **DQ904 `period-tie`:**
  - the change in cash between consecutive balance sheets in force, within #102's spacing,
    against the cash flow's net change. Restricted cash, from `restricted_cash_from`
    (2018-01-01), is a structural explanation;
  - a fiscal year's four quarters against its annual period, for flows. A year with more than
    one annual period in force is not judged (the shape behind rtrimble13/fafnir#217). A year
    with fewer than four quarters is not judged either.
- **`priors/fafnir-statements.toml`**: SF1's candidate identities (plan §4.2), `nonnegative`,
  `never_absent`, `bounds` and the tolerances, installed to `share/dorq/priors/`.
- **Measured on the host before an identity ships at warn.** The issue gives fafnir's operator
  one aggregate-only SQL per identity, to post its pass rate on current versions:
  - an identity that holds on 100% of rows holds by construction, and is dropped;
  - one that holds on less than 99% is investigated before it ships.
- **Release v0.11.0**: DR-0901 to DR-0905, with release notes.

#### Acceptance criteria

- [ ] On dorq-synth: DQ903 precision of at least 0.9 at warn on the injected field errors, and
  the field named correctly in at least 90% of them.
- [ ] On dorq-synth: no warn on NCI, temporary equity, restricted cash or reclassification.
- [ ] The quarter-sum tie names the wrong quarter in a year with one bad quarter.
- [ ] The rtrimble13/fafnir#217 shape gives no report and no duplicate key.
- [ ] Each identity's pass rate on the host is posted on this issue (aggregates only), and the
  shipped list follows from it.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ903.md`, `doc/checks/DQ904.md`; `doc/statements.md` (identities)
- [ ] `README.md`; the plan's status; the release

---

## [DR-0906] The statement value model: DQ911 bad-value, DQ912 unit-error, DQ913 sign-error, DQ919 large-change

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0906` · **Size:** L · **Release:** v0.12.0 · **Depends on:** DR-0905 · **Plan:** §4.1 · **Tracking:** DR-0900

#### Context

The faults commonest in statement data have no check in fafnir: a value or a whole row in the
wrong units, one bad value, a flipped sign. This item is the statement analogue of the DQ2xx
model. It compares explanations for each suspicious value and reports `p_error`.

#### Scope

- **Features.**
  - The `asinh(v / s)` scale, with `s` the median of `|v|` over the series.
  - A prediction from the adjacent periods, and from the same quarter a year either side.
  - A discounted robust scale per item, and a class prior by item class and size.
  - Both views, in force and as first reported (§2.2), with one report per shared value.
- **Screening**, as plan §4.1 lists it.
- **Hypotheses:** `real_change`, `bad_value`, `unit_error` (one value, a whole row, or an era),
  `sign_error`. Their evidence terms carry log Bayes factors, as DQ2xx's do, and include:
  - **identity repair:** the period's DQ903 identities hold once the candidate is repaired;
  - **quarter-sum repair**, from DQ904.
- **Codes:**
  - DQ911 `bad-value`;
  - DQ912 `unit-error`. An era is reported once, as DQ202's is;
  - DQ913 `sign-error`;
  - DQ919 `large-change`, at info.
- **The newest period is provisional**, capped at warn.
- **A ×1,000 in version 1** that version 2 fixes is reported once, on version 1.
- **Suggested actions** name the item, the period and the filing to check. Repairs are SF1's:
  report to Sharadar.
- **`[statements.priors]`**, the defaults that DR-0911 calibrates.

#### Acceptance criteria

- [ ] On dorq-synth, at warn:
  - DQ912 precision of at least 0.95 and recall of at least 0.9 (a whole row moving by one
    factor is decisive);
  - DQ911 precision of at least 0.85;
  - DQ913 precision of at least 0.9.

  The gate holds them.
- [ ] No warn on acquisitions, write-downs, seasonal businesses or pre-revenue companies.
- [ ] The run on 6M synthetic rows is timed and recorded in doc/performance.md.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ911.md`, `DQ912.md`, `DQ913.md`, `DQ919.md`; `doc/statements.md` (the model)
- [ ] `README.md`; the plan's status

---

## [DR-0907] Quarterly structure: DQ914 copied-period, DQ915 absent-as-zero, DQ916 cumulative-quarters

Repo: rtrimble13/dorq · Labels: fundamentals, size:M

**Key:** `DR-0907` · **Size:** M · **Release:** v0.12.0 · **Depends on:** DR-0906 · **Plan:** §3, §4.1 · **Tracking:** DR-0900

#### Context

SF1 writes 0 for an item a filing does not carry (fafnir's backfill.md, probe item 6).
`fundamentals_suspicious_zero` therefore flags absent values and real zeros alike, and
`fundamentals_ytd_suspect` uses one ratio, 1.5 years. Each adds hypotheses to DR-0906's model.

#### Scope

- **DQ914 `copied-period`.** A row equal to the previous period's to the cent on
  `copy_min_items` items, flows included, against the chance of that for this series.
- **DQ915 `absent-as-zero`.** A 0 between reported values, with a prior by item: items in
  `never_absent` against items that are often absent, such as R&D. Real zeros, such as a
  pre-revenue company's revenue, are filing facts.
- **DQ916 `cumulative-quarters`.** `cumulative_quarter` (quarter *k* equals the sum of 1 to
  *k*; Q4 equals the year) and `annual_in_quarter`, with the quarter-sum tie as evidence.
- These succeed `fundamentals_suspicious_zero` and `fundamentals_ytd_suspect`.

#### Acceptance criteria

- [ ] On dorq-synth, precision of at least 0.9 at warn for each code, and the gate holds it.
- [ ] No warn on real zeros or banks' NULLs.
- [ ] A year with two annual periods in force is not judged.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ914.md`, `DQ915.md`, `DQ916.md`
- [ ] `README.md`; the plan's status

---

## [DR-0908] Share counts and share terms: DQ921 share-terms-step, DQ922 share-count-mismatch; v0.12.0

Repo: rtrimble13/dorq · Labels: fundamentals, size:M

**Key:** `DR-0908` · **Size:** M · **Release:** v0.12.0 · **Depends on:** DR-0906 · **Plan:** §4.3 · **Tracking:** DR-0900

#### Context

fafnir stores a share count in the terms of its `known_from` (ADR 0018 §4). The panel converts
it with the warehouse's splits (ADR 0019 §3). ADR 0018 records that "a split missing from
Sharadar's ACTIONS misstates every earlier period's share counts". The error reaches every
market cap and per-share factor, plausibly and silently, and nothing checks for it. dorq already
has the split-ratio mixture (DQ203) and reads the actions.

#### Scope

- **DQ921 `share-terms-step`.** Share counts are converted into the latest version's terms with
  the `--actions` splits. A clean-ratio step between two periods is weighed as
  `unreported_split` (a context gap), `terms_error`, `unit_error` or `real_issuance` (plan §4.3).
  - With `close`, the implied market cap tells them apart: it stays level under an unreported
    split, and steps under a terms error.
  - The suggested action is `add_split` with the ratio and the window, or `check_share_terms`.
- **DQ922 `share-count-mismatch`.** A period's weighted-average and outstanding shares disagree
  by a clean ratio or a power of ten. A split on file between the period's start and the filing
  date explains a ratio equal to that split.
- The split-ratio mixture is shared with the price model, not copied.
- **Release v0.12.0**: DR-0906 to DR-0908.

#### Acceptance criteria

- [ ] On dorq-synth:
  - a split missing from the actions is DQ921 `context_gap`, with the right ratio and window;
  - a terms error is `data_error`;
  - offerings, mergers and splits on file give no warn.
- [ ] The gate holds DQ921 and DQ922.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ921.md`, `DQ922.md`; `doc/context.md` (`--actions` with statements)
- [ ] `README.md`; the plan's status; the release

---

## [DR-0909] The knowledge timeline and revisions: DQ931, DQ932, DQ951

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0909` · **Size:** L · **Release:** v0.13.0 · **Depends on:** DR-0906 · **Plan:** §4.4, §4.6 · **Tracking:** DR-0900

#### Context

A filing date that is too early lets every backtest know the figures too soon: the look-ahead
fafnir's constraints exist to prevent. `fundamentals_implausible_filing_lag` uses one cutoff of
5 days. Restatements are read by eye from one flag per security and run, and a load fault that
breaks a hundred companies looks like a hundred flags.

#### Scope

- **DQ931 `filing-date-implausible`.** Lags are judged against the issuer's own lags, by period
  type, and against the SEC's deadlines.
  - The hypotheses are `ordinary`, `late_filing`, `misdated_early` and `misdated_amendment`.
  - A period known after its successor is evidence.
  - A `misdated_early` can reach error. A late date is warn at most, since it only makes a
    backtest conservative.
- **DQ932 `restatement-implausible`.** Version *n* is weighed against version *n−1* as
  `restatement`, `new_version_error` or `old_version_error`.
  - The evidence is a power of ten, a sign, two fields swapped, or a total moving while its
    components hold.
  - An `old_version_error` is reported on the old version, the one point-in-time reads in its
    interval saw.
- **DQ951 `revision-cohort`.** It is judged at the end of the run, as DQ601 is.
  - Revisions by `known_from` (observed versions), and findings by `loaded_on`, are tested
    against the base rate (Poisson tail of 1e-4 or less).
  - Members' DQ932 and DQ903 reports are downgraded and linked through `detail.members`.
  - A cohort of revisions alone, such as a backfill, is info. A cohort that carries findings is
    a load fault, at warn.

#### Acceptance criteria

- [ ] On dorq-synth:
  - a filing date at the period end is DQ931 at error;
  - late filers are info at most;
  - a ×1,000 restatement is DQ932 `new_version_error`;
  - a corrected bad version is reported on the old version;
  - reclassifying restatements give no warn.
- [ ] The synthetic backfill day is one DQ951 at info, with every member linked and no member
  at warn for an ordinary restatement.
- [ ] The gate holds each code.
- [ ] The bars golden test still passes.

#### Docs move with the code

- [ ] `doc/checks/DQ931.md`, `DQ932.md`, `DQ951.md`
- [ ] `README.md`; the plan's status

---

## [DR-0910] Coverage and freshness: DQ941 to DQ944, the new `--meta` fields; v0.13.0

Repo: rtrimble13/dorq · Labels: fundamentals, size:L

**Key:** `DR-0910` · **Size:** L · **Release:** v0.13.0 · **Depends on:** DR-0902 · **Plan:** §4.5 · **Tracking:** DR-0900

#### Context

These are the successors of `fundamentals_gap`, `fundamentals_ttm_incomplete`,
`fundamentals_stale`, `fundamentals_coverage` and `fundamentals_currency_mismatch`. The SQL
checks use fixed spans (400 days, 200 days) where the issuer's own cadence is the better guide.
They also count a change of fiscal year as a gap.

#### Scope

- **`--meta` gains these fields:** `currency`, `first_trade_date`, `last_trade_date`, `active`,
  `delisted_date`, `sector`, `industry` and `statements_expected`. fafnir's column names are
  accepted. Profiles can match on `sector`, for example to drop identities banks do not carry.
- **Series known only from the meta.** After the input, the engine visits each meta series with
  no rows, in meta-file order, so the output stays deterministic.
- **DQ941 `period-gap`** weighs `missing_period`, `fiscal_year_change`, `mislabel` and
  `not_yet_filed`, while the security traded and from the coverage floor on.
- **DQ942 `statements-stale`** compares the as-of date with the expected next period end plus
  the issuer's usual lag. It is keyed on the latest `fiscal_date`, and reported while it holds.
- **DQ943 `statements-missing`**: `statements_expected` with no rows in the input.
- **DQ944 `reporting-currency`**, at info.
- **Release v0.13.0**: DR-0909 and DR-0910.

#### Acceptance criteria

- [ ] On dorq-synth:
  - a missing quarter is DQ941;
  - a change of fiscal year is info at most;
  - a mislabelled quarter is told apart from a missing one;
  - a delisted series is not stale, and a delinquent filer is.
- [ ] The gate holds each code.
- [ ] The bars golden test still passes, and a bars run with the new meta fields gives the same
  output as without them.

#### Docs move with the code

- [ ] `doc/checks/DQ941.md` to `DQ944.md`; `doc/context.md` (the meta fields)
- [ ] `README.md`; the plan's status; the release

---

## [DR-0911] Labels, calibration and evaluation for statements; a fit on fafnir's labels; v0.14.0

Repo: rtrimble13/dorq · Labels: fundamentals, size:M

**Key:** `DR-0911` · **Size:** M · **Release:** v0.14.0 · **Depends on:** DR-0906 to DR-0910, DR-0913 · **Plan:** §5.7, §6 · **Tracking:** DR-0900

#### Context

`dorq calibrate` fits the price model's priors and its `p_error` map to fafnir's decisions
(development plan §6). Statements need the same. In fafnir their natural labels are vendor
corrections: SF1 fixes its record, and fafnir keeps the superseded row (rtrimble13/fafnir
ADR 0018 §3).

#### Scope

- **`--restore` for statements.** Superseded rows are upserted by key, so dorq sees what fafnir
  held before a correction, as it does for repaired bars.
- **`dorq calibrate` on statement labels.** Each labelled candidate becomes an observation, with
  its per-hypothesis log-likelihood. The fit covers the value model's, the identity model's and
  the timeline model's priors, and an isotonic `p_error` map per sub-family. The output is
  `[statements.priors]` and `[statements.calibration]`.
- **dorq-eval** reports precision, recall, the reliability diagram and precision@k for
  statement codes.
- **The fit on fafnir's labels** (DR-0913). Its holdout figures are recorded here, and the
  result goes into `priors/fafnir-statements.toml` through `include`.
- **Release v0.14.0.**

#### Acceptance criteria

- [ ] A fit on dorq-synth seed 1, applied to seed 2, raises precision at warn, and every gate
  still holds.
- [ ] Expected calibration error is 0.05 or less on dorq-synth.
- [ ] The host fit's holdout precision, at warn and at error, is posted on this issue
  (aggregates only).

#### Docs move with the code

- [ ] `doc/calibration.md`, `doc/labels.md`, `tools/eval/README.md`
- [ ] The plan's status; the release

---

## [DR-0912] dorq for statements: export, run, ingest, shadow, compare, recheck

Repo: rtrimble13/fafnir · Labels: type:feature, area:dq, area:fundamentals, owner:claude-code, size:L

**Key:** `DR-0912` · **Size:** L · **Depends on:** rtrimble13/dorq DR-0902 (v0.11.0) · **Plan:** dorq `doc/plans/dorq-fundamentals-plan.md` §5 · **Tracking:** rtrimble13/dorq DR-0900

#### Context

dorq v0.11.0 reads statements (DQ901 to DQ904). fafnir runs dorq on bars today (ADR 0013,
doc/dorq.md). This item runs it on the three statement tables beside the bars run, in shadow
first, without touching the bars run or the price shadow period.

#### Scope

- **The export** (`src/fafnir/dq/dorq.py`):
  - one `COPY` of the current versions of the three tables, ordered by `security_id`,
    statement, period type, fiscal date and version;
  - `close` from a lateral join on `core.daily_price`, on or before `known_from`;
  - `ref.statement_line_item` as `--items`, and `core.corporate_action` as `--actions`;
  - meta with the new fields: currency, trading span, active and delisted, sector, and
    `statements_expected` (the vendor master's `first_quarter`).
- **The run.** A second `dorq check --kind statements` with `[dq] dorq_statements_config`. New
  `fafnirrc` keys: `dorq_statements` (default false), `dorq_statements_config`, and
  `dorq_statements_shadow` (default true).
  - The default is a full run each night if it stays under 2 minutes, measured on the host.
    Otherwise the run takes the securities with a row loaded since the previous run.
  - It is recorded in `ops.ingestion_run` under its own endpoint, with its own watermark.
- **Ingest** as for bars, with `table_name` taken from each row.
- **Shadow.** Rows go to `<dorq_shadow_dir>/statements/<as-of>.jsonl`, never the shadow
  directory's top level, which `dq compare` reads as the newest bars night.
- **`dq compare --statements`.** Successors for the thirteen SQL fundamentals checks (dorq plan
  §3), matched by security, period and statement, with precision against labels as for bars.
- **Recheck.** Rerun and negate for `dorq_*` statement checks. DQ951 is excluded, and
  `dorq_statement_key` goes in `NEVER_AUTO_RESOLVE`.
- **`fafnir status`** gets a `dorq statements` line.

#### Acceptance criteria

- [ ] Integration tests on synthetic statements, with no SF1 row committed:
  - a statements night in shadow writes its file, and `dq compare` on bars is unchanged;
  - ingest writes with the open and accepted guards;
  - recheck closes a fixed condition and leaves `dorq_statement_key` alone.
- [ ] The bars `dq run` is byte-identical with `dorq_statements` on and off.
- [ ] The host's full statements run time is posted on this issue.

#### Documentation

- [ ] `doc/dorq.md` (a statements section), an ADR 0013 status note, `doc/operations.md`
- [ ] `.claude/skills/fafnir-dba/references/automations.md`

---

## [DR-0913] Fundamentals labels for dorq; playbooks, tiers and NEVER_AUTO_RESOLVE for the statement codes

Repo: rtrimble13/fafnir · Labels: type:feature, area:dq, area:fundamentals, owner:claude-code, size:M

**Key:** `DR-0913` · **Size:** M · **Depends on:** DR-0912 · **Plan:** dorq plan §5.7 · **Tracking:** rtrimble13/dorq DR-0900

#### Context

`dq export-labels` writes the warehouse's decisions about bars as dorq labels. Statements need
the same, from what the warehouse actually decides about them. The warehouse never edits a
statement value: the outcomes are "accept, the filing shows it" and "resolve, the vendor fixed
it" (the fundamentals playbook). Vendor corrections are the repairs.

#### Scope

- **`dq export-labels` for statements.**
  - A vendor correction that moved a value beyond tolerance is a `data_error` label on the
    superseded row. Its `expect` follows from the move: a power of ten is DQ912, a sign DQ913,
    a filing date DQ931, and anything else any DQ9 code.
  - `--restore` writes the superseded rows in the statements input shape.
  - An accepted fundamentals flag, SQL or dorq, is a filing fact on its code's successor. A
    flag resolved because the vendor fixed the value is a data error.
- **`dq label add`** gains the statement keys (`--period-type`, `--fiscal-date`,
  `--version-no`, `--statement`, `--item`) and kinds: `unit_error`, `sign_error`,
  `misdated_filing`, `structural_gap`, `fiscal_year_change`, `real_zero`.
- **The fafnir-dba skill:**
  - a `dq-playbooks.md` entry for each `dorq_*` statement code: how to read its evidence, and
    which outcome it maps to;
  - `sweep-policy.md` tiers;
  - `dorq_statement_key` in `NEVER_AUTO_RESOLVE`;
  - the existing test that pins the tiers and `NEVER_AUTO_RESOLVE` together, extended.

#### Acceptance criteria

- [ ] Unit and integration tests on synthetic corrections and closures.
- [ ] The labels are read by dorq's label reader and dorq-eval: a synthetic round trip.
- [ ] `--verified-only` and `applies_until` behave as they do for bar labels.

#### Documentation

- [ ] `doc/dorq.md` (Labels), the DBA skill references

---

## [DR-0914] Statements shadow period, go/no-go and cutover: the SQL fundamentals checks retire

Repo: rtrimble13/fafnir · Labels: type:ops, area:dq, area:fundamentals, owner:operator, size:M

**Key:** `DR-0914` · **Size:** M · **Owner:** operator, with code by Claude Code · **Depends on:** rtrimble13/dorq DR-0911, DR-0912, DR-0913 · **Plan:** dorq plan §6 · **Tracking:** rtrimble13/dorq DR-0900

#### Context

dorq's statement checks take over from the SQL checks only on evidence, as ADR 0013 requires
for bars. SF1's nightly delta is about 1,100 rows over four days, so the nights alone are too
few to judge by. The judgement is made on a full-history run, sampled and verified against
EDGAR (dorq plan §11, F5).

#### Steps

1. Shadow at least 20 nightly sessions with `dorq_statements = true`, after dorq v0.11.0. A
   later release restarts the count only if it changes a statement check's output.
2. Run the full statements history once.
3. Verify a stratified sample on EDGAR: at least 200 reports, at least 10 per code. State each
   verdict with `dq label add --verified`.
4. Judge against dorq plan §6: precision, recall against the SQL checks, queue volume,
   calibration and runtime. Post the aggregates here.
5. **Go:** set `dorq_statements_shadow = false`.
6. **Cutover code**, by Claude Code:
   - remove the thirteen from `FUNDAMENTALS_CHECKS`, keeping `run_all`'s keys at 0 for one
     release;
   - migrate their open flags by recheck, not by bulk resolve;
   - move `fundamentals_unmapped_fields` into the loader;
   - drop the nightly `fundamentals_restatement` flag to info.

#### Acceptance criteria

- [ ] The figures for each §6 criterion are posted here (aggregates only, with no vendor
  values).
- [ ] The go/no-go is recorded, with the reason.
- [ ] On go: the SQL checks are retired, open flags migrated, and the docs updated in the same
  PR.

#### Definition of done

- [ ] `doc/dorq.md`, `doc/operations.md`, the DBA playbooks, an ADR 0013 status note
- [ ] dorq's plan status for DR-0914, and the M9 tracking issue closed
