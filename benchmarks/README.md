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
| The swarm workload: sizes, snapshots and CPU per phase | 5.3 |
| Metric definitions: percentiles, throughput, CPU, loss | 6 |
| Record schema | 7 |
| Validity, warnings, comparable pairs | 8 |
| Runner: sessions, sample life cycle, summary, calibration | 9 |
| Reading the axis tables | 10 |
| What the numbers do not claim | 12 |

| File | Content |
|---|---|
| `broadcast_benchmark.cpp` | W1, broadcast: target `sintra_broadcast_benchmark` |
| `swarm_benchmark.cpp` | W3, swarm formation and shutdown: target `sintra_swarm_benchmark` |
| `harness.h` | Clock, process CPU time, build facts, per-process report files, aggregation helpers |
| `run_benchmarks.py` | Paired runner: A/B and A/A sessions, calibration, summary and axis tables |
| `workloads.json` | Cells, windowed counts, stored open-loop rates and calibration metadata |

## Build

Configure a Release build with `-DSINTRA_BUILD_BENCHMARKS=ON`, in its own build
directory. The targets link the header-only library with test hooks off.

```text
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DSINTRA_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --target sintra_broadcast_benchmark sintra_swarm_benchmark
```

`--config Release` selects the configuration for a multi-configuration generator
such as Visual Studio, where `CMAKE_BUILD_TYPE` does not; a single-configuration
generator ignores it.

A sample is valid only from a Release configuration with `NDEBUG` defined,
optimization on and test hooks off. Other builds run, and their records say why
they are invalid.

Build both arms of a comparison with the same toolchain and configuration. The
runner refuses arms that differ in the compiler, the configuration, `NDEBUG`,
optimization, test hooks or separate compilation. The arms may differ in a
library constant, which is what a comparison of two library versions can be
about: the eviction settings, `message_ring_size` and the ring ABI version.
Each record carries the constants of its arm, and the runner names the ones
that differ in a notice at the start of the session and in the summary.

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
  read them from the build. When the runner reads earlier records from
  `--output` or `--aa`, it skips a line that does not parse or that is not a
  record of this schema, and says how many lines it skipped.
- **Selection.** Without `--select`, the default grid runs: every cell with
  R in `default_receivers` (1, 4 and 16). `--select` takes a glob over cell IDs
  and may be repeated; it reaches every defined cell, including R = 2 and 8.
- **Open-loop cells without a stored rate** are skipped. The runner lists them at
  the start and in the summary and writes no record for them.
- **Swarm cells** are part of the default grid. The oversubscribed cell,
  `w3.lifecycle.n_over`, has `N_over = min(125, max(oversubscribed_least,
  oversubscribed_per_cpu × C))` leaves, where C is `--cpus` or else
  `hardware_concurrency()`. With C = 0 the runner skips that cell, lists it at
  the start and in the summary, and writes no record for it. When `N_over + 1`
  does not exceed C, its records carry the warning `not_oversubscribed`.
- **`--pairs`** and **`--cpus`** take positive integers. `--cpus` records the
  effective CPU count C of the session and defaults to `hardware_concurrency()`.
- **`--keep`** keeps each sample's run directory under `<output>.runs/`: the
  process reports, the recorded PIDs and the coordinator's output.

Cell IDs:

```text
w1.windowed.<origin>.r<receivers>.b<payload bytes>
w1.open_loop.<origin>.r<receivers>.b<payload bytes>.x<percent of the calibrated rate>
w3.lifecycle.n<leaves>
w3.lifecycle.n_over
```

For example, `--select "w1.windowed.*.r[14].b64"` selects the windowed 64-byte
cells of both origins at R = 1 and R = 4.

### Each sample and its cleanup

The runner creates a fresh run directory, names it in `SINTRA_BENCHMARK_DIR` and
starts the coordinator. It then waits up to 10 s for the report of every
participant, runs `--aggregate`, adds its own fields and appends the record. It
deletes the run directory unless `--keep` is given.

