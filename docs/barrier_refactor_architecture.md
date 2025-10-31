# Barrier Refactor Architecture

## Background

Sintra's current barrier implementation layers a rendezvous RPC with a
"drain everything" local fence. Every participant ends up enforcing the same
delivery semantics, which makes it difficult to support asymmetric needs and
complicates debugging. This document describes a refactor that lets each
participant declare which guarantees it requires and reports the precise outcome
of every requested phase.

## Goals

- Preserve the public barrier API while allowing callers to request specific
  guarantees.
- Split the barrier into independently tracked phases (rendezvous, inbound
  delivery, outbound delivery, processing).
- Return a single completion payload per caller describing the result (or
  downgrade) of each requested phase.
- Make all waits bounded and surface failure reasons explicitly.
- Keep message formats POD-friendly so they can be written directly into the
  reply ring.
- Limit protocol fan-out: at most one acknowledgement per participant per phase.

## Terminology

- **Participant**: Process instance that belongs to the barrier's process group.
- **Barrier sequence**: Coordinator reply-ring `leading_sequence` when the
  completion payload is written. Serves as the barrier token.
- **Requirement flags**: Bitmask supplied by each caller to declare required
  guarantees.
- **Phase**: One of the guarded guarantees (`rendezvous`, `inbound`, `outbound`,
  `processing`).

## Enumerations

```c++
enum class barrier_state : uint8_t {
    not_requested = 0,
    satisfied     = 1,
    downgraded    = 2,
    failed        = 3,
};

enum class barrier_failure : uint8_t {
    none             = 0,
    timeout          = 1,
    peer_draining    = 2,
    peer_lost        = 3,
    coordinator_stop = 4,
};
```

## Phase Status Structure

```c++
struct barrier_phase_status {
    barrier_state         state;
    barrier_failure       failure_code;
    instance_id_type      offender;  // offending peer or invalid_instance_id
    sequence_counter_type sequence;  // watermark or invalid_sequence
};
```

- `state`: Outcome of the phase.
- `failure_code`: Reason when `state` is not `satisfied`.
- `offender`: Participant responsible for the downgrade or failure (or
  `invalid_instance_id` if none).
- `sequence`: Numeric context for the phase (for example, flush watermark).
  Use `invalid_sequence` when no meaningful value exists.

## Completion Payload

```c++
struct barrier_completion_payload {
    sequence_counter_type barrier_sequence; // reply-ring sequence stamped on completion
    uint32_t              request_flags;    // caller's guarantee bitmask

    barrier_phase_status  rendezvous;       // always populated
    barrier_phase_status  inbound;          // valid if inbound bit set
    barrier_phase_status  outbound;         // valid if outbound bit set
    barrier_phase_status  processing;       // valid if processing bit set
};
```

### Request Flags

```c++
constexpr uint32_t barrier_flag_inbound    = 1u << 0;
constexpr uint32_t barrier_flag_outbound   = 1u << 1;
constexpr uint32_t barrier_flag_processing = 1u << 2;
```

- `barrier_flag_inbound`: Caller must have received all pre-barrier traffic
  destined for it.
- `barrier_flag_outbound`: Every participant must confirm it consumed the
  caller's pre-barrier traffic.
- `barrier_flag_processing`: All participants must finish their post-barrier
  processing phase.

The default delivery fence sets only `barrier_flag_inbound`. The rendezvous
variant sets none. The processing fence sets both inbound and processing bits.

## Rendezvous Request Payload

The existing barrier RPC body is extended to carry the caller's bitmask:

```c++
struct barrier_rendezvous_request {
    uint32_t request_flags;
    // existing fields preserved
};
```

## Coordinator State Extensions

`Process_group::Barrier` gains bookkeeping to track guarantees and pending
acknowledgements:

```c++
struct Process_group::Barrier {
    mutex                                   m;
    condition_variable                      cv;
    unordered_set<instance_id_type>         processes_pending;
    unordered_set<instance_id_type>         processes_arrived;
    bool                                    failed = false;
    instance_id_type                        common_function_iid = invalid_instance_id;

    uint32_t                                group_requirement_mask = 0;
    unordered_map<instance_id_type, uint32_t> per_process_flags;

    unordered_set<instance_id_type>         outbound_waiters;
    unordered_set<instance_id_type>         processing_waiters;

    // timeout bookkeeping fields (timestamps, etc.) added in implementation
};
```

