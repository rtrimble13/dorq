# Trading calendars

Several checks need to know which days were sessions:

- DQ105 (bars on non-session days);
- DQ206 (histories dated a day off);
- the coverage checks DQ301–DQ304, which treat a session with no bar as missing.

dorq runs with a built-in calendar by default. A reference calendar file is
optional; when you pass one, it decides which days were sessions within its own
span.

## Built-in calendars

| `--calendar` | Sessions |
|---|---|
| `XNYS` (default; also `NYSE`, `XNAS`, `NASDAQ`) | NYSE trading days: weekdays, minus the holidays and closures below |
| `weekdays` | Monday to Friday |
| `24x7` | every day |

The `XNYS` holidays, with NYSE's observance rules:

| Holiday | Rule |
|---|---|
| New Year's Day | 1 January. Sunday moves to Monday. **Saturday is not observed**: the Friday before is the last session of the year (e.g. 2021-12-31 was open) |
| Martin Luther King Jr. Day | 3rd Monday of January, **from 1998**. The NYSE first closed for it in 1998 |
| Washington's Birthday | 3rd Monday of February |
| Good Friday | two days before Easter (Gregorian) |
| Memorial Day | last Monday of May |
| Juneteenth | 19 June, from 2022. Saturday moves to Friday, Sunday to Monday |
| Independence Day | 4 July. Saturday moves to Friday, Sunday to Monday |
| Labor Day | 1st Monday of September |
| Thanksgiving | 4th Thursday of November |
| Christmas | 25 December. Saturday moves to Friday, Sunday to Monday |

Unscheduled closures are also built in:

- 1994-04-27 (Nixon);
- 2001-09-11 to 14;
- 2004-06-11 (Reagan);
- 2007-01-02 (Ford);
- 2012-10-29 and 30 (Hurricane Sandy);
- 2018-12-05 (G. H. W. Bush);
- 2025-01-09 (Carter).

The rules are those of the modern schedule. They are tested against fafnir's
calendar for 1990–2035, year by year. Before 1990 there are closures dorq does
not list, such as the 1977 blackout and Hurricane Gloria in 1985. After today
there will be closures nobody can list yet. For either, use a reference file.

**One known difference from fafnir:** fafnir's seeded calendar closes MLK Day in
every year. dorq's opens it before 1998, as the NYSE was. fafnir's own outlier
playbook already treats the 1990–1997 MLK bars as real. When fafnir passes its
calendar as a reference file, its table decides within its span (see below), so
the two always agree in fafnir's runs.

## A reference calendar file

```bash
dorq --calendar-file sessions.csv prices.csv
```

The file is CSV or TSV with a header, and these columns:

| Column | Names accepted | Required |
|---|---|---|
| date | `date`, `trade_date`, `session_date`, `session`, `day` | yes |
| open | `is_open`, `open`, `is_session`: true/false, t/f, 1/0, yes/no | no; every listed row is a session without it |
| exchange | `exchange`, `exchange_code`, `mic` | no |

**Within the file's span** (its first to its last listed date), a date is a
session when it is listed and not marked closed. Unlisted dates in the span are
not sessions. This matches fafnir's `ref.trading_calendar`, which lists open days
and marks closures with `is_open = false`. **Outside the span** the built-in
calendar (`--calendar`) still answers, so a file covering 1990–2027 does not leave
2028 without a calendar.

A file that lists several exchanges needs `--calendar-exchange` to pick one.
Without it, the run stops with exit 2 and the error lists the exchanges.

From fafnir:

```sql
\copy (SELECT exchange_code, trade_date, is_open FROM ref.trading_calendar
       WHERE exchange_code = 'NASDAQ' ORDER BY trade_date) TO 'sessions.csv' CSV HEADER
```

In a config file:

```toml
[calendar]
name = "XNYS"            # the built-in calendar, used outside the file's span
file = "sessions.csv"    # relative to the config file
exchange = "NASDAQ"
```

An unreadable or malformed calendar file is a configuration error (exit 2).
Nothing is checked against a calendar dorq could not read.

The output names a reference file by its name, without its directory: "not a
session on XNYS, with sessions.csv for 1990-01-02..2027-12-31". The same sessions
from another directory give the same output, and `config_hash` covers the file's
content, not its path. fafnir writes the file to a new temporary directory each
run.

## Choosing a calendar

- **US equities and ETFs:** the default.
- **Rates, FX, and other markets:** these keep other holidays. A bond-market
  series under XNYS shows Columbus Day and Veterans Day as one-session gaps.
  Without volume, DQ301 keeps these well below the reporting threshold, but a
  reference file of the right market's sessions is better.
- **Crypto, or funds priced seven days a week:** `--calendar 24x7`.