**Timeout.** A sample's timeout is 60 s plus three times its planned duration.
A windowed cell without calibration metadata gets 60 s plus thirty times it,
because its count is only an estimate for the host.

The runner owns the processes of a sample and leaves none behind:

- **On timeout,** it ends the swarm in the way described below; the record has
  `timed_out: true` and the reason `timeout`. The sample is still aggregated,
  because its processes may have written their reports before the stall.
- **On an interrupt or an error of the runner,** it ends the swarm before it
  exits. The sample that was running has no record. An interrupt is Ctrl+C,
  Ctrl+Break, SIGTERM or SIGHUP. It only sets a flag, which the runner polls
  while a sample runs, so a further interrupt cannot cut a cleanup short. A
  signal that the runner was started to ignore, as under `nohup`, stays ignored.
- **After a normal exit,** the record gets the warning `stray_participants` if
  processes of the run are still alive after 10 s. The cleanup follows.
- **If the cleanup is not complete within 10 s,** the record gets the reason
  `cleanup_incomplete`, the run directory is kept, and the session stops with a
  message that lists the PID and the command line of every process that is
  left, so that nothing is measured beside a stray process. The cleanup is
  complete when the coordinator is reaped and two successive looks find no
  other process of the run. A look that fails leaves it incomplete.

The runner never signals a process that it knows only by its PID, not even
right after checking that PID: between the check and the signal, the PID can
pass to another process. This is what it signals, and how it finds the rest:

- **POSIX.** The runner signals only the coordinator, which is its own child:
  it kills the coordinator's process group while the coordinator is unreaped.
  That group holds the coordinator and those of its children that have not
  started their own session yet. A leaf is never signalled. It exits by itself
  through Sintra's lifeline about 100 ms after the coordinator is gone
  ([docs/architecture.md](../docs/architecture.md), "Lifeline ownership"). The
  runner waits for that. It looks for processes of the run by scanning the
  process table for command lines that carry `--run-id` with this run's ID,
  which also finds a child that has not started the executable yet. A leaf that
  cannot exit by itself, such as a stopped one, is reported as left.
- **Windows.** The runner terminates the coordinator and its descendants
  through retained native handles. This reuses
  `tests/runner/platform/windows_process_tree.py`, which the runner loads by
  its path relative to `run_benchmarks.py`; the test framework is not imported.
  Without the `tests` directory, the runner stops at start on Windows with a
  message that names the missing file. If the process table cannot be read, the
  runner still terminates through the handles that it holds, and the cleanup
  stays incomplete until a later look succeeds.

The PID files in the run directory tell the runner how many reports to wait
for. It does not find or signal a process through them.

### Summary

For each cell, the runner prints the number of comparable pairs used, the
number of invalid samples of each arm, and the sample support of the headline
latencies: `n_obs` and `n_messages` of the least supported sample. For a
windowed cell it also prints each arm's median fence time. For each headline
metric it prints the median within-pair comparison with its minimum and maximum
and, beside it, the A/A spread. Nothing is flagged automatically.

- **A/A spread.** The largest deviation within a comparable A/A pair with the
  same effective parameters, taken from the output file and from `--aa`. In an
  A/A session, the session's own pairs are among them. The arm labels of an A/A
  pair are arbitrary, so the spread does not depend on them: a ratio r counts
  as max(r, 1/r) and is printed as a factor, such as `up to x1.012 over 2
  pairs`, and a difference counts as its absolute value.
- **Ratio or difference.** A metric is compared as the ratio B/A only when every
  observation of the cell and of its matching A/A pairs is available and not
  zero. Otherwise it is shown as the median of each arm with the difference
  B-A, and its A/A spread uses differences too. CPU metrics and the lost
  fraction are always shown that way. A slowest-receiver latency follows the
  general rule: it is a ratio until one observation is unavailable or zero. A
  zero observation is never dropped.
