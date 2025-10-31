# Barrier ACK Refactor — Working Notes

## Overview
- **Project:** Barrier acknowledgement refactor on branch `feature/barrier-ack-skeleton`.
- **Goal:** Allow each barrier participant to request specific guarantees (rendezvous, inbound delivery, outbound delivery, processing quiescence) while keeping the public barrier API and contracts unchanged.
- **Reference design:** `docs/barrier_refactor_architecture.md` (phases, payload layout, coordinator state machine) and `docs/windows_delivery_fence_strategy.md` (delivery-drain rationale).

## Contracts In Scope
- **Rendezvous:** All participants reach the barrier RPC; coordinator replies once everyone arrives. No delivery requirement.
- **Inbound delivery:** Caller has consumed all messages that target it with a pre-barrier sequence stamp.
- **Outbound delivery:** Every peer has confirmed consumption of the caller's pre-barrier messages.
- **Processing fence:** All peers confirm they ran their post-delivery processing hooks. Builds on inbound/outbound guarantees.
- Coordinator replies with a `barrier_completion_payload` showing phase state (`satisfied`, `downgraded`, `failed`) and failure metadata per participant as defined in the architecture doc.

## Current Code State
- `include/sintra/detail/barrier_protocol.h` — structs/enums for the new payloads. `barrier_ack_request` now carries `group_instance_id`; helper factories exist.
- `include/sintra/detail/coordinator_impl.h` — last-arrival path emits outbound/processing ack requests with the coordinator's `instance_id()` and waits on condition variables; timeout/downgrade handling still coarse.
- `include/sintra/detail/managed_process_impl.h` — the ack handler runs via `run_after_current_handler`, calls `flush` or `wait_for_delivery_fence()`, and immediately responds; it does **not** yet enforce outbound/processing waits or guard against coordinator teardown.
- `tests/run_tests.py` — when `SINTRA_WINDOWS_DEBUGGER_EXE` is unset, it defaults to `C:/Program Files/Windows Kits/10/Debuggers/x64/cdb.exe` if that binary exists so local Windows crashes capture dumps automatically.
- Untracked helpers: `temp_ring.cpp`, root `NEXT_PROMPT.md`, and a zero-length `NUL` placeholder; ignore for now.

## Testing & Debugging
- Build directory used so far: `..\build-ninja2` (Debug).
- Repro command:
  ```powershell
  python tests/run_tests.py --test sintra_processing_fence_test --timeout 40 --build-dir ..\build-ninja2 --config Debug
  ```
- Always run binaries through the harness above; invoking the test executables directly will hang.
- Crash dumps: `%LOCALAPPDATA%\CrashDumps\sintra_processing_fence_test_debug.exe.<pid>.dmp`.
- Inspect with cdb:
  ```powershell
  "C:/Program Files/Windows Kits/10/Debuggers/x64/cdb.exe" -z "$env:LOCALAPPDATA\CrashDumps\sintra_processing_fence_test_debug.exe.<pid>.dmp" -c ".symfix; .reload; kv; !analyze -v; q"
  ```
- Runner attaches cdb automatically thanks to the new default. Visual Studio had a competing JIT handler; user disabled it during last session.

## Current Failure Snapshot
- Test `processing_fence_test` throws `std::runtime_error("Attempted to make an RPC call using an invalid instance ID.")` inside `Managed_process::barrier_ack_request`, suggesting the coordinator instance id is stale by the time the ack reply fires (coordinator already stopped or mismatched id).
- No outbound/processing ack waits implemented yet, so the coordinator currently believes acks succeed even when the handler faults.

## Migration Plan (per architecture doc)
1. **Protocol plumbing** — done: enums/structs, request flags, ack messages.
2. **Coordinator state machine** — partially done: ack issuance, wait lists, but timeout/downgrade reporting needs polish.
3. **Participant wait logic** — pending: ack handler must synchronously drain outbound/processing targets on the request thread and downgrade on failure.
4. **Wrapper integration** — delivery fence/processing fence wrappers must set the proper flag combinations (currently equivalent to old behaviour).
5. **Testing** — extend coverage for mixed guarantee scenarios and failure cases.

## Immediate Action Items
1. **Fix ack handler**
   - Keep execution on the request thread via `run_after_current_handler`.
   - Wait for outbound delivery by draining the specific target sequence(s) and detect coordinator shutdown; when impossible, send `success = false` with the right failure code.
   - For processing, reuse `wait_for_processing_quiescence()` (covers delivery + post handlers) and return the resulting sequence watermark.
   - Ensure `Process_group::rpc_barrier_ack_response(req.group_instance_id, response)` only runs when `req.group_instance_id` is valid.
2. **Coordinator robustness**
   - When acks timeout or fail, mark the phase downgraded/failed with populated `barrier_phase_status` fields.
   - Handle coordinator shutdown by failing outstanding phases with `barrier_failure::coordinator_stop`.
3. **Investigate crash dump**
   - Confirm whether the invalid instance id comes from late replies (coordinator torn down) or incorrect `group_instance_id` propagation.
   - Use findings to adjust teardown paths (e.g., bail out early if `s_coord == nullptr`).
4. **Testing**
   - Re-run `processing_fence_test`, `delivery_fence_test`, and any multi-process suites exercising barriers.
   - Add new tests once outbound/processing paths are implemented (mixed guarantees, timeout/downgrade scenarios).
5. **Housekeeping**
   - Decide whether to commit the cdb default (safe opt-in).
   - Review untracked helper files and remove or incorporate them before finalizing the branch.

## Prompt for Next Invocation
```
You're continuing the barrier acknowledgement refactor on branch feature/barrier-ack-skeleton. Review docs/barrier_refactor_architecture.md for the target design. The current crash is std::runtime_error("Attempted to make an RPC call using an invalid instance ID.") from Managed_process::barrier_ack_request during sintra_processing_fence_test. Implement the real outbound/processing acknowledgement waits in Managed_process::barrier_ack_request (request-thread, guarded against coordinator shutdown), refine coordinator timeout/downgrade handling, and then rerun the processing_fence_test (and related suites). Use tests/run_tests.py, which now defaults to cdb.exe under C:/Program Files/Windows Kits/10/Debuggers/x64. Examine existing crash dumps under %LOCALAPPDATA%\CrashDumps if needed.
```
