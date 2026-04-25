# sintra direct rings

Include:

```cpp
#include <sintra/rings.h>
```

Summary:

`<sintra/rings.h>` exposes Sintra's single-producer / multiple-consumer
shared-memory ring primitives directly, without the managed-process IPC
layer. The header gives access to the writer (`Ring_W`), reader
(`Ring_R`), the snapshot RAII helpers (`Ring_R_snapshot`,
`make_snapshot`, `try_snapshot_e`), the lightweight `Range<T>` view, the
`Ring_diagnostics` snapshot, capacity helpers (`aligned_capacity`,
`get_ring_configurations`), the ring exception types, and the opt-in
trait `ring_payload_traits` for non-trivially-copyable payloads.

Signature:

```cpp
namespace sintra {

template <typename T>
struct Range {
    T* begin = nullptr;
    T* end   = nullptr;
};

constexpr auto invalid_sequence = ~sequence_counter_type(0);

template <typename T>
struct ring_payload_traits {
    static constexpr bool allow_nontrivial = false;
};

struct Ring_diagnostics {
    sequence_counter_type max_reader_lag                  = 0;
    sequence_counter_type worst_overflow_lag              = 0;
    uint64_t              reader_lag_overflow_count       = 0;
    uint64_t              reader_sequence_regressions     = 0;
    uint64_t              reader_eviction_count           = 0;
    uint32_t              last_evicted_reader_index;
    sequence_counter_type last_evicted_reader_sequence    = 0;
    sequence_counter_type last_evicted_writer_sequence    = 0;
    uint32_t              last_evicted_reader_octile;
    uint32_t              last_overflow_reader_index;
    sequence_counter_type last_overflow_reader_sequence   = 0;
    sequence_counter_type last_overflow_leading_sequence  = 0;
    sequence_counter_type last_overflow_last_consumed     = 0;
    uint64_t              stale_guard_clear_count         = 0;
    uint64_t              guard_rollback_attempt_count    = 0;
    uint64_t              guard_rollback_success_count    = 0;
    uint64_t              guard_accounting_mismatch_count = 0;
};

template <typename T>
std::vector<size_t> get_ring_configurations(
    size_t min_elements, size_t max_size, size_t max_subdivisions);

inline size_t aligned_capacity(size_t requested, size_t element_size);
template <typename T> inline size_t aligned_capacity(size_t requested);

template <typename T>
struct Ring_W {
    Ring_W(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements);

    T*  write(const T* src_buffer, size_t num_src_elements);

    template <typename T2, typename... Args>
    T2* write(size_t num_extra_elements, Args&&... args);

    sequence_counter_type write_commit(const T& value);
    sequence_counter_type write_commit(const T* src_buffer, size_t num_src_elements);

    sequence_counter_type done_writing();
    void                  unblock_global();
    Range<T>              get_readable_range();

    Ring_diagnostics get_diagnostics() const noexcept;
};

template <typename T>
struct Ring_R {
    Ring_R(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements,
           size_t             max_trailing_elements = 0);

    Range<T> start_reading();
    Range<T> start_reading(size_t num_trailing_elements);
    void     done_reading();
    sequence_counter_type reading_sequence() const;
    const Range<T> wait_for_new_data();
    void done_reading_new_data();

    Ring_diagnostics get_diagnostics() const noexcept;
};

template <class Reader>
class [[nodiscard]] Ring_R_snapshot {
public:
    using element_t = typename Reader::element_t;

    Ring_R_snapshot() = default;
    Ring_R_snapshot(Reader& reader, Range<element_t> rg) noexcept;

    Ring_R_snapshot(Ring_R_snapshot&&) noexcept;
    Ring_R_snapshot& operator=(Ring_R_snapshot&&) noexcept;
    ~Ring_R_snapshot() noexcept;

    explicit operator bool() const noexcept;
    Range<element_t> range() const noexcept;
    element_t*       begin() const noexcept;
    element_t*       end()   const noexcept;
    void             dismiss() noexcept;
};

template <class Reader, class... Args>
[[nodiscard]] Ring_R_snapshot<Reader>
make_snapshot(Reader& reader, Args&&... args);

enum class Ring_R_snapshot_error : uint8_t {
    none, evicted, exception
};

template <class Reader, class... Args>
[[nodiscard]] std::pair<Ring_R_snapshot<Reader>, Ring_R_snapshot_error>
try_snapshot_e(Reader& reader, Args&&... args) noexcept;

struct ring_acquisition_failure_exception : public std::runtime_error;
struct ring_reader_evicted_exception      : public std::runtime_error;

} // namespace sintra
```

## Symbol Notes

### Range

Pointer pair returned by ring read operations.

### Ring_diagnostics

Counters captured from a writer or reader with `get_diagnostics()`.

### ring_payload_traits

Opt-in trait for a specific non-trivial payload type used with
`Ring_W::write<T2>`.

### Ring_W

The single writer for a ring. It reserves storage with `write`, publishes with
`done_writing`, or uses `write_commit` for the combined write-and-publish path.

### Ring_R

Reader for a ring. It reads snapshots through `start_reading` /
`done_reading`, or waits for new data through `wait_for_new_data` /
`done_reading_new_data`.

### Ring_R_snapshot

Move-only RAII object that calls `done_reading` when a read snapshot leaves
scope.

### Ring_R_snapshot_error

Status enum returned by `try_snapshot_e`.

### aligned_capacity

Rounds a requested element count up to a legal ring capacity.

### get_ring_configurations

Returns candidate capacities for a requested element range and maximum mapped
size.

### make_snapshot

Creates a throwing `Ring_R_snapshot`.

### try_snapshot_e

Creates a non-throwing `Ring_R_snapshot` and returns a status enum beside it.

