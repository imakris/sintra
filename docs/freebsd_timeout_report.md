# FreeBSD CI Timeout Investigation Report

## Environment Context
- Upstream Linux builds configure and run all tests successfully on this branch. For example, running `ctest --test-dir build --output-on-failure -R sintra.basic_pubsub` after a local GNU/Linux build passes in ~0.6 s.【acf0cc†L1-L11】
- FreeBSD 14.2 Cirrus CI hosts expose two vCPUs and 4 GiB of RAM. They repeatedly hang whenever distributed tests spawn additional processes despite building successfully.

## Observed Failure Pattern on FreeBSD
- Every multi-process test (basic pub/sub, ping-pong, RPC append, recovery, barrier suites, processing fence) times out after the 120 s harness timeout while the single-process smoke tests continue to pass. The overall run therefore reports 27–36 % success.
- Instrumentation shows that each worker process reaches `branch.worker.wait_group` for the `_sintra_all_processes` coordinator group and never logs the matching `branch.worker.got_group` or `branch.barrier.enter/exit` events. This means the worker is blocked during swarm initialization, before any user-level messages are exchanged.
- Coordinator traces confirm that the coordinator process publishes the swarm directory, spawns children successfully, and creates the `_sintra_all_processes` group, but the children never observe the publication.
- Because the wait happens at startup, every affected test consumes the full 120 s timeout, leading to >16 min total wall-clock time for the CI job.

## Hypotheses About Root Cause
1. **Process-group publication visibility:** The instrumentation strongly suggests that group membership updates made by the coordinator are not visible to other processes on FreeBSD. This could stem from:
   - Shared-memory pages (likely POSIX `shm_open`/`mmap`) not being flushed correctly (`msync`/memory fences) when coordinator rewrites group slots.
   - Missing cache coherence primitives (e.g., `std::atomic` with `memory_order_release`/`acquire`) around the publication or membership counters when running on a different libc/libpthread implementation.
2. **File-system semantics:** The coordinator clears and recreates directories under `/tmp/sintra`. If FreeBSD’s `unlink`+`mkdir` race differs, children might open the ring buffers before the coordinator finalizes permissions or before the data becomes visible.
3. **Process start ordering vs. scheduler:** With only two vCPUs, long-running `std::this_thread::sleep_for` or busy loops inside the coordinator could starve the branch threads that should deliver the group update notifications.

## Suggested Next Steps
- Audit `Coordinator::make_process_group` and related data structures to ensure all shared fields are updated with release semantics and read with acquire semantics. Investigate whether we need explicit `std::atomic_thread_fence` calls after republishing the group on BSD.
- Add temporary logging directly around the point where the worker polls for `_sintra_all_processes` (likely `process_branch::wait_for_group`) to verify whether the shared-memory payload changes at all or remains at its initial sentinel values.
- Create a reduced reproducer that repeatedly creates and joins a coordinator group on FreeBSD to narrow down the failing primitive without the full test harness.
- If shared-memory visibility is the culprit, experiment with forcing `msync`/`__sync_synchronize()` after coordinator writes, or, as a diagnostic, place the control block in a memory-mapped file on a traditional filesystem to check whether the issue is tmpfs-specific on Cirrus FreeBSD images.

## Summary for Handoff
The failure is isolated to the swarm bootstrap barrier: workers never learn that `_sintra_all_processes` has been published, so they block indefinitely and every multi-process test times out. Focus further debugging on how coordinator group publications propagate across processes on FreeBSD, ensuring proper synchronization primitives and visibility of shared-memory updates.

## Follow-up Experiment: Atomic Publication + RPC Polling
- Added a shared `ipc_atomic` helper and switched `Transceiver::mark_published` / `mark_unpublished` to use release-store semantics so workers can acquire-load the publication flag reliably across processes.【F:include/sintra/detail/ipc_atomic.h†L1-L32】【F:include/sintra/detail/transceiver.h†L615-L626】
- Updated `Coordinator::make_process_group` to unpublish old group instances, refresh membership, and rely on the new atomic publication helpers before returning the refreshed group id.【F:include/sintra/detail/coordinator_impl.h†L658-L705】
- Replaced the blocking `rpc_wait_for_instance` call in the branch startup path with a short polling loop that repeatedly invokes `rpc_resolve_instance` until the coordinator advertises the `_sintra_*` group, preserving the existing trace breadcrumbs.【F:include/sintra/detail/managed_process_impl.h†L1159-L1185】
- Linux sanity checks still pass instantly after these changes (e.g., `ctest --test-dir build --output-on-failure -R sintra.basic_pubsub --timeout 30` completes in ≈0.5 s).【452c1b†L1-L11】
- FreeBSD runs, however, continue to log `branch.worker.wait_group` without the matching `branch.worker.got_group` event before hitting the 120 s harness timeout, implying that `rpc_resolve_instance` never observes the coordinator’s publication in that environment.

## Outstanding Questions for Further Handoff
1. Why does the coordinator-side `publish_transceiver` update still fail to propagate to `rpc_resolve_instance` on FreeBSD even with atomic stores? We may need to instrument that path to confirm whether `assign_name` succeeds or whether the publication cache is being cleared prematurely.
2. Could the per-process cache (`m_instance_id_of_assigned_name`) be wiped during swarm startup on FreeBSD (e.g., due to coordinator detaching from shared memory), preventing the new polling loop from ever seeing the assigned name? Adding explicit traces in `publish_transceiver` after the map update would confirm.
3. As a follow-up diagnostic, we could expose a one-off RPC that dumps the coordinator’s `m_instance_id_of_assigned_name` contents when the worker is stuck, verifying whether the entry is missing or merely invisible to RPC callers.

