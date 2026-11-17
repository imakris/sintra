# Sintra benchmark plan: first iteration

**Status:** design adopted. All three phases are implemented: the
harness, runner and broadcast workload (W1), request/response (W2), and swarm formation and
shutdown (W3). They were exercised on the development machine with WSL Ubuntu-24.04 (GCC, Clang)
and Windows (MSVC). The benchmarks run in GitHub CI on hosted runners (owner decision, section
1.1). Level 1 is implemented: every CI platform builds them and runs a smoke pass that fails only
on breakage or an invalid record (section 1.3). Level 2, a comparison of each master push with the
previous master on hosted runners, is a proposed design (section 15), not reviewed and not
implemented. It measures its noise and checks the cross-process clock inside each CI job instead
of keeping a stored A/A reference, and uses fixed counts without calibration (sections 15.6 and
15.11). Library facts were verified at commit `3666206f`; the benchmarks change no library code.

As `AGENTS.md` requires, three things are kept apart:
- owner requirements and recorded decisions (section 1);
- facts verified in the code (section 2);
- the adopted design (sections 3 to 13), this plan's own work rather than owner requirements.
  Section 14 lists choices implemented with the stated default that remain the owner's to change,
  and records the owner's decision on the cleanup of a leaf that cannot run.
- Section 15, CI level 2, keeps the same separation within itself. It is a proposal until
  independent reviewers report no blockers.

## 1. Owner requirements, agreed scope and recorded decisions

### 1.1 Owner requirements
- Sintra has many tests that assert correctness but no proper benchmark. The owner uses the tests
  as an unreliable, rough benchmark.
- First establish which characteristics of the library make sense to test, then a concrete set of
  benchmarks.
