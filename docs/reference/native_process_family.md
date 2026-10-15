# sintra::activate_native_family

The native family is an optional process-local shutdown capability. Activate it
before `init()` and child creation. It uses the existing custody admission and
native wait owners; it does not change the shared swarm format.

```cpp
enum class Native_family_scope { TRANSITIVE, OWNED_CHILDREN };

bool activate_native_family(
    Native_family_scope scope = Native_family_scope::TRANSITIVE);
void close_native_family_admission();
Native_family_status native_family_status();
void observe_native_family_changes(const Managed_child_native_change_signal& signal);
bool request_native_family_termination(std::chrono::steady_clock::time_point deadline);
```

Windows and Linux support the default `TRANSITIVE` scope. Windows uses job
membership; Linux requires the existing sole-reaper/subreaper prerequisites.
These backends retain their descendant containment and native exit requirements.

Darwin supports explicit `OWNED_CHILDREN`. Its `native_empty` means every
admitted original managed child and external-child reservation has settled under
closed admission. It does not establish the end of a terminal session, process
group, helper, or descendant that another owner controls. Applications must join
those separate obligations before reporting complete shutdown or releasing a
terminal admission slot. `Native_family_status::scope` reports the actual scope.
Darwin rejects default `TRANSITIVE` activation; the other backends reject
`OWNED_CHILDREN` activation.

Closing admission is irreversible. A launch admitted before closure remains
pending until its original creator establishes either no birth or an actual
created occurrence. A pending launch blocks `native_empty`, including a failed
launch whose native creation result has not been settled. Original managed
custody remains responsible for native actions and exact exit. Family
termination does not bypass an original custody's action serializer. A lost
Darwin wait or retained external observer leaves the family unconfirmed.

## External children on Darwin

`Native_family_external_child` integrates a maintained external creator such as
Qt without transferring its wait authority:

1. Call `reserve()` before native creation. `CLOSED` denies creation; `ADMITTED`
   retains a pending obligation; `NOT_REQUIRED` means family mode is inactive.
2. While the creator still owns the original unreaped child, call
   `publish_created(pid, 0)`. Sintra checks its public birth identity around
   receiver-local `EVFILT_PROC/NOTE_EXIT` registration. The native argument is
   unused on Darwin. This call never waits or reaps the child.
3. Only after the creator has performed the exact native wait, call
   `observe_reaped()`. It accepts the retained original exit observation and
   settles the reservation. Process disappearance alone is insufficient.
4. If publication failed, retain the pending reservation until the creator
   confirms no birth or itself reaps that unpublished occurrence; only then
   call `cancel_uncreated()`.

Dropping a reservation does not erase its obligation. Darwin
`handoff_wait_authority()` cannot establish a replacement waiter and leaves an
unsettled obligation unconfirmed. Preserve the original creator through its
terminal receipt.

Maintained tests:

- [Darwin owned-child contract](../../tests/native_process_family_owned_children_contract_test.cpp)
- [Windows/Linux transitive contract](../../tests/native_process_family_contract_test.cpp)
- [Linux activation prerequisites](../../tests/native_process_family_prerequisite_contract_test.cpp)