### ring_acquisition_failure_exception

Thrown when ring construction cannot acquire files, mappings, or writer
ownership.

### ring_reader_evicted_exception

Thrown when a reader has fallen too far behind and must be reconstructed.

Use when:

- A producer/consumer pipeline needs the same shared-memory ring that
  Sintra uses internally, but without the managed-process layer
  (telemetry sinks, log streams, fan-out diagnostics).
- The application controls the creation order and lifetime of the
  backing files explicitly and does not need transceivers, RPC, or
  group barriers.
- A thin RAII wrapper over `start_reading()`/`done_reading()` is wanted:
  use `make_snapshot` (throws on failure) or `try_snapshot_e` (returns a
  status enum) instead of pairing the calls by hand.

Contract:

- Construction signatures: `Ring_W<T>` and `Ring_R<T>` take a directory
  path, a data filename, the element count, and (for the reader) an
  optional `max_trailing_elements` cap that bounds how far a snapshot
  may look back.
- `num_elements` must be a multiple of 8 (octiles). Use
  `aligned_capacity<T>(requested)` to round a desired element count up
  to the nearest legal value, or `get_ring_configurations<T>(min, max,
  count)` to enumerate page-aligned candidate sizes.
- `Ring_W::write(buffer, n)` reserves space for `n` elements and copies
  them in. It does not publish; the producer must call `done_writing()`
  to advance the leading sequence. Use the convenience
  `write_commit(value)` or `write_commit(buffer, n)` for the common
  write-and-publish pattern.
- The templated overload `Ring_W::write<T2>(num_extra_elements, args...)`
  in-place constructs a `T2` whose footprint is a multiple of `T`. By
  default `T2` must be both trivially copyable and trivially
  destructible. Specialise `ring_payload_traits<T2>::allow_nontrivial =
  true` to opt a specific payload type into non-trivial semantics.
- `Ring_R::start_reading(n)` returns a `Range<T>` view of up to `n`
  trailing elements behind the current leading sequence. `n` must not
  exceed `max_trailing_elements`. Each `start_reading` must be paired
  with `done_reading` before the next call.
- `Ring_R::wait_for_new_data()` blocks until the leading sequence advances
  and returns the newly readable range. Pair every non-empty result with
  `done_reading_new_data()` before the next wait or read operation so the
  reader guard can migrate across octiles.
- `Ring_R_snapshot` enforces that pairing by RAII. `make_snapshot(reader,
  args...)` throws on failure (typically
  `ring_reader_evicted_exception`); `try_snapshot_e(reader, args...)`
  is `noexcept` and returns the snapshot together with a
  `Ring_R_snapshot_error` (`none`, `evicted`, `exception`). Dismiss
  before destruction with `Ring_R_snapshot::dismiss()` if the consumer
  released the data through some other path.
- `invalid_sequence` is the sentinel returned by ring-aware APIs to
  mean "no sequence". Compare against it instead of any literal.
- `Ring_diagnostics` is a snapshot of counters maintained by the ring
  (max reader lag, eviction counters, stale-guard clears, accounting
  mismatches). Read with `get_diagnostics()` on either the writer or
  any reader.
- A reader that falls too far behind may be evicted by the writer when
  `SINTRA_ENABLE_SLOW_READER_EVICTION` is defined (the default). Any
  subsequent `start_reading` or `make_snapshot` throws
  `ring_reader_evicted_exception`; `try_snapshot_e` returns
  `Ring_R_snapshot_error::evicted`. The reader must be reconstructed.

Threading and lifecycle:

- Exactly one writer is permitted across processes. `Ring_W`'s
  constructor takes the cross-process ownership mutex and throws
  `ring_acquisition_failure_exception` if it is already held.
- Multiple readers are supported. Each `Ring_R` acquires a unique
  reader slot in its constructor and releases it in the destructor.
- The active snapshot count per `Ring_R` is one. Reentrant
  `start_reading` against the same reader throws `std::logic_error`.
  Use a separate `Ring_R<T>` instance for concurrent snapshots.
- `Ring_W` should be destroyed on the same thread that constructed it.
  If destroyed elsewhere, the destructor logs an error and continues.
- The last process to detach removes both the data file and the control
  file from the chosen directory.

Failures:

- `ring_acquisition_failure_exception` from the constructors when the
  data file or control file cannot be mapped, when the size does not
  match, or when an exclusive writer slot cannot be obtained.
- `ring_reader_evicted_exception` from `start_reading` or
  `make_snapshot` when the writer evicted the reader for being too far
  behind.
- `std::logic_error` from `start_reading` when called twice without an
  intervening `done_reading`.
- `std::invalid_argument` from a writer attempting to commit more than
  `num_elements / 8` elements in a single write (the single-octile
  limit).
- `static_assert` from `Ring_W::write<T2>` when `T2` is not trivially
  copyable or destructible and `ring_payload_traits<T2>::allow_nontrivial`
  is `false`.

Internal: `SINTRA_ENABLE_SLOW_READER_EVICTION`,
`SINTRA_EVICTION_SPIN_BUDGET_US`, `SINTRA_EVICTION_LAG_RINGS`, and
`SINTRA_STALE_GUARD_DELAY_MS` are tuning macros for the ring
implementation. They are not part of the user-facing API and should
only be overridden when explicitly tuning the ring for a deployment.

Example source:

- [example/sintra/sintra_example_8_ring_helpers.cpp](../../example/sintra/sintra_example_8_ring_helpers.cpp)
- [tests/ring_helpers_test.cpp](../../tests/ring_helpers_test.cpp)
- [tests/ipc_rings_tests.cpp](../../tests/ipc_rings_tests.cpp)

See also:

- [docs/guide.md (Direct Ring Helpers)](../guide.md#direct-ring-helpers)
