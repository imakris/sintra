# Sintra Guide

This guide explains Sintra's mental model, cross-cutting lifecycle and
threading rules, and the compiled examples or tests that demonstrate common
usage patterns. The per-symbol API reference lives in `docs/reference/`.

Use these public includes:

```cpp
#include <sintra/sintra.h>  // managed processes, pub/sub, RPC, barriers, lifecycle
#include <sintra/rings.h>   // direct ring helpers
```

Do not include `sintra/detail/...` headers from application code.

For one-page-per-API lookup, start with the
[API Reference](reference/index.md). This document stays guide-shaped: it
explains concepts, recipes, and relationships between APIs.

To view the reference as a local website:

```sh
python scripts/build_reference_site.py
python -m http.server 8000 --directory docs/reference_site
```

Then open `http://localhost:8000/`.

## Contents

1. [Overview](#overview)
2. [Minimum Working Program](#minimum-working-program)
3. [Terminology](#terminology)
4. [Initialization and Process Topology](#initialization-and-process-topology)
5. [Transceivers](#transceivers)
6. [Messages and Payloads](#messages-and-payloads)
7. [Publish/Subscribe](#publishsubscribe)
8. [RPC](#rpc)
9. [Targeting and IDs](#targeting-and-ids)
10. [Barriers and Fences](#barriers-and-fences)
11. [Lifecycle and Shutdown](#lifecycle-and-shutdown)
12. [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks)
13. [Direct Ring Helpers](#direct-ring-helpers)
14. [Errors and Diagnostics](#errors-and-diagnostics)
15. [Threading and Reentrancy](#threading-and-reentrancy)
16. [Type IDs](#type-ids)
17. [CMake Integration](#cmake-integration)
18. [Recipes](#recipes)
19. [Common Mistakes](#common-mistakes)
20. [API Index](#api-index)

## Overview

Sintra is a C++20 header-only library for single-host interprocess
communication. The high-level API creates a swarm of managed processes that
can exchange typed messages, register slots, call typed RPC methods, coordinate
with barriers, and shut down as a group. The low-level ring API exposes the
shared-memory ring primitives directly for code that wants rings without the
managed-process layer.

Typical high-level shape:

1. Declare message and transceiver types.
2. Call `sintra::init(argc, argv, branches...)`.
3. Run branch-specific logic selected by `sintra::process_index()`.
4. Use `world()`, `local()`, `remote()`, slots, RPC, and barriers for
   communication.
5. Call `sintra::shutdown()` when all live participants finish together, or
   `sintra::leave()` when this process intentionally departs while peers
   continue.

Compiled examples live in [`example/sintra/`](../example/sintra/). Tests in
[`tests/`](../tests/) cover edge cases and guardrails.

## Minimum Working Program

Start from
[`sintra_example_9_minimal_pubsub.cpp`](../example/sintra/sintra_example_9_minimal_pubsub.cpp)
when you need the smallest complete publish/subscribe shape:

```cpp
#include <sintra/sintra.h>

struct Market_tick
{
    int    instrument_id;
    double last_price;
};

int sender_branch()
{
    sintra::barrier("market-tick-ready");
    sintra::world() << Market_tick{17, 101.25};
    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");
    return 0;
}

int receiver_branch()
{
    sintra::activate_slot([](const Market_tick& tick) {
        sintra::console()
            << "instrument " << tick.instrument_id
            << " last price " << tick.last_price << '\n';
    });

    sintra::barrier("market-tick-ready");
    sintra::barrier<sintra::processing_fence_t>("market-tick-processed");
    return 0;
}

int main(int argc, char* argv[])
{
    sintra::init(argc, argv, sender_branch, receiver_branch);
    sintra::shutdown();
    return 0;
}
```

The structural rules are:
- The payload type is a plain message value; see
  [Message payloads](reference/message_payloads.md) for allowed field types.
- Every branch passed to `init` becomes one managed process.
- Receivers activate slots before the sender publishes; the first barrier is
  the ready handshake.
- The `processing_fence_t` barrier is used when the sender must know that
  receiver handler side effects have completed before shutdown.
- Every live finishing participant reaches `shutdown()` through the return
  from `init`.

Minimal CMake target once the Sintra target is available:

```cmake
add_executable(my_sintra_app main.cpp)
target_link_libraries(my_sintra_app PRIVATE sintra::sintra)
```

## Terminology

**Swarm**: The set of Sintra-managed processes participating in one runtime.

**Coordinator**: The starter process that tracks process membership, type
resolution, instance naming, lifecycle state, and barriers.

**Managed process**: A process initialized by Sintra. Every managed process has
request/reply channels and participates in the runtime lifecycle.

**Transceiver**: A typed object that can publish messages and export member
functions as RPC endpoints. User transceivers derive from
`sintra::Derived_transceiver<T>`.

**Instance ID**: A `sintra::instance_id_type` that identifies a transceiver or
a wildcard target. Its high bits identify a process slot and its low bits
identify a transceiver within that process.

**Typed instance ID**: `sintra::Typed_instance_id<T>`, a typed wrapper around an
instance id used for sender filtering and target clarity.

**Named instance**: `sintra::Named_instance<T>`, a string wrapper used when a
transceiver is addressed by name.

**Resolvable instance ID**: `sintra::Resolvable_instance_id`, the generated RPC
target parameter. It accepts a raw `instance_id_type`, `std::string`, or
`const char*`.

**Message type ID**: A `sintra::type_id_type` assigned automatically or pinned
with explicit type-id macros.

**Local delivery**: Delivery to handlers in the same process.

**Remote delivery**: Delivery to handlers in other processes.

**World delivery**: Delivery to local and remote handlers.

**Request channel**: A process's outgoing channel for broadcasts and RPC
requests.

**Reply channel**: A process's outgoing channel for RPC replies and barrier
completions.

**Reader thread**: A Sintra-owned thread that reads incoming messages and
invokes handlers.

**Handler / slot**: User callback registered to consume a message.

**Barrier**: A named synchronization point coordinated through the swarm.

**Lifecycle**: Initialization, normal shutdown, unilateral departure, crash
observation, recovery, and teardown state.

**`sintra::detail`**: Implementation namespace. Application code should not
depend on it.

## Initialization and Process Topology

### `sintra::init`

Signatures:

```cpp
void init(
    int argc,
    const char* const* argv,
    std::vector<Process_descriptor> branches = {});

template <typename... Args>
void init(int argc, const char* const* argv, Args&&... args);
```

`init` initializes the current process as part of a Sintra swarm. The
vector-based overload accepts a prepared branch list. The variadic overload
forwards through `make_branches(...)`.

Preconditions:
- Call once before using high-level Sintra IPC.
- Pass the original process arguments.
- Use `shutdown()`, `leave()`, or deliberate low-level teardown before starting
  another init/teardown cycle in the same process.

Failures:
- Throws `sintra::init_error` when child process initialization fails.
- Throws `std::runtime_error` on invalid lifecycle composition.

Compiled examples:
- [`example/sintra/sintra_example_0_basic_pubsub.cpp`](../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [`example/sintra/sintra_example_2_rpc_append.cpp`](../example/sintra/sintra_example_2_rpc_append.cpp)

### `sintra::make_branches`

Signatures:

```cpp
std::vector<Process_descriptor> make_branches();
std::vector<Process_descriptor> make_branches(std::vector<Process_descriptor>& branches);

template <typename... Args>
std::vector<Process_descriptor> make_branches(const Process_descriptor& descriptor, Args&&... rest);

template <typename... Args>
std::vector<Process_descriptor> make_branches(int multiplicity, const Process_descriptor& descriptor, Args&&... rest);
```

`make_branches` builds the branch vector consumed by `init`. The multiplicity
form repeats a process descriptor.

### `sintra::Entry_descriptor`

Constructors:

```cpp
Entry_descriptor(const std::string& binary_name);
Entry_descriptor(int (*entry_function)());
```

`Entry_descriptor` selects how a branch starts: from a binary path or an
in-process entry function.

### `sintra::Process_descriptor`

Relevant shape:

```cpp
struct Process_descriptor
{
    Entry_descriptor entry;
    std::vector<std::string> sintra_options;
    std::vector<std::string> user_options;
    instance_id_type assigned_instance_id = invalid_instance_id;
};
```

`Process_descriptor` owns the `Entry_descriptor` entry and optional user
arguments. It is the normal unit passed to `init` and `make_branches`.

### `sintra::Spawn_options`

Relevant shape:

```cpp
struct Spawn_options
{
    std::string binary_path;
    std::vector<std::string> args;
    std::vector<std::string> env_overrides;
    instance_id_type process_instance_id = invalid_instance_id;
    std::string readiness_instance_name;
    Lifetime_policy lifetime;
};
```

`Spawn_options` configures dynamic process spawning through
`spawn_swarm_process`. `env_overrides` is merged with the inherited environment
in order, with the later duplicate winning. Environment names are matched
case-insensitively on Windows and case-sensitively on POSIX. Recovery
occurrences reuse the same overrides. Entries are not syntax-validated, so
provide them in `NAME=VALUE` form and do not rely on malformed input.

### `sintra::Lifetime_policy`

Relevant shape:

```cpp
struct Lifetime_policy
{
    bool enable_lifeline = true;
    int hard_exit_code = 99;
    int hard_exit_timeout_ms = 100;
};
```

Spawned processes receive a lifeline by default. When the owner disappears, the
child initiates shutdown and hard-exits after the configured timeout.

### `sintra::spawn_swarm_process`

Signature:

```cpp
Managed_child_custody spawn_swarm_process(const Spawn_options& options);
```

This accepts durable custody before authorizing OS creation. Test the returned
handle for acceptance, then use its deadline-based readiness, release, and
termination operations. Readiness waits are observation-only; `release_until()`
is passive, while `terminate_until()` requests adverse cleanup. Expired
deadlines return confirmed status without discarding custody.

`observe_latest_created_exit()` binds once to the latest OS-created occurrence
and never follows a replacement. Its identity is the exact
`(process_instance_id, occurrence, custody_identity)` triple; a fresh custody
starts at occurrence `0`. Late registration still delivers once while the
owning runtime is active. Keep the returned move-only subscription alive for
delivery, and do not initiate Sintra teardown from its lifecycle-thread
callback.

`detach_until(deadline)` is the explicit survival handoff. A successful result
of `Managed_child_detach_result::disowned` closes recovery, releases the
lifeline, and permanently relinquishes native exit and termination authority
without claiming that the child exited. Later release or termination calls on
that custody report `disowned` and do not kill or wait for the child.

The complete status, failure, exact-exit, callback, cancellation, quiescence,
threading, and teardown contract is in
[`sintra::spawn_swarm_process`](reference/spawn_swarm_process.md).

### `sintra::join_swarm`

Signature:

```cpp
instance_id_type join_swarm(int32_t branch_index, const std::string& binary_name = {});
```

Requests an additional process for an existing branch index. The coordinator
serializes join requests for the same branch index.

Compiled tests:
- [`tests/join_swarm_midflight_test.cpp`](../tests/join_swarm_midflight_test.cpp)
- [`tests/join_swarm_failure_test.cpp`](../tests/join_swarm_failure_test.cpp)

### `sintra::create_external_process_invitation`

Relevant shape:

```cpp
struct External_process_invitation_options
{
    instance_id_type           process_instance_id = invalid_instance_id;
    std::chrono::milliseconds  timeout{std::chrono::seconds(30)};
    bool                       detached = false;
};

struct External_process_invitation
{
    instance_id_type process_instance_id = invalid_instance_id;
    uint32_t occurrence = 0;
    bool valid() const;
    explicit operator bool() const;
    std::vector<std::string> sintra_args() const;
};

External_process_invitation create_external_process_invitation(
    const External_process_invitation_options& options = {});

bool cancel_external_process_invitation(
    instance_id_type process_instance_id);

bool cancel_external_process_invitation(
    const External_process_invitation& invitation);
```

External process invitations let the coordinator pre-admit a process that will
be launched by application code, a shell, a debugger, or another supervisor.
The single-use token and monotonically assigned invitation occurrence bind the
claim to one exact reader generation. Keep the token secret. Canceled, expired,
and retired generations are not reusable until their reader is quiescent.
Externally attached processes may leave, but they are never recovered by
Sintra. See
[`sintra::create_external_process_invitation`](reference/external_process_invitation.md)
for the complete contract.

The default invitation remains coordinator-bound. Set `detached = true` only
when the host will service member lifecycle events and call `leave()` from its
serialized control thread. Detached admission requires an exact live
coordinator PID and start identity, never a PID-only fallback. Detached members
are notified and excluded before collective shutdown takes its
lifecycle-barrier snapshot.

### `sintra::process_index`

Signature:

```cpp
int process_index();
```

Returns the branch index for the current process. The starter process is index
`0`; spawned branches are positive.

### `sintra::disable_debug_pause_for_current_process`

Signature:

```cpp
void disable_debug_pause_for_current_process() noexcept;
```

Disables Sintra's debug pause behavior in the current process.

## Transceivers

### `sintra::Transceiver`

`Transceiver` is the base runtime object that owns instance identity, handler
registrations, message emission, and RPC export plumbing. User code normally
derives through `Derived_transceiver`.

Important public members:

```cpp
instance_id_type instance_id();
bool assign_name(const std::string& name);
void destroy();
void deactivate_all();
```

### `sintra::Derived_transceiver`

Signatures:

```cpp
template <typename Derived_T, typename Parent = void>
struct Derived_transceiver;
```

Use `Derived_transceiver<T>` as the base for a user transceiver. It provides
typed message emission and generated RPC support.

Typed message emission:

```cpp
template <typename MESSAGE_T, typename SENDER_T = Transceiver_type, typename... Args>
void emit_local(Args&&... args);

template <typename MESSAGE_T, typename SENDER_T = Transceiver_type, typename... Args>
void emit_remote(Args&&... args);

template <typename MESSAGE_T, typename SENDER_T = Transceiver_type, typename... Args>
void emit_global(Args&&... args);
```

Use the maildrop helpers (`world()`, `local()`, `remote()`) for simple value
broadcasts where the wrapper message type is not important.

### Transceiver Macros

User-facing macros:

| Macro | Purpose |
| --- | --- |
| `SINTRA_TYPE_ID(idv)` | Pins the transceiver type id. |
| `SINTRA_MESSAGE(name, ...)` | Declares a typed message under a transceiver. |
| `SINTRA_MESSAGE_EXPLICIT(name, idv, ...)` | Declares a typed message with a pinned user id. |
| `SINTRA_RPC(method)` | Exports a member function for blocking and async RPC. Same-process blocking calls may use a direct shortcut. |
| `SINTRA_RPC_STRICT(method)` | Exports a member function through the transported RPC path for local and remote targets. |
| `SINTRA_UNICAST(method)` | Exports a void fire-and-forget targeted message. |

Internal macros that application code should not use:

| Macro | Reason |
| --- | --- |
| `SINTRA_RPC_EXPLICIT` | Uses reserved internal ids. |
| `SINTRA_RPC_STRICT_EXPLICIT` | Uses reserved internal ids. |
| `SINTRA_MESSAGE_BASE` | Macro implementation detail. |
| `SINTRA_MESSAGE_RESERVED` | Reserved internal messages. |

RPC exported functions must not return references. Non-const reference
arguments are rejected. Return values and mutable results should be represented
as values or explicit messages.

Compiled examples:
- [`example/sintra/sintra_example_2_rpc_append.cpp`](../example/sintra/sintra_example_2_rpc_append.cpp)
- [`example/sintra/sintra_example_6_unicast_send_to.cpp`](../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [`example/sintra/sintra_example_7_explicit_type_ids.cpp`](../example/sintra/sintra_example_7_explicit_type_ids.cpp)

## Messages and Payloads

Sintra accepts plain C++ values through the maildrop helpers and transceiver
message types declared by `SINTRA_MESSAGE` or `SINTRA_MESSAGE_EXPLICIT`.
RPC arguments and return values use the same message serialiser. The full
contract is in [Message payloads](reference/message_payloads.md).

### Payload Contract

For user-defined structs sent as plain values, keep the type trivial and
standard-layout:

```cpp
#include <type_traits>

struct Tick
{
    int    symbol;
    double price;
};

static_assert(std::is_trivial_v<Tick>);
static_assert(std::is_standard_layout_v<Tick>);
```

Top-level maildrop values and RPC arguments may also be variable-buffer
compatible values such as `std::string` or `std::vector<T>` where `T` is
trivially copyable. If variable-size data is stored as a field inside a
`SINTRA_MESSAGE` declaration, use the Sintra field wrappers such as
`sintra::message_string` or `sintra::typed_variable_buffer<std::vector<T>>`.

### Plain Values

`world() << value`, `local() << value`, and `remote() << value` wrap values in
Sintra message envelopes automatically.

```cpp
struct Tick
{
    int    symbol;
    double price;
};

sintra::world() << Tick{1, 100.0};

sintra::activate_slot([](const Tick& tick) {
    sintra::console() << tick.symbol << " @ " << tick.price << '\n';
});
```

Maildrop helpers also accept `std::string`, C strings, string literals,
`std::vector<T>`, and fixed arrays through the conversion paths documented in
[`sintra::Maildrop`](reference/maildrop.md).

### Transceiver Message Types

`SINTRA_MESSAGE(name, fields...)` creates a message type nested in the
transceiver. Send it with `emit_local`, `emit_remote`, or `emit_global`.

```cpp
struct Feed : sintra::Derived_transceiver<Feed>
{
    SINTRA_MESSAGE(
        tick,
        int instrument_id,
        sintra::message_string venue
    )
};
```

### Variable-Size Fields

Advanced message payload types:

| Type | Use |
| --- | --- |
| `sintra::variable_buffer` | Ring-buffer resident variable bytes. |
| `sintra::typed_variable_buffer<T>` | Typed variable-size payload with alignment checks. |
| `sintra::message_string` | String payload carried through the message layer. |

Variable-buffer backed data is tied to message storage. Copy the data out before
the handler or receive scope ends if it must outlive that message.

`sintra::detail::message_args` is an internal implementation detail. It may
appear in template diagnostics, but application code should not name it.

## Publish/Subscribe

### Maildrop Helpers

Signatures:

```cpp
Maildrop<any_local>& local();
Maildrop<any_remote>& remote();
Maildrop<any_local_or_remote>& world();
```

Use:
- `local()` to deliver in the current process.
- `remote()` to deliver to other processes.
- `world()` to deliver to local and remote recipients.

### `sintra::activate_slot`

Signature:

```cpp
template <typename FT, typename SENDER_T = void>
auto activate_slot(
    const FT& slot_function,
    Typed_instance_id<SENDER_T> sender_id = Typed_instance_id<void>(any_local_or_remote));
```

Registers a handler for the message type accepted by `slot_function`. The
optional sender id filters which sender may trigger the slot.

Slot handlers run on Sintra reader threads. Protect shared state with normal
C++ synchronization. See [Threading and Reentrancy](#threading-and-reentrancy).

### `sintra::deactivate_all_slots`

Signature:

```cpp
void deactivate_all_slots();
```

Deactivates all slots owned by the current managed process.

### `sintra::receive`

Signatures:

```cpp
template <typename MESSAGE_T>
MESSAGE_T receive();

template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T receive(Typed_instance_id<SENDER_T> sender_id);
```

Blocks a control thread until a matching message arrives. Do not call
`receive<T>()` from a message handler or post-handler callback.

Compiled examples/tests:
- [`example/sintra/sintra_example_0_basic_pubsub.cpp`](../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [`tests/receive_test.cpp`](../tests/receive_test.cpp)

## RPC

`SINTRA_RPC(method)` and `SINTRA_RPC_STRICT(method)` generate:

```cpp
template <typename... Args>
static auto rpc_method(sintra::Resolvable_instance_id instance_id, Args&&... args);

template <typename... Args>
static auto rpc_async_method(sintra::Resolvable_instance_id instance_id, Args&&... args);
```

`SINTRA_UNICAST(method)` generates the same `rpc_method` form, but it is
fire-and-forget and available only for void-returning functions. It does not
generate a usable async API.

### Targets

Generated RPC target parameters use `sintra::Resolvable_instance_id`, which
accepts:
- a raw `sintra::instance_id_type`
- a `std::string`
- a `const char*`

Names resolve through the coordinator. Raw ids are appropriate after a process
has exchanged ids explicitly.

### Blocking RPC

`rpc_<method>(...)` blocks until the reply, remote exception, cancellation, or
teardown outcome is known.

Remote exceptions are propagated back to the caller. Exception conversion covers
standard exception families and unknown exceptions.

### Async RPC

`rpc_async_<method>(...)` returns `sintra::Rpc_handle<T>`.

Relevant API:

```cpp
namespace sintra {

struct rpc_timeout : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

template <typename R>
class Rpc_handle
{
public:
    Rpc_handle();
    Rpc_handle(const Rpc_handle&) = delete;
    Rpc_handle(Rpc_handle&&) noexcept = default;
    Rpc_handle& operator=(const Rpc_handle&) = delete;
    Rpc_handle& operator=(Rpc_handle&& other) noexcept;
    ~Rpc_handle();

    template <typename Clock, typename Duration>
    auto get_until(
        const std::chrono::time_point<Clock, Duration>& deadline) -> R;

    auto get() const -> R;
};

} // namespace sintra
```

Use `get_until(deadline)` for normal bounded result retrieval. It returns the
value, rethrows the remote exception or cancellation, or throws `rpc_timeout`
when the deadline expires and caller-side abandon wins. Deadline expiry does
not cancel remote execution; abandoned late replies are discarded. Destroying
a still-pending handle also abandons caller-side interest without cancelling
remote execution.

Use `SINTRA_RPC_STRICT` when the local target must use the same transported path
as remote targets. This matters for local async-handle behavior.

Compiled examples/tests:
- [`example/sintra/sintra_example_2_rpc_append.cpp`](../example/sintra/sintra_example_2_rpc_append.cpp)
- [`tests/rpc_bounded_result_test.cpp`](../tests/rpc_bounded_result_test.cpp)
- [`tests/rpc_async_lifecycle_test.cpp`](../tests/rpc_async_lifecycle_test.cpp)
- [`tests/finalize_async_rpc_lifecycle_test.cpp`](../tests/finalize_async_rpc_lifecycle_test.cpp)

## Targeting and IDs

### Core ID Types

| Name | Meaning |
| --- | --- |
| `sintra::instance_id_type` | Raw transceiver/process target id. |
| `sintra::type_id_type` | Message or transceiver type id. |
| `sintra::sequence_counter_type` | Ring/barrier sequence watermark type. |
| `sintra::Typed_instance_id<T>` | Type-tagged instance id wrapper. |
| `sintra::Named_instance<T>` | Name wrapper for typed transceiver lookup. |
| `sintra::Resolvable_instance_id` | Generated RPC target conversion type. |

### Wildcards and Sentinels

| Name | Meaning |
| --- | --- |
| `sintra::any_local` | Local recipients. |
| `sintra::any_remote` | Remote recipients. |
| `sintra::any_local_or_remote` | Local and remote recipients. |
| `sintra::invalid_instance_id` | No valid instance. |
| `sintra::invalid_sequence` | No valid sequence; also used when a barrier is treated as satisfied during shutdown/drain handling. |

### Helpers

```cpp
decomposed_instance_id decompose_instance(instance_id_type instance) noexcept;
instance_id_type compose_instance(uint32_t process, uint64_t transceiver) noexcept;
uint64_t get_process_index(instance_id_type instance_id);
instance_id_type process_of(instance_id_type iid);
```

Use named instances where they make the target contract clear, and use typed
wrappers where they make sender filters clear. Use raw ids when ids have been
exchanged as data.
`process_of(iid)` returns the managed-process sentinel instance for the process
that owns `iid`.

### When to Name an Instance

- Name a transceiver with `assign_name(...)` when callers should reach it by a
  stable string known from code or configuration. String RPC targets resolve
  through the coordinator; an unknown name resolves to `invalid_instance_id`.
- Use the raw `instance_id()` when the id can be exchanged at runtime, for
  example through a broadcast, RPC return, or configuration message. This avoids
  a coordinator name lookup at the call site.
- Use `Typed_instance_id<T>` when filtering received messages by a typed sender.

## Barriers and Fences

Tags:

```cpp
struct rendezvous_t {};
struct delivery_fence_t {};
struct processing_fence_t {};
```

API:

```cpp
template<typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(
    const std::string& barrier_name,
    const std::string& group_name = "_sintra_external_processes");

```

Barrier modes:

| Mode | Contract |
| --- | --- |
| `rendezvous_t` | Every participant reaches the point. Pre-barrier messages may still be in flight. |
| `delivery_fence_t` | Default. Pre-barrier messages have been fetched by local readers and queued for handling. Handlers may still be running. |
| `processing_fence_t` | Control-thread fence for completed handler side effects. See handler-context caveat below. |

`barrier()` returns a reply-ring watermark. `seq != invalid_sequence` is true
for a normal completion. `invalid_sequence` can be returned when the
barrier is treated as satisfied during shutdown or draining.

Barrier names beginning with `_sintra_` are reserved. Mixed barrier modes for
the same barrier round fail fast.

Barrier membership is snapshotted atomically at the coordinator when the
barrier round starts. Draining processes are filtered from that snapshot. If a
process becomes draining while a barrier is in flight, it is removed from the
pending/arrived sets and the barrier completes when no pending participants
remain. Barrier completions carry per-recipient reply-ring flush tokens.

Processing fences are designed to keep reader threads draining while a handler
waits, but a handler-context fence skips the currently executing reader and
does not wait for work queued behind it on that same request-reader stream. Use
a control-thread fence when the next phase must include all pre-barrier handler
work. For the full protocol details, see
[`docs/barriers_and_shutdown.md`](barriers_and_shutdown.md).

Compiled examples/tests:
- [`example/sintra/sintra_example_5_barrier_flush.cpp`](../example/sintra/sintra_example_5_barrier_flush.cpp)
- [`tests/processing_fence_test.cpp`](../tests/processing_fence_test.cpp)
- [`tests/barrier_guardrails_test.cpp`](../tests/barrier_guardrails_test.cpp)

## Lifecycle and Shutdown

### `sintra::shutdown`

Signatures:

```cpp
bool shutdown();
bool shutdown(const shutdown_options& options);
```

`shutdown()` is the standard terminal API for the collective lifecycle. All live
participants that are finishing together should call it. The runtime performs
an internal `processing_fence_t` handoff on `_sintra_all_processes` and then
tears down the local runtime.

### `sintra::shutdown_options`

Relevant shape:

```cpp
struct shutdown_options
{
    std::function<void()> coordinator_shutdown_hook;
};
```

The coordinator hook runs on the coordinator after the collective processing
fence and before raw teardown. Non-coordinator processes wait inside the
standard shutdown path. The hook must not start new peer coordination, extra
barriers, or custom protocol steps.

If the hook throws, the runtime still enters the hook-done/finalize path and
then rethrows to the caller. This is a finalization attempt, not a guarantee
that managed-child custody has already settled. If custody keeps finalization
incomplete, the runtime remains retained; settle or terminate the retained
custody and retry `shutdown()` sequentially. The retry skips completed
collective and hook phases, and Sintra does not rethrow the already surfaced
hook exception.

`shutdown()` otherwise returns `false` when no runtime is active or when its
final managed-child custody join is bounded-incomplete. In the latter case,
runtime and teardown state remain available for that sequential retry. The
250 ms bound applies only to the custody join, not to the complete shutdown
protocol. Concurrent, nested, and reentrant shutdown calls remain unsupported.

Compiled tests:
- [`tests/shutdown_options_test.cpp`](../tests/shutdown_options_test.cpp)
- [`tests/shutdown_options_throwing_hook_test.cpp`](../tests/shutdown_options_throwing_hook_test.cpp)

### `sintra::leave`

Signature:

```cpp
bool leave();
```

Use `leave()` when this process intentionally departs while peers may continue.
Call it from a top-level control thread, not from a message handler. A
coordinator may call `leave()` only when it is already the sole remaining known
process.

Compiled tests:
- [`tests/leave_lifecycle_test.cpp`](../tests/leave_lifecycle_test.cpp)
- [`tests/leave_coordinator_guardrails_test.cpp`](../tests/leave_coordinator_guardrails_test.cpp)

### Low-Level Teardown

`sintra::detail::finalize()` is for tests, single-process exceptional paths, or
deliberate primitive-level work. Ordinary multi-process code should use
`shutdown()` or `leave()`.

The raw teardown path announces draining, flushes coordinator reply-ring
visibility when possible, pauses into service mode, unblocks RPC waiters,
deactivates handlers, unpublishes transceivers, and destroys the managed
process.

For deeper lifecycle details, see
[`docs/barriers_and_shutdown.md`](barriers_and_shutdown.md) and
[`docs/process_lifecycle_notes.md`](process_lifecycle_notes.md).

## Recovery and Lifecycle Hooks

### `sintra::enable_recovery`

```cpp
void enable_recovery();
```

Call this from a managed child to opt its custody into recovery. The opt-in
persists across recoveries of that custody, but never crosses to a fresh
custody that reuses the process instance id. Externally attached processes are
not recoverable. Use `s_recovery_occurrence` to distinguish the original run
(`0`) from recoveries (`1`, `2`, ...).

### Coordinator Hooks

```cpp
void set_recovery_policy(Recovery_policy policy);
void set_recovery_runner(Recovery_runner runner);
void set_lifecycle_handler(Lifecycle_handler handler);
bool set_member_lifecycle_handler(Member_lifecycle_handler handler);
```

These coordinator-only hooks filter or delay recovery and observe committed
process retirement. A runner receives a one-shot `Recovery_control`; a retained
control becomes inert when shutdown starts or its exact custody closes. The
normative contracts are in [`sintra::recovery`](reference/recovery.md) and
[`sintra::set_lifecycle_handler`](reference/lifecycle_hooks.md).

`set_member_lifecycle_handler` is separate and effective only in a member
process. A detached member receives a sticky lifeline-release event and one
coordinator-departure event on a Sintra-owned thread. The callback should do
bounded work and post `leave()` to the host's control thread; lifecycle teardown
is rejected when called directly from the callback.

### Windows Host Crash Notification

Sintra does not own the Windows process unhandled-exception filter. A host that
does own the final disposition can call
[`sintra::announce_fatal_windows_exception`](reference/announce_fatal_windows_exception.md)
after it has decided execution will not continue. Unknown exception codes and
calls while Sintra is inactive are no-ops. The CRT and POSIX signal paths are
unchanged.

## Direct Ring Helpers

Direct rings are advanced public API exposed by:

```cpp
#include <sintra/rings.h>
```

Use direct rings when code needs Sintra's shared-memory ring primitives without
managed processes, pub/sub, or RPC.

Primary names:

| Name | Meaning |
| --- | --- |
| `sintra::Ring_W<T>` | Writer side of a single-producer ring. |
| `sintra::Ring_R<T>` | Reader side of a ring. |
| `sintra::Range<T>` | Lightweight `{begin, end}` range view returned by reads. |
| `sintra::Ring_R_snapshot<Reader>` | RAII snapshot for a reader range. |
| `sintra::make_snapshot(reader, args...)` | Creates a throwing snapshot helper. |
| `sintra::try_snapshot_e(reader, args...)` | Creates a snapshot result with explicit error status. |
| `sintra::Ring_R_snapshot_error` | `none`, `evicted`, or `exception`. |
| `sintra::Ring_diagnostics` | Counters and last-seen values for lag, overflow, eviction, and guard accounting. |
| `sintra::ring_acquisition_failure_exception` | Thrown when a ring buffer cannot be acquired. |
| `sintra::ring_abi_mismatch_exception` | Thrown when an existing ring control file or lifecycle anchor was created by an incompatible Sintra ABI. |
| `sintra::ring_reader_evicted_exception` | Thrown when a reader has been evicted by writer progress. |
| `sintra::ring_payload_traits<T>` | Advanced extension point for allowing non-trivial payload semantics. |
| `sintra::aligned_capacity<T>(requested)` | Capacity adjusted for ring alignment constraints. |
| `sintra::get_ring_configurations<T>(...)` | Candidate capacities for a requested shape. |

Reader snapshots pair `start_reading()` and `done_reading()` so callers do not
leak read guards. `Ring_R_snapshot_error::evicted` indicates the reader lost
its place because the writer advanced too far for that reader.

Compiled examples/tests:
- [`example/sintra/sintra_example_8_ring_helpers.cpp`](../example/sintra/sintra_example_8_ring_helpers.cpp)
- [`tests/ring_helpers_test.cpp`](../tests/ring_helpers_test.cpp)
- [`tests/ipc_rings_tests.cpp`](../tests/ipc_rings_tests.cpp)

## Errors and Diagnostics

For reverse lookup from an observed compiler error, exception, or symptom to
the likely rule, see the [Diagnostics guide](diagnostics.md).

### `sintra::init_error`

Thrown by `sintra::init()` when process swarm initialization fails.

Relevant API:

```cpp
class init_error : public std::runtime_error
{
public:
    enum class cause { spawn_failed, barrier_timeout, ipc_setup_failed };

    struct failed_process
    {
        std::string binary_name;
        instance_id_type instance_id;
        cause failure_cause;
        int errno_value;
        std::string error_message;
    };

    const std::vector<failed_process>& failures() const;
    const std::vector<instance_id_type>& successful_spawns() const;
    const std::string& diagnostic_report() const;
};
```

Compiled test:
- [`tests/init_error_spawn_failure_test.cpp`](../tests/init_error_spawn_failure_test.cpp)

### RPC Exceptions

Exceptions thrown by a remote RPC handler are converted and rethrown in the
caller when possible. Unknown exceptions are surfaced through Sintra's remote
exception path.

`sintra::rpc_cancelled` is thrown when an outstanding RPC is unblocked by
teardown or coordinator/process loss.

`sintra::rpc_timeout` is thrown by `Rpc_handle<R>::get_until(deadline)` when
the deadline expires while the RPC is still pending and caller-side abandon
wins; the timeout is local to the caller, does not cancel remote execution,
and late remote results are discarded.

`sintra::rpc_unavailable` is thrown when an RPC target has been unpublished,
destroyed, is shutting down, or its process is gone. It derives from
`std::runtime_error`, but can be caught directly when caller code needs to
distinguish target-side unavailability from caller-side cancellation.

### Logging and Console

`sintra::Console`/`sintra::console` is the coordinator-routed interprocess
console channel. Code commonly writes `sintra::console() << text;`; that syntax
constructs the `console` alias type, not a free function.

`sintra::Log_stream` is an RAII line builder routed through the configurable log
callback.

The default log callback writes `error` and `warning` messages to `stderr`,
writes `info` and `debug` messages to `stdout`, and flushes each message.
`set_log_callback(nullptr)` restores that default sink. `log_raw` copies the
current callback and user data under a mutex, invokes the callback outside the
mutex, and swallows exceptions from the callback.

Logging API:

```cpp
enum class log_level { error, warning, info, debug };
using log_callback_fn = void(*)(log_level level, const char* message, void* user_data);

void set_log_callback(log_callback_fn callback, void* user_data = nullptr);
log_callback_fn get_log_callback(void** user_data = nullptr);
void log_raw(log_level level, const char* message);

Log_stream ls_info();
Log_stream ls_warning();
Log_stream ls_error();
Log_stream ls_debug();
```

Compiled test:
- [`tests/log_stream_move_test.cpp`](../tests/log_stream_move_test.cpp)

## Threading and Reentrancy

This table is the authority for handler-context, blocking-call, and lifecycle
threading rules.

| Context | Do | Do not |
| --- | --- | --- |
| Slot or RPC handler | Protect shared state with mutexes/atomics; emit messages deliberately. | Assume handlers are serialized with application threads. |
| Slot or RPC handler | Use slots, RPC continuations, or control-thread receives for blocking waits. | Call `receive<T>()` from the handler. |
| Slot or RPC handler | Use fences only when handler-context exclusions are OK. | Assume the current handler was fenced. |
| Slot or RPC handler | Keep shutdown decisions on a control thread. | Call `leave()` from handler or post-handler callbacks. |
| Async RPC caller | Use `get_until` for bounded result retrieval; drop the handle to abandon caller-side interest. | Treat deadline expiry as remote cancellation. |
| Collective shutdown | Have every live finishing participant call `shutdown()`. | Call `shutdown()` in only one participant while peers continue. |
| Unilateral departure | Use `leave()` from a top-level control thread. | Use `leave()` from a coordinator that still owns known peers. |
| Shared payload buffers | Copy data out before the valid message scope ends. | Keep variable-buffer backed data past the handler/receive scope. |

Reader threads invoke slots and transported RPC handlers asynchronously. A
barrier coordinates interprocess progress, not in-process data races.

`init()` cannot be called twice without teardown. `shutdown()`, `leave()`, and
`detail::finalize()` are mutually exclusive terminal paths. Concurrent or nested
lifecycle teardown throws.

Non-strict same-process blocking RPC may call the target object directly in the
caller thread. Strict RPC always uses the transported RPC path. Async RPC rejects
same-process non-strict targets; use `SINTRA_RPC_STRICT` when a local async call
must use transport.

`Rpc_handle<T>` is move-only. Destroying a pending handle abandons caller-side
interest but does not cancel remote execution.

## Type IDs

Automatic type ids are sufficient when all processes are built with the same
toolchain and relevant flags. Use explicit ids when ids must be pinned across
build boundaries.

User-facing explicit-id macros:

| Macro | Contract |
| --- | --- |
| `SINTRA_TYPE_ID(idv)` | Pins a transceiver type id. `idv` must be non-zero and fit in `max_user_type_id`. |
| `SINTRA_MESSAGE_EXPLICIT(name, idv, ...)` | Pins a message type id. `idv` must be non-zero and fit in `max_user_type_id`. |

Support names:

| Name | Meaning |
| --- | --- |
| `sintra::max_user_type_id` | Largest user id value accepted by explicit-id macros. |
| `sintra::make_user_type_id(v)` | Tags a user id into Sintra's user-id range. |
| `sintra::is_user_type_id(v)` | Tests whether a type id is tagged as user-provided. |
| `sintra::invalid_type_id` | No valid type id. |

Explicit ids must be unique and consistent across every process in the swarm.
Duplicate explicit ids cause type/message confusion at runtime or fail during
resolution; diagnose them by checking every pinned id in transceiver and message
declarations.

Compiled example:
- [`example/sintra/sintra_example_7_explicit_type_ids.cpp`](../example/sintra/sintra_example_7_explicit_type_ids.cpp)

## CMake Integration

Sintra provides an interface target.

For an in-tree dependency:

```cmake
add_subdirectory(external/sintra)
target_link_libraries(my_app PRIVATE sintra::sintra)
```

For an installed package:

```cmake
find_package(sintra CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE sintra::sintra)
```

The target requires C++20 and links platform support libraries through the
interface target.

## Recipes

Recipes point to compiled examples or tests so copied code starts from checked
sources.

| Task | Use |
| --- | --- |
| Minimal compiled publish/subscribe program | [`example/sintra/sintra_example_9_minimal_pubsub.cpp`](../example/sintra/sintra_example_9_minimal_pubsub.cpp) |
| Basic broadcast and slot | [`example/sintra/sintra_example_0_basic_pubsub.cpp`](../example/sintra/sintra_example_0_basic_pubsub.cpp) |
| Blocking receive on a control thread | [`tests/receive_test.cpp`](../tests/receive_test.cpp) |
| Named transceiver RPC | [`example/sintra/sintra_example_2_rpc_append.cpp`](../example/sintra/sintra_example_2_rpc_append.cpp) |
| Async RPC with bounded result retrieval | [`tests/rpc_bounded_result_test.cpp`](../tests/rpc_bounded_result_test.cpp) |
| Async RPC lifecycle outcomes | [`tests/rpc_async_lifecycle_test.cpp`](../tests/rpc_async_lifecycle_test.cpp) |
| Fire-and-forget unicast | [`example/sintra/sintra_example_6_unicast_send_to.cpp`](../example/sintra/sintra_example_6_unicast_send_to.cpp) |
| Spawn and wait for a process | [`tests/spawn_wait_test.cpp`](../tests/spawn_wait_test.cpp) |
| Admit a manually launched process | [`tests/external_process_invitation_test.cpp`](../tests/external_process_invitation_test.cpp) |
| Collective shutdown | [`example/sintra/sintra_example_1_ping_pong_multi.cpp`](../example/sintra/sintra_example_1_ping_pong_multi.cpp) |
| Coordinator shutdown hook | [`tests/shutdown_options_test.cpp`](../tests/shutdown_options_test.cpp) |
| One process leaves while peers continue | [`tests/leave_lifecycle_test.cpp`](../tests/leave_lifecycle_test.cpp) |
| Processing fence for handler side effects | [`tests/processing_fence_test.cpp`](../tests/processing_fence_test.cpp) |
| Crash recovery | [`example/sintra/sintra_example_4_recovery.cpp`](../example/sintra/sintra_example_4_recovery.cpp) |
| Explicit type ids | [`example/sintra/sintra_example_7_explicit_type_ids.cpp`](../example/sintra/sintra_example_7_explicit_type_ids.cpp) |
| Direct ring snapshot | [`example/sintra/sintra_example_8_ring_helpers.cpp`](../example/sintra/sintra_example_8_ring_helpers.cpp) |
| Error-to-cause lookup | [`docs/diagnostics.md`](diagnostics.md) |

## Common Mistakes

| Mistake | Correct pattern |
| --- | --- |
| Including `sintra/detail/...` for pub/sub, RPC, barriers, or lifecycle. | Include `<sintra/sintra.h>`. |
| Including `sintra/detail/ipc/rings.h` for direct rings. | Include `<sintra/rings.h>`. |
| Calling `receive<T>()` inside a handler. | Use a slot, RPC continuation, or control-thread receive. |
| Updating shared state from handlers without synchronization. | Protect shared state with normal C++ synchronization. |
| Calling `shutdown()` in only one participant while peers continue. | Use `leave()` for unilateral departure, or have all finishing participants call `shutdown()`. |
| Using `leave()` from a process that owns descendants expected to continue. | Perform an application-level ownership handoff before departure. |
| Manually launching a process with only `--swarm_id`, `--instance_id`, and `--coordinator_id`. | Create an external process invitation and pass its `sintra_args()` output. |
| Reusing `_sintra_` barrier names. | Use application-owned barrier names. |
| Mixing barrier modes on one barrier round. | Make every participant use the same mode. |
| Assuming a delivery fence means peer handlers finished. | Use `processing_fence_t` when side effects must be complete. |
| Using `SINTRA_RPC` when local async behavior must use transport. | Use `SINTRA_RPC_STRICT`. |
| Returning references or using non-const reference RPC parameters. | Return by value and model mutable results explicitly. |
| Passing a string RPC target without first naming the transceiver. | Call `assign_name(...)` before the name is used, or exchange and use `instance_id()`. |
| Keeping variable-buffer backed data after the handler/receive scope. | Copy the data out before the scope ends. |
| Adding polling around Sintra state. | Use slots, barriers, lifecycle hooks, or event messages. |

## API Index

| Name | Section |
| --- | --- |
| `SINTRA_MESSAGE` | [Transceivers](#transceivers) |
| `SINTRA_MESSAGE_EXPLICIT` | [Transceivers](#transceivers), [Type IDs](#type-ids) |
| `SINTRA_RPC` | [Transceivers](#transceivers), [RPC](#rpc) |
| `SINTRA_RPC_STRICT` | [Transceivers](#transceivers), [RPC](#rpc) |
| `SINTRA_TYPE_ID` | [Transceivers](#transceivers), [Type IDs](#type-ids) |
| `SINTRA_UNICAST` | [Transceivers](#transceivers), [RPC](#rpc) |
| `sintra::Crash_info` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::announce_fatal_windows_exception` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Entry_descriptor` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::Lifecycle_handler` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Member_lifecycle_handler` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Lifetime_policy` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::Log_stream` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::Named_instance<T>` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::Process_descriptor` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::Recovery_control` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Recovery_policy` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Recovery_runner` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Resolvable_instance_id` | [RPC](#rpc), [Targeting and IDs](#targeting-and-ids) |
| `sintra::Ring_R<T>` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::Ring_R_snapshot<Reader>` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::Ring_R_snapshot_error` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::Ring_W<T>` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::Ring_diagnostics` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::Rpc_handle<T>` | [RPC](#rpc) |
| `sintra::Spawn_options` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::Transceiver` | [Transceivers](#transceivers) |
| `sintra::Typed_instance_id<T>` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::activate_slot` | [Publish/Subscribe](#publishsubscribe) |
| `sintra::aligned_capacity` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::any_local` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::any_local_or_remote` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::any_remote` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::barrier` | [Barriers and Fences](#barriers-and-fences) |
| `sintra::cancel_external_process_invitation` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::compose_instance` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::console` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::create_external_process_invitation` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::deactivate_all_slots` | [Publish/Subscribe](#publishsubscribe) |
| `sintra::decompose_instance` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::delivery_fence_t` | [Barriers and Fences](#barriers-and-fences) |
| `sintra::disable_debug_pause_for_current_process` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::enable_recovery` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::External_process_invitation` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::External_process_invitation_options` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::get_process_index` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::get_ring_configurations` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::get_log_callback` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::init` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::init_error` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::invalid_instance_id` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::invalid_sequence` | [Targeting and IDs](#targeting-and-ids), [Barriers and Fences](#barriers-and-fences) |
| `sintra::invalid_type_id` | [Type IDs](#type-ids) |
| `sintra::is_user_type_id` | [Type IDs](#type-ids) |
| `sintra::join_swarm` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::leave` | [Lifecycle and Shutdown](#lifecycle-and-shutdown) |
| `sintra::local` | [Publish/Subscribe](#publishsubscribe) |
| `sintra::log_callback_fn` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::log_level` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::log_raw` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::ls_debug` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::ls_error` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::ls_info` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::ls_warning` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::make_branches` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::make_snapshot` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::make_user_type_id` | [Type IDs](#type-ids) |
| `sintra::max_user_type_id` | [Type IDs](#type-ids) |
| `sintra::message_string` | [Messages and Payloads](#messages-and-payloads) |
| `sintra::processing_fence_t` | [Barriers and Fences](#barriers-and-fences) |
| `sintra::process_of` | [Targeting and IDs](#targeting-and-ids) |
| `sintra::process_index` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::process_lifecycle_event` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::member_lifecycle_event` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::Range<T>` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::receive` | [Publish/Subscribe](#publishsubscribe) |
| `sintra::remote` | [Publish/Subscribe](#publishsubscribe) |
| `sintra::rendezvous_t` | [Barriers and Fences](#barriers-and-fences) |
| `sintra::ring_acquisition_failure_exception` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::ring_abi_mismatch_exception` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::ring_payload_traits<T>` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::ring_reader_evicted_exception` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::rpc_cancelled` | [RPC](#rpc), [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::rpc_timeout` | [RPC](#rpc), [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::rpc_unavailable` | [RPC](#rpc), [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::sequence_counter_type` | [Targeting and IDs](#targeting-and-ids), [Barriers and Fences](#barriers-and-fences) |
| `sintra::set_lifecycle_handler` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::set_member_lifecycle_handler` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::set_log_callback` | [Errors and Diagnostics](#errors-and-diagnostics) |
| `sintra::set_recovery_policy` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::set_recovery_runner` | [Recovery and Lifecycle Hooks](#recovery-and-lifecycle-hooks) |
| `sintra::shutdown` | [Lifecycle and Shutdown](#lifecycle-and-shutdown) |
| `sintra::shutdown_options` | [Lifecycle and Shutdown](#lifecycle-and-shutdown) |
| `sintra::spawn_swarm_process` | [Initialization and Process Topology](#initialization-and-process-topology) |
| `sintra::try_snapshot_e` | [Direct Ring Helpers](#direct-ring-helpers) |
| `sintra::typed_variable_buffer<T>` | [Messages and Payloads](#messages-and-payloads) |
| `sintra::world` | [Publish/Subscribe](#publishsubscribe) |