- `group_requirement_mask`: Bitwise OR of all participant flags.
- `per_process_flags`: Snapshot for populating per-caller completion payloads.
- `outbound_waiters` and `processing_waiters`: Participants still expected to
  acknowledge the respective guarantees.

## State Machine

1. **Initialisation**
   - Caller issues `Process_group::rpc_barrier` with a `request_flags` bitmask.
   - Coordinator acquires `m_call_mutex`, snapshots membership (filtering
     draining members), records `per_process_flags[caller]`, and accumulates
     `group_requirement_mask`.
   - New barriers populate `processes_pending` from the filtered membership and
     clear `processes_arrived`.

2. **Rendezvous Wait**
   - Arrivals move from `processes_pending` to `processes_arrived`.
   - Non-final callers receive deferral replies to remain blocked on the RPC.
   - If a participant starts draining or is lost, `drop_from_inflight_barriers`
     removes it. When `processes_pending` becomes empty, continue. If this makes
     the rendezvous impossible, mark `rendezvous.state = barrier_state::failed`
     with `failure_code` set to `peer_draining` or `peer_lost`.

3. **Requirement Aggregation**
   - When the last active participant arrives, freeze
     `group_requirement_mask` and initialise `outbound_waiters` and
     `processing_waiters` to the set of active participants if the corresponding
     bits are present.
   - Prepare per-caller `barrier_completion_payload` instances but defer writing
     them until all phases settle.

4. **Inbound Phase**
   - For callers that set `barrier_flag_inbound`, set
     `inbound.state = barrier_state::satisfied` and
     `inbound.sequence = coordinator_reply_leading_sequence`. The caller drains
     locally to that watermark via `Managed_process::wait_for_delivery_fence()`.
   - The coordinator does not wait for acknowledgements; local failures manifest
     as exceptions on the caller side.

5. **Outbound Phase**
   - If `group_requirement_mask` includes `barrier_flag_outbound`, the
     coordinator issues a single `barrier_ack_request` to each participant. The
     request carries the `barrier_sequence` and an `outbound` target sequence.
   - Each participant waits until its reply reader reaches the target and then
     sends a `barrier_ack_response` with `ack_type = outbound`.
   - The coordinator removes responders from `outbound_waiters`. If a participant
     drains or is lost before replying, mark `outbound.state` as
     `downgraded` or `failed` with `failure_code = peer_draining` or
     `peer_lost`.
   - If outstanding acknowledgements remain after the timeout window, mark the
     phase `failed` with `failure_code = timeout` naming the offending
     participant.

6. **Processing Phase**
   - Active only when `barrier_flag_processing` is present.
   - After completing any local inbound drain, each participant calls the
     existing `wait_for_processing_quiescence()` helper. Once that returns, the
     participant dispatches a `barrier_ack_response` with `ack_type = processing`
     containing the current local processing cutoff sequence.
   - The coordinator removes responders from `processing_waiters`, handling
     draining, loss, and timeouts the same way as the outbound phase.

7. **Completion**
   - When every required phase finishes (either satisfied or definitively failed
     or downgraded), the coordinator writes the prepared
     `barrier_completion_payload` to the reply ring for each caller. Phases that
     were not requested are set to `barrier_state::not_requested`.
   - Deferred callers receive their completion payloads. The barrier entry is
     erased from `m_barriers`.

## Acknowledgement Messages

```c++
enum class barrier_ack_type : uint8_t { outbound = 1, processing = 2 };

struct barrier_ack_request {
    sequence_counter_type barrier_sequence;
    barrier_ack_type      ack_type;
    sequence_counter_type target_sequence;
};

struct barrier_ack_response {
    sequence_counter_type barrier_sequence;
    barrier_ack_type      ack_type;
    sequence_counter_type observed_sequence;
    bool                  success; // false if the participant refuses or cannot comply
};
```

- Requests are emitted once per participant per required acknowledgement phase.
- Responses are ignored if the coordinator no longer tracks the referenced
  `barrier_sequence`. A `success == false` response downgrades or fails the
  phase with `failure_code = barrier_failure::peer_draining` and `offender`
  set to the participant.

