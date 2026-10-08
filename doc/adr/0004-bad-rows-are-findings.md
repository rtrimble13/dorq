# ADR 0004: A bad row is a finding; only unreadable input stops the run

- Status: Accepted
- Date: 2026-10-08
- Related: [ADR 0001](0001-stateless-deterministic-linter.md),
  [doc/context.md](../context.md), [DQ104](../checks/DQ104.md),
  [DQ109](../checks/DQ109.md)

## Context

dorq's first run on the fafnir host (0.7.0) stopped with exit status 3 before it
read a bar. The actions export held dividends of zero, which fafnir's
`core.corporate_action` allows (`dividend_amount >= 0`) and dorq's actions reader
refused (it wanted a positive amount). fafnir treats any non-zero status as a
failed run, so one row cost the night every check on every series.

The input contract was already split. A bad row of the bars was a finding: DQ104
reported it, the row was skipped, and the run went on. A bad row of a context file
(`--actions`, `--meta`) was fatal. Context files are exports of other tables, whose
constraints are not dorq's, so they are where a row dorq cannot use is most likely
to turn up.

## Decision

1. **Exit status 3 means a file could not be read as a whole**: it cannot be
   opened, is empty, lacks a required column, is not valid CSV or JSON, or (when
   streamed) has a series split across the input. Nothing useful can be checked
   from it.
2. **A row, or a field, that cannot be used is a finding.** It is reported, skipped,
   and the run checks everything else: DQ104 for the input, DQ109 for a context
   file. A skipped row is never silent: `dorq calibrate`, which reports no
   violations, warns on stderr.
3. **A row that can be read but is implausible is not an unusable row.** It is read,
   and the check that owns it judges it: a dividend of zero or less is DQ705's.
   The reader accepts what the source table accepts.
4. **References stay strict.** The reference calendar and the labels file still
   stop the run on a bad row. A calendar row decides the sessions of every series,
   so skipping one would turn a single bad row into DQ105 and DQ3xx reports across
   the input. Labels feed an operator-run fit, where a dropped label would bias the
   fit without anyone seeing it.

## Consequences

- A context file's problems reach fafnir's queue (`dorq_bad_context_row`,
  `dorq_dividend_implausible`) instead of its error log, under the security they
  belong to.
- A skipped split leaves the price model without it, so its move may also be
  reported as DQ203. DQ109 is an error for an actions row for that reason.
- A DQ109 report names the context file without its directory: fafnir writes its
  exports to a new temporary directory each run, and ADR 0001 needs the same
  output for the same data.

## Alternatives considered

- **Keep the reader strict and have fafnir filter its export** (`dividend_amount >
  0`): hides the rows rather than reporting them, and every other caller would
  meet the same wall.
- **Skip bad rows with a warning on stderr:** fafnir reads stderr only when dorq
  fails, so the warnings would be lost, and nothing would reach the queue.
