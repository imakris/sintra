# sintra::spawn_swarm_process

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`spawn_swarm_process` adds one managed process to a running swarm
after `init` has completed. It launches the requested binary with the
required Sintra arguments, optionally observes a named instance as readiness,
and applies the configured lifeline policy. Only the process hosting the local
coordinator may call it; child processes inherit the swarm and coordinator id
from that process.

Signature:

```cpp
struct Spawn_options
{
    std::string                binary_path;
    std::vector<std::string>   args;
    std::vector<std::string>   env_overrides;
    instance_id_type           process_instance_id = invalid_instance_id;
    std::string                readiness_instance_name;
    Lifetime_policy            lifetime;
};

Managed_child_custody spawn_swarm_process(const Spawn_options& options);

enum class Managed_child_failure_kind
{
    none,
    custody_not_accepted,
    custody_closed,
    occurrence_admission,
    reader_setup,
    lifeline_setup,
    native_spawn,
    native_identity,
    setup_exception,
    setup_worker_start,
    readiness_observation,
    release_worker_start,
    release_worker_execution,
    native_observer
};

struct Managed_child_failure
{
    Managed_child_failure_kind kind = Managed_child_failure_kind::none;
    std::uint32_t occurrence = 0;
    int native_error = 0;
    std::string message;
};

enum class Managed_child_readiness_state
{
    not_requested,
    pending,
    reached,
    observation_stopped
};

enum class Managed_child_release_state
{
    open,
    requested,
    complete
};

enum class Managed_child_custody_state
{
    not_started,
    owner_bound,
    detaching,
    disowned
};

enum class Managed_child_detach_result
{
    not_started,
    settlement_pending,
    disowned,
    definite_non_delivery,
    no_live_occurrence,
    conflict
};

struct Managed_child_status
{
    Managed_child_readiness_state readiness_state =
        Managed_child_readiness_state::not_requested;
    Managed_child_release_state release_state =
        Managed_child_release_state::open;
    Managed_child_custody_state custody_state =
        Managed_child_custody_state::not_started;
    std::size_t admitted_occurrences = 0;
    std::size_t created_occurrences = 0;
    std::size_t exited_occurrences = 0;
    Managed_child_failure last_failure;
};

struct Managed_child_occurrence_identity
{
    instance_id_type process_instance_id = invalid_instance_id;
    std::uint32_t occurrence = 0;
    std::uint64_t custody_identity = 0;
};

enum class Managed_child_exit_status_kind
{
    unavailable,
    exited,
    signaled,
    observation_ended_by_detach
};

struct Managed_child_exit
{
    Managed_child_occurrence_identity occurrence;
    Managed_child_exit_status_kind status_kind =
        Managed_child_exit_status_kind::unavailable;
    std::uint32_t status = 0;
    std::uint32_t native_status = 0;
    bool native_status_available = false;
};

using Managed_child_exit_callback =
    std::function<void(const Managed_child_exit&)>;

class Managed_child_exit_subscription
{
public:
    explicit operator bool() const noexcept;
    void unsubscribe() noexcept;
};

struct Managed_child_exit_observation
{
    Managed_child_occurrence_identity occurrence;
    Managed_child_exit_subscription subscription;
    explicit operator bool() const noexcept;
};

class Managed_child_custody
{
public:
    explicit operator bool() const noexcept;

    Managed_child_status status() const;
    Managed_child_status wait_for_readiness_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_exit_observation observe_latest_created_exit(
        Managed_child_exit_callback callback) const;
    Managed_child_status release_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_status terminate_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_detach_result detach_until(
        std::chrono::steady_clock::time_point deadline) const;
};
```

Use when:

- A new participant must join a running swarm dynamically (a worker pool
  scaling out, a late-arriving subscriber, a tool launching a helper).
- The caller needs to observe whether the new participant publishes a known
  instance name (`readiness_instance_name`) through an explicit absolute
  deadline.

Contract:

- `binary_path` must be non-empty. The spawned program runs that binary,
  receives `args` as positional arguments (Sintra inserts the binary name
  as `argv[0]` if not already present), and gets `--swarm_id`,
  `--instance_id`, and `--coordinator_id` appended automatically.
- `env_overrides` is a sequence of environment entries, normally in
  `NAME=VALUE` form. The child inherits the current environment and Sintra
  merges these entries in order. A later entry for the same name wins. Name
  matching is case-insensitive on Windows and case-sensitive on POSIX. The
  same overrides are reused for recovery occurrences. Sintra does not validate
  entry syntax; callers must not rely on malformed entries.
- `process_instance_id` defaults to a fresh process instance id. Setting it
  pins the new process to a specific valid process id. Transceiver ids,
  wildcard ids, ids with an invalid process component, pending invitation ids,
  and ids with unresolved child custody are rejected before acceptance.
- `readiness_instance_name` is optional. When configured, occurrence setup and
  readiness resolution run as Sintra-owned work and `spawn_swarm_process`
  returns the accepted handle immediately. `wait_for_readiness_until()` waits only on
  the custody record's monotone notification through its absolute steady-clock
  deadline. Deadline expiry returns an incomplete snapshot; it does not request
  release or adverse cleanup.
