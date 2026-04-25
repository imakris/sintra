# sintra::make_branches

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`make_branches` builds the `std::vector<Process_descriptor>` consumed by
`init`. It accepts descriptors directly, accepts per-descriptor multiplicity
counts, and supports an in-place form that appends to a caller-supplied
vector. The variadic `init(argc, argv, ...)` overload routes through
`make_branches`, so writing `init(argc, argv, fnA, fnB, fnC)` is equivalent
to `init(argc, argv, make_branches(fnA, fnB, fnC))`.

Signature:

```cpp
std::vector<Process_descriptor> make_branches();

std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches);

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    const Process_descriptor& descriptor,
    Args&&... rest);

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest);

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    Args&&... rest);

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches,
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest);
```

Use when:

- Building the branch list passed to `init` from a mix of in-process entry
  functions and external binaries.
- Repeating the same descriptor a fixed number of times. Use the variadic
  form `make_branches(N, descriptor, ...)` to append `N` copies of
  `descriptor`. A non-positive multiplicity adds nothing.
- Composing the topology incrementally: call the in-place overload
  `make_branches(branches, ...)` to append further descriptors to an
  existing vector.

Contract:

- A `Process_descriptor` may be constructed from an entry function pointer
  (`int (*)()`), a binary path (`std::string`, `const char*`), or an
  explicit `Entry_descriptor` plus optional user options.
- The variadic forms walk left-to-right. A leading `int` is interpreted as a
  multiplicity that applies to the descriptor that immediately follows; any
  following descriptors revert to multiplicity one until the next integer.
- The starter process is the descriptor that is processed first; further
  descriptors become positive branch indices. `init` consumes the resulting
  vector and assigns one process per element.

Threading and lifecycle:

- `make_branches` is a pure builder. It does not touch the Sintra runtime
  and may be called before `init`.

Failures:

- Compile-time error when arguments cannot be converted to
  `Process_descriptor` or to `int` for the multiplicity slot.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/init_error_spawn_failure_test.cpp](../../tests/init_error_spawn_failure_test.cpp)

See also:

- [sintra::init](init.md)
- [sintra::Process_descriptor](process_descriptor.md)
