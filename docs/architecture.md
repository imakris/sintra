# Sintra Architecture Overview

This document provides a comprehensive architectural overview of Sintra's internals, targeting developers and LLMs who need to understand, maintain, or extend the system.

## Table of Contents
1. [System Overview](#system-overview)
2. [Ring-Based IPC Layer](#ring-based-ipc-layer)
3. [Process Management](#process-management)
4. [Coordinator and Barriers](#coordinator-and-barriers)
5. [Draining and Shutdown](#draining-and-shutdown)
6. [Message Flow](#message-flow)
7. [Key Data Structures](#key-data-structures)

---

## System Overview

Sintra is a header-only C++ library for type-safe inter-process communication (IPC) using shared memory. It provides:

- **Signal/broadcast bus**: Pub/sub messaging across processes
- **Remote Procedure Calls (RPC)**: Synchronous cross-process function invocation
- **Barrier synchronization**: Coordinating process groups
- **Process lifecycle management**: Spawning, monitoring, and recovering processes

### Core Design Principles

1. **Shared-memory communication**: Memory-mapped files for cross-process data exchange
2. **Single-producer/multi-consumer (SPMC) rings**: Lock-free data structures for low-latency messaging
3. **Type-safe APIs**: Compile-time type checking prevents protocol errors
4. **Header-only**: No separate compilation required

---

## Ring-Based IPC Layer

### The Magic Ring Architecture

Sintra's IPC foundation is a **double-mapped circular buffer** ("magic ring"), which allows:
- Linear memory access even at buffer wrap-around points
- Lock-free single-producer/multiple-consumer communication
- Variable-sized messages with minimal overhead

#### Ring Components

```
┌────────────────────────────────────────────────────────────┐
│ Ring_data<T, READ_ONLY>                                    │
│ ┌────────────────────────────────────────────────────────┐ │
│ │ File: <name>_data                                      │ │
│ │ - Double-mapped shared memory region (2× mapping)      │ │
│ │ - Elements stored contiguously                         │ │
│ │ - Mapping trick: Same physical memory appears twice    │ │
│ │   at consecutive virtual addresses                     │ │
│ └────────────────────────────────────────────────────────┘ │
│                                                            │
│ Ring<T, READ_ONLY> : Ring_data<T, READ_ONLY>               │
│ ┌────────────────────────────────────────────────────────┐ │
│ │ File: <name>_data_control                              │ │
│ │ - Control block with atomics                           │ │
│ │ - leading_sequence: Published write head               │ │
│ │ - read_access: 8-byte octile guard bitmap              │ │
│ │ - reading_sequences[max_process_index]: Per-reader     │ │
│ │   sequence tracking and status                         │ │
│ │ - Semaphores for reader wakeup (hybrid policy)         │ │
│ └────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘

Ring_R<T> : Ring<T, true>    // Reader API
Ring_W<T> : Ring<T, false>   // Writer API
```

#### Key Ring Concepts

**Octiles**: The ring is divided into 8 equal sections. Readers hold guards on specific octiles to prevent the writer from overwriting data they're currently reading. Each octile has an 8-bit counter (part of the 64-bit `read_access` field), limiting concurrent readers per octile to 255.

**Sequence Numbers**: Each element written to the ring has a monotonically increasing sequence number. The `leading_sequence` atomic indicates the next element to be written (last published = `leading_sequence - 1`).

**Reader Slots**: Each Ring_R acquires a slot (index into `reading_sequences[]`) at construction, sized by `max_process_index`. Orphaned slots (from crashed processes) are reclaimed via `scavenge_orphans()`.

**Guard Protocol**:
1. Reader takes snapshot: Increments octile guard
2. Reader reads data from the snapshot
3. Reader calls `done_reading()`: Decrements octile guard
4. Writer blocks when entering a guarded octile

**Eviction** (when enabled): If a reader lags by more than one full ring behind the writer, the writer can forcefully evict it, clearing its guard and marking its slot as EVICTED.

#### Ring Helper Utilities

Sintra exposes small helpers to make ring usage less error-prone:

- `aligned_capacity()` and `get_ring_configurations()` compute element counts
  that satisfy the ring's alignment and mapping constraints.
- `Ring_R_snapshot` is an RAII wrapper for a single reader snapshot. Use
  `make_snapshot(reader, ...)` or `try_snapshot_e(reader, ...)` to pair
  `start_reading()` with `done_reading()` automatically.

### Double-Mapping Details

#### Windows
- Reserve 2x + granularity address space via `VirtualAlloc(MEM_RESERVE)`
- Round to granularity boundary (historical layout parity)
- Release reservation
- Map file twice back-to-back at the rounded address

#### Linux/POSIX
- Reserve 2x span with `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS)`
- Map file twice using `MAP_FIXED` to **replace** the reservation
- **Critical**: Use `MAP_FIXED`, **not** `MAP_FIXED_NOREPLACE` (the goal is to replace)

---

## Process Management

### Managed_process

The `Managed_process` class represents a single process in the Sintra system. Each process has:

#### Rings (Per-Process)

```cpp
Message_ring_W*  m_out_req_c;  // Outgoing request channel
Message_ring_W*  m_out_rep_c;  // Outgoing reply channel
```

**Important**: Each process has **two outgoing rings**:
- **Request ring** (`m_out_req_c`): For broadcast signals and initiating RPCs
- **Reply ring** (`m_out_rep_c`): For RPC responses and barrier completions

Other processes read from these rings via `Process_message_reader` instances.

#### Ring Topology

```
Process A (pid=1)                    Process B (pid=2)
┌─────────────────┐                 ┌─────────────────┐
│ m_out_req_c  ────────────────────>│  reader[A].req  │
│ m_out_rep_c  ────────────────────>│  reader[A].rep  │
│                 │                 │                 │
│ reader[B].req <─────────────────────  m_out_req_c   │
│ reader[B].rep <─────────────────────  m_out_rep_c   │
└─────────────────┘                 └─────────────────┘
```

Each process:
- **Writes** to its own `m_out_req_c` and `m_out_rep_c`
- **Reads** from all other processes via `m_readers` collection

#### Communication States

```cpp
enum Communication_state {
    COMMUNICATION_STOPPED,  // Ring threads are stopped
    COMMUNICATION_PAUSED,   // Rings answer only SERVICE_MODE messages
    COMMUNICATION_RUNNING   // Rings in NORMAL_MODE
};
```

**PAUSED** mode is critical for shutdown: readers continue processing, but only handle coordinator messages (enables graceful unpublish without deadlocks).

#### Key Methods

- `init()`: Initialize process from command-line arguments
- `branch()`: Spawn child processes
- `go()`: Start reader threads
- `pause()`: Switch to SERVICE_MODE (coordinator-only messages)
- `stop()`: Stop and join all reader threads
- `flush()`: Wait for a specific sequence to be visible
- `run_after_current_handler()`: Queue deferred task execution (avoids re-entrancy)

#### Lifeline ownership

Spawned processes receive a lifeline pipe/handle from the coordinator process.
The child reads from the lifeline in a dedicated thread; when the owner exits
or unpublishes, the pipe closes and the child:
- schedules shutdown (stop reader threads, unblock RPC),
- then hard-exits after a timeout (configurable via args or policy).

The handle value and timeout/exit code are passed via command-line arguments
(`--lifeline_handle`, `--lifeline_timeout_ms`, and `--lifeline_exit_code`).

The coordinator closes lifeline handles on unpublish and in the destructor so
stale children still terminate. Respawn creates a new lifeline per instance.

---

## Coordinator and Barriers

### Coordinator Structure

The `Coordinator` is a **special transceiver** that runs in process index 0 (the "starter" process). It provides:

- Type resolution (string -> type_id)
- Instance resolution (name -> instance_id)
- Transceiver registry
- **Process groups and barriers**
- **Draining state tracking**

### Barrier Mechanism

#### Process_group Structure

```cpp
struct Process_group : Derived_transceiver<Process_group> {
    unordered_map<string, Barrier>      m_barriers;
    unordered_set<instance_id_type>     m_process_ids;
    mutex                               m_call_mutex;

    struct Barrier {
        mutex                           m;
        condition_variable              cv;
        unordered_set<instance_id_type> processes_pending;
        unordered_set<instance_id_type> processes_arrived;
        instance_id_type                common_function_iid;
    };
};
```

#### Barrier Protocol

1. **Barrier Start** (first arrival):
   ```cpp
   // ATOMICALLY (while holding m_call_mutex):
   b.processes_pending = m_process_ids;
   // Filter out draining processes
   for (auto it = b.processes_pending.begin(); ...) {
       if (coord->is_process_draining(*it)) {
           it = b.processes_pending.erase(it);
       }
   }
   // Now unlock m_call_mutex
   ```

2. **Subsequent Arrivals**:
   - Mark caller as arrived
   - Remove caller from pending
   - If pending is empty -> barrier completes
   - Else -> defer (throw deferral with cleanup lambda)

3. **Barrier Completion**:
   - Last arrival emits completion messages to all waiters
   - Returns flush sequence (watermark)

**Return vs. Per-Recipient Tokens:** The barrier RPC returns a **single**
watermark from the coordinator's **reply ring** for the **caller**. Per-recipient
tokens are computed **inside** the coordinator's emit loop-one token **per
completion message**, at write time. Don't confuse the RPC's return value
(the single marker the caller must flush) with the per-recipient tokens
(embedded in each completion message).

#### Draining and Barriers

**CRITICAL INVARIANT**: A process marked as DRAINING must **never** be included in a barrier's `processes_pending` set, even if the barrier was already in progress.

```cpp
// Atomic draining state (lock-free reads)
std::array<std::atomic<uint8_t>, max_process_index + 1> m_draining_process_states;

bool is_process_draining(instance_id_type process_iid) const {
    const auto draining_index = get_process_index(process_iid);
    // ... bounds check ...
    return m_draining_process_states[slot].load(std::memory_order_acquire) != 0;
}
```

**Lock Hierarchy**: To prevent deadlocks:
```
m_publish_mutex -> m_groups_mutex -> m_call_mutex -> b.m -> atomics
```

---

## Draining and Shutdown

### Lifecycle States

1. **ACTIVE**: Process participates in barriers and normal communication
2. **DRAINING**: Process is shutting down; excluded from new barriers and dropped from in-flight barriers
3. **TERMINATED**: Process exited; resources scavenged

### Shutdown Sequence (sintra::finalize)

```
┌────────────────────────────────────────────────────────────┐
│ 1. Announce Draining                                       │
│    ┌─────────────────────────────────────────────────────┐ │
│    │ Coordinator::begin_process_draining(my_pid)         │ │
│    │ -> Sets m_draining_process_states[my_slot] = 1      │ │
│    │ -> Drops process from in-flight barriers            │ │
│    │ -> Returns flush sequence (reply ring watermark)    │ │
│    └─────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ 2. Flush Coordinator Channel                               │
│    ┌─────────────────────────────────────────────────────┐ │
│    │ flush(flush_sequence)                               │ │
│    │ -> Waits until coordinator's reply ring             │ │
│    │   sequence >= flush_sequence                        │ │
│    │ -> Guarantees all barrier completions are visible   │ │
│    └─────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ 3. Unpublish (Normal Communication Still Active)           │
│    ┌─────────────────────────────────────────────────────┐ │
│    │ unpublish_all_transceivers()                        │ │
│    │ -> Sends unpublish RPCs while communication works   │ │
│    │ -> Coordinator removes from registry                │ │
│    └─────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ 4. Pause Readers                                           │
│    ┌─────────────────────────────────────────────────────┐ │
│    │ pause()                                             │ │
│    │ -> Switches readers to SERVICE_MODE                 │ │
│    │ -> Only coordinator messages processed              │ │
│    └─────────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────┤
│ 5. Stop and Cleanup                                        │
│    ┌─────────────────────────────────────────────────────┐ │
│    │ stop()                                              │ │
│    │ -> Joins reader threads                             │ │
│    │ -> Releases resources                               │ │
│    └─────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘
```

> Implementation note:
> The simplified diagram above presents "Unpublish" before "Pause" for
> readability. In the current implementation (`detail::runtime::finalize`),
> non-coordinator processes first call `flush(process_of(s_coord_id),
> flush_sequence)` to synchronise with the coordinator, then switch readers
> to SERVICE_MODE via `pause()` before deactivating slots and invoking
> `unpublish_all_transceivers()`. The coordinator path additionally waits
> for all known processes to enter the draining state via
> `wait_for_all_draining()`. This tightened ordering avoids races with
> in-flight handlers while keeping the conceptual shutdown model above.

### Draining State Management

**CRITICAL**: Draining bit is **persistent across unpublish**:

```cpp
// In unpublish_transceiver (for Managed_process):
// 1. Mark as draining BEFORE removing from groups
m_draining_process_states[slot].store(1, std::memory_order_release);

// 2. Remove from all process groups
for (auto& group : m_groups) {
    group.second.remove_process(process_iid);
}

// 3. Do NOT reset draining bit here!
// It stays = 1 through teardown and is reset to 0 only when
// a NEW Managed_process is published into this slot (recovery).
```

**Rationale**: Resetting the bit too early re-opens a race where a dying process could be re-included in barriers between the reset and actual termination.

### Barrier Completion During Draining

When a process drains while a barrier is in-flight:

```cpp
// drop_from_inflight_barriers()
1. Lock m_call_mutex
2. For each barrier:
   - Remove draining process from both pending and arrived sets
   - If pending is now empty -> barrier completes immediately
3. Emit completions to remaining waiters
```

Completions are emitted via `run_after_current_handler` to avoid re-entrancy.

---

## Message Flow

### RPC Call Flow

```
Caller (Process A)                Coordinator           Callee (Process B)
     │                                   │                      │
     │ 1. rpc_foo(target, args)          │                      │
     ├─────────────────────────────────> │                      │
     │   Write to m_out_req_c            │                      │
     │   Common fiid = make_instance_id()│                      │
     │                                   │                      │
     │                                   │  2. Reader sees msg  │
     │                                   │ <────────────────────┤
     │                                   │   Dispatch to target │
     │                                   │                      │
     │                                   │ 3. Execute foo()     │
     │                                   │  <───────────────────┤
     │                                   │   Return value       │
     │                                   │                      │
     │ 4. Reply arrives                  │                      │
     │<─────────────────────────────────────────────────────────┤
     │   Reads from B's m_out_rep_c      │   Write to m_out_rep_c
     │   Match common_fiid               │                      │
     │                                   │                      │
     │ 5. Return to caller               │                      │
     └───                                │                      │
```

### Barrier Flow

```
Process A          Process B          Process C (Coordinator with group)
     │                   │                       │
     │ barrier("sync")   │                       │
     ├─────────────────────────────────────────> │
     │  RPC call         │                       │ First arrival:
     │                   │                       │ - Snapshot membership
     │                   │                       │ - Filter draining
     │                  barrier("sync")          │ - Create pending set
     │                   ├─────────────────────> │
     │                   │                       │ Subsequent arrival:
     │                   │                       │ - Add to arrived
     │                   │                       │ - Remove from pending
     │                   │                       │
     │                   │                       │ Last arrival:
     │                   │                       │ - Emit completions
     │   <completion msg>                        │   to all waiters
     │<──────────────────────────────────────────┤
     │                  <completion msg>         │
     │                   │<──────────────────────┤
     │                   │                       │
     │ Returns with      │ Returns with          │
     │ flush_seq         │ flush_seq             │
     └──                 └──                     └──
```

**Flush Sequence**: The watermark returned from a barrier tells the caller which sequence to wait for before proceeding. This ensures all messages (including the barrier completion itself) are visible.

---

## Key Data Structures

### Instance IDs

```cpp
using instance_id_type = uint64_t;
constexpr int  num_process_index_bits     = sintra::num_process_index_bits;     // 8 by default
constexpr int  num_transceiver_index_bits = sizeof(instance_id_type) * 8 - num_process_index_bits;
constexpr int  max_process_index          = sintra::max_process_index;          // 127 with defaults
constexpr auto max_instance_index         = sintra::max_instance_index;

inline instance_id_type make_instance_id() {
    static std::atomic<uint64_t> instance_index_counter(2 + sintra::num_reserved_service_instances);
    assert(instance_index_counter.load(std::memory_order_relaxed) < max_instance_index);
    auto d = sintra::decompose_instance(s_mproc_id);
    const auto index = static_cast<uint64_t>(instance_index_counter++);
    return sintra::compose_instance(d.process, index);
}
```

* The **upper** `num_process_index_bits` encode the process slot together with complement/wildcard flags. The top bit is
  reserved for the complement encoding, which is why `max_process_index` is smaller than `2^(num_process_index_bits)`.

## Process Recovery

1. A worker opts into crash recovery by calling `sintra::enable_recovery()`, which forwards the request to the
   coordinator (`Managed_process::enable_recovery`). The coordinator records the process slot in
   `Coordinator::m_requested_recovery`, so a later abnormal termination routes through `Coordinator::recover_if_required()`
   instead of being treated as a permanent shutdown.
2. `Managed_process::spawn_swarm_process()` caches the executable + argument vector for each spawned child in
   `m_cached_spawns` and increments a per-process occurrence counter. The counter is appended to the `req`/`rep` ring
   filenames (`Message_ring_{R,W}::get_base_filename`) so every recovery attempt attaches to a fresh pair of shared-memory
   files while previous rings remain available for post-mortem inspection.
3. Before launching the replacement child the coordinator spins up new `Process_message_reader` instances for the target
   process, ensuring its request/reply channels are mapped and ready the moment the binary starts executing.
4. We dismantle the previous occurrence's plumbing before a respawn. `spawn_swarm_process()` explicitly erases any
   existing `Process_message_reader` for the crashing slot, which drops the shared_ptr references and unmaps the old
   occurrence's rings before new readers are created. Only the currently active occurrence consumes address space; there is
   no "bank" of pre-reserved mappings for future recoveries.
5. Each IPC ring is double-mapped (`Ring_data::attach`) by reserving a 2 x span and mapping the 2 MiB data file twice. The
   contiguous view is what makes zero-copy wrap-around reads work; it is unrelated to recovery itself but explains the
   large "guard"/reserved ranges seen in macOS cores. Every request/reply channel therefore claims ~4 MiB of virtual
   address space. Prior to 2025-03 we saw ~4.24 GiB logical cores in CI because those spans appeared alongside the
   platform's 4 GiB `__PAGEZERO` reservation. On Linux we call `madvise(..., MADV_DONTDUMP)` on both the data and control
   mappings so the rings are excluded from future cores by default; stack/code/heap pages still participate, so stack
   traces remain intact while the dumps shrink to the tens-of-megabytes range. macOS does not expose `MADV_DONTDUMP`, so
   the recovery test itself sets `RLIMIT_CORE` to zero just before its intentional `std::abort()` to keep GitHub Actions
   runners from filling their disks with multi-gigabyte Mach-O cores.

**What "safe respawn" means**: by pre-mapping the request/reply readers before `spawn_detached()` the coordinator guarantees
that the recovering child sees ready-to-use channels as soon as it reaches user code. No address-space layout promises are
required beyond the double-mapped rings themselves, and the respawned process inherits only the fresh occurrence's mappings.

**How many rings stay mapped during `recovery_test`?** The watchdog, coordinator, and crasher each hold two outgoing rings
(`Message_ring_W`) plus request/reply readers for every other live process. At the point where the crasher aborts that
amounts to a few dozen 4 MiB spans in total-on the order of 250 MiB of virtual address space. Those ranges used to inflate
macOS cores to ~4.24 GiB (4 GiB `__PAGEZERO` + ~0.25 GiB of double-mapped rings + stacks/segments). Linux skips the ring
spans thanks to `MADV_DONTDUMP`; macOS still records them, which is why the recovery test suppresses its intentional core
file entirely.
* The **lower** `num_transceiver_index_bits` track the transceiver within that process slot. Index `1` is reserved for the
  `Managed_process` itself; other transceivers start at `2`.
* Convenience constants (e.g. `any_local`, `any_remote`, `any_local_or_remote`) and helpers such as `compose_instance()` and
  `decompose_instance()` provide ergonomic access to this bit layout.
* `s_mproc_id` expands to the current process's sentinel instance ID via the runtime state helpers in `detail/globals.h`,
  ensuring per-process counters stay anchored to the right slot.

**Special IDs**:
- `invalid_instance_id = 0`
- `make_service_instance_id()` - allocates service IDs from the reserved range attached to the local process slot
- `make_process_instance_id()` - produces the per-process sentinel (`transceiver == 1`) used for membership tracking

### Type IDs

```cpp
using type_id_type = uint64_t;

// Resolved at runtime via coordinator
type_id_type resolve_type(const string& pretty_name);
```

### Deferred Functions (Barriers)

Barriers use C++ exceptions to implement "deferred returns":

```cpp
struct deferral {
    instance_id_type new_fiid;  // Common function ID for this deferral
};

// Throw a pair: deferral + cleanup lambda
throw std::pair<deferral, function<void()>>{
    {.new_fiid = barrier_fiid},
    [&](){ /* unlock barrier.m */ }
};
```

The RPC dispatcher catches these, stores the caller's continuation, and resumes when the barrier completes.

### Handler Registry

```cpp
using handler_registry_type = /* complex nested maps */;
```

Maps `(type_id, handler_type)` -> handler functions. Handlers are activated with `activate_slot<T>(lambda)`.

---

## Per-Recipient Flush Tokens (CRITICAL for Correctness)

### The Problem

**Before Fix**: The coordinator used a single global watermark:
```cpp
// WRONG: Same token for all recipients
const auto flush_seq = s_mproc->m_out_rep_c->get_leading_sequence();
for (auto recipient : recipients) {
    // Embed flush_seq in message to recipient
}
```

**Issue**: `m_out_rep_c->get_leading_sequence()` is the watermark of the **coordinator's** reply ring. But each **recipient** reads from their **own** ring, not the coordinator's. Using a global token can cause:
- Tokens ahead of a recipient's channel -> indefinite wait (hang)
- Tokens from wrong channel -> delivery guarantee violations

### The Solution

**Per-Recipient Watermarks at Write Time**: Compute a flush token **per recipient** at message write time:

```cpp
// CORRECT: Token per recipient computed at write time
for (auto recipient : completion.recipients) {
    // Get watermark JUST BEFORE writing this recipient's message
    const auto flush_seq = s_mproc->m_out_rep_c->get_leading_sequence();

    auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
        vb_size<return_message_type>(flush_seq), flush_seq);

    Transceiver::finalize_rpc_write(
        placed_msg, recipient, completion.common_function_iid, this, ...);
}
```

**Key Insight**: Even though all messages go through `m_out_rep_c`, each call to `write()` advances the `leading_sequence`, so taking the watermark **inside** the loop gives each recipient a token that's valid for **their specific message write time**.

**Note on Return Value**: The barrier RPC itself returns a **single flush token** (the one embedded in the completion message sent to the caller). The "per-recipient" aspect applies to the **completion messages sent to all waiters**, not to the return value. Each waiter receives a token computed specifically for their message write time.

**Future Enhancement**: If Sintra evolves to have per-recipient egress rings (instead of a single shared reply ring), the fix would need `s_mproc->rep_watermark_to(recipient)` to query the specific ring for that recipient.

---

## References

- **Ring Implementation**: `include/sintra/detail/ipc_rings.h`
- **Process Management**: `include/sintra/detail/managed_process.h`
- **Coordinator**: `include/sintra/detail/coordinator.h`, `coordinator_impl.h`
- **Shutdown Semantics**: `docs/barriers_and_shutdown.md`
- **Stress/Timeout Harness**: `tests/run_tests.py`

---

**Version**: 2024-05-01 (Barrier flush + recovery documentation refresh)
**Maintainer**: Ioannis Makris
