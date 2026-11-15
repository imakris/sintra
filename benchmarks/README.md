# Sintra benchmarks

The benchmarks measure latency, throughput, process CPU and loss through the
public API. They run on demand on a reference host, and their results are JSONL
records kept outside git.

This file is the usage guide. [docs/benchmark_plan.md](../docs/benchmark_plan.md)
defines what is measured and how:

| Topic | Plan section |
|---|---|
| Measurement boundaries and CPU per role | 4.4 |
| The broadcast workload, its modes, sampling and the open-loop drain | 5, 5.1 |
| Metric definitions: percentiles, throughput, CPU, loss | 6 |
| Record schema | 7 |
| Validity, warnings, comparable pairs | 8 |
| Runner: sessions, sample life cycle, summary, calibration | 9 |
| Reading the axis tables | 10 |
| What the numbers do not claim | 12 |

| File | Content |
|---|---|
| `broadcast_benchmark.cpp` | W1, broadcast: target `sintra_broadcast_benchmark` |
| `harness.h` | Clock, process CPU time, build facts, per-process report files, aggregation helpers |
| `run_benchmarks.py` | Paired runner: A/B and A/A sessions, calibration, summary and axis tables |
| `workloads.json` | Cells, windowed counts, stored open-loop rates and calibration metadata |

## Build

Configure a Release build with `-DSINTRA_BUILD_BENCHMARKS=ON`, in its own build
directory. The targets link the header-only library with test hooks off.

```text
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DSINTRA_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --target sintra_broadcast_benchmark
```

`--config Release` selects the configuration for a multi-configuration generator
such as Visual Studio, where `CMAKE_BUILD_TYPE` does not; a single-configuration
generator ignores it.

A sample is valid only from a Release configuration with `NDEBUG` defined,
optimization on and test hooks off. Other builds run, and their records say why
they are invalid. Build both arms of a comparison with the same toolchain and
settings: the runner refuses to compare builds whose build facts differ.

## Runner

```text
python benchmarks/run_benchmarks.py --a <build-dir> --a-commit <sha> [--b <build-dir> --b-commit <sha>]
    --output <results.jsonl> [--workloads <file>] [--pairs 5] [--select <cell-id glob>]
    [--note <text>] [--cpus N] [--aa <aa.jsonl>] [--calibrate <new-workloads.json>] [--keep]
```

- **A/B session.** With `--b`, every selected cell runs as pairs of one A and one
  B sample. A runs first in the even pairs and B in the odd ones.
- **A/A session.** Without `--b`, the A binaries run as both arms and the records
  carry `aa: true`. Its within-pair spread is the observed noise of a cell.
- **Output.** One JSON record per sample is appended to `--output` and flushed.
  The commits are the operator's labels for the two arms; the runner does not
  read them from the build.
- **Selection.** Without `--select`, the default grid runs: every cell with
  R in `default_receivers` (1, 4 and 16). `--select` takes a glob over cell IDs
  and may be repeated; it reaches every defined cell, including R = 2 and 8.
- **Open-loop cells without a stored rate** are skipped. The runner lists them at
  the start and in the summary and writes no record for them.
- **`--pairs`** and **`--cpus`** take positive integers. `--cpus` records the
  effective CPU count C of the session and defaults to `hardware_concurrency()`.
- **`--keep`** keeps each sample's run directory under `<output>.runs/`: the
  process reports, the recorded PIDs and the coordinator's output.

Cell IDs:

```text
w1.windowed.<origin>.r<receivers>.b<payload bytes>
w1.open_loop.<origin>.r<receivers>.b<payload bytes>.x<percent of the calibrated rate>
```

For example, `--select "w1.windowed.*.r[14].b64"` selects the windowed 64-byte
cells of both origins at R = 1 and R = 4.

### Each sample and its cleanup

The runner creates a fresh run directory, names it in `SINTRA_BENCHMARK_DIR` and
starts the coordinator with a timeout of 60 s plus three times the planned
duration. It then waits up to 10 s for the report of every participant, runs
`--aggregate`, adds its own fields and appends the record. It deletes the run
directory unless `--keep` is given.

The runner owns the processes of a sample and leaves none behind:

- **On timeout,** it terminates the swarm; the record has `timed_out: true` and
  the reason `timeout`. The sample is still aggregated, because its processes
  may have written their reports before the stall.
- **On Ctrl+C, a termination request, or an error of the runner,** it terminates
  the swarm before it exits. The interrupted sample has no record.
- **After a normal exit,** participants that are still alive after 10 s are
  terminated, and the record gets the warning `stray_participants`.
