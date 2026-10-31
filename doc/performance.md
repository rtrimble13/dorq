# Performance

The targets come from plan section 8. A full history of about 150M bars must
check in **5 minutes or less on 8 cores**. A nightly incremental run, with a
260-session lookback, must take **30 seconds or less**. This page records how
dorq was measured against both (DR-0604), what the profile showed, and what was
changed.

## The benchmark

`dorq-synth bench` writes many dorq-synth universes into one bars file. Copy
*i* uses seed *N + i*, and its series are named `<i>_<id>`. Each copy is the
full synthetic market: 125 series of liquid, mid, thin, penny and
sixteenth-priced names, about 230,000 bars, with every fault and hard negative
in place. The price model therefore does real work: it scores bad prints, scale
eras, splits and crash days, not just clean random walks.

```bash
cmake --preset release && cmake --build --preset release
build/release/tools/synth/dorq-synth bench --copies 650 --seed 1000 --out b650.csv
time build/release/src/dorq b650.csv --config tools/synth/dorq.toml \
     --exit-zero --format csv --threads 4 > out.csv
# The nightly shape: each series' last ~250 sessions
awk -F, 'NR==1 || $2 >= "2021-12-03"' b650.csv > nightly.csv
```

## Results

The machine was a 4-core x86-64 cloud VM (AVX-512 capable) with 15 GB of
memory. The binary was the `release` preset, with GCC 13 and `-O3`.

| Run | Rows | Series | Threads | Time | Peak memory |
|---|---|---|---|---|---|
| Full history (`b650.csv`, 7.7 GB) | 149.8M | 81,250 | 4 | **63.6 s** | 0.80 GB |
| Full history | 149.8M | 81,250 | 1 | 160.2 s | 0.80 GB |
| Nightly (`nightly.csv`) | 19.0M | 78,000 | 4 | **10.2 s** | 0.19 GB |
| Nightly | 19.0M | 78,000 | 1 | 21.9 s | 0.19 GB |
| 10 copies (`b10.csv`) | 2.3M | 1,250 | 1 | 2.6 s | 0.18 GB |
| 10 copies | 2.3M | 1,250 | 4 | 1.3 s | 0.18 GB |

Both targets are met on half the plan's cores, and the full history even on
one. The full history runs in about a
fifth of its budget and the nightly in about a third. The synthetic universe has
more series than fafnir's, so fafnir's nightly will read fewer rows.

One thread checks about 0.9M rows a second. Four threads check about 2.4M.
The two do not scale linearly because reading is serial. Parsing and grouping
the input runs on the calling thread at about 3M rows a second, and the checks
run on the workers. At 150M rows, reading takes about 50 of the 64 seconds, so
8 cores would gain little over 4. If a larger universe ever needs it, the next
step is to parse in parallel by splitting the file at series boundaries.

**Determinism.** The output is byte-identical with 1 and 4 threads on every
file above, the 150M-row one included. The unit and CLI tests check 1 against 8
threads, and buffered against streamed input (DR-0108). `dorq calibrate` writes
the same file with any `--threads`.

## Statements (DR-0901)

The plan's target for statements ([fundamentals plan](plans/dorq-fundamentals-plan.md)
§6) is fafnir's full statements history, about 6M rows, in 2 minutes or less end
to end. DR-0901 reads statements and checks them only for DQ104, so this measures
the reader: 20,000 synthetic securities of 300 rows each (three statements, twenty
years of quarters and years), with all 58 items filled with nine-digit numbers.
That is 4.1 GB of CSV, more than fafnir's export, where each row fills only its
own statement's items. The rows came through stdin, so they were streamed.

| Run | Rows | Series | Threads | Time | Peak memory |
|---|---|---|---|---|---|
| The generator alone, to `/dev/null` | 6.0M | 20,000 | | 4.1 s | |
| The generator piped into `dorq --kind statements` | 6.0M | 20,000 | 4 | **35.8 s** | 20 MB |

The same 4-core machine and `release` build as above (2026-10-31). Reading runs at
about 115 MB a second, a little faster per byte than bars. Memory stays at a few
series, since a security's statements are a few hundred rows. The statement checks
(DR-0904 onward) add their own cost, measured with each.

## The profile

The profile is a callgrind run on one copy (230,000 rows, one thread), before
the changes below. It counts instructions:

| Where | Share |
|---|---|
| Reading: CSV state machine, row assembly, `parse_number` | 48% |
| … of which `parse_number`, mostly outside fast_float (precision measurement, missing-value markers) | 20% |
| Price model (`analyze_prices`) | 27% |
| … of which the Student-t tail (incomplete beta) | 9% |
| Coverage model | 6% |
| Tick inference (`observed_step`) | 6% |
| Volume scale shifts | 5% |

## What changed

- **CSV parsing** copies each run of plain characters at once instead of one at
  a time.
- **`parse_number`** skips the missing-value marker comparison when a field
  starts with a digit or a minus sign. It also measures decimals and
  significant figures in one pass, without allocating. These two changes cut
  the read path by about a third: 1.15 s to 0.78 s on 2.3M rows.
- **Memory.** When the cross-sectional checks are on (by default they are), every
  series' result is held until the end of the run. Held violations below
  `--min-severity` are now dropped as they arrive. The cross-section only ever
  removes, downgrades or adds violations, so this changes no output. On the
  150M-row file, peak memory fell from 2.57 GB to 0.80 GB. Most of what had been
  held was info-level carry-bar notes (DQ502). With `--show-info` they are
  still held and reported, and memory grows with them.

## Build flags

The plan left open whether to build for `-march=x86-64-v3` (AVX2, FMA). On the
benchmark it made no measurable difference: 2.36–2.80 s against 2.53–2.68 s for
the baseline, on 2.3M rows with one thread. The output was also the same. The
release build therefore stays at baseline x86-64. A binary built that way runs
on any x86-64 host, and it cannot pick up floating-point contraction
differences between hosts.
