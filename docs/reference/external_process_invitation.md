# sintra::create_external_process_invitation

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

External process invitations let a coordinator admit a manually launched
process into an existing swarm. The coordinator reserves a process instance id,
starts the reader state for that process, records a single-use token, and
returns the Sintra command-line arguments the process must pass to
`sintra::init(argc, argv)`.

Signature:

```cpp
struct External_process_invitation_options
{
    sintra::instance_id_type   process_instance_id = sintra::invalid_instance_id;
    std::chrono::milliseconds  timeout{std::chrono::seconds(30)};
};

struct External_process_invitation
{
    sintra::instance_id_type               process_instance_id = sintra::invalid_instance_id;
    uint64_t                               swarm_id = 0;
    sintra::instance_id_type               coordinator_id = sintra::invalid_instance_id;
    uint32_t                               occurrence = 0;
    std::chrono::steady_clock::time_point  expires_at{};
    std::string                            token;

    bool valid() const;
    explicit operator bool() const;
    std::vector<std::string> sintra_args() const;
};

External_process_invitation create_external_process_invitation(
    const External_process_invitation_options& options = {});

bool cancel_external_process_invitation(
    sintra::instance_id_type process_instance_id);

bool cancel_external_process_invitation(
    const External_process_invitation& invitation);
```

Use when:

- A CLI tool, plugin host, test helper, or user-launched executable must join
  an already running Sintra swarm.
- The process is launched by application code, a shell, a debugger, or another
  supervisor instead of `spawn_swarm_process`.
- The first publish, RPC, or named transceiver assignment from that process
  must be readable by the coordinator.

Contract:

- Call `create_external_process_invitation` in the coordinator process after
  `sintra::init` has returned.
- If `process_instance_id` is `invalid_instance_id`, Sintra allocates a fresh
  process instance id. If a specific id is supplied, it must be a process id
  and must not already be reserved, active, joining, retained by managed-child
  custody, or fenced by an unquiesced reader generation.
- `timeout` must be positive. Pending invitations expire automatically and are
  cleaned up; shutdown also cancels pending invitations.
- Within one active coordinator runtime, each accepted reservation receives a
  monotonically increasing, non-zero `occurrence` for that process instance
  id. Together with the process id and owning swarm/runtime, it identifies one
  exact external reader generation; values can repeat after shutdown and a
  later runtime initialization. It is distinct from the custody-relative
  recovery occurrence of a managed child and must not be persisted as a
  cross-runtime identity.
- `External_process_invitation::sintra_args()` returns the `--swarm_id`,
  `--instance_id`, `--coordinator_id`, external occurrence, and attach-token
  arguments. Append them to the executable's normal command line.
- The external process calls normal `sintra::init(argc, argv)`. During init it
  claims the invitation with the token. The claim is accepted once; later
  attempts with the same arguments fail.
- Wrong-token, canceled, expired, duplicate-id, and wrong-sender attempts are
  rejected. The claim message must also carry the reserved external occurrence,
  so stale arguments cannot claim or retire a newer generation that reused the
  same process id.
- Canceling by `External_process_invitation` checks the invitation token as
  well as the process id, so an older invitation object cannot cancel a newer
  pending invitation that reused the same explicit id. Canceling by process id
  cancels the pending invitation for that id.
- The token is sensitive. Sintra does not log it. Application code should not
  print the invitation arguments or token to user-visible logs. The token plus
  swarm and coordinator data authenticates the claim; `occurrence` is neither
  a secret nor an authenticator.
- After admission, the process is a normal swarm participant and is added to
  the standard `_sintra_all_processes` and `_sintra_external_processes` groups.
- The external process may call `sintra::leave()` to depart while the swarm
  keeps running.
- External invitations do not create a lifeline pipe/handle and do not provide
  managed-child custody. Externally attached processes are never recovered;
  `enable_recovery()` has no effect for them.
- Cancel and expiry use a rejection grace period before destroying the pending
  reader. The invitation record is removed only after that exact reader has
  stopped, so the same process id cannot be reserved while an earlier reader
  generation can still consume its rings.
- After an admitted external process leaves or crashes, unpublish validates its
  stored occurrence against the current reader. Retirement requests stop, wait
  for, and erase only that exact reader. A same-id replacement remains rejected
  until the predecessor reader is quiescent.

Example:

```cpp
auto invitation = sintra::create_external_process_invitation();
if (!invitation) {
    throw std::runtime_error("could not reserve a Sintra external process");
}

std::vector<std::string> args = {"--mode", "worker"};
auto sintra_args = invitation.sintra_args();
args.insert(args.end(), sintra_args.begin(), sintra_args.end());

// Launch the executable with args through your normal process launcher.
```

The launched executable uses the ordinary init path:

```cpp
int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    Worker_service service;
    service.assign_name("worker.service");

    // Participate in the swarm...

    sintra::leave();
}
```

Failures:

- `create_external_process_invitation` returns an invalid invitation and logs
  an error when no active coordinator exists, the timeout is not positive, the
  supplied process id is not a process id, or secure token generation fails.
- It returns an invalid invitation and logs a warning when lifecycle teardown is
  in progress or the requested process id is already in use.
- `cancel_external_process_invitation` returns `false` when the invitation is
  not pending. The object overload also returns `false` when its token no
  longer matches the pending invitation for that process id.
- In the external process, `sintra::init` throws `std::runtime_error` when the
  invitation claim is rejected or cannot be completed within the bounded claim
  wait.
- If the background reader-retirement worker cannot start, Sintra logs a
  warning and retries after the current handler. If that retry also cannot
  start, it logs an error and deliberately retains the old reader as an
  availability fence: the process id cannot be invited again until runtime
  shutdown destroys the reader. This fail-safe sacrifices same-id availability
  rather than allowing two generations to share transport authority.

Example source:

- [tests/external_process_invitation_test.cpp](../../tests/external_process_invitation_test.cpp)

See also:

- [`sintra::init`](init.md)
- [`sintra::leave`](leave.md)
- [`sintra::spawn_swarm_process`](spawn_swarm_process.md)
- [`sintra::join_swarm`](join_swarm.md)
