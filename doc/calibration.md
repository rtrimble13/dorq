# Calibration

The price model's priors ship as reasonable guesses: how often a large move is a
bad print, a scale error or an unreported split rather than a real move. The
statement models' do too ([Statements](#statements)).
`dorq calibrate` replaces those guesses with what labelled history shows (plan
section 6, DR-0602). It writes a TOML file that a config file includes.

```bash
dorq calibrate prices.csv --labels labels.jsonl --restore before.csv \
     --actions actions.csv --restore-actions before-actions.csv \
     --meta meta.csv --market spy.csv --holdout 0.3 --out priors.toml
echo 'include = "priors.toml"' >> dorq.toml
```

The input options are the same as for `dorq check`, and every one that changes
what the model sees is taken: the files, `--config`, `--isolated`, `--columns`,
`--kind`, `--input-format`, `--threads`, `--calendar`, `--calendar-file`,
`--calendar-exchange`, `--actions`, `--meta` and `--market` (all since 0.6.0 or
0.7.0), and `--restore` and `--restore-actions`. Give calibrate the same
calendar, actions and metadata the checks run with: a split or dividend on file,
a NAV fund's metadata and the session calendar all change how a move is scored.
Profiles apply as they do for a check. A series' price settings and priors are
the global ones with its matching profiles' patches applied. The output is the
same for any `--threads`.

fafnir exports the labels with the bars and the actions as they stood before
its repairs, and runs the fit with the context its nightly check passes:

```bash
fafnir dq export-labels --out labels.jsonl --restore before.csv \
     --restore-actions before-actions.csv
dorq calibrate bars.csv --labels labels.jsonl --config priors/fafnir.toml \
     --calendar-file calendar.csv --calendar-exchange NASDAQ \
     --actions actions.csv --meta meta.csv \
     --restore before.csv --restore-actions before-actions.csv \
     --holdout 0.3 --out fafnir-calibration.toml
```