## Additional Instrumentation (Current Investigation)
- Added deterministic traces on the coordinator when constructing `_sintra_*` process groups and when each worker joins or waits for readiness, including explicit markers for “awaiting initialization,” “waiting for expected members,” and “not ready” shutdown paths. These logs reveal the expected member count and the coordinator’s notion of which instance triggered the RPC.【F:include/sintra/detail/coordinator_impl.h†L781-L855】
- Instrumented the worker bootstrap to log whenever the join RPC returns an invalid id and we fall back to `rpc_resolve_instance`, so FreeBSD logs can confirm whether the RPC ever completed or always blocked.【F:include/sintra/detail/managed_process_impl.h†L1166-L1189】
- Emitted detailed RPC-level traces for the join handshake covering activation, send, unblock, final reply, and cancellation/error paths. These events will tell us whether the request ever leaves the worker, whether the return handler is invoked, and whether `Managed_process::unblock_rpc` cancelled the call.【F:include/sintra/detail/transceiver_impl.h†L907-L964】
- When `unblock_rpc` cancels outstanding calls (e.g., due to coordinator shutdown), we now log which remote instance was unblocked so that FreeBSD traces can correlate cancellation with the join RPC if that occurs.【F:include/sintra/detail/managed_process_impl.h†L1668-L1684】
- The bootstrap state now records when the coordinator drops an expected member because it began draining or was unpublished before joining. The new `coordinator.group.drop_absent` trace reports the swarm, group, member id, and updated expectation so stalled workers can confirm whether the coordinator adjusted the barrier for failed peers.【F:include/sintra/detail/coordinator_impl.h†L120-L167】【F:include/sintra/detail/coordinator_impl.h†L640-L694】

Collectively, these breadcrumbs should let us distinguish between “RPC never written,” “request written but never handled,” and “coordinator handled but failed to publish.” If the FreeBSD logs still lack `coordinator.group.*` events while showing `rpc.join.sent`, that will point directly at the transport/dispatch layer instead of shared-memory publication.

## Latest Trace Review (latest FreeBSD Cirrus run)
- The current Cirrus snapshot still shows each spawned worker logging `branch.worker.wait_group` without ever emitting `branch.worker.got_group`. None of the coordinator-side breadcrumbs (`coordinator.group.*`) appear, suggesting that the join RPC is either not dispatched or never reaches the coordinator process before the harness timeout.
- The latest run adds `branch.worker.join_rpc` entries for every spawned process (for example, worker `216172782113783809` calling coordinator `144115188075855874`), which confirms that each branch issues the join RPC locally. The continued absence of matching `coordinator.group.*` traces indicates the request never surfaces in the coordinator, further narrowing the fault to the RPC transport or dispatch path.
- To expose the existing transport breadcrumbs in future logs, the FreeBSD test harness now includes `rpc.*` in `SINTRA_TRACE_SCOPE`, so upcoming traces will show the `rpc.join.*` events emitted when the request is queued, delivered, cancelled, or replied.
- To tighten the diagnostics, the worker bootstrap now emits `branch.worker.join_rpc` immediately before invoking the join RPC so we can confirm whether the request leaves the process at all.【F:include/sintra/detail/managed_process_impl.h†L1162-L1184】
- `Coordinator::join_and_wait_group` now logs a `coordinator.group.rpc_entry` breadcrumb, a richer `coordinator.group.join` snapshot, and periodic `coordinator.group.wait_ready` updates (every 250 ms) that include the pending member count, joined set, and accounted absentees. These traces should surface instantly if the coordinator even receives the RPC.【F:include/sintra/detail/coordinator_impl.h†L92-L213】【F:include/sintra/detail/coordinator_impl.h†L888-L1015】
- The bootstrap state dumps the membership and absentee sets whenever it initializes, drops a member, or finishes waiting, so we can correlate FreeBSD hangs with the coordinator’s evolving view of the swarm.【F:include/sintra/detail/coordinator_impl.h†L92-L213】
- Cirrus now limits the FreeBSD `ctest` invocation to only the `sintra.basic_pubsub` suite so we capture the canonical stalled bootstrap trace without repeating the same handshake failure in other multi-process variants.【F:.cirrus.yml†L21-L24】
- A subsequent Cirrus attempt emitted no `sintra.trace` lines at all, confirming the harness lost the tracing environment. The FreeBSD job now exports `SINTRA_TRACE_SYNC=1` and the widened `SINTRA_TRACE_SCOPE` directly on the `ctest` command line so future runs always capture the bootstrap instrumentation even if CTest properties are skipped.【F:.cirrus.yml†L23-L24】

If the next FreeBSD run still omits `coordinator.group.rpc_entry` while the worker prints `branch.worker.join_rpc`, the failure almost certainly sits in the RPC transport layer (ring write, reader dispatch, or handler registration). Conversely, the new wait snapshots will show which member(s) the coordinator still expects if the RPC finally lands but the barrier never completes.
