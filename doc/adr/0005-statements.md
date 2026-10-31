# ADR 0005: Statements are a third series kind, with the DQ9xx family

- Status: Accepted
- Date: 2026-10-31
- Related: [ADR 0001](0001-stateless-deterministic-linter.md),
  [ADR 0003](0003-check-codes.md), [the fundamentals plan](../plans/dorq-fundamentals-plan.md)
  (M9, DR-0901), fafnir's ADR 0018 (bitemporal fundamentals)

## Context

fafnir holds Sharadar SF1's financial statements: three tables of line items, one
row per version of each period, on three timelines. A row describes a period (its
`fiscal_date`), is one version of it, and holds over a knowledge interval
(`known_from` to `known_to`, half-open): the dates the market could know that
version. dorq reads bars and point series, one value or bar per date. A statement
is neither. A period has several versions, a version has dozens of values, and
what a backtest reads on a date depends on which version was in force then.

dorq is to check these statements as it checks bars (the fundamentals plan, §1),
and its check codes need a family for them (ADR 0003).

## Decision

1. **Statements are a third series kind**, `statements`, beside `ohlcv` and `point`.
   - An input row is one version of one statement of one period, keyed by
     statement, period type, fiscal date and version.
   - Its knowledge fields (`known_from`, `known_to`, `knowledge_source`,
     `filing_date`, `loaded_on`) are columns of their own.
   - Every other column is a line item, by its own name. An empty value is NULL
     ("does not apply") and is kept apart from 0.
   - `--kind statements` names the kind. It is detected when the columns include a
     statement, a period type and a fiscal date.
   - A series is still one security, and the input is still grouped by series.
     So streaming, bounded memory and the order of results (ADR 0001) hold as they
     do for bars.
2. **DQ9xx is the statement checks' family**, and its tens digit is a sub-family:

   | Codes | Sub-family |
   |---|---|
   | DQ90x | integrity and accounting identities |
   | DQ91x | values over time |
   | DQ92x | share counts and share terms |
   | DQ93x | the knowledge timeline and revisions |
   | DQ94x | coverage and freshness |
   | DQ95x | cross-sectional |

   `--select DQ9` picks every statement check, and `--select DQ91` one sub-family.
   Prefix selection already works at any length.
3. **A check applies to the kinds it was written for.** The bars and point checks
   do not run on statements (DQ103 would read every restatement as a duplicate
   date), and the statement checks run on nothing else. DQ104 (a field that does
   not parse) runs on every kind, and DQ109 (a context row) on every context file.
4. **Checks read three views** of a series' rows:
   - **every version;**
   - **in force at the as-of date:** each period's version with `known_from` on or
     before `--as-of` and `known_to` after it. With no `--as-of`, each period's
     open version (no `known_to`). On an input that holds every current version,
     that is the view at its latest knowledge date. It also needs no other series,
     so a streamed run can build it;
   - **as first reported:** each period's earliest version, normally version 1.
     That is what a backtest dated before a restatement reads.
5. **The taxonomy is an input; the judgements are configuration.** What an item is
   (its statement, class and sign convention) comes from a context file, `--items`,
   in the shape of fafnir's `ref.statement_line_item`. Its content goes into
   `config_hash`, as a reference calendar's does. The identities, tolerances and
   lists of items that are never negative or never absent are a `[statements]`
   table in the config. `config_hash` covers that table only where it differs from
   its defaults, so the release that added it changed no existing hash.
6. **`--since` on statements goes by knowledge, not by period.** A statement
   finding is kept unless every version it names was known (and loaded) before the
   date. A restatement of a 2015 period, loaded last night, is new.

## Consequences

- **dorq stays vendor-neutral.** SF1's names and identities live in fafnir's export
  and in the shipped `priors/fafnir-statements.toml`, not in the binary.
- **One engine.** Statements reuse the reader, the series assembler, the worker
  pool, the result order, the writers and DQ104. The statement checks see a series
  as the other checks do, with the views beside it.
- **A bars run is unchanged.** Nothing in a bars run reads a statement field, and a
  test holds the bars output on a dorq-synth universe to fixed hashes.
- **DQ9xx is spent.** ADR 0003's last free family now has a meaning. A fourth input
  kind would need a new decision about codes. New sources of bars or point series
  reuse the existing families.
- **Statements and bars are separate runs.** One input file holds one kind, and
  fafnir runs statements as a separate invocation with its own config file.

## Alternatives considered

- **A separate statements tool, or a separate engine in dorq.** It would duplicate
  the reader, the assembler, the threading, the writers and the determinism rules,
  for no gain. The series kind is the seam that already exists.
- **Each item as a point series** (`security:item`). This gives no versions and no
  knowledge dates, and no checks across items. Accounting identities, unit errors
  across a whole row, and share counts against splits all need the row.
- **One row per period, with the three statements joined.** The three statements of
  a period become known on different days and carry their own versions (fafnir's
  ADR 0018). A join would have to pick a version of each.
- **Long format** (one row per item value). This is 58 times the rows, and every
  check would rebuild the row.
- **Statement checks in the existing families** (identities in DQ1xx, gaps in
  DQ3xx). `--select DQ1` would then mix two kinds of data that operators triage
  separately, and the families' meanings (price action, volume) would blur.
- **Four-digit codes, or a new prefix** (`DF01`). These break ADR 0003's "DQ and
  three digits", which fafnir's check names and every selection already rely on.
