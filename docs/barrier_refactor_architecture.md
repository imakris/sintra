# Barrier Refactor Architecture

## Background

The legacy barrier combined a rendezvous RPC with an implicit "drain everything" fence that applied to every
participant. The refactor keeps the API simple by letting a group choose the guarantees it needs up front while avoiding
extra coordinator bookkeeping.

Two design principles guide the new barrier:

1. **Single-mode rendezvous** - A barrier instance runs with exactly one requirement mask. The first participant to
   arrive declares the mask; every subsequent participant must request the same guarantees or **that arrival fails
   immediately**.
2. **Fail fast, report clearly** - Optional guarantees either succeed or are downgraded. The library never upgrades or
   downgrades behaviour silently; callers receive a deterministic completion payload and decide how to react.

## Goals

- Treat every rendezvous as single-mode: the first arrival declares the required guarantees and later arrivals must
  match or the rendezvous fails fast.
- Split the barrier into independently tracked phases (rendezvous, outbound delivery, processing).
- Return a single completion payload per caller describing the result of the coordinator-managed phases.
- Keep coordinator state minimal and single-threaded.
- Maintain bounded waits and explicit failure reasons.
- Reuse the existing delivery-fence machinery so no new sequence spaces are introduced.

## Terminology

- **Participant** - Process instance that belongs to the barrier's process group.
- **Barrier sequence** - Coordinator reply-ring `leading_sequence` recorded when the rendezvous completes; acts as the
  barrier token.
- **Barrier epoch** - Monotonic counter per barrier name; combined with `barrier_sequence` to identify a rendezvous
  instance.
- **Participant boot id** - Monotonic counter per participant restart; paired with `instance_id` whenever waiters or
  acknowledgements are tracked.
- **Requirement mask** - Bitmask carried by a rendezvous request describing which guarantees the caller expects.
  Different masks must use different barrier names; we never negotiate across mixed binaries.
- **Phase** - One of the coordinator-managed stages (`rendezvous`, `outbound`, `processing`).
- **`request_flags`** - API term identical in meaning and bit layout to `requirement_mask` (the spec uses
  `requirement_mask` throughout; APIs expose `request_flags`). We keep the historical `requirement_mask` wording inside
  the coordinator state machine description so existing implementation notes stay searchable, while every public entry
  point continues to accept and surface `request_flags`.

## Data structures

```c++
enum class barrier_state : uint8_t {
    not_requested         = 0,
    satisfied             = 1,
    downgraded            = 2,
    failed                = 3,
};

enum class barrier_failure : uint8_t {
    none                  = 0,
    timeout               = 1,
    peer_draining         = 2,
    peer_lost             = 3,
    coordinator_stop      = 4,
    incompatible_request  = 5,
};

struct barrier_phase_status {
    barrier_state         state;
    barrier_failure       failure_code;
    sequence_counter_type sequence;           // rendezvous: captured watermark;
	                                          // outbound/processing: invalid_sequence
};

struct barrier_completion_payload {
    uint64_t              barrier_epoch;      // increments per rendezvous instance
    sequence_counter_type barrier_sequence;   // rendezvous token
    uint32_t              requirement_mask;   // rendezvous mask for the epoch; fast-fail arrivals
	                                          // before freeze echo the caller's request_flags

    barrier_phase_status  rendezvous;         // coordinator authored
    barrier_phase_status  outbound;           // success/failure for the group
    barrier_phase_status  processing;         // success/failure for the group
};
```

### Coordinator state (per barrier)