- **If a participant cannot be removed,** the record gets the reason
  `cleanup_incomplete`, the run directory is kept, and the session stops, so that
  nothing is measured beside a stray process.

The runner signals only what it can identify as its own:

- **Windows.** It retains a native handle of the coordinator it started and of
  each descendant, and terminates through those handles. Recorded PIDs are never
  signalled. This reuses `tests/runner/platform/windows_process_tree.py`, which
  the runner loads by its path relative to `run_benchmarks.py`; the test
  framework is not imported. Without the `tests` directory, the runner stops at
  start on Windows with a message that names the missing file.
- **POSIX.** The coordinator is the runner's child. A leaf starts its own
  session, so the runner finds it through the PID that it recorded in the run
  directory, and signals that PID only while its command line names this
  benchmark executable and this run's ID. On Linux a pidfd pins the identified
  process, so the check and the signal refer to the same process. Elsewhere the
  identity is established immediately before the signal. A recorded PID that
  cannot be identified is not signalled.

### Summary

For each cell, the runner prints the number of comparable pairs used, the
number of invalid samples of each arm, and the sample support of the headline
latencies: `n_obs` and `n_messages` of the least supported sample. For each
headline metric it prints the median within-pair comparison with its minimum
and maximum and, beside it, the observed A/A range: the minimum and maximum of
the same comparison over the comparable A/A pairs with the same effective
parameters, taken from the output file and from `--aa`. In an A/A session, the
session's own pairs are among them. Nothing is flagged automatically.

- **Ratio or difference.** A metric is compared as the ratio B/A only when every
  observation of the cell and of its matching A/A pairs is positive. Otherwise
  it is shown as the median of each arm with the difference B-A, and its A/A
  range uses differences too. CPU metrics and the lost fraction are always shown
  that way. A zero observation is never dropped.
- **Unavailable.** An observation without a value is counted as unavailable, on
  the comparison and on the A/A range: a slowest-receiver latency when no
  sampled message reached every receiver, and every CPU metric of a sample with
  the warning `cpu_not_advancing`. "unavailable" alone means that no pair has a
  value, or that no matching A/A pair exists.

| Mode | Headline metrics |
|---|---|
| Windowed | Publication and delivery rates; CPU µs per message, total and per role; `slowest_from_send` p50 and p99 |
| Open-loop | `slowest_from_intended` p50 and p99; lost fraction; `lateness` p99 |

After the summary, the runner prints the axis tables of plan section 10 for arm
A, as the median over the usable samples of each cell: CPU µs per message by
role against R, total CPU µs per application MB against payload, open-loop
latency and loss against the offered rate, and each role's share of total CPU.
A sample with `cpu_not_advancing` contributes no CPU value to them.

### Calibration

```text
python benchmarks/run_benchmarks.py --a <build-dir> --a-commit <sha> --output <calibration.jsonl>
    --calibrate <new-workloads.json> [--select <cell-id glob>]
```

Calibration runs each selected windowed cell three times with the A binaries.
For a cell whose three runs are all usable, it rescales the windowed count so
that the measured interval is about `target_interval_s` (an integer of two
significant digits), stores the open-loop rates as the `rate_fractions` of the
median windowed **publication** rate (two significant digits), and records the
host label, commit and date. Cells that were not calibrated keep their values.

Commit a calibrated `workloads.json` only from a run on the reference host, so
that both arms and later sessions use the same absolute counts and rates.
Anywhere else, calibrate into a temporary file and pass it with `--workloads`.

## Workloads file

`workloads.json` holds the settings and one entry per origin, receiver count and
payload. Each entry defines one windowed cell and one open-loop cell per rate
fraction.

| Key | Meaning |
|---|---|
| `target_interval_s` | The measured interval that calibration aims a windowed count at; 3 by default |
| `default_receivers` | The receiver counts of the default grid |
| `open_loop.duration_s` | The offered schedule of an open-loop cell; 3 by default |
| `open_loop.rate_fractions` | The fractions of the calibrated publication rate that are stored as rates |
| `cells[].messages` | The windowed count: a positive integer |
| `cells[].open_loop_rates` | null before calibration; else one absolute rate (or null) per rate fraction |
| `cells[].calibration` | `{host_label, commit, date}`, or null |
| `cells[].handler_delay_us`, `cells[].burst` | Optional validation-only options, passed to the executable |

The 3 s of the plan are these defaults. An open-loop cell publishes
`ceil(rate × duration_s)` messages: all i with `i/rate` below the schedule. Its
completion may take longer than the schedule.

The runner rejects a file with a non-positive or fractional count, a rate that
is not finite and positive, a rate list whose length differs from the fraction
list, or two cells that expand to the same ID.

## The broadcast executable

