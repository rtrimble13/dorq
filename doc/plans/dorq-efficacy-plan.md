# Plan: dorq efficacy (DE)

- Status: **accepted** (2026-11-23), not started. Tracked in
  [#40](https://github.com/rtrimble13/dorq/issues/40). The caller's side, in `rtrimble13/fafnir`, is
  `doc/plans/dorq-efficacy.md` there, tracked in fafnir#267.
- Builds on: the [development plan](dorq-development-plan.md) and the
  [fundamentals plan](dorq-fundamentals-plan.md). Their principles (§1.3), engine (§5),
  calibration (§6) and cutover criteria (§8) hold unchanged, except where §2 below states
  the bar more strictly.
- Scope: this repository. The integration and measurement items are fafnir's, keyed
  DE-03..DE-05, DE-15, DE-16, DE-24..DE-26 and DE-40.
- Source: a review of v0.14.0 (commit e013694) against fafnir v3.2.0's SQL checks and 15
  shadow nights, 2026-11-06 to 11-20. File:line references are to v0.14.0. **[I]** marks
  an inference not yet verified on data.
- Status legend: ⬜ planned · 🔄 in progress · ✅ done (PR #) · ⏭ carried over · ✖ dropped
  (reason)

---

## 1. Why

**dorq earns its place only by beating the threshold checks it replaces, on both
precision and recall**, by a margin a test can see. Its first user, fafnir, runs it in
shadow beside its SQL checks. The review found it not yet better there, for reasons that
are general to the design rather than particular to fafnir's data.

**This plan must keep dorq general purpose.** Every change below either:
- is a better model of something every financial dataset has: newest observations with
  no future yet, many series from one feed, a vendor's standardization, a history
  stitched from several sources;
- or adds an **optional, vendor-neutral input**.

Nothing reads a fafnir table, names a fafnir vendor, or hard-codes a fafnir default. What
fafnir needs beyond that is fafnir configuration (`/etc/fafnir`, profiles, priors files)
or fafnir code.

### 1.1 Measured, 2026-11-23 (fafnir's shadow, dorq 0.14.0)

| | dorq | fafnir SQL |
|---|---|---|
| Bars, precision on verified labels (5 nights; 106 of 225 reports judged) | warn+ 0.67, error 0.98. DQ301 1.00, DQ304 0.98. Every judged DQ201, DQ203, DQ107 and DQ705 was false | n/a |
| Bars, recall on confirmed errors | 8/37 (11-13); 0/9 (11-19, 11-20) | 37/37; 6-7/9 |
| Bars, warn+ reports over 15 nights | 924. Stale 293, DQ403 167, DQ301 134, DQ201 96 (62 provisional), DQ705 89, DQ107 64, DQ203 30 (9 provisional) | |
| Statements, standing conditions at warn+ | 225,995 (104,365 after the caller withholds conditions it accepted) | 213 |
| Statements, precision on ~200 EDGAR-verified conditions | ~0.40 at warn+ (DQ941 0.00, DQ904 0.21) | n/a |
| Statements, recall on confirmed errors | 83/95, partly sampled from dorq's own reports; DQ916 17/821 on quarters stored year to date | 31/95; 821/821 by construction |

The judged sets are small and selected by one engine or the other, so neither engine's
superiority can yet be shown (§3, E).

## 2. The bar

Per domain (bars, statements), on an audit sample that neither engine selected, weighted
by its inclusion probabilities (DE-01, fafnir DE-03):

- precision at warn and above ≥ 0.85, and at error ≥ 0.95;
- recall on confirmed errors ≥ the incumbent's, at one-sided 95% (a paired test), on at
  least about 100 confirmed errors;
- queue volume ≤ 30% of the incumbent's;
- expected calibration error ≤ 0.05.

## 3. What the review found

### 3.1 Four mechanisms

**M1. Hypotheses the data cannot yet decide are reported at warn.**
- **Newest bars.** A bad-print block of k bars is not penalised until bar k exists:
  `next_bars` scores an unreverted block as "nothing to score"
  (`src/price/model.cpp` ~761). With `kBlockDecay = 0.5` (model.cpp:47) and
  `revert_max_bars` 5, one bar that holds the new level costs a bad print only about 2×.
- **Why size alone wins.** The return term pits a Student-t market move (`jump_prob`
  0.01, `jump_scale` 6) against a broad Cauchy error (model.cpp:699-702; `error_scale`
  `src/price/features.cpp:204`). On a low-volatility series a ×2 move favours an error
  by size alone, until the bars after it say otherwise.
- **Example.** On 2026-11-19, 13 leveraged ETFs of one sponsor split with no split row
  on file. dorq reported 12 as DQ201 bad prints (p 0.85-0.92), still so with a
  non-reverting bar after; one showed as DQ203. The right report was one family split
  and DQ203s. DQ201's suggested repair deletes a correct bar.
- **Statements, DQ903.** A break that is neither persistent within ±2 periods nor matched
  to a term is "unexplained", and "unexplained" counts as error.
  - From the default log priors (structural −1.4, field −1.05, unexplained −1.05;
    `src/checks/identities.cpp:349-383`), that gives p 0.912, which is error, with no
    positive evidence that a stored value differs from its source.
  - DQ903 is 18% of statements warn+.
- **Statements, DQ904.** The tie checks have no model. A broken cash tie is p 0.85 and a
  cancelling pair 0.95 (identities.cpp:586, 603, `tie_finding` 551-558). That is 43% of
  statements warn+.
- **Statements, DQ941 and DQ902.** DQ941 is a fixed rule: a gap over 126 days is warn at
  0.85 (`src/checks/sequence.cpp:155-175, 254-322`). DQ902 is deterministic at p 1.

**M2. Cross-sectional evidence flows one way.**
- **DQ303's "expected" series.** It counts a series only above `confident_density` 0.95
  (`src/engine/cross_section.cpp:510`).
- **Members can only go down.** A cohort downgrades its members' DQ301 to info and
  never raises them.
- **So a vendor's delivery gap across thin series is invisible.** These are series with
  a bar on 84-100% of sessions and about 100 shares a day. Each member's own density
  explains a day off. This is most of the bar recall gap.
- **DQ601 skips provisional moves.** The engine leaves them out ("nor a newest bar whose
  fate is open", `src/engine/engine.cpp` ~304-310). A family's split is clearest on its
  first night.
- **The sibling evidence comes after the fact.** It is applied once the series has been
  judged (`cross_section.cpp` `sibling_evidence`), and only with a `peer_group`.

**M3. Persistence is judged too locally.**
- **DQ903** learns structure from at most 4 in-force neighbours and a 25% band
  (identities.cpp:287-325). Its structural prior is one global constant.
- **No per-series break rate.** No identity, tie or period-type in the statements has a
  break rate or gap distribution of its own.
- **One prior set for every price series.** Calibration fits one global set
  (doc/calibration.md, "What it fits"), not the class volatility table (development
  plan §4.1) or priors per profile. That one set must cover leveraged ETFs, micro caps,
  closed-end funds and large caps.
- **The one fit on record got worse held out:** log-likelihood −0.652 → −0.683.

**M4. Fixed heuristics that calibration cannot reach.**
- **DQ403** (`src/checks/volume.cpp:388-466`) uses `kErrorPrior = 0.01` against the
  series' own rate with a 0.5 pseudo-count.
  - So a series' first zero-volume move is p ≈ 0.78, which is warn, whatever its size.
  - Seen: shell companies at $10 moving half a cent on zero volume, which reads as an
    indicative quote.
- **DQ107 and DQ705** are heuristics of the same kind. With DQ403 they are 35% of the
  bars' warn+, and none has been judged.
- **On statements**, the fixed-p rules above cannot be mapped, and DQ903 and DQ904 share
  one map.
- **`verified` is parsed but unused** (`src/calibrate/labels.cpp:245-250`; absent from
  `src/calibrate/statements.cpp`). Labels a caller closed by rule weigh as much as labels
  checked against a source.

### 3.2 Other findings

- **No notion of a source or a vintage on bars.**
  - A level shift where a history changes vendor is judged like any other.
  - A batch of rows loaded on one day in one pattern is invisible: for example, late
    fills written as zero-volume copies of the prior close on sessions that traded.
  - DQ951 does this for statements, from `known_from` and `loaded_on`.
- **DQ932 tests a restatement against neighbours that were never restated.** A vendor
  that keeps only the latest restatement leaves a v2 beside unrestated neighbours. It
  fails at the 0.02 spread floor (`src/checks/timeline.cpp:497-597`).
- **DQ921 and DQ922 have thin real-change tails.** The mixture is 0.95·N(0,sd) +
  0.05·N(0,0.35) (`src/statements/shares.cpp:433-434`). A real 3× or 10× share change
  (merger, de-SPAC) reads as a split. The clean-ratio windows (±0.16) barely
  discriminate. DQ922 pairs basic and diluted counts separately.
- **One report per version, per identity, per share type**, rather than one per
  condition.
- **DQ916's year-to-date test is strict.** It needs exactly 4 quarters and 1 year in
  force, and at least 3 flows fitting a clean year-to-date pattern. [I] Partial or mixed
  storage escapes it (17 of 821 found).

## 4. Work items

Keys follow the caller's plan, so one key means one item in both repositories.

| Phase | Key | Item | Size | Status |
|---|---|---|---|---|
| 0 Measurement | [DE-01](https://github.com/rtrimble13/dorq/issues/41) | Survey-weighted evaluation: labels carry an optional `weight` (1/inclusion probability) and `sample` id; dorq-eval and `calibrate` use them; dorq-eval compares two engines on one label set with a paired test and intervals | M | merged: [#56](https://github.com/rtrimble13/dorq/pull/56) |
| | [DE-02](https://github.com/rtrimble13/dorq/issues/42) | Calibration honours `verified` (weight or exclude, configurable); one p_error map per code; the fixed-p rules emit a raw score the map can take | M | in review: [#57](https://github.com/rtrimble13/dorq/pull/57) |
| 1 Precision | [DE-11](https://github.com/rtrimble13/dorq/issues/43) | A series-level structural model for identities and ties (DQ903, DQ904): per (series, identity or tie, period type) a break rate shrunk to the cross-section, and a gap distribution; "unexplained" no longer counts as error by default (`[statements] unexplained_is_error`); DQ904 gets a posterior; chronic breaks become one series-level report | L | ⬜ |
| | [DE-12](https://github.com/rtrimble13/dorq/issues/44) | One condition, one report: fold versions with the same gap or value, identities broken by one field, and basic and diluted counts. Record keys change; a minor version | M | ⬜ |
| | [DE-13](https://github.com/rtrimble13/dorq/issues/45) | Provisional means pending: with the evidence after a bar missing, report at info unless the posterior without those terms reaches the error band and an independent term agrees (OHLC inconsistency, zero volume on a liquid series, an implausible level, peers). `kBlockDecay` and `revert_max_bars` become calibratable | M | ⬜ |
| | [DE-14](https://github.com/rtrimble13/dorq/issues/46) | DQ403, DQ107 and DQ705 become models (pooled rates across class, move size against the grid, an indicative-quote hypothesis) or default to info; their priors move to config | M | ⬜ |
| 2 Recall | [DE-21](https://github.com/rtrimble13/dorq/issues/47) | Cross-sectional evidence in both directions: (a) a latent "load failed on date d for group g" from every series' own miss probability, thin ones included, with group keys configurable from `--meta` (`[coverage] cohort_by`), feeding each member's DQ301 posterior both ways; (b) DQ601 takes provisional moves; (c) the sibling term enters the posterior before severity | L | ⬜ |
| | [DE-22](https://github.com/rtrimble13/dorq/issues/48) | Optional per-bar `source` and `known_from`/`loaded_on` columns: a source-seam evidence term on the scale, segment and bad-print hypotheses (as `precision_term`, model.cpp:1017); a load cohort for bars, as DQ951's | L | ⬜ |
| | [DE-23](https://github.com/rtrimble13/dorq/issues/49) | DQ916's recall on quarters stored year to date: partial storage, one quarter at a time, mixed vintages | M | ⬜ |
| 3 Depth | [DE-31](https://github.com/rtrimble13/dorq/issues/50) | Compare like vintages: judge a version against the view as known at its `known_from` (DQ932); a tie or a quarter sum whose rows mix vintages is reported only if it fails in a single-vintage view (DQ904, DQ903 neighbours) | M | ⬜ |
| | [DE-32](https://github.com/rtrimble13/dorq/issues/51) | A class-conditional price model: calibrate fits the class volatility table, the jump parameters and the priors per profile, with partial pooling; an optional `leverage` field in `--meta` | L | ⬜ |
| | [DE-33](https://github.com/rtrimble13/dorq/issues/52) | Share counts: a heavier real-change tail (t, or log-uniform to log 100); the clean-ratio test within measurement noise (~0.02); DQ922 once per period; a DQ931 lag floor of 7-10 days with a t tail | M | ⬜ |
| | [DE-34](https://github.com/rtrimble13/dorq/issues/53) | DQ941 learns the issuer's cadence; a quarterly hole inside annual coverage is info; a vendor-hole cohort across series; calibratable | M | ⬜ |
| | [DE-35](https://github.com/rtrimble13/dorq/issues/54) | Every check's priors and constants in config and calibratable (one `calibrate` across families); `[statements]` settings by profile | M | ⬜ |

Order: Phase 0 first. After each phase, the caller runs an audit cycle and the bar (§2)
is re-measured. The next phase starts from that evidence, not from this table.

## 5. Generality rules for this plan

1. **New inputs are optional and vendor-neutral.** Name them for what they are
   (`source`, `known_from`, `peer_group`, `leverage`), not for who supplies them. dorq
   runs on a bare CSV as before.
2. **New behaviour has a setting**, and the default is right for a general user. A caller
   that wants otherwise sets it in its own config.
3. **No default is tuned on one user's data without a stated reason.** The reason must
   hold for any financial dataset. A fit on one user's labels is that user's priors file.
4. **Statelessness and determinism hold** (ADR 0001). Nothing here adds state between
   runs; "pending" and "persistent" are judged from the input alone.
5. **Each item ships with dorq-synth cases** for the fault and its hard negatives, and
   the synthetic gate keeps passing.

## 6. For agents working an item

- **Read before starting:** this file, the item's issue, and the caller's plan for the
  measured context.
- **The definition of done** is the issue's acceptance criteria, plus the synthetic gate,
  plus the docs (CONTRIBUTING.md, "Docs move with the code").
- **If an item only makes sense for fafnir, stop.** It belongs in fafnir, or in a
  configuration example, not in dorq.
- **Report the effect on the audit,** not only on synthetic data. The caller's audit is
  the evidence the bar is judged on.