```c++
struct Participant_id {
    instance_id_type iid;
    boot_id_type     boot_id;
    bool operator==(const Participant_id& o) const noexcept {
        return iid == o.iid && boot_id == o.boot_id;
    }
};

struct Participant_id_hash {
    size_t operator()(const Participant_id& p) const noexcept {
        size_t h1 = std::hash<instance_id_type>{}(p.iid);
        size_t h2 = std::hash<boot_id_type>{}(p.boot_id);
        return h1 * 1315423911u ^ h2;
    }
};

struct Barrier {
    std::mutex                                                   m;
    std::unordered_set<Participant_id, Participant_id_hash>      pending;
    std::unordered_set<Participant_id, Participant_id_hash>      arrivals;
    std::string                                                  name;   // stable identifier for timeout validation

    uint32_t                requirement_mask    = std::numeric_limits<uint32_t>::max(); // ~0 signals "unset"
    uint64_t                barrier_epoch       = 0;
    sequence_counter_type   rendezvous_sequence = invalid_sequence;
    bool                    rendezvous_complete = false;

    struct Outbound_phase {
        struct Delivery_target {
            reader_id_type        reader;
            sequence_counter_type cutoff_leading;   // diagnostic only
            sequence_counter_type baseline_reading; // reader reading at first READER_NORMAL
            sequence_counter_type delta_target;     // saturating_diff(cutoff_leading, baseline_reading)
            sequence_counter_type delta_remaining;  // diagnostic, non-increasing
            bool                  armed = false;
        };

        std::unordered_map<Participant_id,
                           std::vector<Delivery_target>,
                           Participant_id_hash> targets;
        std::unordered_set<Participant_id, Participant_id_hash> waiters;
        barrier_phase_status status{ barrier_state::not_requested,
                                     barrier_failure::none,
                                     invalid_sequence };
    } outbound;

    struct Processing_phase {
        std::unordered_set<Participant_id, Participant_id_hash> waiters;
        barrier_phase_status status{ barrier_state::not_requested,
                                     barrier_failure::none,
                                     invalid_sequence };
    } processing;

    barrier_completion_payload completion_template;

    struct Phase_timer {
        bool armed = false;
        std::chrono::steady_clock::time_point deadline{};
        uint64_t armed_epoch = 0;
        sequence_counter_type armed_sequence = invalid_sequence;
        uint64_t armed_generation = 0;
    };

    Phase_timer rendezvous_timer;
    Phase_timer outbound_service_timer;
    Phase_timer outbound_timer;
    Phase_timer processing_timer;

    std::weak_ptr<Barrier> self; // used by timeout monitors
};
```

Every `std::unordered_*` in this document uses `Participant_id_hash` so that `(iid, boot_id)` pairs never alias between
restarts.

`requirement_mask` starts at `~0` (meaning "unset"). The first arrival overwrites it; later arrivals must match or the
rendezvous fails. Outbound bookkeeping keeps per-participant backlog deltas and a waiter set so the coordinator knows
who still owes a response; readers that are not yet in `READER_NORMAL` are ignored until they publish progress.
Processing keeps only the outstanding participants. The shared `completion_template` holds the final phase states that
every participant receives.

## Public contract

The public APIs remain unchanged:

- `barrier<rendezvous_t>` sends an empty requirement mask.
- `barrier<delivery_fence_t>` wraps `rendezvous_t` and runs the local inbound drain after the rendezvous returns.
- `barrier<processing_fence_t>` sets `barrier_flag_inbound | barrier_flag_processing` and performs the local processing
  drain before returning.

The rendezvous RPC returns a `barrier_completion_payload`; the helpers in `barrier<T>` combine it with the local inbound
result and return `true` only when every requested phase succeeded. Participants outside the frozen membership or with
mismatched masks receive a one-off payload populated as:

- `barrier_epoch = current epoch`
- `barrier_sequence = invalid_sequence`
- `requirement_mask = (requirement_mask == ~0 ? caller's `request_flags` : requirement_mask)` (so callers can log the
  expected guarantees)
- `rendezvous = failed / incompatible_request` (with `sequence = invalid_sequence`)
- `outbound` and `processing = not_requested` (both with `sequence = invalid_sequence`)

Note: once the requirement mask is frozen, every successful participant in the epoch receives the same value in
`requirement_mask`.


These fast-fail payloads do not mutate `completion_template`, and the rendezvous continues for valid members. The
coordinator runs at most one rendezvous per barrier name at a time; new arrivals wait until the previous epoch
completes.

## Coordinator flow

