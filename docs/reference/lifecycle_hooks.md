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
reason. Crash events also carry the platform-dependent exit status. The
related signal `Managed_process::terminated_abnormally` is sent before the
unpublish notification and is the underlying message that crash guards
observe.

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
- The handler is invoked once per event. When a crash status was
  recorded for a process, the subsequent unpublish does not produce an
  additional `unpublished` event.
- `process_iid` is the affected process's instance id; `process_slot`
  is the stable slot index for correlation across recoveries.
- `status` is non-zero for crash events when the platform supplies an
  exit signal; it is zero on `normal_exit` and `unpublished` paths.
- The reasons map to the lifecycle as follows:
  - `crash`: the managed process crash handler called the coordinator.
  - `normal_exit`: the process called `shutdown()`, `leave()`, or
    `detail::finalize()` and the coordinator observed the draining bit.
  - `unpublished`: the process unpublished without crash status or
    draining bit (the catch-all for abrupt teardowns that did not flow
    through either path).
- `Managed_process::terminated_abnormally` is an internal reserved
  message sent by the coordinator before the corresponding
  `instance_unpublished` for an abnormally terminated process. It is
  declared with `SINTRA_MESSAGE_RESERVED` and is not part of the user
  message surface; use the `Lifecycle_handler` callback for crash
  observation in user code.

Threading and lifecycle:

- The handler runs on a coordinator thread. Keep work bounded and
  thread-safe; the same handler may be invoked from different events on
  different threads.
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
