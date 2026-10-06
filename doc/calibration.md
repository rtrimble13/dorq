# Calibration

The price model's priors ship as reasonable guesses: how often a large move is a
bad print, a scale error or an unreported split rather than a real move.
`dorq calibrate` replaces those guesses with what labelled history shows (plan
section 6, DR-0602). It writes a TOML file that a config file includes.

```bash
dorq calibrate prices.csv --labels labels.jsonl --restore before.csv \
     --actions actions.csv --meta meta.csv --market spy.csv \
     --holdout 0.3 --out priors.toml
echo 'include = "priors.toml"' >> dorq.toml
```

The input options are the same as for `dorq check`: the files, `--config`,
`--isolated`, `--columns`, `--kind`, `--input-format`, `--threads`, the calendar
options, `--actions`, `--meta` and `--market`. Profiles apply as they do for a
check. A series' price settings and priors are the global ones with its matching
profiles' patches applied. The output is the same for any `--threads`.

| Option | Meaning |
|---|---|
| `--labels F` | Required. The labels ([labels.md](labels.md)) |
| `--restore F` | Bars as they stood before repair ([labels.md](labels.md#series-as-they-stood-before-repair---restore)) |
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
   ([labels.md](labels.md#what-the-price-model-learns-from-a-label)).
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
# Written by `dorq calibrate` (dorq 0.6.0). Include it from a
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

## Re-calibrating

Re-run the fit when the labels have grown by about half, or when an evaluation
shows the reliability diagram drifting off the diagonal. Keep the labels file
and the written TOML under version control together. The `version` names the
pair.