- Measure latency, throughput and scalability, not latency alone.
- Where it runs. Owner decision, 2026-11-16: "my reference host is not relevant. i want
  automation and things happening elsewhere, not on my hosts." Benchmark runs happen in GitHub CI
  on hosted runners; local runs only check function during development. This replaces the earlier
  dedicated Linux reference host ("system specs are not so important. we will have some reference
  system.").
- "leave futex tests... only system independent stuff."
  - Working reading (an interpretation, not the owner's words): no scenario, metric or code path
    depends on futexes, syscall tracing, hardware or kernel counters, or another OS-specific API.
    Only wall-clock timestamps, counts, and process CPU seconds (as the existing broadcast tool
    reports them) are used.
  - This plan applies that reading to what is measured: the executables' only OS-specific
    measurement code is the CPU-time mapping. Recording the process ID, so that the runner can
    count the expected reports, and the runner's per-OS cleanup (open decision 6) are
    operational, not measurement.
- "both cpu seconds and latencies are important", "please don't overcomplicate it", "first
  attempt", "starting point". The first iteration finds hotspots to a basic degree and serves as
  a regression reference; it is not final.
- The settled principles of `docs/design_principles.md` apply: the rings and the star topology
  stay; the coordinator is trusted infrastructure; exactly one complete-frame copy comes out of
  the ring; only what was lost may fail.

### 1.2 Agreed scope (lean first iteration; not to be widened)
- **Reported by every run**, as far as the workload has the item (sections 6 and 7): W1 reports
  all of them; W2 reports calls/s in place of the message rates, and CPU µs per call in place of
  per message; W3 reports its lifecycle times and the CPU of each phase in place of latency,
  throughput, CPU per message and loss.
  - latency: p50 and p99, plus p99.9 and the maximum where the sample count supports them, with
    the sample count always shown;
  - throughput: publication and aggregate delivery rates in messages/s and MB/s, with
    application and framed bytes kept separate;
  - process CPU seconds per role, and CPU µs per message;
  - loss and eviction counts: sequence gaps are primary, the log text corroborates;
  - build and host facts.
- **Workloads:** broadcast; request/response RTT (sync RPC on two routes at four gaps, plus one
  windowed async case for request throughput); swarm formation and shutdown.
- **Modes:** windowed; open-loop (three fixed offered rates); closed-loop (sync RPC).
- **Comparisons:** paired A/B runs with alternating order (5 pairs), plus an A/A run per workload
  as the noise floor.
- **Phases:** (1) harness, schema, paired runner, broadcast port, A/A; (2) request/response;
  (3) swarm formation.

### 1.3 Decisions recorded for this plan (the owner can change them)
- **Plan location:** `docs/benchmark_plan.md`.
- **Results:** JSONL kept outside git history; in CI, as workflow artifacts. Level 2 would keep
  per-run summaries, not records, in a data branch (section 15.8).
- **CI levels** (section 1.1). Level 1, implemented: `.github/workflows/benchmarks.yml` builds the
  benchmarks on every CI platform and runs one A/A pair of the cells of
  `benchmarks/workloads_smoke.json` with `--require-valid`. A job fails on breakage or an invalid
  record, never on a number or a warning. Level 2, proposed (section 15; the owner's decisions for
  it are in section 15.1): a paired comparison of each master push with the previous master on
  hosted runners for Linux, Windows, macOS and FreeBSD, reported and never gating.
- **Validation builds** on the development machine check function, not numbers. This host
  inventory was given to the plan and was not verified from the repository:
  - Linux: WSL `Ubuntu-24.04` (GCC 13.3, Clang 18.1, CMake 3.31, Ninja, 16 cores), built through
    `queued-build`;
  - Windows: Visual Studio 2026 / MSVC v145 / FASTBuild, from a short path under `C:\w`.
- **Hosts:** GitHub-hosted runners: Linux (GCC, Clang), macOS, Windows (MSVC, MinGW) and FreeBSD
  in a VM. The only OS-specific measurement code is the CPU-time mapping.
- **Swarm sizes:** this plan chooses them and the oversubscribed N (section 5.3); the owner can
  tune them.
- **Default broadcast grid:** R ∈ {1, 4, 16} for windowed and open-loop cells. R = 2 and 8 stay
  defined in the workloads file and run only when selected.

## 2. Verified facts (commit `3666206f`)

Paths without a top-level directory are under `include/sintra/detail/`.

### 2.1 The existing benchmark (`benchmarks/`)

All `benchmarks/` citations in this document refer to those files as they were at commit
`3666206f`, before Phase 1 replaced them.

**Build**
- `benchmarks/` holds `CMakeLists.txt`, `README.md`, `broadcast_benchmark.cpp` and
  `run_broadcast_benchmark.py`. `SINTRA_BUILD_BENCHMARKS` adds it (`CMakeLists.txt:152-156`).
- The target links only the interface target `sintra` and `sintra_dev_flags`
  (`benchmarks/CMakeLists.txt:1-2`; `CMakeLists.txt:67`). It does not enable test hooks, and it is
  header-only while `SINTRA_SEPARATE_COMPILATION` is undefined (`include/sintra/sintra.h:110-112`).
- Test hooks are defined by `sintra_test_core` (`CMakeLists.txt:204-209`) and `sintra_test_config`
  (`tests/CMakeLists.txt:1-7`). Hook guards test the macro's presence (`ipc/rings.h:3275`), so an
  external definition, even `=0`, enables them.
- A top-level configure with neither a build type nor configuration types defaults to Debug.
  Multi-configuration generators get Debug and Release (`CMakeLists.txt:55-64`).

**Publishing and fences**
- Only the coordinator publishes, with `world() << message`
  (`benchmarks/broadcast_benchmark.cpp:148-156`, `:272-274`).
- Leaves receive in a slot that takes `const std::string&` (`:107`).
- By default a processing fence closes each burst of `max(1, (message_ring_size/4)/(bytes+512))`
  messages and a final fence closes the run; `--burst` overrides the size, and `--burst 0` runs
  unpaced (`:176-182`, `:257-259`, `:89`, `:160`; `benchmarks/README.md:26`).
- Every process in `_sintra_all_processes` calls each named fence the same number of times:
  ready, one per warm-up burst, warm, one per measured burst, complete, results (`:82-96`,
  `:131-135`, `:144`, `:157-185`).

**Validation and eviction count**
- A wrong-size message is counted as an error and dropped (`:110-113`); warm-up messages return
  before the sequence check (`:116-118`). Measured messages are checked with
  `sequence != received || message.back() != 'x'` (`:122`), so after one loss every later
  message counts as an error, and gaps are not counted.
- Evictions are counted from log text containing "evict" (`:63-67`). The publisher resets its
  count after warm-up (`:165`); receivers never do (`:138-139`).
- `complete` ignores evictions (`:217-218`).

**Timestamps, CPU and throughput**
- The clock is `steady_clock` (`:22`). The payload string is built and filled once (`:150`); the
  send stamp precedes writing the sequence and timestamp fields and serialization (`:152-155`).
  The receive stamp follows the `std::string` argument (`:107`, `:125`).
- All receivers' latencies are pooled (`:212-215`); the percentile index is `floor(q*(n-1))`
  (`:73-80`).
- CPU is user plus system time, from `getrusage(RUSAGE_SELF)` or `GetProcessTimes` (`:42-61`).
  Writer CPU is the coordinator process over the measured interval (`:170`, `:184`).
- Reader CPU spans the first to the last measured handler only on complete reception: `last_cpu`
  is set only at `received == messages` (`:119-129`). After a loss, the span can be negative
  (`:138`).
- Throughput counts aggregate deliveries (`:228`).
- Windows CPU values may be zero, or advance in 15.625 ms steps (`benchmarks/README.md:42-44`).

**Runner**

| `run_broadcast_benchmark.py` setting | Value | Lines |
|---|---|---|
| Pairs; receivers; timeout | 3; 1/2/4/8/16; 30 s | `:68-70` |
| Payload/messages | 64/1,000,000; 4096/100,000; 261632/4096 | `:72` |
| Warm-up; order | `max(64, n/20)`; alternating | `:32`, `:79` |
| Comparable pair | both samples complete with exit 0 | `:85-86` |
| Host CPU | Windows-only, via `GetSystemTimes` | `:16-27`, `:57-59` |
| Timeout kill | Windows `taskkill /T` (process tree); POSIX `killpg` (one process group) | `:43-47` |
| Summary | median delivered/s | `:93-101` |

**Related helpers.** `Shared_directory` reuses a path from its environment variable and never
deletes it (`tests/test_utils.h:149-156`, `:181-184`). `sintra_ring_wait_hint_test --benchmark`
measures raw-ring readers within one process (`tests/ring_wait_hint_test.cpp:487-493`;
`TESTING.md:180-201`); it is outside this plan.

### 2.2 Library behaviour the plan depends on

**Topology**
- Every process reads the coordinator's rings (`process/managed_process_impl.h:3545-3554`); the
  coordinator adds one reader per peer (`:7036-7041`, from `:6150-6152` and
  `process/coordinator_impl.h:623`). Each reader runs a request and a reply thread
  (`messaging/process_message_reader_impl.h:378-400`).
- The coordinator relays into its own rings: leaf events (`:865-868`), requests to other
  processes (`:875-880`), requests to its own service instances (`:744-753`), and replies
  (`:1093-1096`).
- The coordinator's reader for the caller's ring dispatches a request to an ordinary transceiver
  hosted in the coordinator directly, without a relay (`:722-833`).
- So leaf-origin broadcast and leaf-to-leaf RPC pass through the relay, while coordinator-origin
  broadcast and leaf-to-coordinator RPC to a user transceiver do not.

**Reader waits**
- An ADAPTIVE wait spins for 50 µs (`config.h:45`; `ipc/rings.h:3376-3381`), then sleeps in 1 ms
  cycles for up to 1 s (`config.h:49-53`; `:3386-3400`), then parks on a semaphore, nominally
  1.00005 s after the wait began (`:3405-3533`), re-checking on a 1 s watchdog (`:3273`,
  `:3523-3531`). A BLOCKING wait parks at once (`:3345`, `:3373`).
- Managed readers start in BLOCKING (`messaging/process_message_reader_impl.h:694`, `:994`).
- After relevant traffic, a managed reader stores ADAPTIVE for the next wait (`:718-720`, `:728`,
  `:743`, `:844`, `:1034-1036`). For the coordinator, every message that passes validation
  (`:712-715`, `:1029-1032`) is relevant.
- Each wait uses the stored hint. Catching up stores BLOCKING for the following wait
  (`messaging/message.h:961-967`).
- The 1 ms sleep on Linux is `std::this_thread::sleep_for` (`time_utils.h:260-268`).

**Frames and the writer**
- The ring is 2 MiB (`messaging/message.h:40`); a frame is aligned to at least 16 bytes and must
  be strictly smaller than ring/8 = 262144 bytes (`:145-148`, `:183`).
- The frame size is `Message_prefix::bytes_to_next_message` (`:435`). The dispatched frame is
  `sintra::s_tl_current_message` (`messaging/process_message_reader.h:137`, set at
  `messaging/process_message_reader_impl.h:708`).
- One thread at a time writes a ring (`ipc/rings.h:4559-4578`, `:4608`); each commit flushes the
  wakeups of parked readers under the ring spinlock (`:4171-4186`).

**Eviction**
- Eviction is on by default (`ipc/rings.h:161-164`) and considered only while the writer is
  blocked entering an octile that a reader still guards (`:4226-4247`, `:4423`).
- The writer runs a reclamation pass when the configurable budget (default 5000 µs) expires
  (`:166-173`, `:4409`, `:4519-4527`; a zero budget scans at once, `:4515-4517`), or after a spin
  count when `SINTRA_EVICTION_SPIN_THRESHOLD` is defined and positive (`:4404-4406`).
- A reader is eligible if it is active and guarded, and either guards the new octile or lags the
  writer's pending sequence by more than `SINTRA_EVICTION_LAG_RINGS` rings, default 1
  (`:175-177`, `:4297-4310`).
- A reader that is copying, or has a pending guard, is not evicted in that pass (`:4366-4378`).
  Each eviction is counted in the ring control block (`:4382`).
- The evicted reader resynchronises at the published head (`:3725-3750`). Its process then logs a
  warning containing "reader evicted on" through the log callback (`messaging/message.h:1087-1099`;
  `logging.h:87-99`).
- Overload loss is by design (`docs/architecture.md:89-99`).

**Launch budget**
- Process indexes have 8 bits (`config.h:32`), so the maximum index is 127 (`id_types.h:164`).
- A new coordinator resets the counter and takes index 2
  (`process/managed_process_impl.h:3430-3431`).
- Each automatic allocation takes the next index and throws above 127 (`id_types.h:267`,
  `:277-279`). Allocations come from branches (`process/managed_process_impl.h:6528`), spawned or
  invited processes without an explicit ID (`runtime.h:1258-1261`, `:1377-1379`), and joins
  (`process/coordinator_impl.h:2054`).
- Recovery reuses an identity (`process/managed_process_impl.h:6133-6135`). So a coordinator can
  allocate 125 fresh indexes automatically, however many processes are alive at once.

**Lifecycle, RPC, ring files and clock**
- The coordinator's `init` spawns every branch and returns after a startup barrier of all
  processes (`process/managed_process_impl.h:6517-6580`, `:6613-6614`). In a branch, `init` runs
  the entry function before returning (`runtime.h:672-675`; `process/managed_process_impl.h:6690`).
- `shutdown()` runs a processing fence, the coordinator hook, a drain wait for peers, then teardown
  (`docs/reference/shutdown.md:53-58`). On POSIX, `init` calls `setsid()` (`runtime.h:638-644`).
- The final custody join waits at most 250 ms; when it is incomplete, `shutdown()` returns false
  and keeps the runtime for a sequential retry (`runtime.h:777-781`;
  `docs/reference/shutdown.md:26-30`, `:92-94`).
- `init` ends in `go()`, which publishes the process's name to the coordinator and then runs a
  branch's entry function (`process/managed_process_impl.h:6677-6690`).
- A spawned process gets `--lifeline_handle`, `--lifeline_exit_code` and `--lifeline_timeout_ms`
  (`process/managed_process_impl.h:6222-6265`). When its lifeline reaches EOF, the watcher
  (`:1311-1329`) calls `signal_lifeline_shutdown` (`:1268-1289`), which schedules `_exit` after
  the timeout (`:1245-1266`; 100 ms by default, `:1237`, `process/managed_process.h:141`). The
  watcher is a detached thread started in `init` (`:1332`, `:1381-1392`, `:3472`), and no code
  stops it, so a leaf that has finished its `shutdown()` still exits with the coordinator.
- `SINTRA_RPC_STRICT` always uses the transport (`docs/reference/rpc.md:101-108`). For a
  same-process target, a non-strict blocking call takes a direct shortcut
  (`transceiver_impl.h:1554-1568`) and a non-strict async call throws (`:1417-1423`).
- A `Resolvable_instance_id` built from a name asks the coordinator on every construction
  (`resolvable_instance.h:20-23`; `resolvable_instance_impl.h:13-16`). The phrase "on first use"
  in `docs/reference/rpc.md:40-41` is imprecise.
- Ring files live in a private directory under `std::filesystem::temp_directory_path()`
  (`process/managed_process_impl.h:6832`; `ipc/private_resources.h:322-328`).
- On Linux, the library's cross-process timeline uses `CLOCK_MONOTONIC` (`time_utils.h:6-9`,
  `:66-71`), and a swarm shares one time namespace (`docs/design_principles.md:88-89`).

**Processing fence after a leaf's sends** (leaf-origin pacing relies on this)
1. A processing fence is a rendezvous, a local delivery fence, then a second rendezvous
   (`barrier.h:85-123`, `:153-171`, `:189-198`).
2. The delivery fence captures each reader's leading sequences after the rendezvous. It waits
   until the reader's published progress reaches them
   (`process/managed_process_impl.h:7265-7350`, `:7362-7382`).
3. Progress is published after the handlers and the post-handler run
   (`messaging/process_message_reader_impl.h:858-862`, `:883-890`).
4. A leaf writes its events and its barrier request to its own request ring, in call order
   (`transceiver_impl.h:741-757`, `:787`, `:1517`, `:1581-1584`).
5. The barrier targets a `Process_group` service instance (`process/coordinator.h:51-55`).
6. The coordinator's reader for the leaf relays the barrier request into the coordinator's ring
   (`messaging/process_message_reader_impl.h:744-753`). It does so after relaying the leaf's
   earlier events from the same ring (`:865-868`).
7. `relay()` publishes before it returns (`messaging/message.h:1129-1139`).

So the sender's arrival is processed only after its earlier events are in the coordinator's ring,
and every receiver captures its fence target after that point. A receiver that has not been
evicted therefore runs its handlers for those copies before the second rendezvous completes. This
matches `docs/reference/barrier_modes.md:49-53` and `docs/barriers_and_shutdown.md:24-26`.

**After an eviction**
- The reader resumes at the head (`messaging/message.h:1087-1099`); its eviction handler only
  clears the name cache (`messaging/process_message_reader_impl.h:335-339`).
- A fence wait ends when published progress reaches the target, the progress record is gone, the
  stream has stopped, or communication is no longer running
  (`process/managed_process_impl.h:7306-7350`). Progress is published after a fetched frame's
  handlers run (`messaging/process_message_reader_impl.h:883-890`).
- So a live reader with an unsatisfied target can stall when recovery produces no later progress
  publication.

## 3. What to measure (adopted design)

| Characteristic | Why it costs (section 2) | Where measured |
|---|---|---|
| Fan-out | Every receiver reads the coordinator ring and copies each frame | W1, receivers (R) axis |
| Frame size | Frame copy out of the ring, string materialization, relay copy | W1 payload axis, read through `send_call`: the windowed rate is fence-bound (section 12) |
| Coordinator relay | The coordinator reads every ring and relays | W1 leaf vs coordinator origin; W2 routes |
| Wake-up | ADAPTIVE and BLOCKING reader waits | W2, gap axis |
| Request path capacity | One request-reader thread per hop | W2, windowed async |
| Writer backpressure | The writer waits for readers, may evict, and wakes parked readers | W1 send-call time, lateness, loss |
| Lifecycle | Spawning, per-peer readers, startup barrier, shutdown protocol | W3, N axis |

## 4. Harness and executables (adopted design)

### 4.1 Files

The plan extends `benchmarks/` and keeps `SINTRA_BUILD_BENCHMARKS`. Outside `benchmarks/`, it
changes only the option's description at `CMakeLists.txt:152` and adds the CI workflow
`.github/workflows/benchmarks.yml` (section 1.3).

| File | Content |
|---|---|
| `benchmarks/harness.h` | `steady_clock` ns (as now); `cpu_seconds()` moved unchanged, the only OS-specific measurement code (the PID file, written through the library's `sintra::get_current_pid()`, `ipc/platform_defs.h:103`, only lets the runner count the expected reports; no test header is included); the checked barriers and the report-set check that all three executables use; build facts; per-process result files; aggregation; JSON output |
| `benchmarks/broadcast_benchmark.cpp` | W1, target `sintra_broadcast_benchmark`, rewritten on the harness |
| `benchmarks/rpc_benchmark.cpp` | W2, target `sintra_rpc_benchmark` (phase 2) |
| `benchmarks/swarm_benchmark.cpp` | W3, target `sintra_swarm_benchmark` (phase 3) |
| `benchmarks/workloads.json` | Cells, windowed counts, stored open-loop rates, per-cell calibration metadata |
| `benchmarks/workloads_smoke.json` | The CI smoke cells (section 1.3): tiny fixed counts, a low explicit open-loop rate |
| `benchmarks/run_benchmarks.py` | Paired runner; replaces `run_broadcast_benchmark.py`, which is deleted |
| `benchmarks/CMakeLists.txt` | The three targets, linked as now, plus `SINTRA_BENCHMARK_CONFIG="$<CONFIG>"` |
| `benchmarks/README.md` | Rewritten as the usage guide |

The committed `benchmarks/workloads.json` carries no open-loop rates: no calibration for hosted
runners is defined, and level 2 uses fixed counts from its own file (section 15.5). The windowed
counts are the existing ones (section 2.1).

### 4.2 Executable modes
- **`<exe> <cell options> --run-id <id>`** runs one sample as a swarm. Processes keep raw data in
  memory and do no aggregation while measuring. Each writes `<role>-<branch index>.bin` (a
  versioned fixed header plus fixed-size sample records, read only by the same executable) into
  the run directory named by `SINTRA_BENCHMARK_DIR`. W1 and W2 write it right after the last
  rendezvous of section 4.4, before `shutdown()`, whose opening processing fence
  (`docs/reference/shutdown.md:53-58`) can stall on an evicted receiver (section 2.2) and would
  otherwise lose the counts. They then call `shutdown()` until it returns true, at most 40 times;
  a false result after that makes the process exit non-zero. W3 writes it after the
  `shutdown()` call that returns true (or throws), because W3 measures shutdown.
- **`<exe> --aggregate <dir>`** checks the run ID and the expected roles, computes every metric
  and prints one JSON object, exiting 0 whenever it can produce a record, valid or not.
  **`<exe> --build-facts`** prints the build facts.

### 4.3 Build facts

Compiled in: the configuration (`SINTRA_BENCHMARK_CONFIG`); `NDEBUG`; optimization
(`__OPTIMIZE__`, or on MSVC Release without `_DEBUG`, a proxy rather than proof:
`CMakeLists.txt:118-138`); the compiler (`__VERSION__` or `_MSC_FULL_VER`);
`SINTRA_ENABLE_TEST_HOOKS`, `SINTRA_SEPARATE_COMPILATION`, `SINTRA_ENABLE_SLOW_READER_EVICTION`,
`SINTRA_EVICTION_LAG_RINGS`, and `SINTRA_EVICTION_SPIN_BUDGET_US` (or
`SINTRA_EVICTION_SPIN_THRESHOLD` when defined and positive); `message_ring_size` and
`k_sintra_ring_abi_version`. Read at run time: `temp_directory_path()` and
`hardware_concurrency()`. A build is valid only if its configuration is Release (compared
case-insensitively), `NDEBUG` is defined, optimization is on (or the MSVC proxy holds) and test
hooks are off.

### 4.4 Measurement boundaries and CPU per role (W1 and W2)

- **Begin.** After the warm-up fence `benchmark-warm` returns, every process resets its counters
  (including the eviction-log count) and takes its begin snapshot (clock and CPU), then calls the
  rendezvous `benchmark-begin`. The sender starts measured publication or calls only after it
  returns; its throughput interval starts there.
- **End.** After the mode's end barrier returns (windowed: the final fence `benchmark-complete`;
  open-loop: the rendezvous `benchmark-drained`; closed-loop: the rendezvous
  `benchmark-complete`), every process takes its end snapshot, then calls `benchmark-end`. A W1
  process first deactivates its receive slot; deactivation waits for a running handler, so
  reception ends at the end snapshot, and the counters belong to the handler thread (no
  per-message lock) and are read only after it. In W2 the caller thread owns the counters. No
  process writes result files, tears down or shuts down before `benchmark-end` returns.
- **Coverage.** Every CPU interval, the sender's included, contains the begin rendezvous, because
  the snapshot precedes it; non-senders' intervals also contain their waiting for measured
  traffic. The value covers every thread, including Sintra's readers while they spin, poll, or
  enter and leave waits; parked time is not CPU.
- **Roles.** A process with two roles (the coordinator as sender or callee) is reported once,
  under a combined role name. CPU is never split by thread. These boundaries replace the
  existing first-to-last-handler span for receivers.
- **Bypassed barriers.** Every executable calls its barriers through the harness, which checks
  them. A barrier that returns `sintra::invalid_sequence` was not held; the process that saw it
  fails the sample: it writes no report, so the record gets `missing_files`, and it exits
  non-zero.

## 5. Workloads and parameter grids (adopted design)

**Modes.** Every record carries one mode label: `windowed`, `open_loop`, `closed_loop` or
`lifecycle` (W3). Open-loop applies to W1 only.

**Open-loop schedule**
- The measured messages are all i with `i/rate < 3 s`, intended at `t0 + i/rate`, where `t0` is
  10 ms after the sender's begin rendezvous returns. The 3 s is the offered schedule; completion
  may take longer.
- The generator sleeps (`sleep_until`) until 100 µs before each arrival, then polls the clock. It
  never skips a message: a late generator sends at once, so backlog shows up as lateness and as
  latency from the intended arrival.

**Open-loop rates**
- Each cell stores three absolute rates: 0.1X, 0.5X and 0.8X, rounded to two significant digits.
- X is the cell's median windowed **publication** rate over three baseline calibration runs:
  messages the sender published per second over its measured interval, never the aggregate
  delivery rate. Both arms of a pair use the stored rates. Level 2's first version runs no
  calibration (section 15.11), so no rates are stored yet.
- Because the windowed rate is fence-bound, these rates are light loads (section 12).

**Windowed counts.** Calibration scales each count so the measured interval is about 3 s. That
does not by itself make Windows CPU values usable (section 12).

### 5.1 W1: Broadcast (18 windowed + 54 open-loop cells by default; 30 + 90 defined)

**Grid**
- Origin `coordinator`: the coordinator publishes to R receiver leaves.
- Origin `leaf`: branch 1 publishes without a slot to R receiver leaves; the coordinator relays.
- R ∈ {1, 4, 16} by default (R = 2 and 8 only when selected); payloads {64, 4096, 261632} bytes,
  as `std::string` (as now).

**Windowed mode**
- It keeps the existing burst formula, fences and strict validation.
- Every process in `_sintra_all_processes` calls each named fence the same number of times, as
  today. In leaf origin, that includes the coordinator (which publishes nothing), the sender leaf
  and every receiver.
- The sender's fence after a burst covers the relayed copies at every receiver that has not been
  evicted (section 2.2). The protocol therefore carries over to leaf origin unchanged.
- A sample in which an eviction occurs may end by timeout, because the evicted receiver's fence
  can stall (section 2.2).
- The measured interval is mostly fence time, so the windowed rate is a paced, fence-bound rate
  (section 12).

**Open-loop drain**
1. After the last measured message, the sender stays quiet for 100 ms, so a receiver that fell
   behind or was evicted can resynchronise, then writes a sentinel (sequence `UINT64_MAX-1`).
2. A control rendezvous `benchmark-published` follows: non-senders enter it right after
   `benchmark-begin`, the sender after publication, the quiet period and the sentinel.
3. Each receiver's 10 s drain allowance starts when `benchmark-published` returns, so a late
   sender cannot cause `sentinel_missed`. Then come `benchmark-drained` and section 4.4's end.

The sentinel is not a measured message; the publication interval still ends when the last
measured publish call returns.

**Payload, handler, sampling and framed bytes**
- As now, the payload holds the sequence, the send start in ns, filler and a final `'x'`. Warm-up
  sends `max(64, n/20)` messages carrying `UINT64_MAX`, always paced with the default burst, also
  when `--burst 0` unpaces the measured part.
- The receiver handler checks size and final byte, applies the loss accounting of section 6,
  tracks its latest receive time over all measured messages, and stores the sequence, send start
  and receive time of sampled ones.
- A sequence is sampled when `splitmix64(sequence) mod k == 0`, `k = max(1, ceil(n/100000))`: a
  budget of about 100k (all messages when n ≤ 100000, roughly 50k to 100k above). Every process
  and both arms sample the same sequences, so slowest-receiver latency joins on the sequence.
- The first measured handler reads `s_tl_current_message->bytes_to_next_message` through the
  harness. In leaf origin each frame is written twice (sender ring and relay).
- That read is one of the benchmarks' uses of library internals. The others are
  `sintra::detail::message_frame_size_limit` (the payload bound that `broadcast_benchmark.cpp`
  checks), `sintra::detail::k_sintra_ring_abi_version` (a build fact, section 4.3) and the
  `--branch_index` argument of a spawned branch (`swarm_benchmark.cpp`).

**Validation-only options:** `--handler-delay-us` (receiver leaves only, never the coordinator;
principle 3) and `--burst`, including the existing unpaced `--burst 0`. A record is
`validation_only` (section 8) when they change the effective workload: a non-zero handler delay,
or, in windowed mode, a measured burst other than the formula's value. Mere presence of an option
does not count.

**Port of the existing benchmark**
- Kept: the coordinator-origin workload and payloads; the burst formula, fences and warm-up rule;
  alternating order.
- Changed: gap accounting, with strict validation only in windowed mode; resets before the begin
  rendezvous; CPU at common boundaries; nearest-rank percentiles; aggregation after the swarm
  exits; 5 pairs instead of 3.
- Removed: the Windows-only host CPU field (an OS-specific API); p95; `complete`, which
  `outcome.valid` replaces.

### 5.2 W2: Request/response (10 cells: 2 routes × (4 closed-loop gaps + 1 windowed))

**Setup**
- The callee is a transceiver with `uint64_t echo(uint64_t)`, exported with `SINTRA_RPC_STRICT`
  and named with `assign_name`. Every call must return its argument.
- One caller thread, always in another process, resolves the callee's name once and passes the
  retained instance ID in all 1000 synchronous back-to-back warm-up calls (in both modes) and
  every measured call; a name argument would be resolved again on every call (section 2.2).
- Every measured call is recorded; there is no sampling. All counts are fixed in the workloads
  file; W2 has no calibration and no `calibration` key.
- Validation-only options, applied to measured calls only: `--handler-delay-us` (accepted on
  `leaf_to_leaf` only, so the coordinator is never delayed, as in W1), `--wrong-echo-every k` and
  `--throw-every k`. A record is `validation_only` when one is not 0. A `gap_us` on a windowed
  entry or a `window` on a closed-loop entry is a workloads-file error.
- **Reading the cells:** they mostly report the reader wait policy (section 12). Read `rtt`
  together with `actual_gap`. A difference below one poll period is consistent with a phase
  effect, but phase locking can also hide a real change of that size (section 12).

**Routes:** `leaf_to_coordinator` (the callee is a user transceiver in the coordinator; leaf 1
calls) and `leaf_to_leaf` (the callee is leaf 1; leaf 2 calls through the relay). Cell IDs are
`w2.closed_loop.<route>.g<gap_us>` and `w2.windowed.<route>.w<window>`.

**Closed-loop cells (sync `rpc_echo`)**
- The gap is a `sleep_for` before every call, the first one included, so every call meets the
  regime of its gap. `actual_gap` records the inter-call idle time, from the previous return or,
  for the first call, from the begin rendezvous, to the start of the call. It includes the
  caller's bookkeeping of the previous result and the loop as well as the sleep, and it has one
  observation per call only when no call failed. On Windows a 200 µs gap idles 1.0 to 1.5 ms
  (development machine); that is still the same wait regime.
- The end barrier `benchmark-complete` is a rendezvous here and a processing fence in windowed
  mode. A lost reply surfaces only as the runner's `timeout`: the synchronous call has no
  deadline.
- The regimes in the table are those reached on the development machine (section 12), not
  guaranteed for every hop.

| Gap | Calls | Wait regime reached |
|---|---|---|
| 0 | 10000 | spin only part of the time on `leaf_to_coordinator`, in no `leaf_to_leaf` sample; else the 1 ms sleep phase |
| 200 µs | 5000 | 1 ms sleep phase |
| 20 ms | 500 | 1 ms sleep phase |
| 2 s | 60 | parked (the sleep phase ends nominally after 1.00005 s); with 60 calls, p99 is the sample maximum |

**Windowed cells**
- The caller keeps W = 32 `rpc_async_echo` handles outstanding and waits on the oldest, with
  `get_until(issue + 10 s)`, when the window is full; a timeout counts as a lost call. Its
  latency is `issue_to_observation` (section 6). The counts are 200000 calls on
  `leaf_to_coordinator` and 40000 on `leaf_to_leaf`.

### 5.3 W3: Swarm formation and shutdown (4 points)

**Run.** Each run is a fresh coordinator process with N leaf branches whose entry functions are
empty; it uses N of the 125 automatically allocated indexes and yields one scalar per run, not a
percentile population. Cell IDs are `w3.lifecycle.n<N>` and `w3.lifecycle.n_over`; the workloads
keys are `swarm.leaves`, `swarm.oversubscribed_least` and `swarm.oversubscribed_per_cpu`. The
executable takes `--leaves` and `--cpus`. A swarm sample has no planned duration.

**Sizes**
- N ∈ {1, 8, 40, N_over}, `N_over = min(125, max(80, 2C))`, where C is the runner's `--cpus` or
  else `hardware_concurrency()`; with C = 0 the runner skips that point and reports why.
- N_over is 80 for C < 40, 2C for 40 ≤ C ≤ 62, and 125 for C ≥ 63. For C ≥ 126 even
  `N_over+1` does not exceed C, and the point is labelled `not_oversubscribed`.
- 40 matches `mass_join_test` (`TESTING.md:205`). Threads oversubscribe earlier than processes:
  each leaf adds 2 reader threads in the coordinator and 2 of its own. Every point records
  `(N+1)/C`. The sizes are owner-tunable.

**Formation**
- Each process takes its formation snapshot (clock and CPU) immediately after its top-level
  `init` returns, which with empty entry functions is right after formation (section 2.2). It
  then checks `sintra::process_index()` against the `--branch_index` it parsed.
- `time_to_ready_s` is the duration of the coordinator's `init`. A leaf's `init` returns slightly
  later, because the leaf then publishes its name to the coordinator (section 2.2). The CPU a
  process spends between its formation snapshot and its shutdown-begin snapshot (for the
  coordinator, serving those name publications; for all, the rendezvous `benchmark-formed`) is in
  neither phase: under 3.5 % of `time_to_ready_s` at 80 leaves on the development machine.

**Shutdown**
- After `benchmark-formed`, each process takes its shutdown-begin snapshot and calls
  `shutdown()` until a call returns true; the end snapshot follows that call. The shutdown phase
  thus runs from the first call to the return of the call that returns true.
- A false return (the bounded custody join, section 2.2) is a result, not an invalidity. The
  runner's timeout bounds the retries. `shutdown_calls` and `first_shutdown_call_s` describe the
  coordinator's calls. `time_to_shutdown_s` is continuous across the point where a second call
  becomes necessary and does not depend on the 250 ms bound; only those two fields do, and
  `shutdown_calls` can flip between 1 and 2 near that point.
- On the development machine (WSL, 16 CPUs) the coordinator's first call returned false in every
  sample with 80 or more leaves: 80 leaves needed 2 calls, 125 leaves needed 4 calls and 1.38 to
  1.53 s, and at 70 leaves one of four samples needed a second call. On Windows 80 leaves needed
  one call.
- A leaf that has finished its `shutdown()` still exits when the coordinator does, through its
  lifeline (section 2.2).
- Leaves record their `main` entry (for `leaf_start_max_s`) and their own shutdown phase (for
  `leaf_shutdown_max_s`).

**CPU.** Per role, formation CPU is the cumulative CPU at the formation snapshot (including
start-up); shutdown CPU is the change between the shutdown snapshots. The phases are short at
small N and longer at large N. On the development machine (WSL), formation took about 0.1 s at
1 leaf, 1 s at 40, 2 s at 80 and 3.8 s at 125 (with `--cpus 126`); shutdown took at most about
0.55 s up to 80 leaves and 1.4 to 1.5 s at 125. Per-sample CPU values are coarse, at small N
especially, and are read through the pairs; `cpu_low_confidence` is not emitted for W3 at any
size. On Windows every sample has `cpu_not_advancing`, so W3 CPU there is unusable (section 12).

**Validation-only options:** `--fail-init` (a branch with a missing binary, so `init` throws) and
`--leaf-exit-delay-ms` (a leaf outlives its `shutdown()`, so the coordinator's first call returns
false; a very large value makes the timeout fixture). Workloads keys `swarm.fail_init` and
`swarm.leaf_exit_delay_ms`.

## 6. Metric definitions (adopted design)

**Percentiles**
- Nearest rank over each metric's own observations, `x[ceil(q*n_obs)-1]` in µs; each latency
  object reports `n_obs` (observations) and `n_messages` (distinct items) separately.
- p50 and p99 are always reported; p99.9 and the maximum only when `n_messages` ≥ 10000, else null.
- The receive timestamp is taken at handler entry, before any bookkeeping; the send-end timestamp
  immediately after the publish call returns.

| Name | `n_obs` | `n_messages` | Definition |
|---|---|---|---|
| `delivery_from_send` | receiver × sampled message | sampled messages with at least one delivery | receive − send start |
| `slowest_from_send` | sampled messages delivered to every receiver | same | maximum over receivers |
| `delivery_from_intended`, `slowest_from_intended` | as above | as above | as above, from the intended arrival (open-loop) |
| `send_call` | sampled sends | same | send end − send start: the benchmark's two payload-header writes (sequence and send start), then the publish call with its serialization, reservation waits and API overhead (`benchmarks/README.md:34-36` at `3666206f`); includes the overhead of the timestamps |
| `lateness` | sampled sends | same | send start − intended arrival (open-loop) |
| `fence` | bursts | bursts | each burst fence (windowed); the final fence is stored as the sender's last interval and counts only in `throughput.fence_s` |
| `rtt` | calls | calls | return − call start (W2 closed-loop) |
| `issue_to_observation` | calls | calls | `get_until` returning the result − issue start (W2 windowed); a result may be complete before `get_until` observes it (`transceiver_impl.h:1071-1095`) |
| `actual_gap` | calls | calls | inter-call idle time: from the previous return, or from the begin rendezvous, to the call start; includes the sleep and the caller's bookkeeping (W2 closed-loop) |

**Throughput**
- Intervals: windowed, from the sender's begin rendezvous to its end snapshot; open-loop
  publication, from `t0` until the last measured publish call returns; open-loop delivery, from
  `t0` to the latest measured-message handler timestamp across receivers, tracked for every
  measured message regardless of sampling. The sentinel is excluded.
- Each interval yields messages/s, application MB/s (messages × payload / 10^6 / s) and framed MB/s
  (messages × framed bytes / 10^6 / s).
- A windowed W1 cell also records `throughput.fence_s`, the sender's time in processing fences
  (burst fences plus the final fence). The windowed interval includes it, and so do the windowed
  CPU values; on the development machine it was about 50 to 99 % of the interval. Windowed
  throughput is therefore a paced, fence-bound rate, not a capacity (section 12). The fence-free
  rate is published / (`interval_s` − `fence_s`). W2 records carry `fence_s` as null; W3 records
  have no `throughput`, `counts`, `latency` or `cpu` object (they are null).
- W2 reports calls/s over the interval from the return of the caller's begin rendezvous to its end
  snapshot, in both modes.

**CPU**
- `cpu_s` is summed over a role's processes; `max_process_cpu_s` is its largest process;
  `us_per_message` is `cpu_s` × 10^6 divided by the messages published or calls made. Receivers
  also report `us_per_delivery`, and `total` sums all roles.

**Loss**
- Per receiver, with e the next expected sequence and s the received one: s > e adds `s-e` to
  `lost` and one to `gap_intervals`; s < e adds one to `order_errors`; then e = s+1. At the end,
  if e < n, `lost += n-e` and `gap_intervals++` — except in a record with `sentinel_missed`, where
  `lost` holds only confirmed gaps up to the last received message and `unresolved_tail` holds the
  not-yet-observed tail (0 otherwise).
- `eviction_log_matches` counts log lines containing "reader evicted on" after the reset. It
  corroborates the loss counts and is never used alone.

## 7. Record schema (adopted design)

The runner writes one JSON line per sample, with `schema: "sintra-bench/1"`. Units are s, µs and
MB (10^6 bytes). Values that do not apply, or that lack sample support, are null.

| Field | Meaning |
|---|---|
| `run_id`, `pair_id`, `arm`, `order`, `aa` | Sample ID (also in every process file); pair key; `"A"`/`"B"`; 0 if this sample ran first in its pair; A/A flag |
| `commit`, `note` | Operator-supplied commit for this arm; operator note |
| `host` | `{label, os, hardware_concurrency, cpus_effective}`; `os` is Python's `platform.platform()` |
| `build` | Facts of section 4.3, plus `ring_parent_dir` and `valid_build` |
| `cell` | `{id, workload, mode, origin\|route, receivers, payload_bytes, framed_bytes, gap_us, window, n_leaves, offered_rate, planned, warmup, burst, handler_delay_us, drain_timeout_s, validation_only, calibration}`; every key is present, null where it does not apply (also in the runner's own record of a sample that `--aggregate` produced no record for); `burst` is null in W2 and W3, `handler_delay_us` in W3; `calibration` is the cell's `{host_label, commit, date}` or null (always null in W2 and W3) |
| `counts` | `{published, deliveries_expected, deliveries, lost, unresolved_tail, gap_intervals, order_errors, size_errors, call_errors, eviction_log_matches, sentinel_missed}`; `unresolved_tail` is nonzero only with `sentinel_missed` (section 6). In W2, `published` and `deliveries_expected` are the calls made, `deliveries` the calls that returned a value, `lost` the timed-out calls; counts that do not apply are null |
| `throughput` | `{interval_s, delivery_interval_s, fence_s, publication_msg_s, publication_app_mb_s, publication_framed_mb_s, delivery_msg_s, delivery_app_mb_s, delivery_framed_mb_s, calls_s}` |
| `latency.<name>` | `{n_obs, n_messages, p50_us, p99_us, p999_us, max_us}` for the names in section 6 |
| `cpu.<role>` | `{processes, cpu_s, max_process_cpu_s, us_per_message, us_per_delivery}` |
| `lifecycle` | `{time_to_ready_s, time_to_shutdown_s, shutdown_calls, first_shutdown_call_s, leaf_start_max_s, leaf_shutdown_max_s, formation_cpu, shutdown_cpu, oversubscription}`; the two CPU fields are `{coordinator, leaves, total}` in seconds; `oversubscription` is `(N+1)/C`, null when C = 0; `lifecycle` is null when a phase did not complete |
| `outcome` | `{exit_code, timed_out, missing_files, valid, reasons[], warnings[]}`; `missing_files` names the absent reports (`<role>-<branch index>.bin`). In the runner's own record it names the expected reports that are absent from the run directory |
| `runner` | `{started_unix, wall_s, timeout_s, ring_dir_sintra_entries}` |

Roles: `coordinator`, `coordinator_sender`, `coordinator_callee`, `sender`, `receivers`, `caller`,
`callee`, `leaves`, `total`.

## 8. Validity (adopted design)

**Invalid samples.** An invalid sample has `valid=false` and lists its reasons. It is written,
but excluded from comparisons. A sample is invalid when any of these holds:
- the build is not valid (section 4.3);
- the run timed out (`timeout`), the coordinator exited non-zero (`exit_code_nonzero`),
  `--aggregate` produced no record (`aggregation_failed`), or cleanup could not be completed
  (`cleanup_incomplete`: a process of the run is left, or the runner could not establish that
  none is; the session stops and names the processes). `aggregation_failed` applies also after
  a timeout: for a damaged report (bad length or an unterminated string), when no report was
  written, and when the aggregation did not finish within 60 s, which the runner then ends. The
  runner then writes its own record; its `missing_files` names the expected reports that are
  absent from the run directory, with the reason `missing_files` when there is one;
- the report set is wrong, as the shared harness checks it for every workload: a process report
  is missing (`missing_files`, also after a bypassed barrier, section 4.4) or belongs to another
  run (`foreign_run_files`), a report is a duplicate or surplus for a role or branch
  (`unexpected_reports`), or a report has other cell settings (`inconsistent_reports`);
- any order or size error, a wrong final byte, or a call that threw or returned another value
  than its argument (`call_errors`);
- windowed or closed-loop mode: any loss (`loss`, including a timed-out windowed call), or any
  eviction log match;
- open-loop mode: a receiver missed the sentinel within the drain allowance (`sentinel_missed`;
  its tail is reported as `unresolved_tail`). Loss itself is a result there, not an invalidity;
- lifecycle mode: `init` threw (`init_failed`), or a process's `shutdown()` threw
  (`shutdown_threw`); both leave `lifecycle: null`. A shutdown that never completes yields
  `timeout` and `missing_files` with `lifecycle: null`, because the report is written after the
  call that returns true. A false return is not an invalidity.

**Validation-only records.** `validation_only` is a flag that excludes a record from comparisons
and summaries. It does not by itself make the record invalid; validity follows the rules above.

**Warnings.** A warning leaves the sample valid but flagged:

| Warning | Condition |
|---|---|
| `cpu_low_confidence` | W1 and W2: the measured interval is under 2 s. Not emitted for W3 (section 5.3) |
| `cpu_not_advancing` | Some process's CPU delta over the measured interval (W3: over formation or shutdown) is exactly zero; the sample's CPU metrics are treated as unavailable in summaries |
| `shutdown_retried` | W3: some process needed more than one `shutdown()` call |
| `rate_shortfall` | The achieved publication rate is under 95% of the offered rate |
| `not_oversubscribed` | `N_over+1 <= C` |
| `ring_dir_not_empty` | `sintra-*` entries already existed in the ring parent directory |

**Comparable pairs.** Both samples are valid, neither is `validation_only`, and their effective
parameters are equal: the cell (ID, planned message or call counts and warm-up counts, never the
observed `counts` object; offered rate; calibration metadata; framed size and burst excluded, as
they follow from the library), the build configuration (compiler, configuration, `NDEBUG`,
optimization, test hooks, separate compilation), host label, operating system (`host.os`) and C.
The two arms of a session share their host, so the operating system matters for the stored A/A
pairs that the summary takes from earlier sessions (section 9). `host.os` is Python's
`platform.platform()`, which includes the kernel release or the Windows build, so an OS or kernel
update starts a new set of A/A pairs. Library constants are recorded and compared only for a
notice: an A/B of a library constant is a legitimate use.

## 9. Runner (adopted design)

```text
python benchmarks/run_benchmarks.py --a <build-dir> --a-commit <sha> [--b <build-dir> --b-commit <sha>]
    --output <results.jsonl> [--workloads <file>] [--pairs 5] [--select <cell-id glob>]
    [--note <text>] [--cpus N] [--aa <aa.jsonl>] [--calibrate <new-workloads.json>] [--keep]
    [--require-valid]
```

**Session**
- Without `--b`, the session is A/A: the A binaries run as both arms, with `aa=true`.
- Before any run, the runner compares both arms' build configuration (section 8); a difference
  aborts the session. A difference in library constants only prints a notice, at the start and
  in the summary.
- Lines of an earlier output or `--aa` file that do not parse, or carry another schema, are
  skipped and counted.
- Selected open-loop cells that have no stored rate are skipped. They are listed with the reason
  at the start and in the summary, and no record is written for them.
- In pair p, A runs first when p is even and B runs first when p is odd.
- With `--require-valid` (not with `--calibrate`), the runner exits non-zero after the summary
  when a record of the session is invalid; a warning does not count. The CI smoke pass uses it
  (section 1.3).

**Each sample.** The launch is inside one guarded scope, so an interrupt or an exception also
ends the swarm, and the runner never signals a process it does not hold by more than a PID.
1. Create a fresh run directory, set `SINTRA_BENCHMARK_DIR` to it, and start the coordinator.
   The timeout is 60 s plus 3 × the planned duration. An uncalibrated windowed broadcast cell
   gets 60 s + 30 ×; a swarm sample, which has no planned duration, 60 s; and under
   `--calibrate` every sample gets a fixed 900 s. A W2 cell plans calls × (`gap_us` + 5000) /
   10^6 s in closed-loop mode and calls × 250 µs in windowed mode.
2. **POSIX.** The runner signals only the process group of the coordinator it launched, and only
   while that child is still unreaped (`poll() is None`). Leaves start their own sessions and
   leave through Sintra's lifeline: once the coordinator is gone, each hard-exits after about
   100 ms (section 2.2). The one exception (owner decision, section 14): a leaf that cannot run,
   such as one stopped by `SIGSTOP` or a debugger, cannot leave that way. The runner reports it
   by PID with `cleanup_incomplete`, the session stops, and the operator handles the process.
3. **Windows.** The runner keeps `tests/runner/platform/windows_process_tree.py`, which retains
   handles of the coordinator and each descendant, and refuses to start without it.
4. **Leftovers.** On POSIX, a process-table scan for the adjacent arguments `--run-id <id>`
   (procfs, else `ps`) only detects leftovers; on Windows, the retained process tree lists the
   live descendants by PID. With procfs the scan reads only the processes of the runner's user
   (the owner of `/proc/<pid>`), as every participant runs as that user. A process that has
   disappeared is gone. One of the user's processes whose command line cannot be read does not
   end the scan, but leaves the look inconclusive; a failed `ps`, or one still running at the
   deadline (its call is bounded by the time left), fails the look. Cleanup is complete only
   after two consecutive empty looks that both end before its 10 s deadline; the deadline is
   for those looks, not a bound on the cleanup's duration. A leftover, an unreadable process,
   failing looks, or a deadline reached before two such looks give `cleanup_incomplete`; the
   session stops and names the processes (on POSIX by PID and command line, an unreadable one as
   "could not be inspected"; on Windows by descendant PID). The runner never signals a PID taken
   from a file or from the scan; PID files only count the expected reports. The runner waits
   for the swarm before removing evidence or starting the next sample.
5. **Interrupts.** SIGINT, SIGTERM, SIGHUP, SIGQUIT (where they exist) and Ctrl+Break only set a
   flag; a signal the runner was started to ignore stays ignored. Cleanup cannot be aborted, and
   an interrupted sample writes no record. The runner checks the flag immediately before it
   appends a record: that append is the sample's completion boundary. Cleanup measures nothing
   (open decision 6).
6. When the coordinator exits by itself (any exit code) before a timeout or an interrupt, wait
   up to 10 s for the expected reports; then clean up (step 4). Run `--aggregate`, for at most
   60 s; if it produces no record, the runner writes its own (section 8). Add the runner fields
   and the cell's calibration metadata; append and flush the line; only then delete the run
   directory, unless `--keep`. If the record cannot be written, the runner reports the error and
   keeps the run directory; after an incomplete cleanup it still names what is left.

**Summary**
- Per cell: comparable pairs used, per-arm invalid-sample counts, and latency sample support
  (`n_obs`, `n_messages`).
- Per windowed W1 cell: the median fence time per arm and its median share of the interval.
- Per headline metric: the median within-pair comparison with its minimum and maximum, and beside
  it the A/A spread over comparable A/A pairs with equal effective parameters (section 8: the
  same host label, `host.os` and C among them), from the same output file or `--aa`, or
  "unavailable" when there are none. The spread is folded, because an A/A pair's arm
  labels are arbitrary: max(r, 1/r) for ratios, absolute values for differences (for example
  "A/A spread up to x1.061 over 2 pairs").
- Ratio or difference: CPU, the lost fraction and the shutdown calls (W3) are always shown as
  per-arm medians with B-A (`cpu_not_advancing` makes CPU unavailable). Any other metric,
  slowest-receiver latency included, is a B/A ratio unless some observation, in the pairs or in
  the matching A/A pairs, is zero or unavailable; then it is a difference for all of them.
  Unavailable observations are counted, not dropped.
- Nothing is flagged automatically, and no other statistics are computed.

| Mode | Headline metrics |
|---|---|
| Windowed | W1: publication and delivery rates, `send_call` p50. W2: calls/s. Both: CPU µs per message or call, total and per role; `slowest_from_send` (W1) or `issue_to_observation` (W2) p50 and p99 |
| Open-loop (W1) | `slowest_from_intended` p50 and p99; lost fraction; lateness p99; `cpu_s` of the non-sender roles |
| Closed-loop | `rtt` p50 and p99, plus calls/s for the gap-0 cells; CPU µs per call per role |
| Lifecycle | `time_to_ready_s`, `time_to_shutdown_s`, shutdown calls (shown only when one observation is not 1); formation and shutdown CPU per role |

**Calibration (`--calibrate`)** runs each selected windowed broadcast cell three times with `--a`,
rescales its count to about 3 s, derives its three open-loop rates from the median publication
rate, and writes a new workloads file with per-cell calibration metadata. W2 and W3 are not
calibrated. Level 2's first version does not calibrate (section 15.11), so no run produces a
committed file: `--calibrate` writes only to a temporary file, to check function, and validation
runs use that file through `--workloads`.

## 10. Reading hotspots (adopted design)

The runner prints axis tables for one arm. Each reading is a hypothesis about role costs and
end-to-end differences, not an isolated cost.

- **W1: CPU µs per message by role against R**, for each payload and origin, with a fence-share
  column (`fence_s` / `interval_s`). Windowed CPU includes the CPU of the fences, so these
  readings describe the fence-paced cycle (section 12). Within that: sender growth along R
  suggests reader-dependent publication work, such as waking parked readers; growth per delivery
  suggests receiver-side cost; in leaf origin the coordinator role also includes reading its own
  ring and control work, not just relaying.
- **W1: CPU per MB against payload**, with the fence share. It describes the fence-paced cycle,
  not frame cost; `send_call` is the closer reading of publish cost.
- **W1: open-loop latency and loss against offered rate.** At the stored rates it shows the
  readers' wake-up regime (section 12).
- **W2: actual gap p50, rtt p50 and p99, and CPU per call, against gap**, for each route; read
  rtt together with the actual gap (section 12). The difference between `leaf_to_leaf` and
  `leaf_to_coordinator` is end-to-end: the routes also differ in placement, readers and
  scheduling. W2 has no CPU share table.
- **W3: time to ready, time to shutdown and CPU per leaf against N** (shutdown calls when a sample
  retried), and each role's share of a phase's CPU. Comparing N_over with 40 changes swarm size
  as well as oversubscription.
- **Windowed W1 cells: each role's share of total CPU.** Open-loop cells are left out, because
  their sender CPU is mostly the generator polling the clock (section 12).

A cost that grows faster than the work it serves is a hotspot candidate, not a diagnosis.

## 11. Phases, acceptance and cost (adopted design)

Each phase lands as one change, after independent review.

**Phase 1** covers the harness, schema and aggregation; the runner with pairs, A/A and
calibration; the broadcast port for both origins, windowed and open-loop; and deleting
`run_broadcast_benchmark.py` and rewriting the README. It is done when:
1. **Development builds.** It builds and runs on WSL (GCC and Clang, Release) and on Windows (MSVC,
   Release), using a temporary calibrated file and keeping no numbers.
2. **Invalid builds.** A Debug build, and a build whose compiler definitions include
   `SINTRA_ENABLE_TEST_HOOKS=1` (for example through `CMAKE_CXX_FLAGS`), produce records marked
   invalid for those reasons.
3. **Forced-loss checks.** All three cases are `validation_only` records:
   1. *Windowed pacing with a bounded handler delay*, chosen so the run completes within the
      runner timeout: it completes with `lost = 0` and a valid record. The delay only lengthens
      fences, because each burst fits in a quarter ring (`benchmarks/README.md:22-25` at `3666206f`).
   2. *Open-loop overload fixture*: a handler delay and an offered rate that overrun the receiver,
      with the sentinel/rendezvous drain. Expected: `lost > 0`, `gap_intervals > 0`,
      `eviction_log_matches > 0` and a valid record. A missed sentinel or a timeout is recorded
      as such. A loss-free completion means the fixture did not induce overload, and the check
      does not count as exercised.
   3. *Windowed-strictness overload fixture*: unpaced `--burst 0`, a message volume well beyond
      the ring, and a delay long enough to obstruct the writer. A completed sample with observed
      loss must be invalid (`loss`); a timeout is recorded as `timeout`. A loss-free completion
      means the fixture did not induce overload, and the check does not count as exercised.
4. **Comparison on hosted runners (level 2, section 15, proposed).** Level 2 compares two builds
   within one CI job and keeps no absolute reference. It checks the cross-process `steady_clock`
   assumption (section 12) in each job and gives one-way latency no verdict until that check
   passes (section 15.6). Calibration and a stored A/A reference over the default broadcast cells
   are not part of level 2's first version (section 15.11). Until then, results are development
   and CI evidence, not a reference.

**Phase 2** (`rpc_benchmark`): W2 with fixed counts; level 2 would measure its A/A spread inside
each CI job (section 15.5).

**Phase 3** (`swarm_benchmark`): W3; level 2 would measure its A/A spread inside each CI job
(section 15.5). The N_over swarm formed on the development machine at 80 leaves, and at 125 with
`--cpus 126` (WSL).

**CI smoke pass (level 1).** On every CI platform, the workflow builds the three targets and runs
one A/A pair of each cell of `benchmarks/workloads_smoke.json` except `w3.lifecycle.n_over`, with
`--require-valid`. Its cells and counts are chosen so that they are expected to stay well clear
of loss and timeouts on a loaded 3 to 4 core runner; that is an expectation until CI runs confirm
it. The pass aims at about 3 min of benchmark time per job; on the development machine one pass
took 56 to 69 s.

**Estimated cost** (estimates, not measurements; about 2 s of fixed overhead per sample). One pass
over the default cells takes about 6 min for W1 (72 cells), about 6 min for W2 (10 cells; the 2 s
gap alone takes 2 min per route) and probably under 1 min for W3. A full A/B run with 5 pairs is
10 passes, about 2 h; A/A doubles that. `--select` limits a session to affected cells.

## 12. What the first iteration does not claim

- **No capacity claim:** no maximum sustainable rate or saturation point; open-loop rates are
  fixed probes.
- **No universal maximum or cross-host comparison:** numbers describe the host that produced
  them, its toolchain and each run's process placement.
- **No host tuning:** no pinning, affinity, frequency control or isolation; the OS places processes.
- **No thresholds, performance gate or significance testing** beyond the A/A spread;
  five pairs show direction and size, not certainty. The CI smoke pass fails only on breakage or
  an invalid record, never on a number. Level 2's verdicts (section 15.6) use only the pairs'
  agreement and the job's own A/A spread, and never fail CI. Results are not a latency or loss
  contract,
  and open-loop loss under overload is documented behaviour (`docs/architecture.md:89-99`).
- **No per-thread, futex, syscall, hardware-counter or memory metrics.** On Windows process CPU
  often does not advance over a whole sample (zero for whole 3 s samples; every W3 sample has
  `cpu_not_advancing`), so Windows CPU values are not a reference. Linux `getrusage` is.
- **Open-loop sender CPU is not a publishing cost:** it is mostly the generator polling the clock
  for its schedule.
- **W3 has harness work inside the measured phases:** each leaf builds the swarm's launch
  description (N process descriptors, each with a copy of its options), creates its PID file
  during start-up and writes its report before it exits, and the coordinator's custody join waits
  for that exit. These costs were not quantified. The run directory must be on a local
  filesystem.
- **Windows paths outside the active code page are not supported:** the harness handles the
  temporary and run directories as narrow strings, and the runner decodes the executables'
  output with Python's default encoding, so such a path can make `--build-facts` or a report
  fail (not reproduced).
- **Windows process-table errors can look like its end:** the shared helper
  `tests/runner/platform/windows_process_tree.py` treats any failure of `Process32FirstW` or
  `Process32NextW` as the end of the table, so that look misses a descendant after that point.
- **Slowest-receiver latency conditions on delivery to every receiver**, so loss can remove the
  slowest observations; read it together with loss.
- **A clock assumption.** Cross-process latency assumes that `std::chrono::steady_clock` on each
  toolchain reads one system-wide monotonic clock; the library's `CLOCK_MONOTONIC`
  helper does not prove that mapping. Establishing it is a prerequisite of phase 1 acceptance
  item 4. Nanosecond values do not mean nanosecond accuracy, and broadcast latency is sampled
  within a budget of about 100k messages.

**What the first runs showed.** These are findings on the development machine (WSL2 on 16 cores,
and Windows): functional evidence, not reference values. They decide how the results read.

1. **The reader wait policy dominates whatever is idle between messages.** A reader spins for
   50 µs, then polls in 1 ms sleeps for up to 1 s, then parks (`config.h:45`, `:49`, `:53`;
   `ipc/rings.h:3373-3402`, `:3405-3533`). Once a reader is past the spin, a wake-up costs up to
   one sleep cycle per hop.
2. **W2 mostly reports that policy.** The regularities below were seen for the gaps and runs
   named; they are consistent with polling-phase effects, not proof of them.
   - Paced calls looked locked to the poll. In the 200 µs cells on WSL, rtt p50 + `actual_gap`
     p50 was close to a whole number k of poll periods (about 1.07 ms): k = 1 on
     `leaf_to_coordinator` and usually 2 on `leaf_to_leaf`. A 20 ms gap spans many periods; there
     rtt p50 varied between runs (0.5 to 3.0 ms on WSL).
   - Windowed `leaf_to_leaf` calls/s was consistent with window / (k × period): 13,500 to 15,600
     calls/s on WSL, and a Debug build gave the same value. Windowed `leaf_to_coordinator` did
     not look locked to the poll on WSL (`issue_to_observation` p50 110 to 240 µs), and its rate
     varied about 4× between sessions (46,000 to 182,000 calls/s).
   - Closed-loop CPU per call follows the cycle time. That most of it is the readers' spinning and
     polling is a hypothesis consistent with these observations: the measurement is whole-process
     CPU (`benchmarks/harness.h:62`), which does not separate those costs. Windowed CPU per call is
     closer to the call path, but it is whole-process CPU too: it includes waiting and harness work
     and does not isolate the call path.
   - Regimes reached: gap 0 reaches the spin regime only part of the time on
     `leaf_to_coordinator` (in one A/A pair, rtt p50 was 62 µs in one arm and 1121 µs in the
     other) and in no `leaf_to_leaf` sample; 200 µs and 20 ms reach the sleep phase; 2 s reaches
     the parked regime.
   - Reading rule: read rtt together with `actual_gap`. A difference below one poll period is
     consistent with a phase effect, but phase locking can hide a real regression smaller than one
     poll period, so such a difference neither shows nor excludes a change of the call path. A
     rate measured in one regime does not predict the next sample, which is why the W2 counts are
     fixed.
3. **Windowed W1 is fence-bound.**
   - The measured interval is mostly processing-fence time, about 50 to 99 % across cells (51 to
     99 % on WSL, 85 to 98 % on Windows).
   - The 64-byte cells are the least fence-bound (51 to 83 % on WSL): up to about half of their
     interval is publishing and delivery, so their windowed rate does follow the publish path.
   - Fence p50 in one WSL run of the default grid was 3.9 to 5.9 ms with one receiver and 10.8 to
     13.9 ms with sixteen; over all WSL runs, 2.6 to 7.0 ms and 9.9 to 19.5 ms.
   - So the windowed rate, the payload axis and CPU per MB describe the fence-paced cycle, not
     frame cost: at a 90 to 99 % fence share, a 2× change in publish cost moves the headline rate
     by 1 to 10 %. Windowed CPU includes the fences' CPU.
   - The windowed latency headline, `slowest_from_send`, also describes the burst/fence cycle and
     the reader wait policy: a message's latency can include the traffic of its burst published
     before it, and the readers' scheduling and wait effects. With sixteen receivers at 64 B its
     p50 was 2.1 to 2.5 ms in that WSL run; with one coordinator-origin receiver at 64 B its p99
     was about 1 ms, one poll period, in most runs (0.55 to 2.3 ms over all WSL runs); and in one
     A/A pair of the cell with four coordinator-origin receivers at 64 B, p50 was 20 µs in one arm
     and 177 µs in the other.
   - `send_call` and the fence-free rate (section 6) are the closer readings of publish cost.
4. **The open-loop rates are light loads.**
   - They are fractions of the fence-bound rate: about a quarter of the fence-free publish rate
     for the smallest cell and about 1 % for the largest.
   - The open-loop latency table therefore shows the readers' wake-up regime (about 1 ms per hop
     at p99), and p99 at the large payloads rests on tens to hundreds of messages.
   - On Windows, below 10k msg/s, arrivals are about 0.5 to 0.75 ms late at p50 and 0.9 to 2.3 ms
     at p99 (once 5.7 ms), because the generator sleeps until 100 µs before each arrival.
5. **W3 shutdown on Linux needs retries at large N** (section 5.3): 2 calls at 80 leaves, 4 calls
   and 1.38 to 1.53 s at 125, and a second call in one of four samples at 70.

## 13. Candidates for a later iteration (listed, not designed)

Capacity search; fan-in and bystander axes; mesh; barriers; cross-process raw ring; registry
scale; slow-reader isolation; handler-count axis; request/response open-loop.

## 14. Open decisions for the owner

Each entry gives the default that is implemented and the alternative.

| # | Question | Implemented default | Alternative |
|---|---|---|---|
| 1 | Windowed broadcast is fence-bound (section 12, finding 3); the largest payload (261632 B) has a two-message burst. Keep the windowed mode as it is? | Keep: a lossless paced rate, with the fence share shown | Larger bursts where the ring allows; or treat the fence as its own measured item |
| 2 | The open-loop rates are light loads (section 12, finding 4). Keep 0.1, 0.5 and 0.8 of the calibrated windowed publication rate? | Keep, and read them as light loads | Derive them from the fence-free rate; the top probe may then exceed receiver capacity, and loss becomes the result |
| 3 | The W2 cells mostly report the wait policy (section 12, finding 2). Keep them? | Keep the cells as they are and read them by the reading rule | Jitter the gap by up to one poll period; a larger window for a capacity-bound `leaf_to_leaf` cell |
| 4 | Size of the default sweep, and whether open-loop waits for calibration | Keep the open-loop cells defined: they run only once calibration has stored their rates, which level 2's first version does not do (section 15.11); every cell is selectable | A smaller default grid |
| 5 | Fixed count for the W2 gap-0 cells | 10000 | Another fixed count |
| 6 | Is the runner's operational cleanup acceptable under "only system independent stuff"? On POSIX it signals only the launched coordinator's process group, leaves exit through Sintra's lifeline (a leaf that cannot run is reported; see the decision below), and a `--run-id` scan only detects leftovers; on Windows it terminates a retained-handle process tree. It measures nothing | Keep | Drop the per-OS parts and rely on the lifeline alone |
| 7 | Oversubscribed point `N_over = min(125, max(80, 2C))` | Keep. Hosted runners have C = 4 (Linux, Windows, the FreeBSD VM) or 3 (macOS) (section 15.2); level 2 does not run this point (section 15.5) | Another formula |
| 8 | Maximum swarm size: 125, the automatic index budget, reached by N_over from C = 63 | 125 | A lower cap |
| 9 | A/A cadence | An operator procedure: the runner runs no A/A pass by itself and reads A/A records from the output file or `--aa`. The operator runs an A/A session for the selected cells with every comparison session, until the stored A/A spreads prove stable. Level 2 would run A/A pairs next to each cell's comparison in every CI job (section 15.5) | Once per host or toolchain change |
| 10 | Where calibrated counts and rates live | In git (`benchmarks/workloads.json`), so both arms and later sessions use identical absolute rates | Beside the results |
| 11 | The 2 s gap cells of W2 reach the parked regime, whose wake path on Linux is the library's futex (`ipc/semaphore.h:693-700`). The benchmark reaches it only through the public RPC API and contains no futex code. Keep these cells? | Keep the cells at 2 s | A gap of about 1.2 s also passes the park threshold (the 1 s sleep phase, `config.h:53`) and would shorten a W2 pass by about 30 % (development machine); or drop the parked regime, if "leave futex tests" excludes it |

**Decided by the owner**

- **Cleanup of a leaf that cannot run (POSIX); owner decision, 2026-11-16: accept and
  document.** A leaf that is stopped, by `SIGSTOP` or a debugger, cannot run its lifeline
  watcher, so ending the coordinator does not end it. The runner keeps signalling only its own
  coordinator's process group. It reports such a leaf by PID with `cleanup_incomplete`, the
  session stops, and the operator handles the process. The runner does not become a subreaper
  and takes no other ownership of the leaves.

## 15. CI level 2: comparison with the previous master (proposed design)

**Status:** proposed and revised after two independent reviews; not implemented. Sections 15.1
and 15.2 hold the owner's requirements and verified facts; sections 15.3 to 15.13 are this
design's own choices.

### 15.1 Owner requirements and decisions
- "is there a way to make it part of the CI run, so that it can also show if there are performance
  regressions or improvements later on?"
- Hosted runners only: "my reference host is not relevant. i want automation and things happening
  elsewhere, not on my hosts." (section 1.1).
- Decisions of 2026-11-16/17: the history is kept in a data branch inside the repository (for
  example `benchmark-history`); the comparison runs on all platforms (Linux, Windows, macOS,
  FreeBSD); level 2 reports and never gates.
- From section 1.1: "please don't overcomplicate it", "first attempt", "starting point"; "only
  system independent stuff"; "both cpu seconds and latencies are important".

### 15.2 Verified facts

At commit `2fdc3b02`. The hosted-runner facts come from the records and job timings of level 1's
run 43888169852 (the push of `2fdc3b02` on 2026-11-16), downloaded from its artifacts.

**Workflows**
- Level 1 runs on pushes and pull requests that change `src/`, `include/`, `cmake/`, the
  top-level CMake files, `third_party/`, `benchmarks/`, `tests/runner/platform/` or its own file
  (`.github/workflows/benchmarks.yml:7-32`), with a read-only token (`:34-35`).
- Its labels are `ubuntu-latest` (`:40`, and the FreeBSD VM's host, `:344`), `macos-15` (`:136`)
  and `windows-latest` (`:211`, `:280`). FreeBSD 15.0 runs in `vmactions/freebsd-vm`, pinned by
  commit, without a compiler cache (`:342-377`); after a failure the VM skips copyback, and the
  job exports the output with rsync (`:379-390`). `build-freebsd.yml` restores, exports and saves
  a ccache around the VM (`:50-57`, `:118-133`) and budgets its time from a recorded start
  (`:72`, `:112`).
- `ubuntu-latest` is Ubuntu 26.04 with GCC 15 (`.github/workflows/coverage.yml:14-16`, which pins
  `ubuntu-24.04` for an lcov problem).
- Conventions: actions are pinned by commit (`benchmarks.yml:55`); event context reaches scripts
  through the environment (`stress-test-linux.yml:59-65`); a push's diff base is
  `github.event.before` (`build-linux.yml:202`), replaced by the head's parent when it is all
  zeros or missing from the clone (`stress-test-linux.yml:98-123`); a last step carries a
  recorded result into the job's conclusion (`coverage.yml:179-182`).
- Seven checkouts run with `fetch-depth: 0` on a library push (`build-linux.yml:41`,
  `build-macos.yml:41`, `build-windows.yml:41`, `build-windows-mingw.yml:41`,
  `stress-test-linux.yml:91`, `stress-test-macos.yml:91`, `stress-test-windows.yml:101`).

**Runner**
- `run_pairs` runs a session's pairs of one cell back to back and puts A first in even pairs
  (`benchmarks/run_benchmarks.py:1020-1028`), so a one-pair session always runs A first. Without
  `--b` the session is A/A (`:820`). `--build-facts` runs without a timeout (`:678-681`).
- A/A records come from the output file and `--aa` (`:1649-1650`) and serve a comparison only with
  an equal comparison key: cell, configuration, host label (`platform.node()`), `host.os` and C
  (`:840-845`, `:1107-1122`). The configuration is the compiler, configuration name and Boolean
  facts, not the effective flags (`:45-46`, `:684-687`).
- Per headline metric, the summary prints the median, minimum and maximum of the within-pair
  comparisons and the folded A/A spread (`:1288-1367`). It takes the difference B-A instead of
  the ratio for CPU, the lost fraction and the shutdown calls, and for any metric with a zero or
  missing observation (`:1342-1343`); `cpu_not_advancing` makes CPU values unavailable
  (`:1099-1104`). Closed-loop rows show CPU per role without the total (`:1229-1232`).
- Arms with different configurations abort the session; different library constants only give a
  notice (`:833-838`). An incomplete cleanup ends the session (`:1015-1016`). Every W2 sample
  first makes 1000 synchronous warm-up calls (`:186`).

**Benchmark build and latency**
- The targets link `sintra` and `sintra_dev_flags` (`benchmarks/CMakeLists.txt:1-14`), and
  `SINTRA_BUILD_BENCHMARKS` adds them (`CMakeLists.txt:152-156`). `sintra_dev_flags` holds
  options of the repository's own targets, kept out of the public target (`CMakeLists.txt:118-140`).
- The harness uses library internals (`benchmarks/harness.h:186-196`, `:497`, `:594`), so head's
  benchmarks need not compile against an older library.
- One-way latency subtracts a send stamp of one process from a receive stamp of another
  (`benchmarks/broadcast_benchmark.cpp:718-730`).

**Hosted runners (run 43888169852)**
- Jobs run on different machines: Linux GCC on `ip-10-0-134-12` with `Linux-7.0.0-1016-aws`,
  Linux Clang on `runnervmccnms` with `Linux-7.0.0-1019-azure`. Host labels are not machine
  identities: both Windows jobs reported `runnervmm7ud1`, and the FreeBSD VM reported `freebsd`.
- C was 4 on Linux, Windows and the FreeBSD VM, and 3 on macOS (arm64, macOS 15.8.2). Compilers:
  GCC 15.2.0, Ubuntu Clang 21.1.8, Apple LLVM 17.0.0, MSVC (`_MSC_FULL_VER` 195136260), FreeBSD
  Clang 19.1.7. Windows was Server 2025, build 26100.
- Building the three targets took 65 to 100 s. The smoke pass took 46 to 47 s on Linux, 82 to
  87 s on Windows and 224 s on macOS; the FreeBSD VM step, with boot and build, 190 s. The macOS
  job started 21 min after the others.
- W2 at gap 0, 1000 calls, over both arms: `leaf_to_leaf` took 1.475 to 1.917 s on Linux and 12.58
  to 12.61 s on macOS; `leaf_to_coordinator` took 0.023 to 0.115 s on Linux, and 6.08 s and 3.39 s
  on macOS.
- `cpu_not_advancing` appeared in 21 (MSVC) and 29 (MinGW) of the 48 records of each Windows job,
  and in none elsewhere.
- A record is about 3.1 KB; gzip compresses the JSONL about 13-fold. The smoke cells have R = 1
  and 2 and N = 1 and 8, so timings below for R = 4 and N = 40 are extrapolated.

**Repository**
- Public (GitHub API). The local object pack is 9.7 MiB. In the 30 days before 2026-11-17, 26
  first-parent commits on `master` touched level 1's paths other than its workflow file
  (`git log`); one push can carry several.

**GitHub behaviour relied on** (GitHub's documentation, not verified here): a push's path filter
uses the diff from `before`; events caused by `GITHUB_TOKEN` start no workflow run; a concurrency
group keeps one pending run and cancels the earlier pending one; the image behind a versioned
label is updated about weekly and names itself in `ImageOS` and `ImageVersion`; `fetch-depth: 0`
fetches every branch; a run's `created_at` (REST API) stays the same on re-runs; artifacts are
kept at most 90 days; standard hosted runners are free for public repositories.

### 15.3 Trigger, base and concurrency
- **Events.** Pushes to `master` whose diff matches level 1's paths, with this workflow's file in
  place of level 1's; and `workflow_dispatch` without inputs. No pull requests; in a fork every
  job is skipped (`github.event.repository.fork`).
- **Push.** The base is `github.event.before` and the head the pushed commit: a push-range
  comparison that covers every commit of the push. When `before` is all zeros or not a commit of
  the clone, the base is the head's first parent, labelled `fallback base` in the report header
  and the index. A re-run of a push run repeats its comparison.
- **Dispatch.** It compares the dispatched commit (from `master`, the current master) with its
  first parent and writes no history: a test of the workflow.
- **Setup job** (full clone): resolves base and head, and records `library_changed` (a diff over
  `src include cmake CMakeLists.txt CMakePresets.json third_party`), `build_definition_changed`
  (over `CMakeLists.txt cmake CMakePresets.json`) and whether `benchmarks/` differs. A head
  without a parent, or without `benchmarks/workloads_ci.json` (older than level 2), is
  `not_compared`.
- **Unchanged library.** A push that changes only `benchmarks/` or the workflow compares two
  identical libraries. It runs as usual as a null test: any verdict in it is a false call, and
  the index counts them (15.8).
- **Concurrency.** No concurrency group and no cancellation. Each run measures its own base and
  head inside its own jobs, so overlapping runs share no machine and no result. A group would
  cancel the pending run of the middle one of three close pushes and leave that push unmeasured.
  The only shared state is the data branch, which the collector updates with retries (15.8).
  Overlapping runs compete for runners; macOS queues longest.

### 15.4 What is compared
- **Arms.** A is the base library with head's `benchmarks/`; B is the head library with head's
  `benchmarks/`. The same job builds both with one toolchain and level 1's options
  (`benchmarks.yml:104-108`): Release, benchmarks only.
- **Overlay.** The job checks out base and head side by side, replaces the base tree's
  `benchmarks/` with head's and configures arm A from the base tree. The runner, the CI cells and
  `ci_compare.py` come from head, so both arms run under one runner with the same cells, and a
  change in `benchmarks/` cannot appear as a library change.
- **Build definition.** The overlay keeps each tree's top-level build definition, which also sets
  the benchmark targets' private options (`sintra_dev_flags`). Besides `build_definition_changed`,
  each arm's configure answers a CMake File API query, and the job records the benchmark
  targets' effective compile and link flags from it (fragments, definitions, include directories,
  language standard; paths relative to the source and build trees). When these differ between
  the arms, `job.json` has `benchmark_flags_differ`, and the job's verdicts are qualified as
  "library or build flags", not reported as a library change.
- **Fallback.** When head's benchmarks do not build against the base library, the platform
  reports `not_compared: base_build_failed` with the build log, and the next push compares from
  this head. Building the base with its own `benchmarks/` was rejected: that result cannot
  separate a benchmark change from a library change.
- **Labels.** Each record's `commit` is its arm's library commit (`--a-commit <base>`,
  `--b-commit <head>`); its `note` is `ci-compare <platform>, benchmarks <head>`. The job record
  `job.json`, with its own schema `sintra-bench-ci/1`, carries base, head, `fallback_base`,
  `benchmark_sources` (head), `library_changed`, `build_definition_changed`, both arms' flags and
  `benchmark_flags_differ`, the clock check, the event, run ID and attempt, runner label,
  `ImageOS`, `ImageVersion`, CPU model, and the platform's status with its reason. The record
  schema `sintra-bench/1` does not change.

### 15.5 Platforms, cells, schedule and budget

| Job | Runner | Compiler (recorded) |
|---|---|---|
| Linux | `ubuntu-26.04` | GCC, the image's default |
| Windows | `windows-2025` | MSVC x64, set up as in level 1 |
| macOS | `macos-15` (arm64) | Apple Clang |
| FreeBSD | 15.0 VM in `vmactions/freebsd-vm`, pinned as in level 1, on `ubuntu-26.04` | The base system's Clang |

- One compiler per system, its default. Linux Clang and MinGW keep level 1's build and smoke check.
- **Versioned OS labels; recorded toolchains.** Versioned labels replace `-latest`
  (`ubuntu-26.04` is what `ubuntu-latest` is now; level 1's Windows records show Server 2025). A
  label fixes the OS release, not the toolchain: an image update can change the compiler under
  it. Both arms are built in one job with one toolchain, so an update cannot by itself produce a
  verdict, but it changes conditions between runs. Each job records `ImageOS`, `ImageVersion`,
  the compiler (build facts), `host.os` and the CPU model (Linux `/proc/cpuinfo`, macOS `sysctl
  machdep.cpu.brand_string`, Windows `Win32_Processor`, FreeBSD `sysctl hw.model`: operational
  facts that the workflow reads, not measurements); the report header and the index show them.
- **Cells:** a new file `benchmarks/workloads_ci.json` with fixed counts, no calibration and no
  open-loop rates. The driver selects each cell by its exact ID.

| Cell | Count | Covers (section 3) |
|---|---|---|
| `w1.windowed.coordinator.r1.b64` | 300000 messages | Publication and delivery; the least fence-bound payload (section 12, finding 3) |
| `w1.windowed.coordinator.r4.b4096` | 20000 messages | Fan-out and frame size |
| `w1.windowed.leaf.r4.b64` | 200000 messages | Coordinator relay |
| `w2.closed_loop.leaf_to_coordinator.g0` | 2000 calls | Call path, partly in the spin regime |
| `w2.closed_loop.leaf_to_leaf.g0` | 1000 calls | Call path through the relay |
| `w2.windowed.leaf_to_coordinator.w32` | 50000 calls | Request path; not locked to the poll on WSL (section 12, finding 2) |
| `w3.lifecycle.n8`, `w3.lifecycle.n40` | | Formation and shutdown |

  Scaled from the smoke records, the measured interval is mostly 1.5 to 2.5 s on Linux and
  FreeBSD, 2 to 4 s on Windows and 8 to 13 s on macOS; the R = 4 and N = 40 cells are
  extrapolated. On Linux the two `leaf_to_coordinator` cells are shorter and keep
  `cpu_low_confidence`. The counts are owner-tunable.
- **Schedule.** Per cell, six runner calls into one output file, with these numbers of pairs:
  A/A 2, A/B 2, A/A 2, A/B 2, A/A 1, A/B 1. That is 5 A/A and 5 A/B pairs, 20 samples, and the
  A/B pairs run A first three times and B first twice. Interleaving spreads slowly changing noise
  over both sets.
- **Stopping.** The driver ends a cell after the call in which one of its samples was invalid,
  since a verdict needs every pair; that call can still run up to three samples. It ends the
  platform after `cleanup_incomplete`, which also ends the runner's session (`:1015-1016`).
- **Budget.** The workflow records the job's start; the driver's deadline is the start plus the
  job timeout minus 8 min for report and upload. Before each runner call, the driver adds up the
  call's worst case (each sample's runner timeout plus 20 s of cleanup, and its `--build-facts`
  probes at 60 s each) and starts the call only if that ends before the deadline; the calls left
  are `not_run: budget`. Estimated benchmark time, provisional until hosted timings of these
  cells exist: Linux 4.5 min, FreeBSD 6, Windows 9, macOS 31. `timeout-minutes` is 45 (macOS 75).

### 15.6 Clock check and verdict
- **Clock check.** Before the cells, the driver starts 20 short processes one after the other,
  alternating arm A's and arm B's `sintra_broadcast_benchmark --clock-stamp` (a new harness mode
  that prints `steady_clock::now()` in ns; 30 s limit each). It checks that each stamp is greater
  than the previous one and that each step is at most the driver's own monotonic time from the
  earlier process's launch to the later one's exit. Passing means that in this job the processes
  of both arms read one clock that advances with the driver's: a per-process epoch or a per-arm
  clock source fails it. It cannot exclude a constant offset smaller than one step (a few ms);
  a system-wide clock has none. This is the clock assumption of section 12, checked per job.
  Until it passes in the job, one-way latency gets no verdict; same-process measurements
  (`send_call`, `rtt`, `issue_to_observation`, lifecycle times, CPU, rates) are unaffected.
- **Primary metrics** get verdicts; every other headline metric is shown without one.

| Cell | Primary metrics |
|---|---|
| W1 windowed | `send_call` p50; `slowest_from_send` p50 (one-way); CPU µs per message, total |
| W2 closed-loop | `rtt` p50; CPU µs per call, total (from the record, `cpu.total`) |
| W2 windowed | `issue_to_observation` p50; CPU µs per call, total |
| W3 | Time to ready; time to shutdown; formation CPU, total |

- **Deviation** of a pair: |ln(B/A)| for a ratio, |B-A| for a difference, in the representation
  the runner chooses; its direction is the sign of ln(B/A) or B-A. The **spread** s is the
  largest deviation among the cell's 5 A/A pairs.
- **Verdict**, per platform, cell and primary metric (all are better when lower):
  - `regressed` or `improved`: all 10 pairs have a value, the 5 A/B pairs have the same
    direction, and each A/B deviation exceeds s.
  - `inconclusive`: all 10 pairs have a value, and the condition does not hold.
  - `no verdict`: a pair lacks a value (an invalid sample, `cpu_not_advancing`, a stopped cell);
    CPU on Windows, where process CPU often does not advance (section 12); one-way latency
    without a passed clock check. The values are shown all the same.
- **False calls: an estimate under assumptions, not a guarantee.** Assume no library change, 10
  pair deviations that are independent and identically distributed (interleaving aims at that),
  and each sign a fair coin independent of its size. A false call then needs the 5 A/A
  deviations to be the 5 smallest (1 in 252) and the 5 A/B signs to agree (1 in 16): 1 in 4032
  per primary metric. A push has at most 76 primary metrics (21 per platform; 13 on Windows,
  without CPU), so about 0.02 expected false calls per push. Noise correlated over several
  pairs, or different between the arms, breaks the assumptions; the index therefore reports the
  verdicts observed in library-unchanged runs as the running empirical false-call rate (15.8).
- **Code layout.** Sintra is header-only: any library change recompiles the benchmark
  executables, and inlining and code placement can move timings without a change of the measured
  path. A verdict says that head's binaries were faster or slower on that platform, not why.
  Layout effects differ between toolchains, so the combined summary marks a primary metric
  called in the same direction on two or more platforms as a stronger lead.
- **Rejected simpler rules.** A fixed threshold: the noise differs by orders of magnitude between
  cells (in one W2 A/A pair, `rtt` p50 was 62 µs in one arm and 1121 µs in the other, section
  12). Earlier runs as the noise estimate: each job gets another machine (15.2).
- **Wording.** Every summary starts with: "Report only: no number ever fails CI. GitHub-hosted
  runners are shared, noisy machines. `regressed` or `improved` means that all five A/B pairs
  moved the same way, each by more than any of this job's five A/A pairs: a lead to look into,
  not proof." W2 rows carry the reading rule of section 5.2.

### 15.7 Report
- **Job summary** per platform: a header line (push range or `fallback base`, `library_changed`,
  `build_definition_changed`, `benchmark_flags_differ`, clock check, image, compiler, CPU model,
  C, status), the wording above, then one row per cell: comparable A/B and A/A pairs, invalid
  samples, any stop, and each primary metric's verdict with median, [min, max] and A/A spread.
  A collapsed block lists every headline metric.
- **Artifact** `benchmark-compare-<platform>-<run ID>-<attempt>`, kept 90 days: the records, the
  runner log, the build logs, `job.json`, `summary.json` (every number of the summary, with the
  per-pair values) and `summary.md`.
- **Combined summary** in the collector job: one row per platform with its status and its
  `regressed` and `improved` metrics, stronger leads marked; then links to the run and the index.

### 15.8 History
- **Branch** `benchmark-history`: an orphan branch that only the collector writes. It holds no
  workflow files, and its pushes, made with `GITHUB_TOKEN`, start no workflow run (15.2).

```text
README.md                             generated: how to read the files
index.md                              regenerated: one line per run, newest first
runs/<year>/<run ID>/run.json         commits, base kind, changes, creation time, platform results
runs/<year>/<run ID>/<platform>.json  that platform's summary.json
```

- **Index.** One line per push run, ordered by the run's creation time (`created_at`, which
  re-runs keep): date, push range (`fallback base` marked), library and build-definition changes,
  per platform the numbers of `regressed`, `improved`, `inconclusive` and `no verdict` or its
  status, and links to the run files and the workflow run. Its first line gives the running
  empirical false-call rate: the verdicts in library-unchanged runs, over their primary metrics
  that had a value.
- **Stored per platform:** the summary with per-pair values, representations, verdicts,
  provenance and missing-data reasons, so that the rule can be re-applied. Raw records stay in
  the 90-day artifacts.
- **Collector job.** It needs setup and the four platform jobs and runs `if: always()`. Only
  this job has `contents: write` (and `actions: read`, for `created_at`); the workflow's default
  is `contents: read`. It writes the combined summary on every run and the history on push runs
  only. It runs `ci_compare.py` from the workflow's own commit, checks out with
  `persist-credentials: false`, fetches nothing but that commit and the history branch, and
  treats artifacts as data: JSON read with a schema check, nothing from them executed or copied
  unparsed.
- **Writing.** Fetch the whole branch (at most the budget), add the run's directory (a re-run
  replaces it), regenerate `index.md`, commit, and push with the explicit refspec
  `HEAD:refs/heads/benchmark-history`, the token given to that command only. Nothing in this
  design force-pushes. When the push is rejected because another collector pushed first, fetch
  and redo, up to 5 attempts with random waits of 5 to 30 s; then report a history-write failure.
