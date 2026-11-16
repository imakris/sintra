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
| The request/response workload, its routes, gaps and window | 5.2 |
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
| `rpc_benchmark.cpp` | W2, request/response: target `sintra_rpc_benchmark` |
| `swarm_benchmark.cpp` | W3, swarm formation and shutdown: target `sintra_swarm_benchmark` |
| `harness.h` | Clock, process CPU time, build facts, per-process report files, aggregation helpers |
| `run_benchmarks.py` | Paired runner: A/B and A/A sessions, calibration, summary and axis tables |
| `workloads.json` | Cells, windowed counts, stored open-loop rates and calibration metadata |

## Build

Configure a Release build with `-DSINTRA_BUILD_BENCHMARKS=ON`, in its own build
directory. The targets link the header-only library with test hooks off.

```text
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DSINTRA_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --target sintra_broadcast_benchmark sintra_rpc_benchmark sintra_swarm_benchmark
```

`--config Release` selects the configuration for a multi-configuration generator
such as Visual Studio, where `CMAKE_BUILD_TYPE` does not; a single-configuration
generator ignores it.

A sample is valid only from a Release configuration with `NDEBUG` defined,
optimization on and test hooks off. The configuration name counts in any letter
case, as in `-DCMAKE_BUILD_TYPE=release`. Other builds run, and their records
say why they are invalid.

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
- **Selection.** Without `--select`, the default grid runs: every broadcast
  cell with R in `default_receivers` (1, 4 and 16) and every request/response
  cell. `--select` takes a glob over cell IDs and may be repeated; it reaches
  every defined cell, including R = 2 and 8. The build directory must hold the
  executable of every selected workload.
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
w2.closed_loop.<route>.g<gap in µs>
w2.windowed.<route>.w<window>
w3.lifecycle.n<leaves>
w3.lifecycle.n_over
```

For example, `--select "w1.windowed.*.r[14].b64"` selects the windowed 64-byte
cells of both origins at R = 1 and R = 4, and `--select "w2.*.leaf_to_leaf.*"`
selects the five request/response cells of the leaf-to-leaf route.

### Each sample and its cleanup

The runner creates a fresh run directory, names it in `SINTRA_BENCHMARK_DIR` and
starts the coordinator. After the coordinator's normal exit it waits up to 10 s
for the report of every participant. It runs `--aggregate`, adds its own fields
and appends the record. It deletes the run directory unless `--keep` is given.

**Timeout.** A sample's timeout is 60 s plus three times its planned duration.
A windowed broadcast cell without calibration metadata gets 60 s plus thirty
times it, because its count is only an estimate for the host. Under
`--calibrate` every sample has a fixed timeout of 900 s instead: a calibration
has to complete a sample at that estimate, however slow the host is.

| Cell | Planned duration |
|---|---|
| Broadcast, windowed | `target_interval_s` |
| Broadcast, open-loop | `open_loop.duration_s` |
| Request/response, closed-loop | calls × (gap + 5 ms) |
| Request/response, windowed | calls × 250 µs |
| Swarm | none: the timeout is 60 s |

The request/response counts are fixed, so their planned duration does not come
from a calibration. The 5 ms leave room for every hop of a round trip to wait
for a reader's next poll.

The runner owns the processes of a sample and ends them all, with one exception
on POSIX: a leaf that cannot run, such as a stopped one, is reported instead,
and the session stops (see the POSIX paragraph below).

- **On timeout,** it ends the swarm in the way described below; the record has
  `timed_out: true` and the reason `timeout`. The sample is still aggregated,
  because its processes may have written their reports before the stall. If
  `--aggregate` produces no record, the runner's own record has the reason
  `aggregation_failed` as well, and `missing_files` names the expected reports
  that are absent.
- **On an interrupt or an error of the runner,** it ends the swarm before it
  exits. The sample that was running has no record. An interrupt is Ctrl+C,
  Ctrl+Break, SIGTERM, SIGHUP or SIGQUIT. It only sets a flag, which the
  runner polls while a sample runs, so a further interrupt cannot cut a cleanup
  short. A signal that the runner was started to ignore, as under `nohup`,
  stays ignored.
- **After a normal exit,** the runner waits up to 10 s for the reports, then
  cleans up as in the other cases.
- **If the cleanup is not complete within 10 s,** the record gets the reason
  `cleanup_incomplete`, the run directory is kept, and the session stops with a
  message that lists the PID and the command line of every process that is
  left, so that nothing is measured beside a leftover process. The cleanup is
  complete when the coordinator is reaped and two consecutive looks, both ending
  before the deadline, find no other process of the run. A look that fails or
  is inconclusive is not empty: `ps` fails, or the command line of one of the
  runner's own processes cannot be read for another reason than that the
  process has gone. The message names such a process as "could not be
  inspected". That does not mean the process belongs to the run: an unrelated
  process of the runner's user whose command line cannot be read, such as a
  non-dumpable one under `hidepid`, stops the session too.

The runner never signals a process that it knows only by its PID, not even
right after checking that PID: between the check and the signal, the PID can
pass to another process. This is what it signals, and how it finds the rest:

- **POSIX.** The runner signals only the process group of the coordinator,
  which is its own child: it kills that group while the coordinator is
  unreaped. The group holds the coordinator and those of its children that have
  not started their own session yet; such a child receives the group's signal
  too. A leaf that has entered its own session is never signalled, and no PID
  that a scan finds is ever signalled. A leaf exits by itself through Sintra's
  lifeline about 100 ms after the coordinator is gone
  ([docs/architecture.md](../docs/architecture.md), "Lifeline ownership"). The
  runner waits for that. It looks for processes of the run by scanning the
  process table for command lines that carry `--run-id` with this run's ID,
  which also finds a child that has not started the executable yet. With procfs
  it reads only its own user's processes, as every participant runs as that
  user; procfs can deny another user's command line (`hidepid`). A leaf that
  cannot run, such as one stopped by `SIGSTOP` or held by a debugger, cannot
  exit this way, and the runner does not signal it: the record gets
  `cleanup_incomplete`, the session stops with a message that names the leaf's
  PID, and the operator ends it. This is the owner's decision (plan section
  14).
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
windowed broadcast cell it also prints each arm's median fence time. For each
headline metric it prints the median within-pair comparison with its minimum
and maximum and, beside it, the A/A spread. Nothing is flagged automatically.

- **A/A spread.** The largest deviation within a comparable A/A pair with the
  same effective parameters, taken from the output file and from `--aa`. In an
  A/A session, the session's own pairs are among them. The arm labels of an A/A
  pair are arbitrary, so the spread does not depend on them: a ratio r counts
  as max(r, 1/r) and is printed as a factor, such as `up to x1.012 over 2
  pairs`, and a difference counts as its absolute value.
- **Ratio or difference.** A metric is compared as the ratio B/A only when every
  observation of the cell and of its matching A/A pairs is available and not
  zero. Otherwise it is shown as the median of each arm with the difference
  B-A, and its A/A spread uses differences too. CPU metrics, the lost fraction
  and the coordinator's `shutdown()` calls are always shown that way. A
  slowest-receiver latency follows the general rule: it is a ratio until one
  observation is unavailable or zero. A zero observation is never dropped.
- **Unavailable.** An observation without a value is counted as unavailable, on
  the comparison and on the A/A spread: a slowest-receiver latency when no
  sampled message reached every receiver, and every CPU metric of a sample with
  the warning `cpu_not_advancing`. "unavailable" alone means that no pair has a
  value, or that no matching A/A pair exists.
- **Comparable pairs.** Two samples are compared when both are usable and have
  the same cell, toolchain and configuration, host label, operating system
  (`host.os`) and C. The operating system matters for the A/A pairs of earlier
  sessions: `host.os` is Python's `platform.platform()`, with the kernel release
  or the Windows build, so A/A records from another OS or kernel do not supply
  the spread. Library constants do not have to match. If the arms differ in
  one, the summary names it in a notice, as the start of the session does.
- **Fence time.** A windowed broadcast sender waits in a processing fence
  after every burst and once at the end. The measured interval includes those
  waits, so windowed rates and CPU per message include fence time.
  `throughput.fence_s` holds their sum. `send_call` does not include it: it
  runs from just before the sender writes the payload's two header fields
  (sequence and send time) to the return of the publish call, so it covers
  those two writes, the publish call and the overhead of the timestamps. With
  the MSVC build tested on Windows (development machine), `steady_clock` ticked
  in steps of 100 ns, which quantises `send_call`: the p50 of two arms was
  often exactly equal.
- **Open-loop sender CPU.** An open-loop sender polls the clock for the last
  100 µs before each intended arrival, so its CPU is mostly the generator's
  own. Do not read it as the cost of publishing. The summary shows the CPU
  seconds of the other roles: the receivers, and the coordinator of a
  leaf-origin cell.
- **Open-loop rates.** The stored rates are fractions of the windowed
  publication rate, which includes fence time. They are therefore light loads
  relative to the rate at which the sender can publish without fences.

| Mode | Headline metrics |
|---|---|
| Windowed | Publication and delivery rates; CPU µs per message, total and per role; `send_call` p50; `slowest_from_send` p50 and p99 |
| Open-loop | `slowest_from_intended` p50 and p99; lost fraction; `lateness` p99; CPU seconds of the roles that do not send |
| Request/response, windowed | Calls/s; CPU µs per call, total and per role; `issue_to_observation` p50 and p99 |
| Request/response, closed-loop | `rtt` p50 and p99; calls/s for the back-to-back cells (gap 0); CPU µs per call per role |
| Lifecycle | `time_to_ready_s`, `time_to_shutdown_s`; the coordinator's `shutdown()` calls, where a sample needed more than one; formation and shutdown CPU per role |

After the summary, the runner prints the axis tables of plan section 10 for arm
A, as the median over the usable samples of each cell: CPU µs per message by
role against R, total CPU µs per application MB against payload, open-loop
latency and loss against the offered rate, and each role's share of total CPU.
The two windowed tables show the fence share beside the CPU: `fence_s` as a
percentage of `interval_s`. The open-loop table shows the CPU seconds of the
roles that do not send. The share table covers windowed broadcast cells only,
because of the open-loop sender's polling. A sample with `cpu_not_advancing`
contributes no CPU value to them.

Closed-loop request/response cells have one table per route: against the gap,
the `actual_gap` p50, `rtt` p50 and p99, and each role's CPU µs per call. The
idle time before each call stands beside `rtt` because the two are read
together (see the request/response executable below).

Swarm cells have their own tables: the two lifecycle times and each role's
formation and shutdown CPU per leaf against N, with the coordinator's
`shutdown()` calls where a sample needed more than one, and each role's share
of a phase's CPU.

### Calibration

```text
python benchmarks/run_benchmarks.py --a <build-dir> --a-commit <sha> --output <calibration.jsonl>
    --calibrate <new-workloads.json> [--select <cell-id glob>]
