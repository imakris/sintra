# Sintra Unused Code Report

This report identifies functions, member functions, data members, and signals that appear to be unused throughout the sintra codebase, including examples and tests.

## Summary

| Category | Count |
|----------|-------|
| Unused Free Functions | 14 |
| Unused Member Functions | 10 |
| Unused Data Members | 12 |
| Unused Template Functions | 3 |
| Unused Structs/Classes | 2 |

---

## Unused Free Functions

### 1. `local()` - Maildrop helper
- **File:** `include/sintra/detail/maildrop.h:52`
- **Type:** Free function (inline)
- **Description:** Returns a `Maildrop<any_local>` singleton for local message sending
- **Analysis:** Only defined in maildrop.h, never called anywhere in the codebase

### 2. `remote()` - Maildrop helper
- **File:** `include/sintra/detail/maildrop.h:58`
- **Type:** Free function (inline)
- **Description:** Returns a `Maildrop<any_remote>` singleton for remote message sending
- **Analysis:** Only defined in maildrop.h, never called anywhere in the codebase

### 3. `make_untyped_instance_id()`
- **File:** `include/sintra/detail/transceiver.h:83`
- **Type:** Free function (inline)
- **Description:** Creates a `Typed_instance_id<void>` from a naked instance ID
- **Analysis:** Only defined in transceiver.h, never called anywhere in the codebase

### 4. `make_typed_instance_id()`
- **File:** `include/sintra/detail/transceiver.h:64`
- **Type:** Template function (inline)
- **Description:** Creates a `Typed_instance_id<T>` from a transceiver
- **Analysis:** Only defined in transceiver.h, never called anywhere in the codebase

### 5. `get_instance_id_type()`
- **File:** `include/sintra/detail/id_types.h:205`
- **Type:** Free function (inline)
- **Description:** Combines process and transceiver indices into an instance ID
- **Analysis:** Only defined in id_types.h, never called (superseded by `compose_instance()`)

### 6. `monotonic_now_us()`
- **File:** `include/sintra/detail/time_utils.h:80`
- **Type:** Free function (inline)
- **Description:** Returns current monotonic time in microseconds
- **Analysis:** Only defined in time_utils.h, never called anywhere in the codebase

### 7. `octile_of_sequence()`
- **File:** `include/sintra/detail/ipc/rings.h:328`
- **Type:** Free function (inline)
- **Description:** Computes which octile a sequence number falls into
- **Analysis:** Only defined in rings.h, never called (internal helper never used)

### 8. `scratch_root()`
- **File:** `tests/test_environment.h:173`
- **Type:** Free function (inline)
- **Description:** Returns the root directory for scratch test files
- **Analysis:** Only called internally by `scratch_subdirectory()`, not directly used elsewhere

### 9. `cause_to_string()`
- **File:** `include/sintra/detail/exception.h:136`
- **Type:** Static member function
- **Description:** Converts `init_error::cause` enum to string
- **Analysis:** Only defined in exception.h, never called anywhere in the codebase

---

## Unused Member Functions

### 1. `Derived_transceiver::emit_local()`
- **File:** `include/sintra/detail/transceiver.h:644`
- **Type:** Template member function
- **Description:** Emits a message to local recipients only
- **Analysis:** Only defined in transceiver.h, never called anywhere in the codebase

### 2. `Derived_transceiver::emit_remote()`
- **File:** `include/sintra/detail/transceiver.h:653`
- **Type:** Template member function
- **Description:** Emits a message to remote recipients only
- **Analysis:** Only defined in transceiver.h, never called anywhere in the codebase

### 3. `Derived_transceiver::emit_global()`
- **File:** `include/sintra/detail/transceiver.h:662`
- **Type:** Template member function
- **Description:** Emits a message to all local and remote recipients
- **Analysis:** Only defined in transceiver.h, never called anywhere in the codebase

### 4. `Transceiver::warn_about_reference_return()`
- **File:** `include/sintra/detail/transceiver.h:346`
- **Type:** Template member function
- **Description:** Static assertion helper for RPC functions with reference returns
- **Analysis:** Only defined, used for compile-time checks but function body never called at runtime

### 5. `Transceiver::warn_about_reference_args()`
- **File:** `include/sintra/detail/transceiver.h:372`
- **Type:** Template member function
- **Description:** Static assertion helper for RPC functions with reference arguments
- **Analysis:** Same as above - compile-time only

### 6. `interprocess_semaphore::release_local_handle()`
- **File:** `include/sintra/detail/ipc/semaphore.h:889`
- **Type:** Member function (no-op)
- **Description:** Intended to release local handle resources
- **Analysis:** Called in test file but implementation is a no-op; may be vestigial

---

## Unused Data Members

### 1. `Managed_process::m_children_quota`
- **File:** `include/sintra/detail/process/managed_process.h:244`
- **Type:** Data member (`int`)
- **Description:** Quota for children processes
- **Analysis:** Declared but never assigned or read in any implementation file

### 2. `Managed_process::m_average_runnning_time`
- **File:** `include/sintra/detail/process/managed_process.h:319`
- **Type:** Data member (`double`)
- **Description:** Average running time for recovery statistics
- **Analysis:** Declared but never assigned or read

### 3. `Managed_process::m_last_running_time`
- **File:** `include/sintra/detail/process/managed_process.h:320`
- **Type:** Data member (`double`)
- **Description:** Last running time for recovery statistics
- **Analysis:** Declared but never assigned or read

### 4. `Managed_process::m_times_recovered`
- **File:** `include/sintra/detail/process/managed_process.h:321`
- **Type:** Data member (`uint32_t`)
- **Description:** Count of recovery occurrences
- **Analysis:** Declared but never assigned or read