- **Unavailable.** An observation without a value is counted as unavailable, on
  the comparison and on the A/A spread: a slowest-receiver latency when no
  sampled message reached every receiver, and every CPU metric of a sample with
  the warning `cpu_not_advancing`. "unavailable" alone means that no pair has a
  value, or that no matching A/A pair exists.
- **Comparable pairs.** Two samples are compared when both are usable and have
  the same cell, toolchain and configuration, host label and C. Library
  constants do not have to match. If the arms differ in one, the summary names
  it in a notice, as the start of the session does.
- **Fence time.** A windowed sender waits in a processing fence after every
  burst and once at the end. The measured interval includes those waits, so
  windowed rates and CPU per message include fence time. `throughput.fence_s`
  holds their sum.
- **Open-loop sender CPU.** An open-loop sender polls the clock for the last
  100 µs before each intended arrival, so its CPU is mostly the generator's
  own. Do not read it as the cost of publishing.

| Mode | Headline metrics |
|---|---|
| Windowed | Publication and delivery rates; CPU µs per message, total and per role; `slowest_from_send` p50 and p99 |
| Open-loop | `slowest_from_intended` p50 and p99; lost fraction; `lateness` p99 |
| Lifecycle | `time_to_ready_s`, `time_to_shutdown_s`; the coordinator's `shutdown()` calls, where a sample needed more than one; formation and shutdown CPU per role |

After the summary, the runner prints the axis tables of plan section 10 for arm
A, as the median over the usable samples of each cell: CPU µs per message by
role against R, total CPU µs per application MB against payload, open-loop
latency and loss against the offered rate, and each role's share of total CPU.
The share table covers windowed cells only, because of the open-loop sender's
polling. A sample with `cpu_not_advancing` contributes no CPU value to them.

Swarm cells have their own tables: the two lifecycle times and each role's
formation and shutdown CPU per leaf against N, with the coordinator's
`shutdown()` calls where a sample needed more than one, and each role's share
of a phase's CPU.

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
| `swarm.leaves` | The swarm sizes N of W3: one lifecycle cell each, 1 to 125 leaves |
| `swarm.oversubscribed_least`, `swarm.oversubscribed_per_cpu` | The size of the oversubscribed cell: `min(125, max(least, per_cpu × C))`; 80 and 2 by default |
| `swarm.fail_init`, `swarm.leaf_exit_delay_ms` | Optional validation-only options of every swarm cell, passed to the executable |

The plan's 3 s windowed interval and 3 s open-loop schedule are defaults: the
shipped `workloads.json` sets `target_interval_s` and `open_loop.duration_s` to
3, and another workloads file can set other values. An open-loop cell publishes
`ceil(rate × duration_s)` messages: all i with `i/rate` below the schedule. Its
completion may take longer than the schedule.

The runner rejects a file with a non-positive or fractional count, a rate that
is not finite and positive, a rate list whose length differs from the fraction
list, or two cells that expand to the same ID.

A file without `swarm` defines no swarm cells. With it, the runner rejects a
size outside 1 to 125 and a repeated size.

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

Every step of the measurement protocol is a Sintra barrier over all processes.
Sintra treats a barrier as satisfied without holding it while a process shuts
down or drains, and reports that in the barrier's result. A process that gets
such a result stops its measurement and writes no report, so the sample is
invalid with `missing_files`; the coordinator also exits non-zero.

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
workload: the delay is not zero, or, in windowed mode, the burst differs from
the burst formula. Open-loop publication has no bursts, so `--burst` does not
change an open-loop cell. A validation-only record stays out of comparisons,
summaries and calibration; its validity follows the normal rules. The delay
applies to receiver leaves only. Warm-up is always paced with the burst
formula, also under `--burst 0`. An unpaced run that loses messages can also
fail to complete, and the runner then reports the timeout.