1. **Arrival**
   - Lock the barrier and, if this is the first arrival, snapshot the process-group membership (including `(instance_id,
     boot_id)` pairs obtained from the membership registry) into `pending` and advance `barrier_epoch`. Subsequent
     arrivals must appear in `pending`; participants outside that set are rejected individually with
     `incompatible_request` but the rendezvous continues. These fast-fail responses are built on the fly and do not
     mutate `completion_template`.
   - If the registry reports a replacement `(iid, new_boot)` in the same update that removed `(iid, old_boot)`, treat it
     as a swap: update the entry in `pending` without changing `rendezvous.state`. When a replacement arrives later
     (i.e., the registry removed the old boot id without simultaneously publishing the new one), treat it as a restart
     inline: remove the stale `(iid, old_boot)` from `pending`. If `rendezvous.state` is `not_requested` or `satisfied`,
     set it to `downgraded / peer_lost`; if it is already `downgraded` or `failed`, leave it untouched. Then insert
     `(iid, new_boot)` as the arriving participant (still subject to mask validation). This keeps the rendezvous from
     waiting indefinitely on the previous incarnation while preserving phase precedence.
   - If `requirement_mask` is `~0`, record the caller's request_flags value. It is now fixed for the lifetime of the
     rendezvous.
   - Otherwise, compare the caller's request_flags with the frozen mask. A mismatch causes that caller to receive
     `incompatible_request` immediately; the rendezvous set remains unaffected.
   - Move the caller from `pending` to `arrivals` and update `completion_template.requirement_mask`.

2. **Rendezvous completion**
   - When `pending` becomes empty, capture `rendezvous_sequence` from the reply ring. If `rendezvous.state` is still
     `not_requested` set it to `satisfied`; if it was already `downgraded` or `failed`, leave it untouched to respect
     the precedence rules. **Disarm `rendezvous_timer` (configured by `barrier_timeout_rendezvous`)**, stamp the
     captured sequence, set `rendezvous_complete = true`, and freeze membership (`pending` will no longer change for
     this barrier instance).
   - Initialise outbound/processing bookkeeping only if the mask requests those phases; otherwise the phase state
     remains `not_requested`.