## Timeout Defaults and Configuration

- `barrier_timeout_rendezvous`: 30 seconds. Bounds how long the coordinator waits
  for the remaining participants to arrive. Exposed via the existing runtime
  configuration table (`Managed_process::config()`).
- `barrier_timeout_outbound`: 30 seconds. Applies to outbound acknowledgement
  waits. Also configurable at runtime.
- `barrier_timeout_processing`: 60 seconds. Processing phases typically involve
  application callbacks, so we allow a longer default. Configurable in the same
  table.

Timeouts are evaluated using steady-clock timestamps captured when the phase
starts. Exceeding a timeout marks the phase `failed` with
`failure_code = timeout` and `offender` set to the first outstanding participant.

## Processing Acknowledgement Integration

The existing processing fence path calls `wait_for_processing_quiescence()`,
which in turn drains inbound delivery and executes any queued post-handlers on
the request thread. We reuse that mechanism:

1. When a participant receives a `processing` acknowledgement request it:
   - Drains inbound delivery using the watermark already provided in the
     completion payload (if requested).
   - Executes `wait_for_processing_quiescence()` to guarantee handlers and post
     tasks have run.
2. After the helper returns, the participant emits `barrier_ack_response` with
   `ack_type = processing`, `observed_sequence` set to its local processing
   cutoff (the same marker returned by `wait_for_processing_quiescence()`), and
   `success = true`.
3. If the participant is shutting down or cannot honour the request, it replies
   once with `success = false`. The coordinator treats that as a downgrade or
   failure and releases the barrier with `failure_code = peer_draining`.

> **Implementation note:** keep the acknowledgement handler lean. Historical
> builds tried to “double check” membership by looking inside the thread-safe
> maps that track published transceivers, which reopened long-dead spinlock
> crashes during teardown. Let the coordinator’s existing RPC failure handling
> drive any downgrades instead of doing extra bookkeeping in the participant.

This keeps processing acknowledgements on the request thread, reusing the
existing quiescence semantics without introducing extra worker threads.

## Timeout and Failure Semantics

- **Rendezvous**: Timeout or loss marks `rendezvous.state = failed` and releases
  every caller with the populated payload.
- **Inbound**: The coordinator never waits. The caller handles local failures by
  throwing if its drain cannot complete (for example, communication halted).
- **Outbound**: Missing acknowledgements lead to `timeout`, `peer_draining`, or
  `peer_lost` failure codes. The coordinator only releases outbound waiters when
  the acknowledgement set empties.
- **Processing**: Same approach as outbound. Timeouts and peer loss map to the
  corresponding failure codes.
- **Coordinator shutdown**: If the coordinator begins stopping mid-barrier, all
  outstanding phases flip to `barrier_state::failed` with
  `failure_code = coordinator_stop` before replies are written.

## Testing Strategy

1. **Unit tests**
   - Serialize and deserialize `barrier_completion_payload`,
     `barrier_ack_request`, and `barrier_ack_response`.
   - Validate enum conversions and failure-code handling utilities.

2. **Integration tests**
   - Rendezvous-only barriers across multiple processes.
   - Mixed guarantees: some participants request inbound only, others outbound
     or processing, verifying the payloads match expectations.
   - Timeout scenarios: delay acknowledgements to trigger `timeout` failures.
   - Draining scenarios: initiate `sintra::finalize()` mid-barrier to exercise
     `peer_draining` handling.
   - Crash scenarios: terminate a participant to confirm `peer_lost` handling.

3. **Regression tests**
   - Re-run existing delivery fence, processing fence, and stress suites to
     ensure default semantics remain intact.

Instrumentation (temporary during rollout) can log `barrier_sequence` and phase
states to simplify debugging.

## Migration Plan

1. Introduce enums, structs, and request flags guarded behind feature toggles.
2. Extend `Process_group::rpc_barrier` to accept and propagate `request_flags`.
3. Implement coordinator state machine and acknowledgement handling.
4. Adapt `barrier<delivery_fence_t>` and `barrier<processing_fence_t>` wrappers
   to set the appropriate flags.
5. Enable new behaviour in integration tests and remove feature toggles once
   validation passes.

---

This design enables per-caller guarantees, predictable failure reporting, and
bounded phases while keeping the public API stable.