- `lifetime` controls the lifeline policy applied to the child (see
  `Lifetime_policy`).
- A return handle that converts to `true` means Sintra accepted durable logical
  custody before OS creation authority. `Managed_child_custody::status()` separately
  reports confirmed readiness, admitted/created/exited occurrence counts, and
  release state. OS-create failure remains an accepted no-child record rather
  than a fabricated native identity.
- Once custody is accepted, synchronous setup failures do not escape and discard
  the handle. A failure before native creation returns accepted no-child custody
  and requests release. A failure after native creation returns accepted custody
  while retained cleanup continues through exact exit confirmation.
- Calls made while a lifecycle teardown protocol is active are rejected and
  return an empty handle with a warning logged; rejection creates no child.
- Calling `observe_latest_created_exit()` with an empty callback returns an
  empty observation without starting the exit dispatcher or retaining state.
- `observe_latest_created_exit()` atomically selects the most recently admitted
  occurrence for which OS creation actually succeeded. The returned immutable
  `(process_instance_id, occurrence, custody_identity)` identifies that exact
  occurrence. `occurrence` is relative to one custody: `0` is its original
  launch, `1` its first recovery, and so on. A fresh custody starts again at
  `0`, including when it reuses a process instance id or is created by a
  mid-flight swarm join.
- `custody_identity` is an opaque, runtime-scoped token. It distinguishes fresh
  custodies that use the same process instance id and occurrence number, and is
  shared by all recovery occurrences in one custody. It is unique only among
  custodies owned by the same active runtime; values may repeat after shutdown
  and a later runtime initialization. Do not infer ordering or persist the token
  as a cross-runtime identifier.
- Identity equality includes all three fields. The
  subscription never follows a later recovery occurrence; call the method
  again after a newer occurrence is created to observe that occurrence.
- A newer admitted no-child occurrence does not displace the latest created
  occurrence. If no occurrence was created, the observation is empty and the
  callback is not retained. Older exited occurrences may be skipped by the
  latest-created selection.
- Registration establishes dispatcher custody before retaining the callback.
  If the dispatcher thread cannot start, registration returns an empty
  observation and retains no callback, including when the selected occurrence
  has already exited.
- Exit observation requires the active coordinator runtime that created the
  custody. Registration during teardown, after `shutdown()`, or after a later
  `init()` when the custody belongs to an earlier runtime returns an empty
  observation and does not retain the callback. While the owning runtime
  remains active, registration after the selected occurrence exited schedules
  exactly one late delivery.
- `Managed_child_exit::status` is the portable normalized value selected by
  `status_kind`: an exit code for `exited`, a signal number for `signaled`, and
  zero when unavailable or not normalized. Windows reports every termination
  as `exited` because its process status does not distinguish forced exit.
  `native_status`, when available, preserves the complete Windows 32-bit exit
  code or the POSIX wait-status bit pattern. Prefer `status_kind` and `status`
  unless platform-specific diagnostics are required. An unexpected POSIX
  wait-status classification reports `unavailable` while retaining that native
  bit pattern.

Threading and lifecycle:

- Call from a top-level user thread in the coordinator process. Worker-process
  calls are rejected before acceptance and cannot create a child. Acceptance
  takes the teardown admission lock; `wait_for_readiness_until()` does not enter
  coordinator work and waits only on custody notifications.
- Successful spawns participate in subsequent barriers and coordinator
  membership once setup completes. `wait_for_readiness_until()` is the way to gate
  later code on the exact participant having published its readiness name.
- The custody handle's boolean conversion is the sole validity and durable
  acceptance fact. Status does not duplicate it. An empty handle's default
  status is `not_requested` / `open`.
- Exit observations and subscriptions are move-only. Their boolean conversion
  reports successful registration. Destroying the subscription or calling
  `unsubscribe()` cancels pending delivery; an external unsubscribe waits for
  an executing callback. After it returns, that callback is not running and
  cannot start. Self-unsubscription does not wait on itself; removal completes
  after the callback returns.
- Exit callbacks run once on a Sintra-managed lifecycle thread without custody
  or native-observer locks held. Delivery may start before registration
  returns, observer ordering is unspecified, and callback exceptions are logged
  without retry and never escape the worker. Non-teardown Sintra APIs may be
  reentered subject to their normal thread rules. Callbacks must not initiate
  Sintra teardown and should post longer application work to the caller's
  executor. `shutdown()`, `leave()`, and `detail::finalize()` throw
  `std::logic_error` before teardown admission changes when called there.
- `readiness_state` is `not_requested` when no readiness name was configured,
  `pending` while the exact target may still be observed, `reached` once the
  coordinator observed it, and `observation_stopped` when the target can no
  longer be reached. Readiness does not imply release or any later retirement
  fact.
- `release_state` is `open` before release is requested, `requested` while
  retirement remains incomplete, and `complete` only after every admitted
  occurrence has reached the complete-release contract.
- `custody_state` is `owner_bound` while Sintra retains native authority,
  `detaching` while a committed detach is settling, and `disowned` after that
  authority has been irreversibly relinquished. `disowned` does not mean that
  the OS process exited and does not make `release_state` complete.
