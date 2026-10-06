# Plan: dorq, a Bayesian data-quality linter for financial time series

- Status: **accepted** (2026-10-05). The defaults in §10 are confirmed, and Q9 is answered. M0 is done
  (PR #1), M1 is done (PR #2), M2 is done (PR #3), M3 is done (PR #4), M4 is done (PR #5), and M5
  is implemented and awaiting merge.
- Scope: this repository (the C++ CLI), plus one milestone of integration work in `rtrimble13/fafnir` (M7).
- Status legend: ⬜ planned · 🔄 in progress · ✅ done (PR #) · ⏭ carried over · ✖ dropped (reason)

---

## 1. What dorq is for

fafnir's DQ layer already *detects* well. What it lacks is a way to tell which detections matter. The
checks in `src/fafnir/dq/checks.py` are fixed thresholds: a 50% close-to-close move, 80% session
density, two sessions behind. They cannot tell a data error from a real market event. That judgement
is done by hand today, from playbooks. The playbooks are good (`.claude/skills/fafnir-dba/references/
outlier-classification.md` is effectively a hand-written classifier), and they are expensive to apply.
The record so far:

- The per-session `gap` check wrote **599,808** rows for ~2,900 thinly-traded securities and buried the
  ~29 that were genuinely broken. The fix was a hard cutoff (`GAP_MIN_SESSION_DENSITY = 0.80`), and the
  code documents why that constant is not safe to tune.
- One triage session took **300** open `outlier` flags down to 166. Most of the 134 it closed were
  *accepted* as market facts or explained splits. They were not repairs.
- `stale` produced ~300 flags a night from vendor publishing lag until `STALE_MIN_SESSIONS_BEHIND` was
  raised to 2.
- The outlier check skips a split only when `ex_date = trade_date`. Any split dated on a weekend or
  holiday is therefore flagged every night, even though the adjusted series is correct.

**dorq's job is to put the playbook's reasoning into a model.** For each suspicious observation it
returns a *calibrated probability that the observation is a data error*, the competing explanations
with their probabilities, the evidence behind them, and a suggested repair. It works the way `flake8`
does: data goes in, a set of built-in checks runs, violations come out, and the exit code reflects the
result. It never modifies data.

### 1.1 What counts as a true positive

The definition matters, and it is where the current checks fall short. A 60% drop on a failed drug
trial is anomalous and correct. dorq sorts every candidate into one of three outcomes:

| Class | Meaning | fafnir disposition today | dorq default |
|---|---|---|---|
| **Data error** | The stored value is wrong: a bad print, the wrong scale, the wrong date, a missing split, a feed outage | repair (`prices delete/shift/rescale`, `actions add/redate`, re-ingest) | **reported** (`warn`/`error`) |
| **Market fact** | Anomalous and correct: an earnings gap, a crash day, a one-tick move on a sub-dime stock, a thin name that doesn't trade | `dq accept` | suppressed; shown with `--show-info` |
| **Context gap** | The data is right, but the context needed to explain it is missing (a split the vendor never reported, a merger exchange ratio) | `actions add` | reported, with the missing context named |

A "true positive" means a *data error* or *context gap* that dorq reports at `warn` or above. Precision
is measured against that definition (§8).

### 1.2 Evaluation of the proposed features

| Proposal | Assessment |
|---|---|
| C++ CLI in the style of black/flake8 | **Good fit.** One static binary, and it is fast enough to scan fafnir's ~150M bars in minutes. One correction: black *rewrites* its input and flake8 only *reports*. dorq should behave like flake8 and emit a `suggested_action` rather than a fix. Repairs stay in fafnir, where `ops.operator_override` records them. |
| Works out of the box and is configurable | **Good fit.** Defaults come from fafnir's own experience. Config uses `dorq.toml` or `[tool.dorq]` in `pyproject.toml`, the same discovery black uses. |
| OHLCV bars and single-value series | **Good fit.** One engine with two schemas. A single-value series is a bar with only a close, plus an optional transform (log or difference) for series that can be ≤ 0, such as interest rates. |
| Sparse data check | **Strong candidate for Bayesian treatment.** A missing session should be judged against *how often this security usually trades*. The current check applies one cutoff to every security. See §4.2. |
| Unusual price action check | **Should change what it asks.** The question is not "is this move unusual?" but "which explanation fits best: a bad print, a scale shift, an unreported split, or a real move?" See §4.3. |
| Bayesian methods | **Recommended as the core.** There are few labels, the outputs need to be calibrated probabilities with explanations, domain knowledge from the playbooks is naturally expressed as priors, and pooling across securities handles short histories. Use closed-form and online inference, not MCMC (§5). |
| Output to the command line, to fafnir's DQ table, and as JSON | **Good fit.** Output formats are `text`, `json`, `jsonl`, `csv`, and `fafnir`. The `fafnir` format maps one-to-one onto `ops.data_quality_flag` columns. |

### 1.3 Design principles

1. **Stateless and pure.** Given the same input and config, dorq always produces the same output: no
   database connection, no network, no random seeds, and results that don't depend on thread count.
   Deduplication, acceptance, and resolution stay in fafnir. Determinism is also what allows fafnir to
   implement `dq recheck` for dorq checks by re-running dorq and treating a missing result as resolved.
2. **Explain every violation.** Every violation carries its evidence, as named features with their
   log Bayes factors. fafnir's operators write resolution notes, and dorq should give them the material.
3. **Say less, with more confidence.** Precision at `warn`/`error` is the metric that matters. Low
   posterior anomalies go to `info`, which is hidden by default.
4. **Degrade gracefully with context.** dorq runs on a bare CSV. Adding a trading calendar, corporate
   actions, security metadata, or a market reference series each improves precision. Checks that need
   an input that wasn't supplied are skipped and listed in the run summary.

---

## 2. Command-line interface

```
dorq [check] [OPTIONS] [FILE ...]      # 'check' is the default subcommand; '-' or no FILE reads stdin
dorq list-checks [--format text|json]  # code, name, default severity, inputs needed
dorq explain DQ203                     # full description of a check, with examples and knobs
dorq config show|init                  # print the effective config / write a starter dorq.toml
dorq calibrate --labels L.jsonl --data D.csv --out priors.toml   # fit priors from labelled history (M6)
dorq version
```

### 2.1 Key options for `check`

| Option | Default | Notes |
|---|---|---|
| `--kind ohlcv\|point\|auto` | `auto` | Detected from the columns present |
| `--input-format csv\|tsv\|jsonl\|json\|auto` | `auto` | Detected from the extension, falling back to sniffing the content |
| `--columns date=trade_date,series=security_id,label=symbol,...` | auto-map | Case-insensitive aliases such as `Date`, `timestamp`, `Adj Close`, `vol` |
| `--format text\|json\|jsonl\|csv\|fafnir` | `text` | `json` is an object with `violations` and `summary`; `jsonl` is one violation per line |
| `--select`, `--ignore`, `--extend-select` | all checks enabled by default | Accept a code prefix (`DQ2`) or a check name (`bad-print`) |
| `--min-severity info\|warn\|error` | `warn` | The reporting threshold |
| `--fail-on warn\|error\|never` / `--exit-zero` | `warn` | Controls the exit code |
| `--calendar XNYS\|weekdays\|24x7` / `--calendar-file F` | `XNYS` | The file is a list of open dates. fafnir passes an export of `ref.trading_calendar` |
| `--actions F` | none | Splits and dividends. Enables the DQ7xx checks and split-aware price hypotheses |
| `--meta F` | none | Per-series `asset_type`, `nav_priced`, `tick_size`, `peer_group`, `exchange`. Selects the config profile |
| `--market F` | none | A reference series such as SPY. Price checks score the residual after removing market beta |
| `--as-of DATE` | the latest date in the input | Needed for freshness checks |
| `--since DATE` | none | Report only on dates ≥ `--since`. Earlier data is still used as lookback, which is the incremental mode |
| `--show-evidence`, `--show-info`, `--statistics` | off | `--statistics` works like flake8's per-code counts |
| `--threads N` | all cores | Output is identical for any N |
| `--buffer` | off | Read all input before checking, for large input not grouped by series (§2.2) |
| `--color auto\|always\|never` | `auto` | Colour in text output; `auto` means a terminal and no `NO_COLOR` |
| `--config F` / `--isolated` | discovered | `--isolated` ignores any config file |

**Exit codes:** `0` means no violations at or above `--fail-on`. `1` means violations were found. `2`
means a usage or config error. `3` means the input could not be read or parsed.

### 2.2 Input contract

- **Long format**: one row per `(series, date)`. The `series` column is optional; without it, each
  file is one series named after its file stem.
- **OHLCV**: `date, open, high, low, close, volume[, vwap]`. **Point**: `date, value`.
- The input may be unsorted. Up to 256 MiB of input in total is read whole before checking. Larger
  input, and stdin, is streamed with bounded memory (§5.3), which needs every row of a series together
  (as `ORDER BY series, date` gives, and as fafnir's export does). A series that reappears is then an
  input error (exit 3) that says to sort the input, or to pass `--buffer` and read everything first.
  *(Decided in M1. The plan first said dorq would fall back to buffering when it met unsorted input, but
  by the time a series reappears its first half has already been checked and reported.)*
- Prices are parsed with fast_float into `double` (§5.1 explains why not `std::from_chars`), and dorq also records how many decimal places
  each value had as written, for the precision check DQ106.
- `--actions`: `series, ex_date, type(split|dividend), numerator, denominator, amount`.
- `--meta`: `series, asset_type, nav_priced, tick_size, peer_group, exchange`.

### 2.3 Output

Text output, one line per violation, in flake8's style:

```
AAPL  2020-08-31  DQ203 error  unreported-split  close 499.23→129.04 (×0.2585 ≈ 1:4)  P(error)=0.97
      → add split 4:1 ex 2020-08-31   [volume ×3.9 after; level held 40/40 sessions]
MAIR  2019-03-04..2019-03-29  DQ302 info  sparse-series  density 0.26 over 5,210 sessions
XYZ   2026-09-29  DQ201 warn  bad-print  close 0.0005 between 0.07/0.071 (reverts next bar)  P(error)=0.88  provisional
```

A `jsonl` record:

```json
{"series":"12345","label":"AAPL","date":"2020-08-31","code":"DQ203","check":"unreported-split",
 "severity":"error","p_error":0.971,"classification":"context_gap",
 "hypotheses":{"market_move":0.02,"bad_print":0.009,"unreported_split":0.971,"scale_error":0.0},
 "evidence":[{"feature":"ratio_to_clean","value":0.2585,"nearest":"1:4","log_bf":4.1},
             {"feature":"volume_shift_inverse","value":3.9,"log_bf":2.7},
             {"feature":"level_holds","value":"40/40","log_bf":1.9}],
 "suggested_action":{"kind":"add_split","ratio":"4:1","ex_date":"2020-08-31"},
 "provisional":false,"record_key":{"trade_date":"2020-08-31"},
 "dorq":{"version":"0.6.0","config_hash":"9f2c…"}}
```

The `fafnir` format is the same record projected onto the columns of `ops.data_quality_flag`:

```json
{"security_id":12345,"table_name":"core.daily_price","record_key":{"trade_date":"2020-08-31"},
 "check_name":"dorq_unreported_split","severity":"error","detail":{ …the fields above… }}
```

`record_key` keeps fafnir's existing `{"trade_date": …}` shape. That keeps `add_dq_flag_once`'s
deduplication and `dq_triage`'s `cohort_size` working unchanged. A violation spanning a run of dates
(an outage, or an era stored at the wrong scale) is keyed on its first date, with `end_date` and
`length` in `detail`.

---

## 3. The checks

Codes follow the flake8 convention of a prefix plus a number, and each check also has a stable
kebab-case name. The column **Needs** lists inputs beyond the series itself. Checks marked **B** are
Bayesian and report `p_error`. Checks marked **D** are deterministic, and a violation from them has
`p_error = 1`.

### DQ1xx: integrity (D)
| Code | Name | Detects |
|---|---|---|
| DQ101 | `ohlc-bounds` | `high < max(open, close, low)` or `low > min(...)` |
| DQ102 | `non-positive` | A price ≤ 0 on a series that should be positive (configurable for point series) |
| DQ103 | `duplicate-date` | The same `(series, date)` more than once. Reported as `error` if the values differ, otherwise `info` |
| DQ104 | `missing-field` | NaN, empty, or unparseable values |
| DQ105 | `non-session-bar` | A bar on a date the calendar marks closed |
| DQ106 | `precision-shift` | The number of decimal places changes regime, e.g. 2 dp becomes 6+ dp. Vendors produce this when they back-adjust a feed that should be raw (the WZRD and SMUP cases) |
| DQ107 | `zero-range-with-volume` | `O = H = L = C` on at least the series' median traded volume. A single trade on a thin day prints a flat bar legitimately, so a bare "volume > 0" would have flagged every thin name. This is the general form of `price_scale_collapse`. NAV-priced series are exempt via their profile |

### DQ2xx: price action (B; the core of dorq)
| Code | Name | Detects | Suggested action |
|---|---|---|---|
| DQ201 | `bad-print` | An isolated wrong value, or a block of up to K bars, that reverts: a spike-and-revert | delete or re-fetch the bar(s) |
| DQ202 | `scale-shift` | The level changes by a clean power-of-ten ratio or an implausible one, *without* the inverse volume shift a split would cause. Covers an era stored at the wrong scale, or a split applied backwards | rescale the era |
| DQ203 | `unreported-split` | The level changes by a clean split ratio, volume shifts inversely, the new level holds, and no split is on file | add the split |
| DQ204 | `ohlc-close-mismatch` | The close is far from a bar whose own open/high/low sit at the prior level, meaning only the close field is wrong | re-fetch the bar |
| DQ205 | `history-segment` | A long gap followed by a change of regime, i.e. a new listing that inherited someone else's ticker history (the first bar of a new ETF at ~$25, or a SPAC at ~$10) | split the history |
| DQ206 | `date-shift` | Weekday counts show the whole history shifted by a day (Sunday bars, no Friday bars: the FVI and WLL cases) | shift the dates |
| DQ209 | `large-move` | A move that is anomalous but is probably a market fact. `info` only; this is the successor to the old 50% rule, kept for review | none |

### DQ3xx: coverage and sparsity (B)
| Code | Name | Detects |
|---|---|---|
| DQ301 | `missing-run` | A run of missing sessions whose posterior P(feed outage) is high, given how densely *this* series normally trades. Reported once per run, not once per day |
| DQ302 | `sparse-series` | `info`: the series trades on few of its sessions. One violation per series, carrying the posterior density and a credible interval |
| DQ303 | `cohort-gap` | Needs more than one series in the input. Many series that normally have a bar are missing the same date, which points to a failed load rather than individual gaps. Members of the cohort have their DQ301 downgraded and are linked to it |
| DQ304 | `stale-feed` | Needs `--as-of`. The series ends before the as-of date by more sessions than its trading density and NAV lag allowance can explain (the successor to `stale`) |
| DQ305 | `frequency-gap` | For a point series: missing periods at the series' inferred or configured frequency (D, W, M, or Q) |

### DQ4xx: volume (B)
| Code | Name | Detects |
|---|---|---|
| DQ401 | `volume-scale-shift` | Volume changes by a clean ratio with no price change. Either the volume was back-adjusted, which ADR 0004 says inflates it by the split ratio squared, or its units changed |
| DQ402 | `volume-spike-no-move` | `info`: extreme volume with no price response |
| DQ403 | `move-on-zero-volume` | A price change on zero volume for a session-traded asset, which suggests an indicative quote or a stale carry that was later corrected |

### DQ5xx: stale values (B)
| Code | Name | Detects |
|---|---|---|
| DQ501 | `repeated-price` | A run of k identical closes (or full OHLC bars) with volume > 0, whose probability under the series' own volatility and tick size is negligible. Example: EQC's 0.9475 three days running on 3.7–5.8M shares |
| DQ502 | `carry-bar` | `info`: a zero-volume bar that repeats the previous close. Normal for thin names, but it matters as evidence of which date a split took effect |

### DQ6xx: cross-sectional (needs more than one series)
| Code | Name | Detects |
|---|---|---|
| DQ601 | `cohort-move` | Many series move by the same ratio on the same date. This is either a vendor mass adjustment, or a fund family splitting together, which is evidence *for* DQ203 among siblings in the same `peer_group` (iShares JKD/JKG/JKI, 2020-05-04) |
| DQ602 | `market-day` | Not a violation. With `--market` given, or with a broad enough cross-section, a crash day (e.g. March 2020) widens every series' volatility, so correlated moves stop producing flags |

### DQ7xx: corporate actions (needs `--actions`)
| Code | Name | Detects |
|---|---|---|
| DQ701 | `split-misdated` | The raw series shows one jump at the split's ratio on a session within ±5 of the ex-date, but not on the ex-date itself |
| DQ702 | `split-without-jump` | A split on file with no raw jump, meaning the split is fabricated or has already been applied to the bars. The pair AKR, HUN, and JKL show up here together with DQ202 |
| DQ703 | `split-ratio-mismatch` | A jump occurs on the ex-date, but its ratio differs from the ratio on file |
| DQ704 | `split-double-applied` | Two jumps of the split's size close together (the BHAT, CATB, ELOX cases) |
| DQ705 | `dividend-implausible` | The dividend is at or above the prior close, or is off by an order of magnitude from the series' dividend history |
| — | *split between bars* | Not a violation. A split whose ex-date falls between two stored bars *explains* the jump. This one rule removes the largest class of false positives from the current outlier check |

### DQ8xx: cross-vendor (stretch, M8)
| Code | Name | Detects |
|---|---|---|
| DQ801 | `source-disagreement` | Two sources for the same series disagree. dorq reports the posterior probability that each one is the wrong one, based on how consistent each is with its own neighbouring values. Directly useful for the Sharadar/FMP 60-session parallel run (SA-0506) |

---

## 4. The models

Every model below uses closed-form or online inference that costs O(n) per series. Nothing samples.

### 4.1 Shared per-series features (computed once, read by every check)

- The log price `y_t` for OHLCV, or the transformed value for a point series (`log`, `diff`, or `auto`,
  where `auto` uses `log` if every value is positive and `diff` otherwise).
- The return between consecutive bars `r_t = y_t − y_{t−1}`, scaled for elapsed time: its variance
  grows with the number of sessions since the previous bar, Δ_t, taken from the calendar.
- **Local volatility** from a discounted Normal-Inverse-Gamma posterior (discount factor λ ≈ 0.97,
  i.e. an effective window of about 33 bars). This gives a Student-t predictive for `r_t` that adapts
  to volatility regimes, has fat tails, and is trivial to compute. The candidate bar is excluded from
  its own prior.
- **Hierarchical prior on volatility.** Before scoring, a first pass estimates hyperpriors for each
  class (`asset_type` × price-level bucket × liquidity bucket) by empirical Bayes. A series with a
  short history is pulled toward its class. This replaces fafnir's `GAP_MIN_SESSIONS_FOR_DENSITY`
  special case with a principled one.
- Rolling median and MAD of close and log-volume (a Hampel filter), plus the trailing median volume.
- Tick size, taken from `--meta` if given, otherwise inferred from the price level and date (1/16
  before 2001, then 0.01, then 0.0001 below $1).

**As built in M3** (doc/checks/DQ201.md is the reference):

- **The class prior is applied within the series.** A pre-pass across series
  needs the input read twice, which stdin and streaming rule out. The class
  default (by price level and dollar volume) is a fixed table, combined with the
  series' own robust scale (the MAD of its returns), weighted as ten returns.
  `dorq calibrate` (M6) will fit the table.
- **Carry bars are not prices.** A zero-volume bar repeating the last close
  records that nothing traded; the next trade's return spans all the sessions
  since the last real one. Counting carry bars as zero returns understated thin
  names' volatility and made their next trade look like a jump.
- **Every ordinary move includes the bid-ask bounce**, 0.7 steps of the price
  grid. The grid is the exchange's tick or the coarser grid most prices are
  written on (a sub-dime stock quoted in cents). Without it, a one-tick move and
  its bounce back on a 2-cent stock looked exactly like a bad print.
- **Volatility is estimated both ways** (a forward and a backward discounted NIG,
  clipped at 4 standard deviations), and a bar is judged against both sides,
  leaving out the bar and the few after it. Ordinary returns are Student t with 4
  degrees of freedom, scaled to the estimated variance.

### 4.2 Coverage: a two-state hidden Markov model (DQ301–DQ305)

Take the calendar sessions in a series' window, from its first bar to its last (or to `--as-of` for
DQ304). Let `z_s ∈ {present, missing}` for each session. The hidden state is `h_s ∈ {healthy, outage}`.

- `P(present | healthy) = p_{i,s}`. This is the series' trading density, with a Beta prior per
  liquidity class, updated over a trailing window of 120 sessions so it follows changes in liquidity.
  If `--meta` or the bars supply volume, the class prior is conditioned on log median volume.
- `P(present | outage) = ε ≈ 0`.
- Transitions: `healthy → outage` with probability α (default 1e-3), and `outage → healthy` with
  probability β (default 0.2).

The forward-backward algorithm gives `P(outage)` for each missing session. Consecutive missing sessions
are merged into runs, and a run is reported as DQ301 when the maximum posterior exceeds the threshold.
Consequences:

- For a liquid name with p ≈ 0.999, **a single missing day is a high-posterior outage.** That is the
  correct result.
- For MAIR, whose median volume is one share, missing days are explained by the healthy state. Only an
  implausibly long run is reported. The 0.80 cutoff goes away, and no securities are silently exempted.
- **Cohort (DQ303):** during the first pass, count per date how many series had `P(present) > 0.95`
  and how many of those were missing. A binomial test against each series' own `p` identifies failed
  load dates. The members of such a cohort are linked and downgraded, which automates what fafnir's
  sweep policy calls "the whole test", `cohort_size`.

**As built in M2** (doc/checks/DQ301.md is the reference). Five things changed on contact with the
model:

- **The density is estimated around each run, leaving the run out.** The density used for a run
  comes from its block of 60 sessions and the blocks either side. It leaves out the run itself and
  the sessions that the other runs' posteriors already attribute to an outage. It is estimated by EM
  over two passes, starting from the density volume alone implies; started from the data, two
  isolated gaps each made the other look like ordinary sparsity.
- **Volume sets a floor, not a prior.** Pooled counts can never say that a liquid name misses fewer
  than about 1 session in *n*, and a real gap in such a name is far rarer than that. So when bars
  carry volume, the healthy density is at least 1 − e^(−V/`trade_size`), with *V* the median volume
  and `trade_size` 1,000. That is a round-lot Poisson model of trades. The first attempt, a
  log-normal fitted to traded days' volume, claimed that a name trading 1–4 shares trades every day.
- **The outage rates are α = 1e-4 and β = 0.05**, giving a mean outage of 20 sessions. With those, a
  lone missing day on a name trading a million shares has P = 0.83 (warn). A 90-session hole in a
  name with 30% density is > 0.99. Ordinary gaps in thin names stay below the info threshold.
- **The window ends at the last bar; DQ304 judges what follows.** DQ304 uses an as-of date: the
  latest session with a bar anywhere in the input, unless `--as-of` gives one. It also allows a
  publication lag (default 1 session). A series that stops counts toward the cohort test on its
  first missing session, so a failed latest-night load is one DQ303.
- **Output waits for the end when a cross-sectional check runs.** DQ303 and DQ304 are on by default,
  so results are held until the input ends and written in input order then (§5.3). Without them,
  output streams as before.

**Changed in M3**, found by dorq-synth: the volume floor applies only where the
counts already show a bar on 90% or more of nearby sessions (a thin name's
2,000-share print is one trade, not two, and the floor had claimed thin names
trade daily); EM starts from that same floored density rather than from volume
alone; and a DQ301 run is downgraded for a cohort only when every session in it
is a cohort date (a 222-session gap containing one failed-load date had been
hidden).

**Frequency** is inferred from the lower-quartile gap between dates, not the median. A daily name
trading on a third of its sessions has a median gap of about five days, which read as weekly and
flooded DQ305.

### 4.3 Price action: comparing explanations (DQ201–DQ206, DQ209)

**Screening.** Only a small set of candidates is scored. A bar becomes a candidate if its predictive
tail probability is below 1e-3, or `|r_t| > log 1.5` (so dorq can never miss something the current
check would catch), or its bar is internally inconsistent (DQ204). This keeps the expensive scoring
to a tiny fraction of bars.

**Hypotheses** scored at candidate t, using up to W=40 bars on each side:

| H | Generative story | Evidence that favours it |
|---|---|---|
| `market_move` | `r_t` comes from a jump-diffusion: with probability 1−q from t(σ_t), and with probability q from t(κσ_t), κ≈6 | volume surge on the day; the level holds; peers or the market moved too (with `--market`) |
| `bad_print` | An additive outlier: `y_{t..t+k−1} = x + δ` for some k ≤ K (default 5), then the series reverts | the repaired return `y_{t+k} − y_{t−1}` is ordinary under t(σ√(k+1)); volume on the day is zero or low; the OHLC is internally inconsistent; the bar is off the Hampel median |
| `unreported_split` | A level shift by ρ, where the prior on ρ is a mixture of narrow Gaussians (σ_ρ ≈ 0.03) centred on log clean ratios {2, 3, 4, 5, 8, 10, 15, 20, 25, 50, 100, 3/2, 5/4, …} | volume shifts inversely, `log(v_post/v_pre) ≈ −ρ`; the level holds (≥90% of the next 40 sessions within 35%); no reversion; volume > 0 on the day; no split on file within ±120 days; siblings in `peer_group` shift on the same day |
| `scale_error` | A level shift by ρ where ρ is a power of ten or is implausible for the asset, and volume does *not* shift inversely, or the price levels are implausible (AKR's 149,613,176) | no inverse volume shift; flat identical prices on heavy volume beforehand; a DQ106 precision shift at the same boundary |
| `explained_split` | *Needs `--actions`.* A split on file with an ex-date in (previous bar, t] and a ratio matching e^ρ | the ratio matches within tolerance, **and the plausibility gate passes**: levels either side are plausible for the asset class, so a fabricated split that matches a mis-scaled history is not treated as an explanation |
| `tick_move` | The price changed by n ≤ 2 ticks | the price is below $0.10, or a tick is a large fraction of the price (ARWR 1/16 → 3/16) |

Posterior: `P(H | data) ∝ π_H · p(data | H)`. The likelihoods factorise into roughly independent
evidence terms: the return, the post-window behaviour, volume, OHLC consistency, the action file, and
peers. dorq reports each term's log Bayes factor. `p_error = P(bad_print) + P(scale_error) +
P(unreported_split)`, where `unreported_split` counts as a context gap. The reported code is the MAP
error hypothesis. If `market_move`, `tick_move`, or `explained_split` wins, the bar becomes DQ209
`info`, or produces nothing.

**The newest bars.** When fewer than a configurable number of bars follow t (default 3), the evidence
about reversion and whether the level holds is missing. The posterior stays near the prior and the
violation is marked `provisional: true` with its severity capped at `warn`. fafnir re-evaluates it the
next night. This matches the playbook rule "never delete a newest bar on the first night" (XNDX,
MILK, MSEP, NODE).

**Default priors** (per candidate, before calibration): market_move 0.90, bad_print 0.05,
unreported_split 0.02, scale_error 0.02, tick_move 0.01. With `--actions` present, explained_split is
given 0.10, taken from market_move. `dorq calibrate` replaces these (§6).

**As built in M3** (doc/checks/DQ201.md is the reference). Synthetic data
(DR-0308) drove these changes:

- **Two more hypotheses.** `bad_close` (only the close field is wrong; DQ204)
  and `history_segment` (after `segment_gap` sessions without a bar; DQ205), each
  with its own prior.
- **The bars after a move are a term of every hypothesis.** A bad print's block
  ends with a return that undoes it. A real move stirs volatility up (half the
  time, by a quarter of its size, about √α of a GARCH model); a split, a scale
  error or a bad print leaves the next bars calm. This replaced the plan's "level
  holds" fraction, and took earnings-gap reversals off the false positives.
- **A split's ratio prior depends on the price.** Companies split forward from
  high prices and reverse-split from low ones: the share of forward splits rises
  log-linearly from 5% at $5 to 95% at $40. Ratios are weighted by how often US
  companies use them (2:1 most; 1:10, 1:5 and 1:20 among reverse splits). A split
  or scale error is the clean ratio plus an ordinary return, with
  `ratio_tolerance` (0.01, not 0.03) as slack.
- **A scale error is a power of ten.** Its fallback for other ratios is weighted
  0.001; an implausible price level carries that case through the
  `plausible_level` term.
- **Volume on the day rises with how surprising a real move is**, from none at 2
  standard deviations to e times at 6, so a tick move on a quiet day is not
  evidence of an error. The volume shift after a move uses the sampling noise of
  the two medians, and the same drift (0.5 on the log scale) for every hypothesis
  but a split (0.3); only their centres differ.
- **Screening** also takes every bar after a gap of `segment_gap` sessions, and
  every close outside a range that holds its open.
- **DQ209** is a scored bar whose `p_error` is below the info threshold. Between
  info and warn, the bar is reported at info under its error's code.
- **Two opposite scale shifts** (both with `p_error` ≥ 0.5) are one DQ202 era.
- **Configuration**: `hold_window` became `volume_window`, `clean_ratios` became
  `split_ratios` (as "2:1" strings), and the priors table adds `bad_close` and
  `history_segment`.

### 4.4 Stale values (DQ501)

Under the return model, `q = P(|Δp| < tick/2) = F_t(tick/2) − F_t(−tick/2)`, measured on the price
scale. A run of k unchanged closes with volume > 0 has likelihood `q^k` if the series is healthy,
against a carry-forward hypothesis with a small prior. Where volatility is high relative to the tick,
three unchanged prints on heavy volume are very unlikely, so they are reported. For a thin sub-dime
stock they are expected, so they are not.

**As built in M4** (doc/checks/DQ501.md is the reference): `q` is the larger of
the model's chance and the series' own repeat rate nearby (counted as a chain:
repeats after a move, repeats after a repeat), so a stock whose repeats come in
stretches is judged by them. The grid includes the lattice the prices nearby sit
on, read on each side of the run. Prices 20 grid steps wide or fewer are not
judged. The prior is `stale_run` = 3 × 10⁻⁵ per bar: at 10⁻⁴, chance runs across
a universe of 300,000 bars outnumbered the stale feeds. Whole-bar repeats are
reported, but not used as evidence, because a quiet low-priced stock repeats its
open, high and low with its close.

### 4.5 Volume (DQ401–DQ403)

`log(1 + v)` is modelled with a robust local level (Hampel) and a discounted NIG scale. DQ401 applies
the same clean-ratio mixture as DQ203 to a level shift in volume that has **no** matching price shift.

**As built in M4** (doc/checks/DQ401.md): the level step is the difference of
40-bar medians of log volume. Its uncertainty uses an effective sample size that
allows for autocorrelation. Volume runs in spells, and without that, ×165 read as
too far from ×100. A natural shift is Student t (scale 0.5) against the
clean-ratio mixture (prior 5%). Steps that a price finding or an inverse price
level explains are left to DQ2xx. Two opposite steps at the same clean ratio
form one era. DQ402 is info: a spike of ×10 or more on a quiet day, classified
as an error only on a clean ratio. DQ403 weighs one zero-volume move against
the series' own rate of them.

### 4.6 Point series

The same machinery applies to `value` with the configured transform. Point-specific additions are:
configured `bounds` (e.g. a 10-year yield in [−5, 25]); unit-shift ratios {100, 1000, 10⁶} in the
`scale_error` prior, to catch percentages stored as decimals or thousands stored as millions; and a
frequency model for DQ305.

**As built in M4** (doc/checks/DQ201.md, "Point series"): `[price] transform`
picks the scale. `diff` puts moves on the value itself; `auto` takes the log
when every value is positive. On the value scale a wrong value's jump scales with
the series' magnitude, and a unit shift multiplies (`f(v/k − prev)/k`). Only
ratios of 100 or more apply, and the size of the moves either side is evidence:
a unit change scales them too. Without that term, a rate falling a quarter point
to zero looked like the end of a ×100 era. `bounds` drives both DQ108 and the
model's plausibility term.

---

## 5. Architecture

### 5.1 Language, toolchain, dependencies

- **C++20**, CMake ≥ 3.25 with `CMakePresets.json` (version 6, for workflow presets), Ninja. CI
  targets GCC 13 and Clang 18 on Ubuntu 24.04 (fafnir's host OS, where those are the system
  compilers) and AppleClang on macOS.
- Dependencies are fetched with `FetchContent`, each pinned to a commit with its tag in a comment, and
  are all header-only or small. Each one is added in the PR that first uses it. M0 brought in CLI11
  and doctest; M1 added fast_float and toml++.

  | Need | Library |
  |---|---|
  | CLI parsing | CLI11 |
  | Number parsing | fast_float. libc++ (macOS) did not implement `std::from_chars` for floating point until LLVM 20, so it can't be relied on across the platforms dorq releases for |
  | JSON input and output | Neither library. Input records are flat objects of scalars, so a ~250-line parser (`src/io/json.cpp`, fuzzed) does the job, streams JSON Lines, and keeps each number's raw text so its written precision survives (DQ106 needs it). A library's DOM would have discarded that text. Output is written directly. *(Changed in M1; the plan had simdjson and nlohmann/json.)* |
  | TOML config | toml++, pinned past v3.4.0 to the commit fixing marzer/tomlplusplus#305 (undefined behaviour on some non-ASCII input), which dorq's config fuzzer found |
  | Special functions (the t CDF, lgamma, incomplete beta) | dorq's own (`src/stats/`): M2 needed the incomplete beta and gamma anyway, one implementation keeps results independent of the platform, and the C library's `lgamma` is not thread-safe. *(Changed in M2–M3; the plan had Boost.Math.)* |
  | Formatting | `std::to_chars` (shortest round-trip), with the macOS deployment target set to 13.3, the first release whose libc++ provides it. No {fmt}. *(Changed in M1.)* |
  | Tests and benchmarks | doctest, Google Benchmark |

- No Eigen, Arrow, or database client in v1. Parquet input is a stretch item behind a CMake option.

### 5.2 Layout

```
dorq/
  CMakeLists.txt  CMakePresets.json  cmake/Deps.cmake
  include/dorq/            # public headers (enables a future library or pybind11 module)
  src/
    cli/                   # main.cpp, subcommands, option parsing, exit codes
    io/                    # csv/jsonl readers, schema sniffing, column mapping; writers/{text,json,jsonl,csv,fafnir}
    core/                  # Series (structure of arrays), Calendar (XNYS rules + file), Config, Profile, Violation
    stats/                 # special, student_t, nig, robust, hmm2
    price/                 # per-series features (§4.1), the price model (§4.3), tick sizes
    checks/                # registry.cpp + integrity/, price/, coverage/, volume/, stale/, cross/, actions/, point/
    engine/                # streaming runner, thread pool, two-pass cross-sectional coordinator, deterministic sort
  tests/  unit/  golden/  property/
  tools/synth/             # dorq-synth: synthetic series with injected, labelled faults
  bench/
  doc/  checks/DQxxx.md  adr/  plans/
```

### 5.3 Engine and data flow

```
reader (streaming, grouped by series)
   └─► pass 1, per series, in parallel: features → candidate screen → per-series checks → Candidate buffer
                                    └─► cross-sectional accumulators (per-date counts, ratio histograms)
   pass 2, after all series: cohort and peer evidence folded into the buffered candidates → posteriors
   → severity filter → sort by (series, date, code) → writer
```

- **Bounded memory.** When the input is sorted by `(series, date)`, only one series per worker is held
  in memory, plus the small candidate buffer and per-date accumulators. Scanning fafnir's full history
  should need well under 1 GB. When no cross-sectional check is selected, output streams directly.
- **Determinism.** Each series is identified by its position in the input. Output is sorted before it
  is written. Floating-point reductions use a fixed order. A test runs with `--threads 1` and
  `--threads 16` and requires byte-identical output.

### 5.4 The Check interface

```cpp
struct CheckInfo { std::string_view code, name; Severity default_severity;
                   Needs needs; /* calendar, actions, meta, market, multi_series */ };

class Check {
 public:
  virtual ~Check() = default;
  virtual const CheckInfo& info() const = 0;
  // Per-series work. May emit final violations or buffered candidates.
  virtual void run(const SeriesContext& ctx, Sink& out) const = 0;
  // Optional: fold cross-sectional evidence into this check's buffered candidates.
  virtual void finalize(const CrossSection&, CandidateSpan, Sink&) const {}
};
```

Checks register themselves in `registry.cpp`. `list-checks` and `explain` are generated from
`CheckInfo` together with `doc/checks/DQxxx.md`, which is compiled into the binary.

### 5.5 Configuration

Configuration is discovered in this order: `--config`, then `dorq.toml` or `[tool.dorq]` in
`pyproject.toml`, searching upward from the current directory, then `$XDG_CONFIG_HOME/dorq/dorq.toml`.
CLI flags override the file. **Profiles** apply per series, matched against `--meta` fields, which
lets NAV-priced funds, ETFs, crypto (24x7), and rates each have their own rules without extra flags.

```toml
[tool.dorq]
select = ["DQ1", "DQ2", "DQ3", "DQ5", "DQ7"]
ignore = ["DQ402"]
min_severity = "warn"
calendar = "XNYS"

[tool.dorq.severity]           # thresholds on p_error
warn = 0.60
error = 0.90

[tool.dorq.price]
candidate_tail_prob = 1e-3
floor_move = 0.5               # always screen moves ≥ 50%, so dorq never misses what the old check caught
revert_max_bars = 5
volume_window = 40
ratio_tolerance = 0.01
split_ratios = ["2:1", "3:1", "3:2", "4:1", "1:5", "1:10", "1:20"]   # and more by default

[tool.dorq.priors]             # replaced wholesale by `dorq calibrate` output via `include`
market_move = 0.90
bad_print = 0.05
unreported_split = 0.02
scale_error = 0.02
tick_move = 0.01

[tool.dorq.coverage]
outage_enter = 1e-3
outage_exit = 0.2
report = "run"                 # or "session" for one violation per missing day

[tool.dorq.profiles.nav_fund]
match = { nav_priced = true }
coverage.lag_sessions = 1      # NAV_LAG_TRADING_DAYS
ignore = ["DQ107", "DQ4"]      # O=H=L=C and zero volume are how a NAV strike looks

[tool.dorq.profiles.rates]
match = { asset_type = "rate" }
transform = "diff"
bounds = [-5.0, 25.0]
```

`dorq config show` prints the effective config along with its `config_hash`, which is written into
every output record so a flag can be traced to the settings that produced it.

---

## 6. Calibration: learning from fafnir's own decisions

fafnir already holds a labelled dataset, which is uncommon. It should be used from the beginning.

| Source | Label |
|---|---|
| `ops.operator_override` with `target = 'daily_price'` (delete, shift, rescale) | **data error**. `detail` keeps the bar *as it stood when removed*, so the pre-repair series can be rebuilt exactly |
| `ops.operator_override` with `target = 'corporate_action'` and `operation = 'add'` | **context gap** (unreported split) |
| `redate` pairs | **context gap** (misdated split) |
| `outlier`/`gap` flags with `accepted_at` set and a note saying "market fact", "split between bars", or "one tick" | **market fact** |
| Flags closed by `dq recheck` after a repair | **data error** |
| Flags left open with a documented "leave open" reason (§2j of the playbook) | excluded (unknown) |

`accepted_note` and `resolution_note` are free text, so the export needs a one-time labelling pass: a
keyword map, then review by hand. The fafnir-dba agent is well suited to that review.

`dorq calibrate` takes the reconstructed pre-repair series together with the labels. It fits the
hypothesis priors π_H and a few scale hyperparameters (κ, σ_ρ, α, β) by maximising the marginal
likelihood of the labels, then fits a monotone (isotonic) map from raw `p_error` to calibrated
`p_error`. The output is a `priors.toml` that a config file pulls in with `include`. It is versioned
and recorded in `config_hash`.

**On other ML methods:**

| Method | Verdict |
|---|---|
| **The Bayesian hypothesis comparison above** | **Primary.** Needs few labels, gives calibrated and explainable output, encodes the playbook, runs fast |
| Gradient-boosted trees (LightGBM or XGBoost) on the same evidence features | **A good stage 2, once there are about 1,000 labels.** Use it as a re-ranker whose input is the Bayesian evidence vector. Train it in Python and ship it as a JSON tree ensemble evaluated in C++ (≈200 lines, no runtime dependency). Only adopt it if it beats the calibrated Bayesian model on held-out precision |
| Isolation Forest / LOF / one-class SVM | **Not recommended** as a primary. They answer "is this unusual?", which is the question fafnir already over-answers. Their scores are uncalibrated and hard to explain in a resolution note |
| LSTM or transformer autoencoders | **Not recommended.** Need a lot of data, are opaque, are poorly calibrated, and add a heavy runtime for no gain on daily bars |
| Bayesian online changepoint detection (Adams & MacKay) | **Useful as a helper**, not as a check. Use it to find era boundaries for DQ202, DQ205, and DQ106 when a shift is gradual or noisy |
| Full MCMC hierarchical models (Stan) | **Offline only.** Too slow for a nightly scan of 150M bars. Could be used in research to validate the closed-form approximations |

---

## 7. Integration with fafnir (M7, in `rtrimble13/fafnir`)

1. **Deploy.** Unpack a GitHub release archive under `/opt` with `/opt/dorq` symlinked to it, or build
   from source with the `release` preset and `cmake --install` (both are in the README). Either way
   the binary is `/opt/dorq/bin/dorq`. Whether to build with `-march=x86-64-v3` is decided in the M6
   performance pass (DR-0604). Then and add `[dq] dorq_path` and `dorq_config` to `fafnirrc`. `fafnir status`
   reports the dorq version.
2. **Export → run → ingest.** Add a new module, `src/fafnir/dq/dorq.py`:
   - `COPY (SELECT security_id, trade_date, open, high, low, close, volume FROM core.daily_price WHERE …
     ORDER BY security_id, trade_date) TO STDOUT (FORMAT csv, HEADER)`, piped into `dorq check
     --format fafnir --calendar-file <ref.trading_calendar export> --actions <core.corporate_action
     export> --meta <core.security export> --since <last run − 5 sessions> -`.
   - Nightly runs an incremental scan with a 260-session lookback. A weekly or on-demand run scans the
     full history.
   - Output is loaded into a temporary table, then written with one set-based `INSERT … SELECT` carrying
     **the same two `NOT EXISTS` guards** (open and accepted) used in `dq/checks.py`. Deduplication and
     acceptance therefore behave exactly as they do today.
3. **Check names.** New names are prefixed `dorq_` (`dorq_bad_print`, `dorq_missing_run`, …). The
   existing checks stay in place until cutover. `record_key` keeps the `{"trade_date": …}` shape.
4. **Shadow mode first.** `fafnir dq run --engine sql|dorq|both --shadow` writes dorq's output to
   `var/dorq-shadow/<date>.jsonl` instead of to the queue. A comparison report (`fafnir dq compare`)
   shows overlap with the SQL checks and precision against the §6 labels. dorq output starts going
   into the queue only when the §8 criteria are met.
5. **Recheck.** Add the `dorq_*` checks to `RECHECKABLE` using a "rerun and negate" strategy: run dorq
   over the securities that have open dorq flags, and close each flag whose `(check_name, record_key)`
   is not emitted again. This works because dorq is deterministic. `detail.dorq.version` is compared,
   and a version change is noted in the resolution note.
6. **Policy and skill.** Place each `dorq_*` check in a tier in `sweep-policy.md` and in
   `NEVER_AUTO_RESOLVE` (initially: `dorq_scale_shift`, `dorq_split_without_jump`, and
   `dorq_split_double_applied` go in the Never tier). Add dorq entries to `dq-playbooks.md`.
   `outlier-classification.md` shrinks to a reading of dorq's evidence.
7. **Labels export.** Add `fafnir dq export-labels`, which writes the §6 dataset as JSONL, including the
   reconstructed pre-repair windows, as input to `dorq calibrate`.
8. **Cutover.** Retire the SQL `outlier`, `gap`/`sparse_coverage`, and `stale` checks in favour of their
   dorq successors. Existing open flags are migrated by recheck, not by bulk resolve.

**Sequencing with the Sharadar plan.** SA-0604 and SA-0703 anticipate a DQ flood from the dead-issuer
backfill. dorq in shadow mode by then would let that triage start from ranked posteriors instead of a
raw queue. DQ801 is the natural engine for SA-0506 (`fafnir vendor reconcile`). Neither milestone of
this plan depends on that timing.

---

## 8. Measuring success

**Evaluation harness** (M6): `dorq-synth` together with the §6 label set.

- `dorq-synth` (built in M3; tools/synth/README.md) generates t-GARCH and jump-diffusion series of realistic lengths and liquidity classes,
  then injects labelled faults: spikes, scale eras, unreported and misdated splits, doubled splits,
  outages, stale runs, date shifts, precision shifts, and inherited ticker histories. It also injects
  "hard negatives" that should *not* be reported: earnings gaps, crash days, thin names, tick moves on
  sub-dime prices, and splits between bars.
- Reports cover precision, recall, and F1 per check, reliability diagrams for `p_error`, and precision@k.

**Acceptance criteria for fafnir cutover** (M7):

| Metric | Target |
|---|---|
| Precision at `warn` and above, on the held-out fafnir labels | **≥ 0.85** (≥ 0.95 at `error`) |
| Recall of confirmed data errors (operator overrides, repairs closed by recheck) | **≥** the current SQL checks' recall on the same set |
| Queue volume: new flags per night after shadow period | **≥ 70% fewer** than SQL checks, with no loss in recall |
| Calibration: expected calibration error of `p_error` | ≤ 0.05 |
| Runtime: full history (~150M bars) on the fafnir host | ≤ 5 min on 8 cores; nightly incremental ≤ 30 s |
| Determinism | byte-identical output for `--threads 1` and `--threads N`, and across repeated runs |

---

## 9. Milestones and work items

Sizes: S ≈ ½ day, M ≈ 1–2 days, L ≈ 3–5 days of implementation. Each milestone ends with a tagged
release; items within a milestone are listed in dependency order.

### M0: Foundations (tag v0.0.1)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0001 | CMake + presets (dev, release, ci, asan, coverage), FetchContent pinned deps, `dorq --version` / `dorq version [--format json]` with the commit recorded at build time | M | ✅ PR #1 |
| DR-0002 | CI: GCC 13 and Clang 18 on Linux, AppleClang on macOS, ASan+UBSan, clang-format and clang-tidy (both pinned to 18), unit and CLI contract tests | M | ✅ PR #1 |
| DR-0003 | `doc/adr/0001-stateless-deterministic-linter.md`, `0002-bayesian-core-no-mcmc.md`, `0003-check-codes.md` | S | ✅ PR #1 |
| DR-0004 | README with a quickstart, CONTRIBUTING, and a PR template that requires docs to change with code (fafnir's docs-gate convention) | S | ✅ PR #1 |
| DR-0005 | Release workflow: a `vX.Y.Z` tag builds and tests on Linux x86-64 and macOS arm64, refuses a tag that disagrees with the binary's version, and publishes archives with `SHA256SUMS` | S | ✅ PR #1 |

**Done when:** CI is green on an empty `dorq` binary that prints its version, and tagging `v0.0.1`
publishes release archives.

### M1: I/O, data model, deterministic checks (v0.1.0)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0101 | Series data model (structure of arrays, OHLCV and point), decimals and significant figures as written, the `Violation` type | M | ✅ PR #2 |
| DR-0102 | CSV/TSV reader: an incremental RFC 4180 parser, fast_float, column aliases and `--columns`, kind detection, stdin, buffered or streamed grouping by series | L | ✅ PR #2 |
| DR-0103 | JSONL and JSON-array readers (a flat-record parser of dorq's own, not simdjson; see §5.1) | M | ✅ PR #2 |
| DR-0104 | Writers: text (colour when writing to a terminal), json, jsonl, csv, fafnir; `--statistics` | M | ✅ PR #2 |
| DR-0105 | Config: toml++, discovery including `[tool.dorq]`, unknown keys rejected, profiles (matching on kind or series until `--meta` in M5), `config show`/`init`, `config_hash` | M | ✅ PR #2 |
| DR-0106 | Check registry, flake8-style `--select`/`--ignore`, `list-checks`, `explain` (pages compiled in), exit codes | M | ✅ PR #2 |
| DR-0107 | DQ101–DQ104, DQ106, DQ107 | M | ✅ PR #2 |
| DR-0108 | Engine: thread pool, bounded in-flight series, results delivered in input order; determinism test (1 vs 8 threads, and buffered vs streamed) | M | ✅ PR #2 |
| DR-0109 | libFuzzer targets for the readers (with every check behind them) and the config parser; run for a minute each in CI | S | ✅ PR #2 |

**Done when:** `dorq prices.csv` reports the integrity violations with correct exit codes in all five
output formats, and the determinism test passes.

### M2: Calendar and coverage (v0.2.0)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0201 | Built-in calendars (`XNYS` with NYSE holiday rules, MLK Day from 1998 and the unscheduled closures; `weekdays`; `24x7`). An optional reference file (`--calendar-file`, fafnir's `ref.trading_calendar` shape) decides within its span, and the built-in calendar answers outside it. `--calendar-exchange`. Tested year by year against fafnir's calendar for 1990–2035 | M | ✅ PR #3 |
| DR-0202 | DQ105 (non-session bar; one summary violation past ten), DQ206 (date shift: multinomial weekday likelihood, shift ±1 vs aligned) | M | ✅ PR #3 |
| DR-0203 | `stats/special` (log-gamma, incomplete beta and gamma, Beta quantile, Poisson tail, normal CDF) and `stats/hmm2` (forward-backward in log space), tested against SciPy values and brute-force path enumeration | M | ✅ PR #3 |
| DR-0204 | DQ301 missing-run, DQ302 sparse-series, DQ304 stale-feed (publication lag; NAV lag via a profile), DQ305 frequency gap; frequency inferred per series | L | ✅ PR #3 |
| DR-0205 | Cross-sectional pass: results held until the input ends; per-day expected/missing/healthy-miss accumulators; DQ303 cohort-gap by Poisson tail; members' DQ301/DQ304 downgraded and linked; series that stop count on their first missing session | M | ✅ PR #3 |

**Done when:** on a synthetic universe with thin names, liquid names, and one injected failed load
date, DQ303 fires once, thin names produce no DQ301, and a single missing day on a liquid name does.

### M3: The price action model (v0.3.0), the core milestone
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0301 | `stats/student_t` (dorq's own, over M2's incomplete beta; not Boost.Math), `stats/nig` (discounted Normal-Inverse-Gamma), `stats/robust` (median, MAD), tick size by era and price plus the grid prices are written on | M | ✅ PR #4 |
| DR-0302 | Shared feature pass (§4.1): bars without carry bars, session-scaled returns, forward and backward volatility; the class prior combined with the series' own scale (within the series, not a cross-series pre-pass; see §4.1) | L | ✅ PR #4 |
| DR-0303 | Candidate screening: tail probability, the 50% floor, long gaps, a close outside a range that holds the open | S | ✅ PR #4 |
| DR-0304 | Hypothesis scoring: market_move, tick_move, bad_print (k ≤ K), bad_close, history_segment; evidence terms with log Bayes factors | L | ✅ PR #4 |
| DR-0305 | The clean-ratio mixture (weighted by frequency and by price level), unreported_split vs scale_error by the volume shift, plausible price levels | L | ✅ PR #4 |
| DR-0306 | DQ201–DQ205, DQ209; provisional handling of the newest bars; `suggested_action`; scale eras reported once | M | ✅ PR #4 |
| DR-0307 | `--show-evidence` in text; `hypotheses`, `evidence`, `suggested_action` and `provisional` on every JSON record | S | ✅ PR #4 |
| DR-0308 | `dorq-synth` v1 (faults used by M2 and M3, and hard negatives) and a CTest gate on precision and recall per check (`tools/synth/gates.txt`, seeds 1 and 2) | L | ✅ PR #4 |

**Done when:** on `dorq-synth` data, DQ201 and DQ203 each reach precision ≥ 0.9 at `warn`, and the
hard-negative set (earnings gaps, crash days, tick moves) produces no `warn`.

**Result:** on seeds 1 and 2 (the CTest gate) DQ201 precision is 1.0 and 0.94,
DQ203 1.0 and 0.94, with recall 1.0; DQ202, DQ204 and DQ205 are at 1.0. No
earnings gap, crash day or tick move is reported at warn on seeds 1–14. Thin
names' spike-and-revert trades remain the ambiguous case: up to three per
universe, and the gate allows two.

**Also in M3:** dorq-synth showed DQ107 reporting over a thousand flat bars a
universe on sub-dime stocks quoted in cents, where flat bars are ordinary. DQ107
now judges a flat bar against the range of the bars nearby in steps of the price
grid (the exchange tick, the decimals written, or the lattice the prices sit on),
and against how often they are flat: 0–4 such reports remain, and every injected
copied-close bar is still found (doc/checks/DQ107.md).

### M4: Volume, stale values, point series (v0.4.0)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0401 | DQ401–DQ403 | M | ✅ PR #5 |
| DR-0402 | DQ501 repeated-price, DQ502 carry-bar | M | ✅ PR #5 |
| DR-0403 | Point series: transforms (log, diff, auto), bounds (DQ108), unit-shift ratios, a `rates` example profile | M | ✅ PR #5 |

**Result:** dorq-synth gained volume-unit eras, moves on zero volume, stale-feed
runs, and eight rate series (`points.csv`, read with the `rates` profile in
`tools/synth/dorq.toml`) with a ×100 print, a ×0.01 era, an out-of-bounds value,
and quarter-point policy moves near zero as hard negatives. On seeds 1–6, DQ108,
DQ403 and the rates' DQ201/DQ202 are at precision and recall 1.0. DQ401 has
precision 1.0 and recall 0.83–1.0. DQ501 has precision 0.83–1.0 and recall
0.63–1.0: its misses are two-repeat runs, which a quiet stock prints now and
then. A rate's quarter-point move that half-reverts the next day is reported on
at most one series a universe, and the gate allows one.

### M5: Context inputs (v0.5.0)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0501 | `--actions` reader; the explained_split hypothesis; the "split between bars" rule | M | 🔄 awaiting merge |
| DR-0502 | DQ701–DQ705 | L | 🔄 awaiting merge |
| DR-0503 | `--meta` reader, profile matching, `peer_group` sibling evidence (DQ601) | M | 🔄 awaiting merge |
| DR-0504 | `--market` reference series: rolling beta, scoring on residual returns, DQ602 market-day widening | M | 🔄 awaiting merge |

**As built** (doc/context.md is the reference):

- **The context files** are CSV or TSV under dorq's column names or fafnir's
  (`core.corporate_action`, `core.security`), matched to series by id, or else by
  label. A malformed file stops the run with exit 3.
- **`explained_split`** has prior 0.9 (`[priors] explained_split`) where it
  applies: only to a bar with a split on file between it and the bar before. The
  plan's 0.10 "taken from market_move" assumed it applied to every candidate.
  It shares every term but the return with `unreported_split`, so the
  plausibility gate is the existing `plausible_level` term.
- **DQ701–DQ704 are one model** over the bars within 20 sessions of each split on
  file: confirmed, misdated (within ±5 sessions), not in the bars, the wrong
  ratio (inverted, weight 0.3, or another clean ratio), or applied twice. They
  share `split_on_file_error` (0.06). A move a DQ70x report accounts for is not
  reported again as DQ203. A zero-volume bar that moves by a split's ratio across
  its ex-date is not a DQ403 move.
- **DQ705** is deterministic: a dividend at or above the close before it (error),
  or one whose amount *and* yield are both ten times off the median of at least
  three others (warn). The amount alone flagged every dividend across an
  unreported split or a scale era.
- **DQ601** is judged at the end of the run, like DQ303: three or more series
  moving by the same clean split ratio on the same date, classified as a family
  split (one `peer_group`) or a mass adjustment. Within a peer group, siblings
  moving by the same ratio are a Bayes factor of 100 for `unreported_split`; a
  move that becomes a DQ203 replaces the series' own report of that bar. Moves a
  split on file explains, moves DQ70x claims, and the provisional newest bars do
  not count.
- **`--market`**: returns are taken net of a rolling beta (a discounted
  regression either side of each bar, shrunk toward the series' own beta, shrunk
  toward 1), and the ordinary move widens by half the market's move (a beta
  error) on a big market day. DQ602 reports market days of five standard
  deviations or more at info. Deriving a market from a broad cross-section, with
  no `--market`, needs every series read first, which streaming rules out; it is
  not done.
- **`tick_size`** from `--meta` replaces the inferred tick in the price model,
  DQ107 and DQ403. The class prior does not yet use `asset_type`: that waits for
  `dorq calibrate` (M6) to fit per-class tables.

**Result:** dorq-synth writes `actions.csv`, `meta.csv` and `market.csv`. The
actions file holds splits on file the bars show, three of them between two
stored bars of a thin name, and faulty splits: misdated, not in the bars, at the
wrong ratio, and applied twice. It also holds quarterly dividends, five of them
wrong. A family of four liquid names splits together, unreported. The gate now
runs with all three files. On seeds 1–8:

- DQ701, DQ702 and DQ705 are at precision and recall 1.0. So is DQ601, the family
  found once a universe with every member a DQ203.
- DQ703 has recall 0.75–1.0: a mismatched ratio within a few percent of the
  ratio on file, plus a large ordinary move, can pass as the split on file.
- DQ704 has recall 0.67–1.0: in a volatile spell a second move of the ratio is
  less surprising.
- No split on file is reported, at its ex-date or between bars.

The synthetic market is mild (a 10–15% crash), so `--market` changes little
here: thin-name hard negatives move by one either way per universe, and the gate
allows three.

### M6: Calibration and evaluation (v0.6.0)
| ID | Item | Size | Status |
|---|---|---|---|
| DR-0601 | The label schema (JSONL) and the ingestion of reconstructed pre-repair windows | M | ⬜ |
| DR-0602 | `dorq calibrate`: marginal-likelihood fit of π_H and hyperparameters, isotonic map, `priors.toml` output | L | ⬜ |
| DR-0603 | `tools/eval`: per-check precision and recall, reliability diagrams, precision@k, reports in HTML and markdown | M | ⬜ |
| DR-0604 | Performance pass: benchmarks, profiling, and the runtime target of §8 on a 150M-row synthetic set | M | ⬜ |

### M7: fafnir integration (fafnir v-next, see §7)
| ID | Repo | Item | Size | Status |
|---|---|---|---|---|
| DR-0701 | fafnir | `fafnir dq export-labels`, then the one-time labelling pass (operator plus agent) | M | ⬜ |
| DR-0702 | fafnir | `dq/dorq.py`: export, run, temp-table ingest with the open and accepted guards; `fafnirrc` keys | L | ⬜ |
| DR-0703 | fafnir | `--engine`/`--shadow`, `fafnir dq compare` | M | ⬜ |
| DR-0704 | dorq | Calibrate on the fafnir labels and ship `priors/fafnir.toml` | S | ⬜ |
| DR-0705 | fafnir | Recheck by rerun-and-negate for the `dorq_*` checks; tests | M | ⬜ |
| DR-0706 | fafnir | Skill and playbook updates, tiers, `NEVER_AUTO_RESOLVE`, the test pinning the two | M | ⬜ |
| DR-0707 | fafnir | Shadow period (≥ 20 sessions), a go/no-go against §8, cutover, retiring the SQL checks | M | ⬜ |

### M8: Stretch
- DQ801 cross-vendor disagreement (FMP vs Sharadar) for the parallel run.
- A gradient-boosted re-ranker, only if there are ≥ 1,000 labels and it beats M6 on held-out precision.
- Parquet/Arrow input; a pybind11 module so `duk` can call dorq in-process; support for intraday bars.

**Rough effort:** M0–M3 ≈ 4 weekly sprints; M4–M6 ≈ 3 sprints; M7 ≈ 2 sprints including the shadow
period. That is about 9 one-week sprints to cutover, with implementation by Claude Code and operator
time mainly in M7 (labelling, go/no-go).

---

## 10. Open questions (answered 2026-10-05)

The defaults below were confirmed as the decisions. Q9 was answered differently from its default.

| # | Question | Decision |
|---|---|---|
| Q1 | Should dorq stay a pure file/stdin tool, or also read Postgres directly (libpq)? | Pure. fafnir exports and pipes (§7). This keeps dorq stateless, testable, and usable outside fafnir |
| Q2 | Should dorq **replace** fafnir's `outlier`/`gap`/`sparse_coverage`/`stale`, or sit beside them? | Shadow first, then replace at cutover (§7.8) |
| Q3 | Should dorq see **raw** bars plus actions, or **adjusted** bars? | Raw plus `--actions`. Only that combination can find misdated, fabricated, or doubled splits |
| Q4 | May fafnir's dispositions (`accepted_note`, `resolution_note`, `ops.operator_override`) be used as ground truth? | Yes, after a one-time labelling pass (§6) |
| Q5 | **Is dorq public?** Fixtures built from FMP bars (AKR, EQC, RUSS…) probably can't be redistributed under FMP's terms | Public-safe: dorq's tests use only synthetic replicas of those shapes; real-data regression cases live in fafnir's test suite |
| Q6 | Runtime budget and the fafnir host's CPU and RAM? Nightly incremental or full? | Incremental nightly with a 260-session lookback, full weekly; the targets in §8 |
| Q7 | Asset classes and frequencies in scope for v1: US equities, ETFs, and mutual funds only? Crypto, FX, intraday? Which point series (FRED rates, NAVs, fundamentals)? | Daily US equities, ETFs, and funds, plus point series at D/W/M/Q. Crypto and FX via the `24x7` calendar only. No intraday |
| Q8 | Is precision really preferred over recall? | Yes at `warn`/`error`; recall is preserved at `info` and by the 50% screening floor |
| Q9 | Distribution: GitHub release tarballs, a conda package (fafnir uses `environment.yaml`), or both? | **GitHub release binaries** (Linux x86-64, macOS arm64), **or building from source on the fafnir host**. No conda package |
| Q10 | Check-code style: `DQ201` plus a kebab-case name, as proposed, or flake8's single letter plus number? | `DQ` + 3 digits plus a name |