3. **Outbound phase (if requested)**
   - At rendezvous completion capture, determine the set of **relevant coordinator request readers** per participant:
     readers that existed at capture time and consume pre-barrier traffic originating from that participant (i.e., the
     coordinator request readers through which the participant's RPCs arrive). **The relevant-reader set is sealed at
     rendezvous completion and never grows; only token-validated replacements may be adopted.** The routing snapshot
     supplies `covers_all_paths(participant)` to indicate whether every possible path for that participant was observed,
     and it also yields continuity tokens per reader. Both the coverage predicate and the tokens are captured at
     rendezvous completion and never refreshed mid-epoch. If the routing layer cannot compute `covers_all_paths`, treat
     it as returning `false`, in which case either disable outbound guarantees for this rendezvous or accept that
     outbound downgrades immediately because coverage is unknown. For each relevant reader store the coordinator-side
     `leading` as the **cutoff watermark** alongside the participant's `(instance_id, boot_id)`. Participants with zero
     relevant readers are treated as immediately satisfied **only when** `covers_all_paths(participant) == true`; they
     never enter `outbound.waiters` and **do not contribute to keeping `outbound_service_timer` armed** (the timer
     disarms once no captured reader is pending NORMAL), and they are ignored when determining whether every reader has
     reached `READER_NORMAL`. When coverage is unknown or false, requesting outbound causes an immediate downgrade for
     that participant (`peer_draining` or `peer_lost`, depending on registry metadata). Readers already in
     `READER_NORMAL` apply the baseline/delta rules immediately (the next bullet); readers still in service mode keep
     the cutoff but remain unarmed until they publish progress.
   - The routing snapshot also carries an optional **continuity token** per `(participant, reader)` sequence space.
     Equal continuity tokens prove that a post-capture reader reuses the same sequence space as the captured reader
     (e.g., a reconnect that did not reset offsets). When tokens are available, the coordinator adopts a replacement
     reader iff the tokens match; otherwise the replacement is treated as unrelated traffic and outbound downgrades for
     the affected participant. (Example: reader destroyed and recreated with the same token is adopted when it reaches
     `READER_NORMAL`; a mismatched or missing token downgrades immediately.) Deployments without token support must
     either disable outbound guarantees or accept that any reader churn downgrades outbound, because there is no safe
     way to adopt a new reader without risking ABA errors.
   - Arm the **service-mode grace timer** (`outbound_service_timer`) at rendezvous completion. Readers that do not
     publish their first progress (enter `READER_NORMAL`) before this deadline cause outbound to fail with `timeout`.
     When the last pending reader enters `READER_NORMAL`, disarm the service timer.
   - When a reader transitions to `READER_NORMAL` for the first time (using the existing reader-state callback), record
     `baseline_reading = reader_position_now`, compute `delta_target = saturating_diff(cutoff_leading,
     baseline_reading)` using the delivery-fence helper (wrap-safe), initialise `delta_remaining = delta_target`, mark
     the target armed, and schedule a recheck on the coordinator request loop. Subsequent NORMAL notifications for that
     reader are ignored.
   - Once every required reader has become NORMAL (or was already NORMAL)-where "required" means the captured relevant
     readers for this participant that were not immediately downgraded at capture-arm the main `outbound_timer`
     (deadline = now + timeout) and begin evaluating progress. Re-evaluation occurs whenever (a) the delivery-fence
     helper reports reader progress, (b) a reader arms itself by becoming NORMAL, or (c) a timer fires or is cancelled.
     A participant is removed from `outbound.waiters` only when **all** of its armed reader targets satisfy
     `seq_distance(reader_position, baseline_reading) >= delta_target`. `delta_target` was captured when the reader
     first entered `READER_NORMAL`, `baseline_reading` is the same value, and `seq_distance` reuses the wrap-safe helper
     from the delivery fence. `delta_remaining` is diagnostic only; once armed it never increases because it is derived
     from `delta_target - seq_distance`.

    All sequence math reuses the delivery-fence helpers:
    - `saturating_diff(target, baseline)` for wrap-safe differences (never negative).
    - `seq_distance(now, baseline)` for wrap-safe progress.

   - When coverage or continuity checks force a downgrade at capture, **do not insert that participant into
     `outbound.waiters`**, record the downgrade (e.g., set a `downgrade_seen` flag), and set `outbound.status =
     downgraded / peer_*` immediately (unless it is already `failed`). Otherwise continue tracking it in
     `outbound.waiters`.

     The epoch-level invariant is: once any capture-time downgrade is recorded, outbound for that epoch must end
`downgraded` unless a later timeout escalates it to `failed`; clearing the waiter set alone never yields `satisfied`.
   - The phase as a whole becomes `satisfied` only when `outbound.waiters` is empty **and no downgrades were recorded**
     for this epoch. If either the service-mode grace timer or the main outbound timer expires while `outbound.waiters`
     is non-empty, mark the phase `failed` with `failure_code = timeout` and record the offending `(participant,
     reader)` pairs for tracing. As soon as outbound becomes terminal (`satisfied`, `downgraded`, or `failed`), disarm
     both outbound timers. When `covers_all_paths(participant)` is `false` and outbound was requested, the coordinator
     always downgrades the phase immediately; operators may disable outbound for that rendezvous to avoid the downgrade.

4. **Processing phase (if requested)**
   - Insert each participant into `processing.waiters` (keyed by `(instance_id, boot_id)`) and issue a
     `barrier_ack_request` with `ack_type = processing`. Replies carry `(iid, boot_id)` and the coordinator drops any
     ACK whose tuple does not match an outstanding waiter, logging it as stale when tracing. The phase settles when
     every waiter replies or the timeout elapses. A timeout marks the phase failed with `failure_code = timeout` and
     records the remaining waiters as offenders. When the phase becomes terminal the processing timer is disarmed
     immediately.

5. **Completion emission**
   - When every requested phase reaches a terminal state (`satisfied`, `downgraded`, or `failed`), stamp the states into
     `completion_template`, copy it once per participant, and write the replies from the coordinator request thread. The
     per-recipient reply watermark is taken immediately before each write. After the replies are emitted the phase
     states are frozen; any late timer callbacks re-check the terminal flag and no-op.

Timeout monitors only mark phase state and post a recheck onto the request loop; all reply writes occur on that loop.

## Participant flow

1. **Barrier entry** - Send the rendezvous request with the desired mask and include the participant's `boot_id`. The
   call can fail immediately if the mask conflicts with the frozen mask or if the participant is not part of the frozen
   membership snapshot; in that case the participant receives a one-off payload with `rendezvous = failed /
   incompatible_request` and the rendezvous continues for valid members.
2. **Inbound (optional)** - After the rendezvous returns, call `wait_for_delivery_fence()` locally to drain inbound
   traffic. The boolean returned by `barrier<>` combines this local result with the coordinator payload.
3. **Processing acknowledgement** - Mark the barrier sequence as pending (recording the participant's `boot_id`) and
   continue pumping the request reader. Each time `notify_delivery_progress()` fires (for example when a handler
   finishes) the request reader checks the pending entries and enqueues a `barrier_ack_response` onto the dedicated
   sender queue. That queue drains outside the handler/request-reader critical path, so the reply is emitted without
   re-entering the reader or blocking the request ring. If the queue is full, the descriptor stays on the pending list
   and a local retry timer (jittered backoff, default 1s) schedules another enqueue attempt; no coordinator-driven tick
   is required. No helper threads are spawned.
4. **Completion handling** - The RPC returns a `barrier_completion_payload`. The caller combines the payload with its
   local inbound result to decide whether the barrier succeeded.

## Phase status semantics

- `rendezvous.sequence` is the coordinator reply-ring watermark recorded when the rendezvous completed.
- `outbound.sequence` is `invalid_sequence`; outbound is satisfied or failed solely by the coordinator's delta wait.
- `processing.sequence` is `invalid_sequence`; only the phase state conveys success or failure.

Inbound status is purely local and therefore absent from the coordinator payload.
Offending participant identifiers are recorded in tracing only; the payloads do not carry an `offender` field.

### Failure codes and state monotonicity

- `incompatible_request`: participant was not part of the frozen membership for this barrier or requested a mismatched
  mask (only that participant is rejected).
- `timeout`: rendezvous wait expired, an outbound delta never completed (including readers that never reached
  `READER_NORMAL`), or a processing acknowledgement did not arrive in time.
- `peer_draining`: registry reported a graceful departure or a reader/service transition the coordinator can attribute
  to deliberate draining.
- `coordinator_stop`: coordinator stopped mid-barrier.
- `peer_lost`: a participant (or its verifying reader) departed without a replacement in the same sequence space,
  whether detected before or after rendezvous completion.

Each phase transitions exactly once from `not_requested` to one of `{satisfied, downgraded, failed}`. While the barrier
instance is in flight, transitions may only escalate in severity (`satisfied -> downgraded -> failed`). Timeouts only
escalate phases that still have real waiters: if a phase already downgraded due to membership loss (`peer_draining` /
`peer_lost`), later timers leave it downgraded. Under the lock we ignore any attempt to move to a weaker state or to
reapply the same state. After completion emission the phase states are frozen and never upgrade or downgrade.

### Phase severity precedence

To make the monotonic escalation rule explicit, implementers should treat the following ordering as authoritative:

| Current phase state      | Event                                                                              | Next state                        | Notes                                                                                           |
|--------------------------|------------------------------------------------------------------------------------|-----------------------------------|-------------------------------------------------------------------------------------------------|
| `not_requested`          | Phase requested by the frozen mask                                                 | `satisfied` (phase activates)     | Phase becomes active with a satisfied baseline; later events may downgrade or fail it.          |
| `satisfied`              | Participant/reader departs before the phase finishes (registry marks graceful)     | `downgraded / peer_draining`      | Preserves visibility that the membership changed even if remaining work completes later.        |
| `satisfied`              | Participant/reader departs abruptly                                                | `downgraded / peer_lost`          | Same as above, but distinguishes abrupt loss.                                                   |
| `downgraded`             | All remaining obligations finish                                                   | `downgraded` (unchanged)          | Downgrade is sticky; successful completions do not upgrade.                                     |
| `satisfied`/`downgraded` | Phase timeout fires while obligations remain                                       | `failed / timeout`                | Escalate only when the waiter set is non-empty; rendezvous failure short-circuits later phases. |
| Any terminal state       | Late events (additional departures, late timers, duplicate completions)            | No change                         | Handlers **must** ignore attempts to weaken or reapply the phase outcome.                       |

This table restates the existing prose rules so implementers can audit their transition code paths without inferring
precedence from context.

## Phase aggregation

For each phase the coordinator records the strongest severity observed (`failed` > `downgraded` > `satisfied`) and only
ever escalates that value under the lock. Once a phase reaches its final state the value is immutable; repeated attempts
to mark it are ignored.

If every reader that was waiting in service mode enters `READER_NORMAL` before the grace deadline, the coordinator
disarms **`outbound_service_timer` (configured by `barrier_timeout_outbound_service`)** immediately and relies solely on
the main outbound timer.

## Timeouts and downgrades

Four independent timeouts guard the pipeline (`rendezvous_timer`, `outbound_service_timer`, `outbound_timer`,
`processing_timer`, each sourced from the similarly named `barrier_timeout_*` config knobs):

- `barrier_timeout_rendezvous` - armed at first arrival; disarmed when rendezvous becomes terminal.
- `barrier_timeout_outbound_service` - armed at rendezvous completion; readers that never report `READER_NORMAL` before
  this deadline cause the phase to fail.
- `barrier_timeout_outbound` - armed once every required reader is in `READER_NORMAL`; expiring while `outbound.waiters`
  is non-empty marks the phase failed.
- `barrier_timeout_processing` - armed when the last processing acknowledgement request is issued.

Timeout monitors run on lightweight helper threads that hold a weak reference to the barrier. When a timer arms, it
stores an immutable snapshot (`armed_epoch = barrier_epoch`, `armed_sequence = rendezvous_sequence` or
`invalid_sequence` pre-rendezvous) and bumps `armed_generation`. The helper thread captures `(expected_epoch,
expected_sequence, expected_generation)` before parking. On wake it locks the barrier exactly once, compares the stored
tuple with `(barrier_epoch, rendezvous_sequence, armed_generation)`, and if any element differs the monitor simply exits
because the timer was disarmed/re-armed in the meantime. If the phase is already terminal or the corresponding waiter
set is empty, the monitor disarms and returns. Otherwise it marks the phase `failed` with the appropriate failure code
(never reducing severity), disarms the timer (bumping `armed_generation`), and schedules a recheck on the coordinator
request loop. Only the coordinator request thread decides whether all phases are terminal and emits completions.

**Arm points:**

- `rendezvous_timer` - first valid arrival of the epoch.
- `outbound_service_timer` - rendezvous completion (before any reader arms).
- `outbound_timer` - immediately after the last required reader becomes `READER_NORMAL` (and all relevant readers are
  armed).
- `processing_timer` - once the final processing acknowledgement request is issued.

Late acknowledgements are ignored unless the coordinator still tracks `(barrier_name, barrier_epoch, barrier_sequence,
ack_type, instance_id, boot_id)`. They are logged when tracing is enabled. States never upgrade once they are `failed`
or `downgraded`.

## Membership changes

If a participant that still resides in `pending` departs before the rendezvous completes, the coordinator removes its
`(instance_id, boot_id)` from `pending`. When the registry simultaneously announces a replacement for the same
`instance_id`, the swap occurs without downgrading. Otherwise the rendezvous records `rendezvous = downgraded /
peer_lost` (or `peer_draining` when the registry sets the `graceful_departure` flag) to reflect the reduced membership
and continues waiting for the remaining arrivals. If the registry cannot discriminate, map every departure to
`peer_lost`. If the rendezvous timer eventually expires after such a downgrade, the state remains downgraded; the timer
simply disarms and finalises the barrier without escalating.

`drop_from_inflight_barriers()` removes a participant from outbound/processing waiters only after the rendezvous has
completed:

1. Remove the participant from `outbound.waiters` and `processing.waiters` if those phases were requested. Leave its
   `Delivery_target` entries intact until the barrier terminates so diagnostics and timer checks retain context. Targets
   are cleared only when the barrier instance is GC'd.
2. If a waiter set becomes empty because every delta/ack finished, the phase stays `satisfied`. If it becomes empty
   because the participant departed, mark the phase `downgraded` with `peer_lost` (or `peer_draining` when the registry
   flagged a graceful departure) and record the offender for tracing. If a timeout fires while the waiter set is
   non-empty, the phase becomes `failed / timeout` via the timer path (never downgraded).

This helper is the only code path that shrinks membership after the rendezvous starts, keeping the state machine easy to
audit.
After all phases reach a terminal state the coordinator clears `outbound.targets`, waiter sets, and any late-ack
tracking map for `(name, barrier_epoch, barrier_sequence, ack_type, iid, boot_id)`. Barrier instances are then eligible
for GC, and late acknowledgements arriving afterwards are dropped by the transport layer.

## Shutdown

When the coordinator shuts down it marks every in-flight barrier with `coordinator_stop`, finalises the template, and
emits completions from the request thread. Outstanding acknowledgement requests are abandoned; late responses are logged
and ignored.

Participants that observe the coordinator stop convert their local results to `coordinator_stop` before returning.

## Instrumentation

Setting `SINTRA_TRACE_BARRIER=1` enables detailed tracing:

- Rendezvous arrivals, mask decisions, and the frozen membership snapshot.
- Waiter set sizes and phase transitions.
- Coordinator delta waits for outbound plus timeout decisions.
- Processing ack requests/responses.
- Timeout firings together with the participants still outstanding.
- Late or duplicate acknowledgements that arrive after completion.

Operators should also export counters and histograms: per-phase `{satisfied, downgraded, failed}` counts keyed by
`failure_code`, duration histograms (`barrier_rendezvous_time_ms`, `barrier_outbound_time_ms`,
`barrier_processing_time_ms`), and gauges for outbound coverage decisions (e.g.,
`barrier_outbound_zero_reader_satisfied`, `barrier_outbound_downgraded_no_token`).

## API invariants

- `barrier_epoch` strictly increases for a given barrier name.
- `barrier_sequence` is valid (not `invalid_sequence`) if and only if the rendezvous completed.
- For successful participants in an epoch, `requirement_mask` is identical across every payload.

## Testing strategy

Add tests for:

 1. Mask mismatch - the **arrival** receives `incompatible_request` immediately (fast-fail payload).
 2. Happy paths for rendezvous, delivery fence, and processing fence.
 3. Outbound timeout marks the phase failed (`failure_code = timeout`).
 4. Processing acknowledgement timeout while other phases succeed.
 5. Participant leaving after rendezvous - outbound/processing downgrade visible in the payload (`peer_lost`) with the
    offender identity noted in tracing.
 6. Pre-rendezvous departure - rendezvous reports `downgraded / peer_lost` and never upgrades to `satisfied`.
 7. Participant with zero relevant readers - `covers_all_paths=true` satisfies immediately, `covers_all_paths=false`
    forces an immediate downgrade.
 8. Reader stuck in service mode vs. late-normal path - service timer fails if never NORMAL, main outbound timer arms
    only after the last reader becomes NORMAL.
 9. Timer snapshot correctness - arm/disarm each timer and prove stale monitors no-op when `armed_generation` or `(epoch,
    sequence)` change.
10. Late acknowledgement after timeout - logged/ignored, state unchanged.
11. Coordinator shutdown during each phase.
12. Fast-fail payload when the mask is still unset - ensure the payload carries the caller's mask rather than `~0`.
13. Participant restart mid-barrier or ACK from old boot_id - ensures `(iid, boot_id)` scoping rejects stale replies and
    the inline restart logic removes the stale waiter.
14. Unexpected arrivals - verify only the offending participant receives `incompatible_request` while the barrier
    proceeds.
15. Reader created after rendezvous capture without a continuity token - outbound downgrades for that participant
    immediately; with a matching token it arms successfully.
16. Rendezvous downgrade precedence - pre-rendezvous departure marks `rendezvous = downgraded`, and a later rendezvous
    timeout leaves it downgraded.
17. Timer fires after a participant already downgraded another phase - verify the phase stays `downgraded / peer_lost`
    and does not escalate.
18. Back-to-back barriers with the same name - epoch/sequence keep acknowledgements correctly partitioned and late ACKs
    from epoch *n* never satisfy epoch *n+1*.
19. Immediate outbound downgrade with zero waiters - every participant lacks coverage, so outbound has no waiters and
    finishes `downgraded`; timers disarm without reporting `satisfied`.