- `status()` is an immediate snapshot. `wait_for_readiness_until()`, `release_until()`,
  and `terminate_until()` return the same status shape after waiting only until
  their absolute steady-clock deadlines. An incomplete snapshot reports only
  confirmed facts; it does not invent a lifecycle milestone.
- `Managed_child_custody::release_until()` closes recovery before requesting retirement and
  waits for graceful/natural retirement, returning only confirmed facts by its
  absolute deadline. It does not release the child's lifeline or initiate
  adverse cleanup. The operation is idempotent: another call requests the same
  graceful release and waits on the same retained custody through its new
  absolute deadline.
- `Managed_child_custody::terminate_until()` is the explicit adverse-path escalation. It closes
  recovery and monotonically asks Sintra's retained custody owner to drain the
  exact admitted occurrences, release their lifelines, retire publication and
  communication authoritatively, and confirm OS exit. The deadline bounds only
  the caller's wait; an incomplete return retains ownership and cleanup normally
  continues. A recorded release-attempt blocker means its associated attempt
  stopped retryably. While release remains incomplete, callers should repeat an
  idempotent release or termination call to retry. A retained historical
  `last_failure` alone does not prove that a later or current attempt is stopped.
- Repeated release and termination calls operate on the same opaque record; they do not
  reconstruct authority from a process id or name. A cleanup escalation cannot
  be downgraded by a later graceful release request.
- `Managed_child_custody::detach_until()` is a separate, irreversible
  operation for the latest exact live occurrence. It closes recovery, commits
  one lifeline-release byte, relinquishes native termination and exit-observer
  authority, and returns only by its absolute deadline. `disowned` confirms
  commit and settlement; `settlement_pending` means the monotone transition is
  still settling. `definite_non_delivery` means the byte was not written and
  owner authority was restored. `no_live_occurrence` and `conflict` report an
  already-exited occurrence or an admitted release/termination operation.
  `not_started` covers an invalid or stale handle, teardown or
  collective-shutdown exclusion, deadline admission failure, and unavailable
  or mismatched exact coordinator identity.
- After detach commits, `release_until()` and `terminate_until()` report
  `custody_state == disowned` without waiting for or terminating the child.
  Authority cannot be reacquired through an old custody handle.
- Successful detach completes existing exact-exit subscriptions once with
  `status_kind == observation_ended_by_detach`. This is explicit loss of
  observation authority, not an OS-exit event.
- Complete release joins authoritative exact-occurrence publication and
  communication retirement with confirmed OS exit for every admitted
  occurrence. Dropping the handle does not drop Sintra's retained obligation.

Failures:

- Returns an empty handle (with a logged error) when validation or lifecycle
  admission rejects before custody acceptance.
- Resource exceptions before durable acceptance may propagate; no OS creation
  authority has been granted at that point.
- A readiness deadline reached while the requested instance is still absent
  returns a valid custody handle with `readiness_state == pending` and leaves
  `release_state == open`; it does not request cleanup or by itself record a
  managed-child failure. Call `terminate_until()` to request cleanup explicitly.
  Release or termination deadline expiry likewise returns incomplete confirmed
  facts without minting a failure. An exception in readiness observation may
  instead be recorded as `readiness_observation`.
- `last_failure` is a retained historical diagnostic, not the current custody
  state. Its custody-relative `occurrence` identifies the original launch (`0`)
  or recovery (`1`, `2`, ...) to which `kind`, `native_error`, and `message`
  apply. Later successful progress, including complete release, does not erase
  an earlier report.
- Failure to start a release worker is reported as `release_worker_start`; an
  exception escaping its lifecycle work is reported as
  `release_worker_execution`. Both retain custody and permit a later release or
  termination call to retry with the same authority.
- A Windows exit-observer failure is reported as `native_observer`; exact
  process-handle authority remains available for release or termination retry.
- `Managed_child_failure_kind::none` means no managed-child failure report has
  been recorded. It is not an independent success result; use the other status
  states to decide whether the requested milestone is confirmed.

Example source:

- [tests/spawn_wait_test.cpp](../../tests/spawn_wait_test.cpp)
- [tests/spawn_detached_test.cpp](../../tests/spawn_detached_test.cpp)
- [tests/lifeline_basic_test.cpp](../../tests/lifeline_basic_test.cpp)
- [tests/managed_child_public_cleanup_contract_test.cpp](../../tests/managed_child_public_cleanup_contract_test.cpp)
- [tests/managed_child_exact_exit_observation_contract_test.cpp](../../tests/managed_child_exact_exit_observation_contract_test.cpp)
- [tests/managed_child_detach_transaction_contract_test.cpp](../../tests/managed_child_detach_transaction_contract_test.cpp)
- [tests/detached_member_exact_watch_contract_test.cpp](../../tests/detached_member_exact_watch_contract_test.cpp)

See also:

- [sintra::join_swarm](join_swarm.md)
- [sintra::create_external_process_invitation](external_process_invitation.md)
- [sintra::recovery](recovery.md)
- [sintra::Process_descriptor](process_descriptor.md)
- [sintra::init](init.md)
