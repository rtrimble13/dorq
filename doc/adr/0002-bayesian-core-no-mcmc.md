# ADR 0002: A Bayesian core with closed-form inference, no MCMC

- Status: Accepted
- Date: 2026-10-05
- Related: [development plan §1, §4, §6](../plans/dorq-development-plan.md)

## Context

fafnir's checks are fixed thresholds: a 50% close-to-close move, 80% session
density, two sessions behind. Thresholds detect well but cannot tell a data error
from a market fact. A 60% drop on a failed drug trial and a close stored at 1/20 of
its price both cross the line. Telling them apart has been done by hand, using
playbooks such as fafnir's `outlier-classification.md`. Those playbooks reason by
elimination over a few explanations: a split between the bars, a spike that
reverts, a clean ratio with an inverse volume shift, a one-tick move on a sub-dime
price. The record shows the cost: 599,808 gap flags that buried about 29 real
problems, and a triage session in which most outlier flags closed as market facts.

Whatever replaces the thresholds has to meet these requirements:

- **Calibrated probabilities**, so a severity cutoff means the same thing for every
  check, and fafnir can rank its queue.
- **Explanations**, because operators write a note for every disposition.
- **Few labels needed.** fafnir has hundreds of labelled dispositions, not hundreds
  of thousands.
- **Speed.** About 150M bars must be scanned in minutes, and a nightly incremental
  run in seconds.
- **Domain knowledge must be expressible**, because the playbooks are the best
  available knowledge.

## Decision

1. **Each candidate observation is scored by comparing explicit hypotheses**:
   market move, bad print, unreported split, scale error, explained split, tick
   move. Each has a prior and a likelihood. The posterior gives `p_error`, and the
   most probable error hypothesis gives the violation's code.
2. **Inference is closed-form or online, O(n) per series.** Discounted
   Normal-Inverse-Gamma volatility gives Student-t predictives. The coverage model is
   Beta-Bernoulli with a two-state HMM, solved by forward-backward. A
   mixture-of-Gaussians prior over clean split ratios scores level shifts. Hyperpriors
   per class of security come from a first empirical-Bayes pass over the input. There
   is no MCMC and no variational inference at runtime.
3. **A cheap screen runs first.** Only candidates the screen selects get full
   scoring. The screen always includes every move of 50% or more, so dorq cannot
   miss what fafnir's current check catches.
4. **Every violation carries its evidence**: one log Bayes factor for each evidence
   term (return, reversion, volume shift, OHLC consistency, corporate action, peers).
5. **Priors and a few scale hyperparameters are calibrated from fafnir's own
   dispositions** with `dorq calibrate`. A monotone (isotonic) map then corrects the
   final probabilities.
6. **Other machine learning is admitted only as a second stage.** A gradient-boosted
   re-ranker over the Bayesian evidence vector may be added once there are about
   1,000 labels, and only if it beats the calibrated model on held-out precision.

## Consequences

- dorq runs fast enough for a nightly full scan on one host, and its output can be
  explained term by term in a resolution note.
- **The evidence terms are treated as independent.** That is a naive-Bayes
  simplification, and it makes raw posteriors overconfident when terms are
  correlated (a volume surge and a level that holds often come together).
  Calibration (decision 5) is therefore required, not optional, and calibration
  quality is an acceptance criterion (expected calibration error ≤ 0.05).
- **The approximations need offline validation.** A full hierarchical model in Stan,
  run in research on a sample, is the yardstick for the closed-form posteriors. It
  is never part of dorq.
- **Adding a check means writing down its hypotheses and likelihood terms**, not
  just a threshold. That is more work per check, and it is where the precision
  comes from.

## Alternatives considered

| Alternative | Why not |
|---|---|
| Keep fixed thresholds, tune them | Tuning trades recall for queue size. fafnir's own comments warn against moving `GAP_MIN_SESSION_DENSITY` for exactly this reason |
| Isolation Forest, LOF, one-class SVM | These answer "is this unusual?", which is the question already over-answered. Their scores are uncalibrated and hard to explain |
| LSTM or transformer autoencoders | Need a lot of data, are opaque, poorly calibrated, and heavy at runtime for daily bars |
| Full hierarchical MCMC at runtime | Orders of magnitude too slow for 150M bars nightly. Kept as an offline yardstick |
| Supervised gradient boosting only | Too few labels to start with, and its probabilities are uncalibrated. Kept as an optional stage 2 |