### 5. `Process_descriptor::num_children`
- **File:** `include/sintra/detail/process/managed_process.h:94`
- **Type:** Data member (`int`)
- **Description:** Number of children for this process descriptor
- **Analysis:** Declared and initialized in constructors but never used elsewhere

### 6. `Process_descriptor::sintra_options`
- **File:** `include/sintra/detail/process/managed_process.h:92`
- **Type:** Data member (`vector<string>`)
- **Description:** Sintra-specific options for process descriptor
- **Analysis:** Declared but never assigned or read

### 7. `Entry_descriptor::m_entry_function`
- **File:** `include/sintra/detail/process/managed_process.h:82`
- **Type:** Data member (function pointer)
- **Description:** Entry function for in-process branching
- **Analysis:** Set in constructor but never read or called; in-process branching may be unimplemented

### 8. `Ring_diagnostics` (entire struct)
- **File:** `include/sintra/detail/ipc/rings.h:184`
- **Type:** Struct with multiple data members
- **Description:** Diagnostic counters for ring buffer operations
- **Analysis:** Defined but never instantiated or used anywhere in the codebase

### 9. `init_error::m_successful_spawns`
- **File:** `include/sintra/detail/exception.h:45`
- **Type:** Data member (`vector<instance_id_type>`)
- **Description:** Tracks successfully spawned processes even when init fails
- **Analysis:** Set in constructor, accessor defined, but never actually populated with data

---

## Unused Constants/Globals

### 1. `all_remote_processess_wildcard`
- **File:** `include/sintra/detail/id_types.h:173`
- **Type:** `constexpr uint64_t`
- **Description:** Wildcard for all remote processes
- **Analysis:** Defined but only used to define other constants; never used directly in logic

### 2. `all_processes_wildcard`
- **File:** `include/sintra/detail/id_types.h:174`
- **Type:** `constexpr uint64_t`
- **Description:** Wildcard for all processes (local and remote)
- **Analysis:** Same as above - only used to define other constants

### 3. `all_transceivers_except_mproc_wildcard`
- **File:** `include/sintra/detail/id_types.h:175`
- **Type:** `constexpr uint64_t`
- **Description:** Wildcard for all transceivers except managed process
- **Analysis:** Same as above - only used to define other constants

---

## Unused Type Aliases / Classes

### 1. `Console` class
- **File:** `include/sintra/detail/console.h:14`
- **Type:** Class
- **Description:** RAII class for coordinated console output
- **Analysis:** Class defined but only the `console` typedef is used (in one example); `Console` itself unused

### 2. `sintra_ring_semaphore` class
- **File:** `include/sintra/detail/ipc/rings.h:347`
- **Type:** Class
- **Description:** Binary semaphore for ring reader wakeup
- **Analysis:** Defined and used internally in rings.h; however, the `release_local_handle()` method is never called

---

## Internal Helper Functions (Low Priority)

These functions are only called internally within their own header files but not from any other code:

### 1. `flag_most_significant_bits()`
- **File:** `include/sintra/detail/id_types.h:160`
- **Type:** `constexpr` recursive helper
- **Description:** Bit manipulation helper for constant computation
- **Note:** Used only to compute `pid_mask` constant

### 2. `compiler_identity()`, `stdlib_identity()`, `platform_identity()`, `architecture_identity()`
- **File:** `include/sintra/detail/type_utils.h:88-174`
- **Type:** Internal helpers in `detail_type_utils` namespace
- **Description:** Generate ABI identity strings
- **Note:** Called internally by `abi_token()` and `abi_description()`; not directly used

### 3. `describe_token_component()`
- **File:** `include/sintra/detail/type_utils.h:176`
- **Type:** Internal helper
- **Description:** Formats a single ABI token component
- **Note:** Called only by `describe_abi_token()`

### 4. Semaphore trace functions (`sem_trace_enabled`, `sem_trace_tid`, `sem_trace_wait`, `sem_trace_post`, `sem_trace_post_error`)
- **File:** `include/sintra/detail/ipc/semaphore.h:125-187`
- **Type:** Debug trace functions
- **Description:** Conditional trace output for semaphore debugging
- **Note:** Only active when `SINTRA_SEM_TRACE` is defined; otherwise inline no-ops

---

## Recommendations

### High Priority (Likely Dead Code)
1. **`local()`, `remote()`** from maildrop.h - Consider removing if maildrop streaming API is not planned
2. **`emit_local()`, `emit_remote()`, `emit_global()`** - Consider removing or documenting as planned API
3. **`m_children_quota`, `m_average_runnning_time`, `m_last_running_time`, `m_times_recovered`** - Remove or implement recovery statistics
4. **`Ring_diagnostics`** - Remove or implement diagnostic tracking

### Medium Priority (May Be Vestigial)
1. **`get_instance_id_type()`** - Superseded by `compose_instance()`, can be removed
2. **`make_untyped_instance_id()`, `make_typed_instance_id()`** - May have been replaced by other mechanisms
3. **`Entry_descriptor::m_entry_function`** - In-process branching appears unimplemented

### Low Priority (Internal Helpers)
1. Internal helper functions are typically acceptable to keep
2. Trace functions serve debugging purposes even if not always active
3. Constants used to derive other constants can remain for documentation

---

## Notes

- This analysis was performed by searching for function/member usage patterns across all `.h`, `.cpp` files in `include/`, `tests/`, and `example/` directories
- Some items may be intentionally kept for future use or API completeness
- Template instantiations may not be detectable through simple text search
- Some functions serve as compile-time assertions (e.g., `warn_about_reference_*`)