**Open-loop drain.** After the last measured message the sender stays quiet for
100 ms and then writes a sentinel. A control rendezvous, `published`, follows:
every other process enters it right after the begin rendezvous returns, and the
sender enters it after publication, the quiet period and the sentinel. A
receiver's 10 s drain allowance for the sentinel starts when `published`
returns, so a sender that finishes late cannot cause `sentinel_missed`. The
runner's timeout remains the outer bound. A receiver that does miss the sentinel
makes the sample invalid; its `lost` then holds only the gaps up to its last
received message, and `counts.unresolved_tail` holds the tail it had not reached.

## The swarm executable

```text
sintra_swarm_benchmark <cell options> --run-id <id>    one sample, as a swarm
sintra_swarm_benchmark --aggregate <run directory>     the sample's record, as one JSON object
sintra_swarm_benchmark --build-facts                   the build facts, as one JSON object
```

A sample is a fresh coordinator with N leaf branches whose entry functions are
empty. Every process takes its formation snapshot right after `sintra::init()`
returns, meets the others in one rendezvous, and takes its shutdown snapshots
before its first `sintra::shutdown()` call and after the call that returns
true. It writes `<role>-<branch index>.bin` after that, because the sample
measures shutdown. The run directory and `--aggregate` work as for the broadcast
executable, and so does the rendezvous: a process whose rendezvous was bypassed
writes no report. A swarm sample has no planned duration, so the runner gives
it the 60 s base timeout.

Keep `--output`, and with it the run directories, on a local filesystem. A leaf
writes its PID file during its start-up and its report before it exits, and
both are inside the measured phases of a swarm sample.

| Cell option | Meaning | Default |
|---|---|---|
| `--leaves N` | Leaf branches, 1 to 125 | 1 |
| `--cpus C` | The session's C, for the oversubscription ratio `(N+1)/C`; 0 when unknown | `hardware_concurrency()` |
| `--fail-init` | Adds a branch whose binary does not exist, so that `init` throws | off |
| `--leaf-exit-delay-ms d` | Every leaf stays alive for d ms after its `shutdown()` returned | 0 |

**Lifecycle metrics**

| Field | Meaning |
|---|---|
| `time_to_ready_s` | The duration of the coordinator's `init`: from just before the call to the coordinator's formation snapshot. A leaf's `init` returns shortly after it |
| `time_to_shutdown_s` | The coordinator's shutdown phase: from its first `shutdown()` call to the return of the call that returned true |
| `shutdown_calls`, `first_shutdown_call_s` | The `shutdown()` calls that the coordinator needed, and the duration of the first one |
| `leaf_start_max_s` | The longest time of a leaf from the entry of `main` to its formation snapshot. An early leaf waits in the startup barrier for the later ones, so this follows `time_to_ready_s` |
| `leaf_shutdown_max_s` | The longest shutdown phase of a leaf. A leaf's `shutdown()` returns true or throws, so it is one call |
| `formation_cpu` | Each role's cumulative process CPU at the formation snapshot, start-up included |
| `shutdown_cpu` | Each role's process CPU over the shutdown phase |
| `oversubscription` | `(N+1)/C`; null when C is 0 |

What a process does between its formation snapshot and its shutdown-begin
snapshot is in neither CPU phase: the rendezvous and, in the coordinator, its
part of the leaves' name publication, which each leaf issues at the end of its
own `init`. A record has no `lifecycle` when a process report is missing or
surplus, when `init` failed, or when a `shutdown()` threw.

**Warnings.** `cpu_not_advancing` applies when the formation CPU or the
shutdown CPU of some process is exactly zero. `cpu_low_confidence` is not used
in lifecycle mode: the phases of a swarm sample are short at every size, so its
per-sample CPU values are coarse.

**A `shutdown()` that returns false.** The coordinator's `shutdown()` returns
false when the bounded wait for its managed children, at the end of the call,
ends while a leaf process is still alive, and a sequential retry resumes the
teardown (`docs/reference/shutdown.md`). That is a result, not an invalidity:
the process calls `shutdown()` again until it returns true, the shutdown phase
runs until then, and the record carries the number of calls and the warning
`shutdown_retried`. The retries have no bound of their own: a shutdown that
never completes ends at the runner's timeout, and a direct run without the
runner lasts until every leaf has exited.