```

Calibration runs each selected windowed broadcast cell three times with the A
binaries. Request/response counts and swarm sizes are fixed, so a calibration
leaves those cells out.
For a cell whose three runs are all usable, it rescales the windowed count so
that the measured interval is about `target_interval_s` (an integer of two
significant digits), stores the open-loop rates as the `rate_fractions` of the
median windowed **publication** rate (two significant digits), and records the
host label, commit and date. Cells that were not calibrated keep their values.

Commit a calibrated `workloads.json` only from a run on the reference host, so
that both arms and later sessions use the same absolute counts and rates.
Anywhere else, calibrate into a temporary file and pass it with `--workloads`.

## Workloads file

`workloads.json` holds the settings and the cells of every workload. A
broadcast entry of `cells` stands for one origin, receiver count and payload,
and defines one windowed cell and one open-loop cell per rate fraction. A
request/response entry of `cells` defines one cell; its keys follow the first
table. The swarm cells come from the `swarm` key.

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

| Key of a request/response entry (`"workload": "rpc"`) | Meaning |
|---|---|
| `route` | `leaf_to_coordinator` or `leaf_to_leaf` |
| `mode` | `closed_loop` or `windowed` |
| `gap_us` | Closed-loop only: the idle gap before each call, a non-negative integer of microseconds |
| `window` | Windowed only: the number of calls kept outstanding, a positive integer |
| `calls` | The measured calls: a positive integer. It is fixed, not calibrated |
| `handler_delay_us`, `wrong_echo_every`, `throw_every` | Optional validation-only options, passed to the executable. `handler_delay_us` needs the `leaf_to_leaf` route |

The plan's 3 s windowed interval and 3 s open-loop schedule are defaults: the
shipped `workloads.json` sets `target_interval_s` and `open_loop.duration_s` to
3, and another workloads file can set other values. An open-loop cell publishes
`ceil(rate × duration_s)` messages: all i with `i/rate` below the schedule. Its
completion may take longer than the schedule.

The runner rejects a file with a non-positive or fractional count, a rate that
is not finite and positive, a rate list whose length differs from the fraction
list, or two cells that expand to the same ID. In a request/response entry it
also rejects a `gap_us` on a windowed entry, a `window` on a closed-loop entry,
and a `handler_delay_us` on the `leaf_to_coordinator` route.

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
such a result stops its measurement, writes no report and exits non-zero, so
the sample is invalid with `missing_files`. Only that process exits non-zero:
the record has `exit_code_nonzero` as well only when it was the coordinator.

Every process leaves the swarm through `sintra::shutdown()`. A call that
returns false has retained the runtime for a retry
(`docs/reference/shutdown.md`), so the process repeats the call, at most 40
times, and exits non-zero if the runtime is still up after that.

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
| `--handler-delay-us d` | Busy time in a receiver's handler of every measured message | 0 |

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

## The request/response executable

```text
sintra_rpc_benchmark <cell options> --run-id <id>    one sample, as a swarm
sintra_rpc_benchmark --aggregate <run directory>     the sample's record, as one JSON object
sintra_rpc_benchmark --build-facts                   the build facts, as one JSON object
```

The run directory, the process reports, `--aggregate`, the barriers and the
shutdown work as for the broadcast executable: a process whose barrier was
bypassed writes no report. The callee is a transceiver whose `echo` is exported
with `SINTRA_RPC_STRICT` and returns its argument. One caller thread in the
last leaf resolves the callee's name once and passes the instance ID in every
call.

| Route | Callee | Caller | Roles |
|---|---|---|---|
| `leaf_to_coordinator` | A transceiver in the coordinator | Leaf 1 | `coordinator_callee`, `caller` |
| `leaf_to_leaf` | Leaf 1; the coordinator relays | Leaf 2 | `coordinator`, `callee`, `caller` |

| Cell option | Meaning | Default |
|---|---|---|
| `--mode closed_loop\|windowed` | Measurement mode | `closed_loop` |
| `--route leaf_to_coordinator\|leaf_to_leaf` | Where the callee lives | `leaf_to_coordinator` |
| `--calls n` | Measured calls, at least 1 | 10000 |
| `--warmup n` | Back-to-back synchronous warm-up calls; the runner passes 1000 | 1000 |
| `--gap-us g` | Closed-loop only: idle time before each call, in microseconds | 0 |
| `--window w` | Windowed only: calls kept outstanding, at least 1 | 32 |
| `--handler-delay-us d` | Busy time in the callee's handler of every measured call; `leaf_to_leaf` only | 0 |
| `--wrong-echo-every k` | The callee answers every k-th measured call with a wrong value | 0 (never) |
| `--throw-every k` | The callee throws in every k-th measured call | 0 (never) |

**The reader's wait policy decides what the cells report.** A ring reader
spins for 50 µs after its last message, then polls in 1 ms sleeps for up to
1 s, then parks (`include/sintra/detail/config.h:45`, `:49`, `:53`;
`include/sintra/detail/ipc/rings.h:3373-3402`, at commit `3666206f`). Once a
reader is past its spin, a strictly periodic caller can lock to the reader's
poll, and the runs so far were consistent with that (plan section 12). The
cells then mostly report the wait policy, not the call path. That is a property
of the library, which the cells show and do not work around. The counts are
fixed for that reason: a rate that was measured in one phase does not predict
the next sample.

**Closed-loop mode** makes one synchronous `rpc_echo` call at a time. The gap is
a `sleep_for` before every call, the first one included; with a gap of 0 the
calls run back to back without a sleep. `latency.rtt` is the time from the
start of a call to its return, and `latency.actual_gap` is the inter-call idle
time: from the return of the call before it, or from the begin rendezvous for
the first call, to the start of the call. The end barrier is a rendezvous after
the last call.

- **The back-to-back cell (gap 0) sits at the edge of the spin window.** A call
  that reaches a reader just after its spin waits for that reader's next poll,
  up to one poll period per hop. On the hosts tried so far, the cell's `rtt`
  distribution has two modes: tens of µs or less while every hop is caught
  spinning, and a millisecond or more once hops fall into the sleep phase. p50
  can move between the two from sample to sample. Read it together with
  calls/s, which follows the mean, and with the A/A spread.
- **Read `rtt` together with `actual_gap`.** In the 200 µs cells on WSL, `rtt`
  p50 plus `actual_gap` p50 was close to a whole number of poll periods. A
  difference in `rtt` that is smaller than a poll period is consistent with a
  phase effect: the call met the poll earlier or later. It does not show that
  the call path is unchanged, because phase locking can hide a regression
  smaller than one poll period. The axis table shows the `actual_gap` p50
  beside `rtt`.
- **Closed-loop CPU per call follows the cycle time.** That most of it is the
  readers' spinning and polling over one cycle is a hypothesis consistent with
  the runs so far: the CPU is whole-process CPU, which does not separate those
  costs. If so, a change of the call path can stay inside its A/A spread.
- **`actual_gap` is the inter-call idle time, not the sleep alone.** It also
  holds the caller's bookkeeping of the previous result and its loop.
  `sleep_for` follows the platform's timer: on Windows a 200 µs gap idles 1.0
  to 1.5 ms. That is still the wait regime the gap aims at, past the spin
  window and inside the one-second sleep phase.
- **A lost reply surfaces only as the runner's `timeout`.** The synchronous
  call has no deadline, so closed-loop mode never counts a call as `lost`.

**Windowed mode** keeps `--window` `rpc_async_echo` handles outstanding. When
the window is full, the caller waits on the oldest handle with `get_until`, up
to 10 s after that call's issue; a call that is still pending then counts as
lost. `latency.issue_to_observation` runs from the start of the issue to the
return of `get_until`. The end barrier is the final processing fence.

- **In the sleep phase, windowed calls/s was consistent with window / (k ×
  period),** for a whole number k of poll periods per window, on `leaf_to_leaf`
  on WSL. It did not follow the call path then: a Debug build gave the same
  calls/s as a Release build.
- **Windowed CPU per call is closer to the call path,** but it is whole-process
  CPU, which includes waiting and harness work.

Every call is recorded; there is no sampling. `throughput.interval_s` runs from
the return of the caller's begin rendezvous to its end snapshot, and
`throughput.calls_s` is the calls made over it. The record's counts read as
follows, and its other counts are null:

| Count | Meaning |
|---|---|
| `published`, `deliveries_expected` | The calls made |
| `deliveries` | The calls that returned a value |
| `lost` | Windowed mode: the calls that timed out |
| `call_errors` | The calls that threw, plus those that returned another value than their argument |

**Validation-only.** `--handler-delay-us`, `--wrong-echo-every` and
`--throw-every` exist to check the detection of errors, loss and timeouts. A
record is `validation_only` when one of them is not zero. They apply to
measured calls only. The delay runs on the reader thread that dispatches the
call. On the `leaf_to_coordinator` route that is a reader of the coordinator,
which a validation run does not slow down, so the executable and the runner
reject a delay on that route.

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
leaves the swarm through `sintra::shutdown()`, writes no report and exits
non-zero. A swarm sample has no planned duration, so the runner gives it the
60 s base timeout.

Keep `--output`, and with it the run directories, on a local filesystem. A leaf
writes its PID file during its start-up and its report before it exits, and
both are inside the measured phases of a swarm sample. So is the launch
description of the swarm, which every leaf builds as the coordinator does. These
costs were not quantified.

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
in lifecycle mode, at any size. The phases are short at small N, so per-sample
CPU values are coarse there; on the development machine (WSL) formation took
about 0.1 s at 1 leaf, 2 s at 80 and 3.8 s at 125 (plan section 5.3).

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

Plan section 7 defines the record and section 6 its metrics. Three of its
fields in more detail:

| Field | Content |
|---|---|
| `cell.burst` | Messages per fenced burst of the measured part; 0 for an unpaced run. Open-loop publication does not use it |
| `cell.handler_delay_us` | The handler delay of a validation run; otherwise 0 |
| `throughput.fence_s` | Windowed: the sender's time in its burst fences and its final fence, which `interval_s` includes. Open-loop: null |

`latency.fence` describes the burst fences only, not the final fence. In a
request/response record `cell.burst` and `throughput.fence_s` are null, and
`cell.handler_delay_us` is the callee's handler delay.

These are the reasons and warnings that the executable and the runner write:

| Reason | Condition |
|---|---|
| `build_not_release`, `build_without_ndebug`, `build_not_optimized`, `build_with_test_hooks` | The build is not valid |
| `timeout` | The runner terminated the sample at its timeout |
| `exit_code_nonzero` | The coordinator exited non-zero |
| `cleanup_incomplete` | A process of the run is left, or the runner could not establish that none is; the session stops |
| `missing_files`, `foreign_run_files` | A process report is missing, or belongs to another run |
| `unexpected_reports`, `inconsistent_reports` | A report is a duplicate or of a branch the cell lacks, or carries other cell settings |
| `aggregation_failed`, `run_id_mismatch` | `--aggregate` produced no record, also after a timeout, and when it did not finish within 60 s and the runner ended it; or it produced one of another run |
| `order_errors`, `size_errors` | An order error, or a wrong payload size or final byte |
| `call_errors` | Request/response: a call threw, or returned another value than its argument |
| `loss`, `eviction_log_matches` | Windowed and closed-loop mode: any loss (a lost message, or a call that timed out), or any eviction log match |
| `sentinel_missed` | Open-loop mode: a receiver did not see the sentinel within its drain allowance |
| `init_failed` | Lifecycle mode: `init` threw in a process |
| `shutdown_threw` | Lifecycle mode: a `shutdown()` call of a process threw |

| Warning | Condition |
|---|---|
| `cpu_low_confidence` | The measured interval is under 2 s |
| `cpu_not_advancing` | The CPU delta of some process over the measured interval is exactly zero |
| `rate_shortfall` | The achieved publication rate is under 95% of the offered rate |
| `ring_dir_not_empty` | `sintra-*` entries were already present in the ring parent directory |
| `not_oversubscribed` | Lifecycle mode: the oversubscribed cell has `N_over + 1 <= C` |
| `shutdown_retried` | Lifecycle mode: a process needed more than one `shutdown()` call |

A record with a missing or surplus process report is invalid and has null
`counts`, `throughput`, `latency` and `cpu`. A report of another run, or with
other cell settings, is set aside and makes the record invalid too. If the
expected reports are complete without it, the metrics are computed from those;
if it stands in for an expected report, that report is missing. When
`--aggregate` produces no record, for example because the sample left no report
at all, the runner writes its own record, with null metrics and the reason
`aggregation_failed`. Its `missing_files` names the expected reports that are
absent from the run directory, with the reason `missing_files` when there is
one. Its `cell` has every key of plan section 7; a key that only the executable
knows, such as `framed_bytes` and `drain_timeout_s`, is null there.

**Process CPU on Windows.** The Windows process CPU clock is coarse and can stay
at zero for a whole sample, however long the interval: neither 2 s nor 3 s makes
it usable. A sample with `cpu_not_advancing` has no usable CPU metric. A sample
without the warning is not reliable either: its process CPU can be several
times too low. CPU figures from a Windows host are not a reference. Read CPU
from Linux runs.