- **Cancelled or failed platform jobs.** `run.json` records each job's result (success, failure,
  cancelled, skipped) and, when its artifact exists, its status. The index shows the status; the
  other platforms are written as usual.
- **Size budget: 20 MB of packed branch history** (`git count-objects -v` after the fetch and the
  new commit). A run adds roughly 30 KB (four summaries of about 20 KB before compression, and
  the index change), so the budget holds about 600 runs: about two years at the observed cadence,
  a projection. Full clones and the seven `fetch-depth: 0` checkouts of each library push (15.2)
  fetch every branch, so at the budget each downloads up to 20 MB more, about three times
  today's repository. When a run would pass the budget, the collector does not push: archiving
  stops, the collector reports a history-write failure, the job summaries and artifacts are
  unaffected, and the owner decides how to continue.

### 15.9 Failures and level 1
- **Red means that level 2 could not produce its report**, by its own fault or the
  infrastructure's; never a number.
  - Red: a step of level 2's own fails (checkout, toolchain setup, an error of the driver or the
    report, the upload); arm A fails to build while `benchmarks/` and the build definition are
    the same in base and head, so that arm A is exactly the base tree that master built before;
    the runner rejects its arguments or the CI workloads file; a job times out or the
    infrastructure fails (no runner, the VM does not start); a platform delivers no report; the
    history cannot be written (rejections, budget).
  - Reported, not red: any number; invalid samples; arm A failing with head's `benchmarks/` or
    another build definition (`not_compared: base_build_failed`); a failed head build
    (`not_compared: head_build_failed`; level 1 builds the same commit); no records after
    successful builds without an established level-2 fault (`not_compared: no_records`, with the
    runner's message, for example a crashed `--build-facts` probe); a stopped cell or platform;
    calls not run for the budget.
- **Mechanism.** The builds and the driver run with `continue-on-error: true`, and their outcomes
  reach the report step through the environment; the driver records each runner call's exit code
  and message in `job.json`. The report step classifies, writes the summary and sets
  `level2_fault`; the upload runs `if: always()`; a last step, `if: always()`, fails the job when
  the report step did not succeed or set `level2_fault` (as `coverage.yml:179-182` does). On
  FreeBSD the VM step's script writes the build outcomes and the driver's status to files and
  exits non-zero only when its own commands fail or the VM does not come up; the files are
  exported with rsync and reported on the host. The collector fails after writing what it can
  when a platform delivered no report or the history could not be written.
- **Level 1** stays as it is and keeps gating breakage. Level 2 is a separate workflow on the same
  pushes and does not wait for level 1. It posts no commit status and no badge.

### 15.10 Cost
Per job: two builds (65 to 100 s each in level 1, with level 1's sccache; FreeBSD reuses the
ccache arrangement of `build-freebsd.yml`; whether a cache serves arm A from the previous run's
head build, built in another checkout directory, is unverified), the clock check (under a
minute), the benchmark time of 15.5 and the report. Estimated: Linux about 10 min, Windows and
FreeBSD about 14 min each, macOS about 37 min, setup and collector about 2 min: about 75
runner-minutes per push, and about 33 hours a month as a projection at the observed cadence.
Standard hosted runners are free for this public repository. The real costs are the wait for a
report (macOS queued 21 min in level 1) and runner concurrency shared with the other workflows of
each push.

### 15.11 Left out of the first version
- Open-loop cells and calibration: rates would need a calibration per runner, and fixed light
  rates mostly show the wait regime (section 12, finding 4). Open decision 4 stands.
- The other cells: R = 16, the 261632-byte payload, W2 gaps of 200 µs, 20 ms and 2 s, windowed
  `leaf_to_leaf`, `n_over`.
- Comparisons with Linux Clang and MinGW, on pull requests, or of chosen commits.
- Gating, commit statuses, badges, issues and notifications.
- A stored A/A reference across jobs: each hosted job runs on another machine (15.2).
- Trend tables: the index links the per-run summaries instead.
- Statistics beyond the rule of 15.6.

### 15.12 Defaults the owner can change
- Red only when level 2 cannot produce its report (15.9).
- Pushes that leave the library unchanged run as null tests (15.3).
- Summaries only in the branch, with per-pair values (15.8).
- GCC on Linux (15.5).
- A history budget of 20 MB (15.8).

### 15.13 Implementation changes
1. `.github/workflows/benchmark-compare.yml` (new). Triggers as in 15.3; `permissions: contents:
   read`. Jobs: `setup` (full clone: base, head, the change flags); `linux`, `windows`, `macos`,
   `freebsd` (head and base in two directories, the overlay, two builds with a CMake File API
   query, the compiler cache, `TMPDIR` or `TEMP` set to a private directory as in level 1,
   `ci_compare.py run` and `report`, step summary, artifact, the final status step; FreeBSD
   builds and measures in the VM and reports on the host); `collect` (15.8). Actions pinned by
   commit and context passed through the environment, as in the other workflows.
2. `benchmarks/workloads_ci.json` (new): the cells of 15.5.
3. `benchmarks/ci_compare.py` (new): `run` (clock check, six runner calls per cell, stopping,
   deadline, `job.json`); `report` (flag comparison, the classification of 15.9, the verdicts of
   15.6 through the runner's `comparable_pairs`, `metric_values` and `compare`, `summary.json`,
   `summary.md`); `collect` (combined summary, `run.json`, `index.md`, size check, push).
4. `benchmarks/run_benchmarks.py`: the ratio-or-difference choice (`:1342-1343`) becomes a
   function that the summary and `ci_compare.py` share, and `--build-facts` gets a 60 s timeout.
   No new option.
5. `benchmarks/harness.h`: the `--clock-stamp` mode. Nothing measured or recorded changes.
6. `benchmarks/README.md`: the level 2 paragraph of "Continuous integration" (lines 348-349) and
   the calibration note (lines 312-314). This plan: status lines once implemented.
- Unchanged: the record schema and level 1. No larger or self-hosted runners. Repository
  settings: none, unless a branch rule covers `benchmark-history`; then the Actions token must be
  allowed to push to it.
