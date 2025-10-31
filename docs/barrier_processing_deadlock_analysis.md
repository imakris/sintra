

## Current Experiments

Several code spikes were attempted to break the circular wait, none of which landed cleanly in the current tree. The following observations explain why they did not ship and clarify the remaining work:

- Running the ack lambda synchronously (instead of deferring via `run_after_current_handler`) eliminates the circular dependency, but it also reintroduces the exact spinlock crashes that triggered the refactor in the first place: the sync path touches coordinator-owned containers while the request reader still holds the spinlocked data structures that were being torn down. Without reworking the teardown ordering, that approach is unsafe.

- Redirecting the reply through a new signal (`Managed_process::barrier_ack_notify` -> `Process_group::barrier_ack_signal`) breaks the dependency on `tl_post_handler_function`, but the participants are not allowed to send signals of coordinator types. The type system enforces this (see the `sender_capability` static assertion in `Transceiver::send`). Introducing a dedicated notification signal would require widening the allowed sender set or routing through another transceiver (for example, via a neutral broker). That work was not completed.

- Emitting the notification from a detached thread (to unblock the post-handler queue) sidesteps the `run_after...` constraint, but it pushes coordinator RPCs onto a brand new thread without any of the existing teardown guards. This raises new ordering issues and still relies on the coordinator RPC path we are trying to retire.

Any final solution must deliver the barrier acknowledgement to the coordinator on a path that (a) does not depend on the blocked barrier RPC, and (b) maintains the safety guarantees that motivated the refactor (no spinlocked container access during teardown).
