# sintra::set_lifecycle_handler

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`set_lifecycle_handler` registers a coordinator-side callback that is
invoked once per process lifecycle event (crash, normal exit, or
unpublished without prior signal). The callback receives a
`process_lifecycle_event` with the affected process's identity and the
reason. A crash event carries the integer supplied by the internal
`Managed_process::terminated_abnormally` message, whose prefix also carries
the exact reader generation that authorizes the crash retirement transaction.

Signature:

```cpp
void set_lifecycle_handler(Lifecycle_handler handler);

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

Contract:

- Effective only on the coordinator process. Calls from non-coordinator
  processes are no-ops.
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

Failures:

- None. Exceptions thrown by the handler are caught by the runtime's
  exception boundary and logged.

Example source:

- [tests/lifecycle_handler_test.cpp](../../tests/lifecycle_handler_test.cpp)
- [tests/leave_lifecycle_test.cpp](../../tests/leave_lifecycle_test.cpp)
- [tests/recovery_policy_test.cpp](../../tests/recovery_policy_test.cpp)

See also:

- [sintra::recovery](recovery.md)
- [sintra::leave](leave.md)
- [sintra::shutdown](shutdown.md)
- [docs/process_lifecycle_notes.md](../process_lifecycle_notes.md)
