# ADR 0003: Check codes and names

- Status: Accepted
- Date: 2026-10-05
- Related: [development plan §3](../plans/dorq-development-plan.md),
  [ADR 0001](0001-stateless-deterministic-linter.md)

## Context

Each check needs an identifier. Users select and ignore checks by it, fafnir
stores it in `ops.data_quality_flag.check_name` (where it keys deduplication and
acceptance), and documentation and notes refer to it. Once it is in a database
column, or in an operator's note that says "accepted DQ203 because…", changing it
breaks history.

Flake8's style is a letter plus a number (`E501`). Its letters are claimed by
flake8 and its plugins, and one letter gives few families.

## Decision

1. **Every check has a code and a name.**
   - The code is `DQ` followed by three digits, e.g. `DQ203`.
   - The name is kebab-case and describes the problem, e.g. `unreported-split`.
2. **The hundreds digit is the family:**

   | Family | Checks |
   |---|---|
   | DQ1xx | integrity: structure and field validity |
   | DQ2xx | price action |
   | DQ3xx | coverage and sparsity |
   | DQ4xx | volume |
   | DQ5xx | stale values |
   | DQ6xx | cross-sectional (needs more than one series) |
   | DQ7xx | corporate actions (needs `--actions`) |
   | DQ8xx | cross-vendor |
   | DQ9xx | reserved |

3. **Codes are permanent.** A code is never renumbered and never reused. A retired
   check keeps its code as *reserved* in the registry and in `doc/checks/`, and
   still shows in `dorq list-checks --all`.
4. **Names are stable too.** Renaming one keeps the old name as an alias for at least
   one minor release, and is a breaking change from 1.0.0 on.
5. **`--select`, `--ignore` and the config file accept** a full code (`DQ203`), a
   prefix (`DQ2`, `DQ`), or a name (`unreported-split`).
6. **The fafnir check name is derived, not chosen.** It is `dorq_` followed by the
   name with hyphens turned into underscores, e.g. `dorq_unreported_split`. This
   keeps fafnir's `check_name LIKE 'dorq_%'` and its existing glob conventions
   working.
7. **Each code has a page**, `doc/checks/DQxxx.md`, covering what the check detects,
   the hypotheses and evidence it uses, its configuration keys, its inputs, and
   examples. `dorq explain DQxxx` prints the same page; it is compiled into the
   binary.
8. **New codes take the next free number in their family.** The numbers in the plan
   are provisional until each check is implemented. From then on they are permanent.

## Consequences

- `--select DQ2` turns on a whole family, which is how a user will usually think
  ("price checks only").
- A flag in fafnir's queue stays meaningful for as long as the queue keeps it,
  because neither the code nor the derived `check_name` ever means anything else.
- One hundred codes per family is more than enough room. DQ9xx is held back rather
  than given a meaning before one is needed.

## Alternatives considered

- **Flake8-style letters** (`P201`, `C301`): collides with other tools' letters,
  and gives fewer families than digits.
- **Names only:** cannot select a family, and names are long in tables.
- **Codes only:** unreadable in a queue and in notes, where `unreported-split` says
  what `DQ203` does not.
