# ADR 0001: dorq is a stateless, deterministic linter

- Status: Accepted
- Date: 2026-10-05
- Related: [development plan §1.3, §7](../plans/dorq-development-plan.md),
  [ADR 0003](0003-check-codes.md)

## Context

dorq's first user is fafnir. fafnir's data-quality queue (`ops.data_quality_flag`)
is stateful. It deduplicates (a condition already open is not written again), it
remembers decisions (an *accepted* condition is never re-raised), and it closes
flags by re-evaluation (`dq recheck` re-runs a check's own predicate and closes
what it no longer selects). All three behaviours rest on a check giving the same
answer about the same data every time it is asked.

A tool could hold some of that state itself, with a baseline file, a cache of what
it reported last night, or a database connection. Each of those would make the
answer to "why was this flagged?" depend on something other than the data and the
settings.

## Decision

**dorq is a pure function from (input, configuration, binary) to output.**

1. **No state between runs.** No database connection, no network access, no cache,
   no history file. Deduplication, acceptance and resolution belong to the caller
   (fafnir), which already does them.
2. **No side effects on data.** dorq reads its input and writes violations to
   stdout. It never modifies, repairs or deletes anything. A violation can carry a
   `suggested_action`, and the caller decides whether to apply it. In this respect
   dorq is `flake8`, not `black`.
3. **Deterministic output.** For a given binary, input and configuration, the
   output is byte-identical:
   - no random numbers (if an algorithm ever needs them, the seed is part of the
     configuration);
   - output is sorted by series, date and code before it is written, so thread
     count and scheduling cannot change it;
   - reductions over many series run in a fixed order;
   - nothing in the output depends on the clock, locale or environment. The as-of
     date is an input (`--as-of`), not `now()`;
   - floating-point contraction is off (`-ffp-contract=off`), and `-ffast-math` is
     never used.
4. **Traceable output.** Every machine-readable record will carry the dorq version
   and a hash of the effective configuration (from M1). That way any flag can be
   traced to the code and settings that raised it.

## Scope of the guarantee

The guarantee is **per binary**, not across platforms. Different C libraries
(glibc vs Apple's libm) round transcendental functions such as `log`, `exp` and
`lgamma` differently in the last bits. So a probability computed on Linux and on
macOS can differ in its final digits, and in rare cases fall on different sides of
a threshold. Production runs therefore use one platform's binary, and every record
says which version produced it. Cross-platform agreement is tested within a stated
tolerance, not byte for byte.

## Consequences

- **fafnir can implement `dq recheck` for dorq checks by re-running dorq** over the
  affected series and closing any flag dorq no longer emits. That is only correct
  because of decision 3.
- **Incremental runs need their history as input.** A nightly run passes a lookback
  window together with `--since`. dorq cannot remember yesterday's run.
- **Cross-sectional checks need the whole universe in one invocation**: failed-load
  cohorts, sibling funds splitting together. The engine streams series one at a
  time and keeps per-date summaries, so this stays within bounded memory
  ([plan §5.3](../plans/dorq-development-plan.md)).
- **Tests can assert exact output.** Golden files and a "same output at 1 and N
  threads" test are possible, and both are required.
- dorq is usable outside fafnir on any CSV or JSON file, with no setup.

## Alternatives considered

- **A baseline file of accepted violations** (like the baselines of some linters):
  this duplicates fafnir's `accepted_at`, and the two copies would drift. It may be
  added later for standalone users, as an input that only filters output. The
  checks themselves would never read it.
- **Direct database access (libpq):** this would remove the export step, but it
  ties dorq to fafnir's schema, adds a runtime dependency and credentials, and
  makes runs depend on database state at the moment of reading. Rejected for v1
  (plan, open question Q1).
