# sintra::Process_descriptor

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`Process_descriptor` is the unit of swarm topology that `init` consumes. Each
descriptor describes one branch process: how to start it
(`Entry_descriptor`), the user arguments passed to that branch, and an
optional pinned instance id. `Entry_descriptor` selects between an
in-process entry function and an external binary.

Signature:

```cpp
struct Entry_descriptor
{
    Entry_descriptor(const std::string& binary_name);
    Entry_descriptor(int(*entry_function)());
};

struct Process_descriptor
{
    Entry_descriptor              entry;
    std::vector<std::string>      sintra_options;
    std::vector<std::string>      user_options;
    instance_id_type              assigned_instance_id = invalid_instance_id;

    Process_descriptor(
        const Entry_descriptor& aentry,
        const std::vector<std::string>& auser_options = {});

    Process_descriptor(
        const std::string& binary_path,
        const std::vector<std::string>& auser_options = {});

    Process_descriptor(
        const char* binary_path,
        const std::vector<std::string>& auser_options = {});

    Process_descriptor(int(*entry_function)());
};
```

Use when:

- Declaring a swarm with `init` or `make_branches`.
- Pinning user arguments for a specific branch (`user_options`).
- Reusing a binary path with different argument vectors across branches.

Contract:

- Exactly one of `entry.m_binary_name` or `entry.m_entry_function` is
  populated, controlled by which `Entry_descriptor` constructor was used.
- When an entry function is supplied, the spawned process re-executes the
  same binary as the starter and dispatches to the function selected by the
  branch index.
- When a binary path is supplied, the corresponding executable is launched
  on each spawn.
- `assigned_instance_id` is normally left at `invalid_instance_id`; the
  runtime picks an id at spawn time. Pinning a value is an advanced use that
  the caller must keep unique.
- `sintra_options` is reserved for runtime-internal arguments; user code
  should populate `user_options` instead.

Threading and lifecycle:

- `Process_descriptor` and `Entry_descriptor` are plain value types. They
  are typically constructed inline by `make_branches` or by the variadic
  `init` form, both of which run before the Sintra runtime is up.

Failures:

- Asserts at construction if both forms of `Entry_descriptor` are populated
  simultaneously (defensive: the public constructors prevent this).

Example source:

- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)
- [tests/init_error_spawn_failure_test.cpp](../../tests/init_error_spawn_failure_test.cpp)

See also:

- [sintra::init](init.md)
- [sintra::make_branches](make_branches.md)
- [sintra::Spawn_options](spawn_swarm_process.md)