```text
sintra_broadcast_benchmark <cell options> --run-id <id>    one sample, as a swarm
sintra_broadcast_benchmark --aggregate <run directory>     the sample's record, as one JSON object
sintra_broadcast_benchmark --build-facts                   the build facts, as one JSON object
```

A sample needs `SINTRA_BENCHMARK_DIR` to name an existing, empty run directory.
Every process records its PID there at start-up and writes
`<role>-<branch index>.bin` right after the last rendezvous of the measurement
protocol, before `sintra::shutdown()`. `--aggregate` requires exactly one report
per process of the cell, all of one run and one cell, and prints the record. It
exits 0 whenever it can produce a record, valid or not.

| Cell option | Meaning | Default |
|---|---|---|
| `--mode windowed\|open_loop` | Measurement mode | `windowed` |
| `--origin coordinator\|leaf` | Publishing process | `coordinator` |
| `--receivers R` | Receiver leaves, 1 to 64 | 1 |
| `--bytes N` | Payload size, 17 to 261632 | 64 |
| `--messages n` | Measured messages, at least 1 | 20000 |
| `--warmup n` | Warm-up messages; the runner passes `max(64, n/20)` | 1000 |
| `--rate r` | Offered rate in messages per second; open-loop only, finite and positive | none |
| `--burst n` | Messages per fenced burst of the measured part; 0 runs it unpaced | the burst formula |
| `--handler-delay-us d` | Busy time in every receiver handler | 0 |

An option and its value are separate arguments. A number must be the whole
argument: `9.5` or `9x` is rejected for a count, and `inf` for a rate.

**Validation-only.** `--handler-delay-us` and `--burst` exist to check loss
detection. A record is `validation_only` when they change the effective
workload: the delay is not zero, or the burst differs from the burst formula.
Such a record stays out of comparisons, summaries and calibration; its validity
follows the normal rules. The delay applies to receiver leaves only. Warm-up is
always paced with the burst formula, also under `--burst 0`. An unpaced run
that loses messages can also fail to complete, and the runner then reports the
timeout.

**Open-loop drain.** After the last measured message the sender stays quiet for
100 ms and then writes a sentinel. A control rendezvous, `published`, follows:
every other process enters it right after the begin rendezvous returns, and the
sender enters it after publication, the quiet period and the sentinel. A
receiver's 10 s drain allowance for the sentinel starts when `published`
returns, so a sender that finishes late cannot cause `sentinel_missed`. The
runner's timeout remains the outer bound. A receiver that does miss the sentinel
makes the sample invalid; its `lost` then holds only the gaps up to its last
received message, and `counts.unresolved_tail` holds the tail it had not reached.

## Record

Plan section 7 defines the record and section 6 its metrics. These are the
reasons and warnings that the executable and the runner write:

| Reason | Condition |
|---|---|
| `build_not_release`, `build_without_ndebug`, `build_not_optimized`, `build_with_test_hooks` | The build is not valid |
| `timeout` | The runner terminated the sample at its timeout |
| `exit_code_nonzero` | The coordinator exited non-zero |
| `cleanup_incomplete` | A participant could not be removed; the session stops |
| `missing_files`, `foreign_run_files` | A process report is missing, or belongs to another run |
| `unexpected_reports`, `inconsistent_reports` | A report is a duplicate or of a branch the cell lacks, or carries other cell settings |
| `aggregation_failed`, `run_id_mismatch` | `--aggregate` produced no record, or one of another run |
| `order_errors`, `size_errors` | An order error, or a wrong payload size or final byte |
| `loss`, `eviction_log_matches` | Windowed mode: any loss, or any eviction log match |
| `sentinel_missed` | Open-loop mode: a receiver did not see the sentinel within its drain allowance |

| Warning | Condition |
|---|---|
| `cpu_low_confidence` | The measured interval is under 2 s |
| `cpu_not_advancing` | The CPU delta of some process over the measured interval is exactly zero |
| `stray_participants` | Participants outlived the coordinator's normal exit and were terminated |
| `rate_shortfall` | The achieved publication rate is under 95% of the offered rate |
| `ring_dir_not_empty` | `sintra-*` entries were already present in the ring parent directory |

A record with missing, surplus or inconsistent process reports has null
`counts`, `throughput`, `latency` and `cpu`. A sample that left no report at all
has those too, and carries only the cell parameters that the runner knows.

**Process CPU on Windows.** The Windows process CPU clock is coarse and can stay
at zero for a whole sample, however long the interval: neither 2 s nor 3 s makes
it usable. A sample with `cpu_not_advancing` has no usable CPU metric, and a
sample without the warning can still be coarse. Read CPU from Linux runs.
