# Sintra lifecycle hooks

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`set_lifecycle_handler` registers the coordinator-side process-retirement
callback. `set_member_lifecycle_handler` registers the distinct member-side
callback used after explicit detach. Coordinator events identify a retired
process generation; member events report lifeline release and coordinator
departure for the current runtime epoch.

Signature:

```cpp
void set_lifecycle_handler(Lifecycle_handler handler);

bool set_member_lifecycle_handler(Member_lifecycle_handler handler);

using Lifecycle_handler =
    std::function<void(const process_lifecycle_event&)>;

struct process_lifecycle_event
{
    enum class reason
    {
        crash,
        normal_exit,
        unpublished
    };

    instance_id_type process_iid = invalid_instance_id;
    uint32_t         process_slot = 0;
    reason           why = reason::unpublished;
    int              status = 0;
};

struct member_lifecycle_event
{
    enum class kind
    {
        LIFELINE_RELEASED,
        COORDINATOR_DEPARTED
    };

    enum class departure_cause
    {
        NONE,
        UNPUBLISHED,
        SIGNALED_CRASH,
        EXACT_OS_WATCH,
        COLLECTIVE_SHUTDOWN
    };

    kind why = kind::LIFELINE_RELEASED;
    departure_cause cause = departure_cause::NONE;
};

using Member_lifecycle_handler =
    std::function<void(const member_lifecycle_event&)>;

// Internal: not for direct use by application code.
struct Managed_process {
    SINTRA_MESSAGE_RESERVED(terminated_abnormally, int status);
};
```

Use when:

- The coordinator needs to react to peer departures: log the cause,
  display a "crashed" indicator versus a "completed" indicator, or
  trigger a follow-up workflow.
- A test or supervisor wants to wait for a specific peer to leave and
  inspect the reason. Combine with a condition variable or future to
  observe the event from outside the callback.
- A detached member must learn that its lifeline release was consumed or that
  its coordinator departed, then post cooperative `leave()` work to the
  application's serialized control thread.

Contract:

- `set_lifecycle_handler` is effective only on the coordinator process. Calls
  from non-coordinator processes are no-ops.
- The handler is invoked once for each successful process-publication
  retirement. Crash status is passed directly into that exact transaction; it
  is not cached by process instance id for a later unpublish. A duplicate or
  stale crash message therefore cannot produce another event or retire a
  replacement generation.
- `process_iid` is the affected process's instance id; `process_slot`
  is the stable slot index for correlation across recoveries.
- For `crash`, `status` is exactly the integer carried by
  `Managed_process::terminated_abnormally`. The signal-dispatch path supplies
  the signal number. This field is not a normalized exit code or native wait
  status and is not guaranteed to be non-zero. It is zero on `normal_exit` and
  `unpublished` paths. For OS-authoritative normalized and native managed-child
  status, use
  [`Managed_child_custody::observe_latest_created_exit()`](spawn_swarm_process.md).
- The reasons map to the lifecycle as follows:
  - `crash`: an exact abnormal-termination message retired its matching
    managed-child or external-reader generation with that message's status.
  - `normal_exit`: no crash information was supplied and the process slot was
    already draining when exact unpublish committed. This usually follows the
    process beginning `shutdown()`, `leave()`, or `detail::finalize()`, but
    coordinator shutdown also marks admitted external processes as draining.
    The reason therefore does not prove that the peer called a graceful API or
    that its OS process exited cleanly.
  - `unpublished`: no crash information was supplied and the process slot was
    not previously draining when exact unpublish committed.
- `Managed_process::terminated_abnormally` is an internal reserved
  message emitted by the abnormal process path. Its message prefix carries
  managed custody identity and occurrence, or the external invitation
  occurrence. The coordinator revalidates that identity against the registry
  and reader before committing unpublish. It is not part of the user message
  surface; use the `Lifecycle_handler` callback for crash observation.
- `set_member_lifecycle_handler` is the distinct member-side hook. It returns
  `false` unless called by an active non-coordinator member or if its dedicated
  lifecycle dispatcher cannot start. Passing an empty handler clears it.
- `LIFELINE_RELEASED` is sticky for the active runtime epoch: registering the
  handler after a managed child consumed the release byte still delivers the
  event once. Detached external invitations have no lifeline and therefore do
  not produce this event.
- `COORDINATOR_DEPARTED` is delivered at most once even when graceful
  unpublish, an abnormal-termination signal, the exact native watch, and the
  collective-shutdown notice race. `cause` is the first observed source and is
  best-effort provenance; applications should make the same departure decision
  for every non-`NONE` cause.
- Coordinator departure makes coordinator communication unavailable and
  quiesces the detached member's current-epoch communication. Sintra leaves
  host teardown cooperative: the callback should notify the application, and
  its serialized control thread should call `leave()`.

Threading and lifecycle:

- Publication registry removal is committed atomically before the handler.
  `instance_unpublished` and any released delayed publications are enqueued on
  the coordinator request-ring FIFO before a replacement publication can be
  enqueued. The handler then runs outside coordinator publication, group,
  custody, and reader locks. Managed-child communication retirement has been
  requested, but its asynchronous reader join can still be incomplete; the
  handler does not certify communication quiescence or custody release.
  Recovery, when eligible, is considered only after the handler returns.
- The handler runs on a coordinator thread. Keep work bounded and thread-safe;
  different events may invoke it from different threads.
- The handler is set globally on the coordinator. Replacing it with a
  new value supersedes the previous one; passing an empty
  `std::function` clears it.
- The member handler runs on one Sintra-owned lifecycle thread, never the
  application's control thread. Keep it bounded and thread-safe. It must not
  call `shutdown()`, `leave()`, or `detail::finalize()` directly; those calls
  throw `std::logic_error` from this callback context. Post the action to the
  host's control thread instead.
- On Windows, Sintra does not install or own the process unhandled-exception
  filter. A host that owns the final unhandled disposition may call
  [`announce_fatal_windows_exception`](announce_fatal_windows_exception.md)
  only after deciding that execution will not continue. The existing
  per-thread CRT signal path is separate and unchanged.
- Crash notification runs inside the failing process and is necessarily
  best-effort if corruption or termination prevents its bounded dispatch from
  completing. Managed-child exit observation remains OS-authoritative for the
  native exit status, but a Windows exit code alone is not crash provenance.

Failures:

- `set_member_lifecycle_handler` returns `false` outside an active member
  runtime or if its lifecycle dispatcher cannot start. Exceptions thrown by
  either handler are caught by the runtime's exception boundary and logged.

Example source:

- [tests/lifecycle_handler_test.cpp](../../tests/lifecycle_handler_test.cpp)
- [tests/leave_lifecycle_test.cpp](../../tests/leave_lifecycle_test.cpp)
- [tests/recovery_policy_test.cpp](../../tests/recovery_policy_test.cpp)
- [tests/managed_child_detach_transaction_contract_test.cpp](../../tests/managed_child_detach_transaction_contract_test.cpp)
- [tests/detached_member_exact_watch_contract_test.cpp](../../tests/detached_member_exact_watch_contract_test.cpp)

See also:

- [sintra::recovery](recovery.md)
- [sintra::announce_fatal_windows_exception](announce_fatal_windows_exception.md)
- [sintra::leave](leave.md)
- [sintra::shutdown](shutdown.md)
- [docs/process_lifecycle_notes.md](../process_lifecycle_notes.md)