**Validation-only.** `--fail-init` and `--leaf-exit-delay-ms` exist to check
the validity rules, and a record with either is `validation_only`. With
`--fail-init`, the coordinator's `init` throws, the coordinator exits non-zero
and the record has the reason `init_failed`. While a leaf outlives its
`shutdown()`, the coordinator's `shutdown()` returns false: a delay of a few
seconds gives a valid record with `shutdown_calls` above 1 and the warning
`shutdown_retried`, and a delay beyond the runner's timeout gives a sample that
the runner terminates, without a report of the coordinator. Its leaves exit
through the lifeline like any other: a leaf watches the lifeline after its
`shutdown()` as well.

## Record

Plan section 7 defines the record and section 6 its metrics. Beyond them, a
record has these fields:

| Field | Content |
|---|---|
| `cell.burst` | Messages per fenced burst of the measured part; 0 for an unpaced run. Open-loop publication does not use it |
| `cell.handler_delay_us` | The handler delay of a validation run; otherwise 0 |
| `throughput.fence_s` | Windowed: the sender's time in its burst fences and its final fence, which `interval_s` includes. Open-loop: null |

`latency.fence` describes the burst fences only, not the final fence.

These are the reasons and warnings that the executable and the runner write:

| Reason | Condition |
|---|---|
| `build_not_release`, `build_without_ndebug`, `build_not_optimized`, `build_with_test_hooks` | The build is not valid |
| `timeout` | The runner terminated the sample at its timeout |
| `exit_code_nonzero` | The coordinator exited non-zero |
| `cleanup_incomplete` | A process of the run is left, or the runner could not establish that none is; the session stops |
| `missing_files`, `foreign_run_files` | A process report is missing, or belongs to another run |
| `unexpected_reports`, `inconsistent_reports` | A report is a duplicate or of a branch the cell lacks, or carries other cell settings |
| `aggregation_failed`, `run_id_mismatch` | `--aggregate` produced no record, or one of another run |
| `order_errors`, `size_errors` | An order error, or a wrong payload size or final byte |
| `loss`, `eviction_log_matches` | Windowed mode: any loss, or any eviction log match |
| `sentinel_missed` | Open-loop mode: a receiver did not see the sentinel within its drain allowance |
| `init_failed` | Lifecycle mode: `init` threw in a process |
| `shutdown_threw` | Lifecycle mode: a `shutdown()` call of a process threw |

| Warning | Condition |
|---|---|
| `cpu_low_confidence` | The measured interval is under 2 s |
| `cpu_not_advancing` | The CPU delta of some process over the measured interval is exactly zero |
| `stray_participants` | Processes of the run outlived the coordinator's normal exit by 10 s |
| `rate_shortfall` | The achieved publication rate is under 95% of the offered rate |
| `ring_dir_not_empty` | `sintra-*` entries were already present in the ring parent directory |
| `not_oversubscribed` | Lifecycle mode: the oversubscribed cell has `N_over + 1 <= C` |
| `shutdown_retried` | Lifecycle mode: a process needed more than one `shutdown()` call |

A record with a missing or surplus process report is invalid and has null
`counts`, `throughput`, `latency` and `cpu`. A report of another run, or with
other cell settings, is set aside and makes the record invalid too. If the
expected reports are complete without it, the metrics are computed from those;
if it stands in for an expected report, that report is missing. A sample that
left no report at all has null metrics, and carries only the cell parameters
that the runner knows.

**Process CPU on Windows.** The Windows process CPU clock is coarse and can stay
at zero for a whole sample, however long the interval: neither 2 s nor 3 s makes
it usable. A sample with `cpu_not_advancing` has no usable CPU metric. A sample
without the warning is not reliable either: its process CPU can be several
times too low. CPU figures from a Windows host are not a reference. Read CPU
from Linux runs.
