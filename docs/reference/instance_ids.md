# instance_id_type and related helpers

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`instance_id_type` is the 64-bit identifier used to address transceivers and
processes in the Sintra runtime. It packs a process index in its high bits and
a transceiver index in its low bits. A small set of named sentinels and helper
functions are provided for constructing, decomposing, and inspecting these
ids.

Signature:
```cpp
namespace sintra {

using instance_id_type    = uint64_t;
using sequence_counter_type = uint64_t;

constexpr instance_id_type invalid_instance_id;
constexpr instance_id_type any_local;
constexpr instance_id_type any_remote;
constexpr instance_id_type any_local_or_remote;

constexpr auto invalid_sequence = ~sequence_counter_type(0);

struct decomposed_instance_id
{
    uint32_t process;
    uint64_t transceiver;
    uint32_t process_complement;
    uint64_t transceiver_complement;
};

constexpr decomposed_instance_id decompose_instance(instance_id_type instance) noexcept;
constexpr instance_id_type      compose_instance(uint32_t process, uint64_t transceiver) noexcept;

uint64_t           get_process_index(instance_id_type instance_id);
instance_id_type   process_of(instance_id_type iid);

} // namespace sintra
```

Use when:
- Storing or comparing the result of `Transceiver::instance_id()`.
- Targeting a specific process by deriving the process id from a transceiver
  id with `process_of`.
- Building or inspecting wildcard sender ids such as "all local senders".
- Reading or producing a barrier sequence value (`sequence_counter_type`),
  which is also the return type of `barrier(...)`.

Contract:
- `invalid_instance_id` is `0`. It is the default for default-constructed
  `Resolvable_instance_id` values and the value returned by name resolution
  helpers when a name is not currently published.
- `any_local` matches any transceiver in the local process other than the
  local Managed_process itself.
- `any_remote` matches any transceiver in any other process.
- `any_local_or_remote` matches any transceiver, local or remote.
- `decompose_instance(iid)` returns the four-way decomposition: the process
  index, the transceiver index, and the bitwise complement of each. The
  complement fields are useful for building wildcards over the same id space.
- `compose_instance(process, transceiver)` is the inverse and produces the
  packed `instance_id_type`.
- `get_process_index(iid)` returns the process portion of `iid`.
- `process_of(iid)` returns the instance id of the Managed_process that owns
  `iid`. The transceiver index of that result is always `1`, the reserved
  index of the local Managed_process.
- `sequence_counter_type` is the 64-bit counter type used by the Sintra
  rings for ordering messages and reporting barrier completion. The
  sentinel `invalid_sequence` is `~sequence_counter_type(0)`. Treat
  `sequence_counter_type` as opaque outside the ring helpers; the value
  returned by `barrier(...)` is compared with `invalid_sequence`.

Threading and lifecycle:
- All listed helpers are pure value-level operations and have no runtime
  dependencies. They are safe to call from any thread.

Failures:
- None. The helpers do not throw.

Example source:
- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)

See also:
- [Resolvable_instance_id](resolvable_instance_id.md)
- [Typed_instance_id](typed_instance_id.md)
- [type_id_type and helpers](type_ids.md)