| Option | Meaning |
|---|---|
| `--labels F` | Required. The labels ([labels.md](labels.md)) |
| `--restore F` | Bars as they stood before repair ([labels.md](labels.md#series-as-they-stood-before-repair---restore)) |
| `--restore-actions F` | Corporate actions as they stood before repair, over `--actions` ([labels.md](labels.md#actions-as-they-stood-before-repair---restore-actions)) |
| `--out F` | Where to write the fitted settings. Without it they go to stdout and the summary goes to stderr |
| `--name T` | The `[calibration] version`. The default is the labels file's stem |
| `--holdout X` | Hold out this share of series (0–0.9), chosen by a hash of the series id, and report on them |
| `--clean-unlabelled` | Count a scored move that matches no label as a market fact. Use this only when every fault is labelled, as with dorq-synth |
| `--no-grid` | Keep `jump_prob`, `jump_scale` and `ratio_tolerance` as configured |
| `--no-isotonic` | Write no `p_error_map` |

## What it fits

1. **Observations.** dorq scores every series under the effective settings,
   exactly as `dorq check` would. Each scored move within 3 calendar days of a
   usable label becomes an observation. An observation records the move's
   log-likelihood under each hypothesis and the hypotheses the label allows.
   Labels about checks other than the price model are left out
   ([labels.md](labels.md#what-the-price-model-learns-from-a-label)). Every
   label is used, whatever its `applies_until` or `verified`: the fit runs on
   the series as they stood (`--restore`), which is what the labels describe.
2. **Priors.** The objective is the sum over observations of
   log P(the label's hypotheses | the evidence). It is the labels' marginal
   likelihood given the priors. A Gaussian penalty keeps each log prior within
   about 1.5 e-folds of its configured value, so a handful of labels cannot drive
   a prior to zero. The objective is maximised by gradient ascent with
   backtracking.
3. **Hyperparameters.** Step 2 is repeated for 36 settings: `jump_prob` in
   {0.01, 0.02, 0.03, 0.05}, `jump_scale` in {4, 6, 8} and `ratio_tolerance` in
   {0.005, 0.01, 0.02}. The configured setting is tried too. The setting with the
   best mean penalized log-likelihood per observation wins. Different settings
   score slightly different sets of moves, so the mean is used rather than the
   sum.
4. **The p_error map.** The fitted model's `p_error` on the training observations
   is regressed onto whether each one was an error. The regression is isotonic:
   up to 20 equal-count bins, then pool adjacent violators. The result is a
   rising list of knots. dorq interpolates linearly between knots and holds the
   end values beyond them. `p_error` stays monotone in the evidence, so the
   ranking of findings does not change. What changes is the number reported and
   the severity it maps to.

Determinism holds throughout. The series are scored in input order, the fit
starts from the configured priors, and knots are rounded to six decimals. The
same input, labels and configuration therefore give the same file.

## The output

```toml
# Written by `dorq calibrate` (dorq 0.10.0). Include it from a
# config file: include = "priors.toml". See doc/calibration.md.
#
# labels: 499, of which 413 about price moves and 240 matching a scored move
# observations: 574 (91 errors), 0 held out
# settings tried: 37
# mean log P(label | evidence), training: -0.1680 before, -0.1274 after
# expected calibration error of p_error: 0.0199 before, 0.0080 after

[price]
jump_prob = 0.05
jump_scale = 8
ratio_tolerance = 0.02

[priors]
market_move = 0.8815
...

[calibration]
version = "labels"
p_error_map = [
  [0.000005, 0.000000],
  ...
]
```

The expected calibration error is the mean |mean p_error − observed error rate|
over ten equal-width bins of `p_error`, with each bin weighted by its share of the
observations. With `--holdout` it is measured on the held-out series, and without
it on the training ones. The "before" figure applies the configured
`p_error_map`, if there is one.

## Using it

`include` takes a path, or a list of paths, relative to the including file. The
included files are read first, so the including file's own keys still win
([configuration.md](configuration.md#include)). `dorq config show` prints the
effective settings and their hash, so a calibration change shows as a config change. In
fafnir, `detail.dorq.config_hash` then records which calibration produced each
flag.

On dorq-synth, a fit on seed 1 applied to seeds 2–6 removes every report on the
thin-name and rate hard negatives. DQ201 keeps a precision of 1.0. Precision at
warn on seed 2 rises from 0.71 to 0.83. To measure a fit on your own labels, run
[dorq-eval](../tools/eval/README.md) before and after.

## Statements

`dorq calibrate` on a statements input fits the statement checks to labels about
statements (DR-0911). It takes the same options as `dorq check` on statements:
`--items` and `--as-of` as well as `--actions` and `--meta`. `--restore` takes
a statements restore file. One input holds one kind, so bars and statements are
fitted in separate runs.

```bash
fafnir dq export-labels --out labels.jsonl --restore-statements before-statements.csv
dorq calibrate statements.csv --labels labels.jsonl --items items.csv \
     --actions actions.csv --meta meta.csv --as-of 2026-11-02 \
     --config /etc/fafnir/dorq-statements.toml --restore before-statements.csv \
     --holdout 0.3 --out fafnir-statements-calibration.toml
```

1. **Candidates.** The statement checks run as `dorq check` runs them, with every
   candidate reported, at any probability. The value model's real changes are
   reported too. A candidate is an observation when a statements label covers it
   ([labels.md](labels.md#labels-about-statements)), by series, statement,
   period type, version and item. A fault label must be within 3 days of the
   candidate, and a market fact on the candidate itself. With
   `--clean-unlabelled`, a candidate no label covers is innocent.
2. **Priors.** Three models' priors are fitted as the price model's are, to the
   labels' marginal likelihood with a Gaussian penalty about the configured
   values:

   | Model | Codes | Priors |
   |---|---|---|
   | values | DQ911, DQ912, DQ913, DQ919 | `bad_value`, `unit_error`, `sign_error` (a real change has the rest) |
   | identities | DQ903 | `identity_structural`, `identity_reclassification`, `identity_field_error`, `identity_unexplained` (relative weights) |
   | filings | DQ931 | `filing_late`, `filing_early`, `filing_amendment` (an ordinary filing has the rest) |

   Each observation's evidence under each hypothesis is read from its
   posterior over its prior. A fault label names the error it allows by its
   code: a DQ912 label on a DQ911 candidate is the value model's `unit_error`.
   A row's or an era's DQ912 is left out, since its posterior is not the value
   model's. With 30 held-out observations or more, the penalty's width is
   chosen from {0.25, 0.5, 1, 1.5, 3} e-folds by their log-likelihood.
   Otherwise it is 1.5.
3. **The p_error maps.** Each sub-family (DQ90x, DQ91x, DQ92x, DQ93x, DQ94x)
   gets an isotonic map of its own, fitted on the codes that apply it: DQ903,
   DQ904, DQ911 to DQ916, DQ919, DQ921, DQ922, DQ931, DQ932 and DQ942. The
   rules with a fixed `p_error` (DQ901, DQ902, DQ941, DQ943, DQ944) and DQ95x
   are not mapped.
   - The regression is pool adjacent violators over the training observations
     themselves, not over bins. A statements fit sees thousands of innocent
     candidates near 0 and a few faults near 1, and an equal-count top bin
     would pool them.
   - Each block is a knot at its mean `p_error`, as for the price model, with
     at most 40 knots. Its rate is shrunk toward that mean by two
     pseudo-observations, so that a few labels cannot pin a range at 0 or 1.
   - A fitted model's candidates are mapped from their `p_error` under the
     fitted priors.
   - A candidate that only a fault label naming another code covers is left
     out. The fault is real, but whether it is the thing reported is not
     known; dorq-eval leaves such a report out of its reliability diagram too.
   - A sub-family is mapped only with 20 training observations, among them 2
     errors and 2 innocent ones.

The output is a `[statements.priors]` table, with the three models' keys, and
`[statements.calibration]`, with the fit's summary in comments:

```toml
# values: 1110 observations (25 errors, 281 held out), spread 0.5; mean log P(label | evidence) -0.0384 before, -0.0339 after; held out -0.0066 before, -0.0062 after
# DQ91x: 1141 observations (56 errors); expected calibration error 0.0027 before, 0.0024 after
# ...

[statements.priors]
bad_value = 0.0008877
unit_error = 0.0005827
...

[statements.calibration]
version = "labels"
dq90 = [
  [0.001024, 0.000004],
  [0.886028, 0.997495],
]
...
```

Include it from the statements config as the bars fit is included
([configuration.md](configuration.md#include)). On dorq-synth, a fit on
seed 1 (`--clean-unlabelled --holdout 0.3`) and applied to seeds 2 to 20:

- lowers dorq-eval's expected calibration error over the statement checks
  with a posterior on every seed (seed 2: 0.007 to 0.004; the largest, seed
  6: 0.011 to 0.007);
- raises precision at warn and above on seed 5 (0.989 to 0.996) and leaves it
  as it was on every other seed: 1 on fifteen, and 0.996, 0.992 and 0.996 on
  seeds 14, 16 and 18;
- lowers no seed's recall, and raises seed 9's (0.977 to 0.984);
- and brings the seeds that failed tools/synth/gates.txt from three to one.
  Seed 14's DQ915 precision is 0.8 against a gate of 0.9, before the fit and
  after it.

CTest's `synth.statements-calibrate` holds the seed 1 to seed 2 case. On a
warehouse, measure the fit with dorq-eval on held-out labels, before and after.

## Re-calibrating

Re-run the fit when the labels have grown by about half, or when an evaluation
shows the reliability diagram drifting off the diagonal. Keep the labels file
and the written TOML under version control together. The `version` names the
pair.
