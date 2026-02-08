# Sintra: Duplication & Simplification Report

## Executive Summary

| Area | Current LOC | Estimated Recoverable |
|------|-------------|----------------------|
| Library (`include/`) | ~18,800 | ~1,100–1,400 |
| Tests (`tests/`) | ~19,900 | ~1,100 |
| **Total** | **~38,700** | **~2,200–2,500** |

Findings are grouped into four priority tiers. **Tier 1** items yield the largest
line savings and/or require holistic changes. **Tier 4** items are architectural
observations that do not directly map to a concrete refactor but are worth
considering for the long term.

---

## Table of Contents

- [Tier 1 — High-Impact Refactors](#tier-1--high-impact-refactors)
  - [1A. Rewrite `call_function_with_message_args.h` with `std::invoke` + `std::index_sequence`](#1a-rewrite-call_function_with_message_argsh)
  - [1B. Split `spawn_detached()` and deduplicate internals](#1b-split-spawn_detached-and-deduplicate-internals)
  - [1C. Test suite — extract multi-process test harness](#1c-test-suite--extract-multi-process-test-harness)
  - [1D. Test suite — consolidate reimplemented helpers](#1d-test-suite--consolidate-reimplemented-helpers)
  - [1E. Unify Win32/POSIX signal infrastructure in `managed_process_impl.h`](#1e-unify-win32posix-signal-infrastructure)
- [Tier 2 — Medium-Impact, Targeted Deduplication](#tier-2--medium-impact-targeted-deduplication)
  - [2A. Barrier-drop-and-emit helper in `coordinator_impl.h`](#2a-barrier-drop-and-emit-helper)
  - [2B. Draining-state slot access in `coordinator_impl.h`](#2b-draining-state-slot-access)
  - [2C. Merge `pause()` and `stop()` in `managed_process_impl.h`](#2c-merge-pause-and-stop)
  - [2D. Cache-then-RPC resolve pattern in `managed_process_impl.h`](#2d-cache-then-rpc-resolve-pattern)
  - [2E. Unify file-backing `create()` in `rings.h`](#2e-unify-file-backing-create)
  - [2F. Unify page-size queries](#2f-unify-page-size-queries)
  - [2G. Extract handler dispatch in `process_message_reader_impl.h`](#2g-extract-handler-dispatch)
  - [2H. Unify reader thread startup/shutdown bookkeeping](#2h-unify-reader-thread-startupshutdown-bookkeeping)
  - [2I. Eliminate duplicate exception dispatch tables](#2i-eliminate-duplicate-exception-dispatch-tables)
  - [2J. Collapse `make_branches()` overloads in `runtime.h`](#2j-collapse-make_branches-overloads)
- [Tier 3 — Lower-Impact Cleanup](#tier-3--lower-impact-cleanup)
  - [3A. Get-current-PID triplication](#3a-get-current-pid-triplication)
  - [3B. Windows header guard duplication](#3b-windows-header-guard-duplication)
  - [3C. ASCII art section dividers](#3c-ascii-art-section-dividers)
  - [3D. `get_wtime.h` redundant with `time_utils.h`](#3d-get_wtimeh-redundant-with-time_utilsh)
  - [3E. `SINTRA_NODISCARD` macro is unnecessary](#3e-sintra_nodiscard-macro)
  - [3F. Simplify `sintra_ring_semaphore` lazy init](#3f-simplify-sintra_ring_semaphore-lazy-init)
  - [3G. Dead code removal](#3g-dead-code-removal)
  - [3H. Duplicate `to_wide` / `env_key_of` in `utility.h`](#3h-duplicate-to_wide--env_key_of)
  - [3I. Duplicate `mach_timebase_info` init in `time_utils.h`](#3i-duplicate-mach_timebase_info-init)
  - [3J. `Dispatch_lock_guard` verbose type aliases](#3j-dispatch_lock_guard-verbose-type-aliases)
  - [3K. `message_args` four `get()` overloads](#3k-message_args-four-get-overloads)
  - [3L. Redundant `file_mapping` constructor overloads](#3l-redundant-file_mapping-constructor-overloads)
  - [3M. Test `_getpid()` platform guard](#3m-test-_getpid-platform-guard)
- [Tier 4 — Architectural Observations](#tier-4--architectural-observations)
  - [4A. `platform_utils.h` is an 856-line monolith](#4a-platform_utilsh-monolith)
  - [4B. Scattered `using std::` declarations](#4b-scattered-using-std-declarations)
  - [4C. `static inline thread_local` linkage conflict](#4c-static-inline-thread_local-linkage)
  - [4D. `spinlocked_containers.h` unsafe iterators](#4d-spinlocked_containersh-unsafe-iterators)
  - [4E. `variable_buffer::Statics<void>` simplification](#4e-variable_bufferstatics-simplification)
  - [4F. Test choreography scaffolding](#4f-test-choreography-scaffolding)

---

## Tier 1 — High-Impact Refactors

### 1A. Rewrite `call_function_with_message_args.h`

**File:** `include/sintra/detail/messaging/call_function_with_message_args.h` (~200 lines)
**Estimated saving:** 130–170 lines

#### Problem

The file contains two parallel recursive template chains that manually unpack
message arguments one-by-one:

- `call_function_with_message_args_sfb(...)` for free/static functions
- `call_function_with_message_args_mfb(...)` for member functions

Both use `message_args_size<TVector>` to stop recursion and repeatedly call
`get<I>(t)` to expand arguments. The logic is duplicated across the two
backends; only the call form differs.

#### Current code (simplified pattern)

```cpp
template <typename F, typename MA, typename... Args,
          typename = enable_if_t<message_args_size<MA>::value == sizeof...(Args)>>
auto call_function_with_message_args_sfb(
    const F& f, const MA&, const Args&... args) -> decltype(f(args...))
{
    return f(args...);
}

template <typename F, typename MA, typename... Args,
          typename = enable_if_t<message_args_size<MA>::value != sizeof...(Args)>>
auto call_function_with_message_args_sfb(
    const F& f, const MA& a, const Args&... args)
{
    return call_function_with_message_args_sfb(
        f, a, args..., get<sizeof...(Args)>(a));
}

// Same pattern duplicated for member functions with (obj.*f)(args...).
```

#### Proposed replacement

```cpp
#pragma once

#include "message_args.h"
#include <functional>
#include <utility>

namespace sintra { namespace detail {

// Implementation: unpack message_args via index_sequence.
template <typename F, typename MA, std::size_t... Is>
inline auto call_with_msg_args_impl(
    F&& f, const MA& args, std::index_sequence<Is...>)
{
    return std::invoke(std::forward<F>(f), get<Is>(args)...);
}

template <typename TObj, typename F, typename MA, std::size_t... Is>
inline auto call_with_msg_args_impl(
    TObj& obj, F&& f, const MA& args, std::index_sequence<Is...>)
{
    return std::invoke(std::forward<F>(f), obj, get<Is>(args)...);
}

// Public entry point.
template <typename F, typename MA>
inline auto call_function_with_message_args(F&& f, const MA& args)
{
    constexpr auto arity = message_args_size<MA>::value;
    return call_with_msg_args_impl(
        std::forward<F>(f), args, std::make_index_sequence<arity>{});
}

// Overload for member-function + object.
template <typename TObj, typename F, typename MA>
inline auto call_function_with_message_args(
    TObj& obj, F&& f, const MA& args)
{
    constexpr auto arity = message_args_size<MA>::value;
    return call_with_msg_args_impl(
        obj, std::forward<F>(f), args, std::make_index_sequence<arity>{});
}

}} // namespace sintra::detail
```

This handles **any arity** with zero manual specializations.

#### Steps

1. Replace the entire file body with the ~40-line implementation above.
2. Use `message_args_size<MA>::value` (already defined in `message_args.h`) to
   derive the arity. No `std::tuple_size` specialization is needed.
3. Audit all call sites — there are only a handful in `transceiver_impl.h` and
   `process_message_reader_impl.h`.
4. Run the full test suite.

---

### 1B. Split `spawn_detached()` and deduplicate internals

**File:** `include/sintra/detail/utility.h`, lines 553–1192 (~640 lines in one function)
**Estimated saving:** 30–50 lines (dedup) + major readability improvement

#### Problem

`spawn_detached()` is a single function containing two ~300-line `#ifdef`
branches (Windows and POSIX). Inside the Windows branch:

- A local lambda `to_wide` (line ~579) duplicates the free function
  `to_wide_utf8()` already defined at line 397 of the same file.
- A local lambda `build_command_line` (lines ~595–633) is 38 lines of logic
  that should be a standalone helper.

The POSIX branch similarly has `env_key_of()` and merge logic, but the Windows
side compares environment keys case-insensitively via `_wcsicmp`. Any merge
helper must preserve that behavior.

#### Proposed refactor

```
spawn_detached()        // thin dispatcher
|-- spawn_detached_win32()     // ~280 lines, Windows-only
|   |-- to_wide_utf8()         // reuse existing free function (line 397)
|   |-- build_command_line()   // extract as named function
|   |-- env_key_of<wstring>()  // template, shared
|   `-- env_key_equal(wstring) // case-insensitive compare
`-- spawn_detached_posix()     // ~300 lines, POSIX-only
    |-- env_key_of<string>()   // template, shared
    `-- env_key_equal(string)  // case-sensitive compare
```

#### Implementation details

1. **Templatize `env_key_of`** — currently two separate overloads for
   `std::wstring` (line 411) and `std::string` (line 478) with identical logic:
   ```cpp
   template <typename StringT>
   inline StringT env_key_of(const StringT& entry) {
       auto pos = entry.find(typename StringT::value_type('='));
       if (pos == StringT::npos) return entry;
       return entry.substr(0, pos);
   }
   ```
   Keep a key-compare helper so Windows stays case-insensitive:
   ```cpp
   inline bool env_key_equal(const std::wstring& a, const std::wstring& b) {
       return _wcsicmp(a.c_str(), b.c_str()) == 0;
   }
   inline bool env_key_equal(const std::string& a, const std::string& b) {
       return a == b;
   }
   ```

2. **Templatize the environment-override merge** — the 15-line block at
   lines 442–458 (Windows) and 500–511 (POSIX) is identical in structure, but must use the correct equality check:
   ```cpp
   template <typename StringT>
   inline void merge_env_overrides(
       std::vector<StringT>& env,
       const std::vector<StringT>& overrides)
   {
       for (auto& ov : overrides) {
           auto key = env_key_of(ov);
           env.erase(
               std::remove_if(env.begin(), env.end(),
                   [&](const StringT& e) { return env_key_equal(env_key_of(e), key); }),
               env.end());
           env.push_back(ov);
       }
   }
   ```

3. **Replace the local `to_wide` lambda** at line 579 with a small overload
   that forwards to `to_wide_utf8()` but still handles `const char*` and nulls:
   ```cpp
   inline std::wstring to_wide_utf8(const char* value)
   {
       if (!value || !*value) return {};
       return to_wide_utf8(std::string(value));
   }
   ```

4. **Extract `build_command_line()`** as a free function above
   `spawn_detached_win32()`.

5. **Split the `#ifdef` into two named functions** called from a thin
   `spawn_detached()` dispatcher.

---

### 1C. Test suite — extract multi-process test harness

**Files:** Many test `.cpp` files (apply only to those matching the exact pattern)
**Estimated saving:** ~460 lines

#### Problem

Many test files repeat the same `main()` boilerplate:

```cpp
int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("ENV_VAR", "test_name");

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_a);
    processes.emplace_back(process_b);
    // ...

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("final-barrier", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        // read result file, check status, return 0/1
    }

    return 0;
}
```

#### Proposed implementation

Add to `tests/test_utils.h`:

```cpp
// Generic multi-process test runner.
// - Verifier signature: int(const std::filesystem::path& shared_dir)
//   It reads result files and returns 0 on success, non-zero on failure.
//   It is only called by the coordinator (non-spawned) process.
template <typename Verifier>
int run_multi_process_test(
    int argc, char* argv[],
    const char* env_var,
    const char* test_name,
    std::vector<sintra::Process_descriptor> processes,
    Verifier&& verify,
    const char* final_barrier = "final-barrier",
    const char* barrier_group = "_sintra_all_processes")
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared(env_var, test_name);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier(final_barrier, barrier_group);
    }

    sintra::finalize();

    if (!is_spawned) {
        return verify(shared.path());
    }

    return 0;
}
```

Each test `main()` then collapses to ~5 lines:

```cpp
int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc, argv,
        "PING_PONG_DIR", "ping_pong_multi",
        { process_a, process_b },
        [](const auto& dir) {
            auto lines = sintra::test::read_lines(dir / "result.txt");
            return (lines.size() == 1 && lines[0] == "ok") ? 0 : 1;
        });
}
```

**Scope notes:** Some tests do extra work in `main()` (extra barriers, custom
shutdown ordering, or additional file verification). Those should either keep
their explicit flow or use an extended helper that accepts optional pre/post
hooks and custom barrier names.

#### Affected files (subset)

| File | Current `main()` LOC |
|------|---------------------|
| `ping_pong_multi_test.cpp` | ~25 |
| `basic_pub_sub.cpp` | ~22 |
| `barrier_flush_test.cpp` | ~20 |
| `barrier_rapid_reuse_test.cpp` | ~18 |
| `barrier_stress_test.cpp` | ~20 |
| `complex_choreography_test.cpp` | ~28 |
| `rpc_append_test.cpp` | ~22 |
| `processing_fence_test.cpp` | ~20 |
| `lifecycle_handler_test.cpp` | ~22 |
| `recovery_test.cpp` | ~30 |
| *(+ 13 more with similar structure)* | ~18–25 each |

---

### 1D. Test suite — consolidate reimplemented helpers

**Files:** scattered across 20+ test files
**Estimated saving:** ~300 lines

Six utility functions are independently reimplemented across multiple test
files. Each should be consolidated into `test_utils.h` or `test_environment.h`.

#### 1D-i. `wait_for_file()` — 6 copies

| File | Line | Function name | Poll interval |
|------|------|--------------|---------------|
| `lifeline_basic_test.cpp` | 158 | `wait_for_file()` | 10ms |
| `recovery_runner_thread_test.cpp` | 36 | `wait_for_file()` | 50ms |
| `barrier_drain_and_unpublish_test.cpp` | 37 | `wait_for_file()` | 10ms |
| `rendezvous_barrier_error_paths_test.cpp` | 75 | `wait_for_file()` | 5ms |
| `managed_process_publish_test.cpp` | 109 | `wait_for_path()` | 20ms |
| `lifecycle_handler_test.cpp` | 107 | `wait_for_signal_file()` | 10ms |

There is also `wait_for_path()` in `managed_process_publish_test.cpp` and
`wait_for_file_until()` in `lifeline_basic_test.cpp`. A single helper with
timeout + poll interval plus a small deadline wrapper would cover all of these.

**Proposed canonical implementation** in `test_utils.h`:

```cpp
inline bool wait_for_file(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
    std::chrono::milliseconds poll = std::chrono::milliseconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!std::filesystem::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(poll);
    }
    return true;
}
```

Then delete all 6 local copies and replace calls with
`sintra::test::wait_for_file(path)`.

#### 1D-ii. `write_result()` — 6 copies

Found in: `complex_choreography_test.cpp:116`, `barrier_flush_test.cpp:59`,
`barrier_complex_choreography_test.cpp:111`, `basic_pub_sub.cpp:117`,
`barrier_delivery_fence_repro_test.cpp:60`,
`manual/barrier_delivery_fence_repro_test.cpp:60`.

These functions are **not identical**: some write only ok/fail + detail, while
others write extra counters and custom filenames. Two helpers cover the common
cases without forcing a single format.

**Proposed helper for simple ok/fail cases only:**

```cpp
inline void write_test_result(
    const std::filesystem::path& file,
    bool success,
    const std::string& detail = {})
{
    std::ofstream ofs(file);
    ofs << (success ? "ok" : "fail");
    if (!detail.empty()) ofs << "\n" << detail;
    ofs << "\n";
}

inline std::pair<bool, std::string> read_test_result(
    const std::filesystem::path& file)
{
    auto lines = read_lines(file);
    bool ok = !lines.empty() && lines[0] == "ok";
    std::string detail = lines.size() > 1 ? lines[1] : "";
    return {ok, detail};
}
```

For structured results (counts, multiple fields), keep a small per-test wrapper
that uses `write_lines()` / `read_lines()` from `test_utils.h`.

#### 1D-iii. `append_line()` — 3 copies

Found in: `join_swarm_midflight_test.cpp:31`,
`barrier_pathological_choreography_test.cpp:104`, `recovery_test.cpp:97`.

```cpp
inline void append_line(const std::filesystem::path& file, const std::string& line) {
    std::ofstream ofs(file, std::ios::app);
    ofs << line << "\n";
}
```

#### 1D-iv. `assert_true()` / `fail()` / `expect()` — 5 copies

Found in: `spawn_detached_test.cpp:81`, `spawn_wait_test.cpp:65`,
`interprocess_mutex_test.cpp:17`, `interprocess_file_mapping_test.cpp:19`,
`spinlock_recovery_test.cpp:31`.

The `interprocess_mutex_test.cpp` and `interprocess_file_mapping_test.cpp`
versions are character-for-character identical.

```cpp
#define SINTRA_TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << (msg) << " (" << __FILE__ \
                      << ":" << __LINE__ << ")\n"; \
            return 1; \
        } \
    } while (0)
```

#### 1D-v. Argument parsing helpers — 5 copies

Found in: `managed_process_publish_test.cpp:47`, `spawn_wait_test.cpp:41`,
`recovery_test.cpp:273`, `rendezvous_barrier_error_paths_test.cpp:50`,
`lifeline_basic_test.cpp:110`.

```cpp
inline bool has_argv_flag(int argc, char* argv[], std::string_view flag) {
    for (int i = 1; i < argc; ++i)
        if (flag == argv[i]) return true;
    return false;
}

inline std::string get_argv_value(
    int argc, char* argv[], std::string_view flag,
    std::string_view default_val = {})
{
    for (int i = 1; i + 1 < argc; ++i)
        if (flag == argv[i]) return argv[i + 1];
    return std::string(default_val);
}

inline std::string get_binary_path(int argc, char* argv[]) {
    (void)argc;
    return argv[0];
}
```

#### 1D-vi. `disable_core_dumps_for_intentional_crash()` — 6 copies

Found in: `recovery_test.cpp:107`, `lifecycle_handler_test.cpp:152`,
`lifeline_basic_test.cpp`, `barrier_drain_and_unpublish_test.cpp`,
`recovery_runner_thread_test.cpp`, `spinlock_recovery_test.cpp`.

Note: some files name this helper differently (e.g. `disable_core_dumps_for_intentional_abort`)
and guard it to specific platforms (Apple-only in `recovery_test.cpp`).

Add to `test_environment.h`:

```cpp
inline void prepare_for_intentional_crash()
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
    struct rlimit rl{};
    getrlimit(RLIMIT_CORE, &rl);
    rl.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &rl);
#endif
}
```

---

### 1E. Unify Win32/POSIX signal infrastructure

**File:** `include/sintra/detail/process/managed_process_impl.h`
**Estimated saving:** ~40+ lines

#### Problem

- `drain_pending_signals()` (POSIX, line ~1340) and
  `drain_pending_signals_win()` (Windows, line ~1350) have **identical bodies**
  ??? the function contains no platform-specific code, only the name differs.

- `signal_slot` is **platform-specific** (Windows stores a function pointer,
  POSIX stores a `sigaction`), so both definitions are required.

- `signal_index` is forward-declared **three times** (lines ~83, ~114, ~284).

#### Fix

1. Keep one `drain_pending_signals()` with no platform suffix, guarded only at
   the call site.
2. Keep both `signal_slot` definitions (they differ by platform).
3. Reduce `signal_index` forward declarations to one.

---

## Tier 2 — Medium-Impact, Targeted Deduplication

### 2A. Barrier-drop-and-emit helper

**File:** `include/sintra/detail/process/coordinator_impl.h`
**Estimated saving:** ~25 lines

#### Problem

The 20-line block that collects barrier completions, schedules deferred
emission via `run_after_current_handler`, and notifies `m_all_draining_cv` is
copy-pasted between `unpublish_transceiver()` (lines ~530–582) and
`begin_process_draining()` (lines ~639–679). The only differences:

- `unpublish` also calls `remove_process()`.
- The lambda lock scope differs slightly.

#### Proposed extraction

```cpp
void collect_and_schedule_barrier_completions(
    instance_id_type iid,
    bool also_remove_process)
{
    auto completed = drop_from_barriers_and_collect(iid);
    if (also_remove_process) {
        remove_process(iid);
    }
    if (!completed.empty()) {
        run_after_current_handler([this, completed = std::move(completed)] {
            for (auto& bc : completed) {
                emit_barrier_completion(bc);
            }
        });
    }
    m_all_draining_cv.notify_all();
}
```

Note: keep the existing lock ordering around `m_groups_mutex` and the
`run_after_current_handler` lambda. The helper should not move that lock
inside/outside in a way that changes sequencing.

---

### 2B. Draining-state slot access

**File:** `include/sintra/detail/process/coordinator_impl.h`
**Estimated saving:** ~16 lines

The `get_process_index()` → bounds-check → `m_draining_process_states[slot]`
access pattern appears 4 times (lines ~431, ~522, ~633, ~687).

```cpp
std::optional<size_t> draining_slot_of(instance_id_type iid) const {
    auto idx = get_process_index(iid);
    if (idx < 0 || static_cast<size_t>(idx) >= m_draining_process_states.size())
        return std::nullopt;
    return static_cast<size_t>(idx);
}
```

---

### 2C. Merge `pause()` and `stop()`

**File:** `include/sintra/detail/process/managed_process_impl.h`, lines ~2234–2282
**Estimated saving:** ~20 lines

These two methods differ only in:
- The guard condition.
- The reader action (`pause` vs `stop_nowait`).
- The target state.

```cpp
enum class comm_transition { pause, stop };

void transition_communication(comm_transition t)
{
    // common preamble (lock, state check)
    auto action = (t == comm_transition::pause)
        ? &Reader::pause : &Reader::stop_nowait;
    auto target_state = (t == comm_transition::pause)
        ? State::paused : State::stopped;
    // ... common body
}

void pause() { transition_communication(comm_transition::pause); }
void stop()  { transition_communication(comm_transition::stop);  }
```

---

### 2D. Cache-then-RPC resolve pattern

**File:** `include/sintra/detail/process/managed_process_impl.h`
**Estimated saving:** ~15 lines

`get_type_id()` (lines ~986–1005) and `get_instance_id()` (lines ~1014–1035)
are structurally identical: check local cache → RPC to coordinator → cache result.

```cpp
template <typename Map, typename Key, typename RpcFn, typename Invalid>
auto cached_resolve(Map& cache, const Key& key, RpcFn&& rpc, Invalid invalid)
{
    {
        auto scoped = cache.scoped();
        auto it = scoped.get().find(key);
        if (it != scoped.get().end()) return it->second;
    }

    // Keep the explicit temporary; it matters when the coordinator is local.
    auto result = rpc(key);
    if (result != invalid) {
        cache[key] = result;
    }
    return result;
}
```

---

### 2E. Unify file-backing `create()`

**File:** `include/sintra/detail/ipc/rings.h`
**Estimated saving:** ~30 lines

`Ring_data::create()` (lines ~557–592) and `Ring::create()` (lines ~1388–1418)
are structurally similar: create file → debug-fill or truncate → close. Both
use the "UNINITIALIZED" fill pattern (not `0xCD`). `Ring_data::create()` also
ensures the directory exists before creating the data file, so any helper needs
to either accept a `create_directory` flag or be used from a wrapper.

```cpp
inline void create_ring_backing_file(
    const std::filesystem::path& path,
    size_t size,
    bool debug_fill = false)
{
    // create, optionally fill, truncate to exact size, close
}
```

---

### 2F. Unify page-size queries

**Files:** `file_mapping.h:243–257`, `platform_utils.h:77–111`
**Estimated saving:** ~15 lines

`file_mapping.h` defines its own `get_page_size()` in an anonymous namespace,
while `platform_utils.h` has the more robust `system_page_size()`. The former
should simply call the latter (or include a tiny local wrapper to avoid pulling
in the full `platform_utils.h` dependency):

```cpp
// file_mapping.h — replace anonymous namespace function with:
inline size_t get_page_size() { return system_page_size(); }
```

Or just use `system_page_size()` directly at the call sites.

---

### 2G. Extract handler dispatch

**File:** `include/sintra/detail/messaging/process_message_reader_impl.h`
**Estimated saving:** ~20 lines

Lines ~323–342 (local events) and ~410–451 (remote events) are near-identical
handler-lookup-and-dispatch blocks. The only difference is the sender-ID array:

- Local:  `{sender_id, any_local, any_local_or_remote}`
- Remote: `{sender_id, any_remote, any_local_or_remote}`

```cpp
void dispatch_handlers(
    const Message& m,
    std::initializer_list<instance_id_type> scope_ids)
{
    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);
    auto& ah = s_mproc->m_active_handlers;
    auto it_mt = ah.find(m.message_type_id);
    if (it_mt == ah.end()) return;

    for (auto sid : scope_ids) {
        auto shl = it_mt->second.find(sid);
        if (shl != it_mt->second.end()) {
            for (auto& e : shl->second) {
                e(m);
            }
        }
    }
}
```

Then the two call sites become one-liners.

Note: the remote-event path currently has optional `SINTRA_TRACE_WORLD` logging.
If you extract a helper, keep a trace hook or a flag so that logging remains
identical.

---

### 2H. Unify reader thread startup/shutdown bookkeeping

**File:** `include/sintra/detail/messaging/process_message_reader_impl.h`
**Estimated saving:** ~25 lines

The `lock / increment reader count / unlock` + `start_reading` +
`set_running` startup pattern appears at lines ~272–281 and ~562–586.
The mirror shutdown pattern appears at lines ~483–493 and ~721–731.

Extract `begin_reading_session()` and `end_reading_session()` helpers.

Note: the request reader also sets TLS (`tl_is_req_thread`,
`s_tl_current_request_reader`) and updates request-specific progress flags.
Any shared helper needs parameters or wrapper lambdas to keep those semantics.

---

### 2I. Eliminate duplicate exception dispatch tables

**File:** `include/sintra/detail/exception_conversions_impl.h`, lines ~87–156
**Estimated saving:** ~30 lines

`string_to_exception()` has both a `switch` (reserved IDs) and an
`unordered_map` (type IDs) with overlapping sets of exception types. The
reserved-id path is still necessary because those IDs are not the same as
`get_type_id<T>()`.

**Approach:** Use a shared list (constexpr array or macro list) to generate
both the switch cases and the map entries, keeping the two lookup paths but
eliminating the duplicated type list:

```cpp
#define SINTRA_EXCEPTION_LIST(X) \
    X(std::exception,          "std::exception") \
    X(std::runtime_error,      "std::runtime_error") \
    X(std::logic_error,        "std::logic_error") \
    X(std::invalid_argument,   "std::invalid_argument") \
    // ...
```

---

### 2J. Reduce `make_branches()` repetition (keep multiplicity support)

**File:** `include/sintra/detail/runtime.h`, lines ~59–138
**Estimated saving:** ~35 lines

Six overloads of `make_branches()` plus two recursive `collect_branches`
helpers. The overloads are not fully collapsible because the API supports
`int multiplicity, Process_descriptor` pairs. Any refactor should keep that
behavior.

```cpp
// Keep the existing overloads but reduce repetition by sharing the
// "create vector + call collect_branches" steps in a small helper.
// The multiplicity overloads still need to remain explicit.
```

---

## Tier 3 — Lower-Impact Cleanup

### 3A. Get-current-PID triplication

**Files and locations:**

| File | Line | Function | Return type |
|------|------|----------|-------------|
| `process_id.h` | 18–25 | `get_current_process_id()` | `process_id_type` (uintmax_t) |
| `platform_utils.h` | 259–265 | `get_current_pid()` | `uint32_t` |
| `debug_pause.h` | 45–49 | inline `getpid()` call | `auto` |

All three are the same `#ifdef _WIN32 GetCurrentProcessId() #else getpid()`.

**Fix:** Keep `process_id.h::get_current_process_id()` as the single canonical
implementation (it is the smallest, lowest-level header). Have
`platform_utils.h::get_current_pid()` and the inline call in `debug_pause.h`
delegate to it, with the appropriate cast.

---

### 3B. Windows header guard duplication

`#ifndef NOMINMAX / #define NOMINMAX / WIN32_LEAN_AND_MEAN` appears in 4 files:

1. `sintra_windows.h:27–32` — the intended canonical location
2. `platform_utils.h:28–32`
3. `time_utils.h:16–20`
4. `sintra.h:41`

**Fix:** `platform_utils.h` and `time_utils.h` should `#include "sintra_windows.h"`
instead of including `<Windows.h>` directly.

---

### 3C. ASCII art section dividers

Decorative multi-line ASCII art banners total ~120 lines across:

- `rings.h` (~60 lines of `/*****/` and box-drawing banners)
- `message.h` (~80 lines)
- `transceiver.h` (~48 lines)
- `managed_process_impl.h` (~12 lines)
- `call_function_with_message_args.h` (~16 lines)

**Fix:** Replace with single-line `// === Section Name ===` comments.

---

### 3D. `get_wtime.h` redundant with `time_utils.h`

`get_wtime()` returns `double` seconds. `monotonic_now_ns()` (in `time_utils.h`)
returns `uint64_t` nanoseconds.

**Fix:** Add `get_wtime()` next to `monotonic_now_ns()` in `time_utils.h` and
have `get_wtime.h` forward to it (or keep `get_wtime.h` as the thin wrapper
if you want to avoid pulling `time_utils.h` into every include site):

```cpp
inline double get_wtime() {
    return static_cast<double>(monotonic_now_ns()) * 1e-9;
}
```

---

### 3E. `SINTRA_NODISCARD` macro

**File:** `rings.h:2806–2810`

```cpp
#if __cplusplus >= 202002L
#  define SINTRA_NODISCARD [[nodiscard]]
#else
#  define SINTRA_NODISCARD
#endif
```

`[[nodiscard]]` is standard C++17, which sintra already requires. The check
for C++20 is wrong. **Fix:** Remove the macro entirely, replace all uses with
`[[nodiscard]]` directly.

---

### 3F. Simplify `sintra_ring_semaphore` lazy init

**File:** `rings.h`, lines ~350–493
**Estimated saving:** ~40 lines

Manual 3-state atomic + placement-new + `std::aligned_storage_t` (deprecated
in C++23). Replace with:

```cpp
std::once_flag m_init_flag;
std::optional<sintra_semaphore> m_sem;

sintra_semaphore& get() {
    std::call_once(m_init_flag, [this] { m_sem.emplace(/* args */); });
    return *m_sem;
}
```

Note: `std::call_once` is one-shot. This is fine as long as the semaphore is
not expected to be re-initialized after destruction within the same object.

---

### 3G. Dead code removal

| Item | File | Lines |
|------|------|-------|
| `class No` (unused empty class) | `message.h` | 282–287 |
| `sintra_clone_message` (unused) | `process_message_reader_impl.h` | 46–57 |

**Fix:** Delete both.

---

### 3H. Duplicate `to_wide` / `env_key_of`

**File:** `utility.h`

- `to_wide_utf8()` (line 397) and `to_wide` lambda (line 579): delete the
  lambda, call the free function.
- `env_key_of(wstring)` (line 411) and `env_key_of(string)` (line 478):
  replace both with a single template (see [1B](#1b-split-spawn_detached-and-deduplicate-internals)).

---

### 3I. Duplicate `mach_timebase_info` init

**File:** `time_utils.h`

Lines 55–61 and 87–91 each independently cache `mach_timebase_info_data_t`
with different patterns (one uses a function-local static, the other an
inline block).

**Fix:** Extract a shared helper:

```cpp
inline const mach_timebase_info_data_t& get_timebase_info() {
    static mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    return info;
}
```

---

### 3J. `Dispatch_lock_guard` verbose type aliases

`Dispatch_lock_guard<std::shared_lock<std::shared_mutex>>` appears 11 times;
`Dispatch_lock_guard<std::unique_lock<std::shared_mutex>>` appears 7 times
across `transceiver_impl.h`, `coordinator_impl.h`, and
`managed_process_impl.h`.

**Fix:** Add type aliases near the `Dispatch_lock_guard` definition:

```cpp
using Dispatch_shared_lock = Dispatch_lock_guard<std::shared_lock<std::shared_mutex>>;
using Dispatch_unique_lock = Dispatch_lock_guard<std::unique_lock<std::shared_mutex>>;
```

This is a readability improvement more than a LOC reduction, but it reduces
line width significantly at 18 call sites.

---

### 3K. `message_args` four `get()` overloads

**File:** `message_args.h`, lines ~108–133

Four cv/ref-qualified overloads of `get<I>()`. `message_args` is not a tuple
and does not expose `m_tuple`, so this cannot be collapsed into a trivial
`std::get` forwarding overload. If you want to reduce repetition, factor the
shared body into a small helper (e.g. `get_impl<storage_t>(value)`), and keep
the four overloads thin.

---

### 3L. Redundant `file_mapping` constructor overloads

**File:** `file_mapping.h`, lines ~41–58

Three constructors for `const char*`, `std::string`, and
`std::filesystem::path`. The `const char*` overload currently provides a null
check; dropping it would lose that validation. If you want to dedupe, keep the
`const char*` overload (for the guard) and forward the `std::string` overload
to the `path` overload. Saves a few lines without reducing input validation.

---

### 3M. Test `_getpid()` platform guard

**Files:** 11 test files, ~20 occurrences of:

```cpp
#ifdef _WIN32
    auto pid = _getpid();
#else
    auto pid = getpid();
#endif
```

**Fix:** Add to `test_environment.h`:

```cpp
inline auto get_pid() {
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}
```

Saves ~40 lines of `#ifdef` blocks.

---

## Tier 4 — Architectural Observations

These are broader structural opportunities that do not map to a single targeted
refactor. They're worth considering if the project is undergoing a larger
restructuring pass.

### 4A. `platform_utils.h` monolith

At 856 lines, this file contains:

- Page size, system info
- File handle utilities, write helpers
- PID/TID retrieval
- Process liveness (`is_process_alive`)
- Process start timestamps (`query_process_start_stamp`)
- Run marker I/O
- Cache line detection + mismatch warnings
- Directory utilities
- Modular arithmetic helpers

**Suggestion:** Split into ~3 focused headers:

```
platform_utils.h          → platform_defs.h      (types, page size, PID/TID)
                          → process_utils.h      (liveness, timestamps, run markers)
                          → file_utils.h         (write helpers, directory ops)
```

This does not reduce LOC but significantly improves navigability. Each sub-file
also presents more deduplication opportunities when its scope is clear.

### 4B. Scattered `using std::` declarations

Multiple files import the same standard names into the `sintra` namespace:

| Declaration | Occurrences | Files |
|-------------|-------------|-------|
| `using std::mutex` | 5 | utility, transceiver, coordinator, managed_process |
| `using std::string` | 9 | resolvable_instance, transceiver, coordinator, managed_process, message, reader |
| `using std::function` | 5 | utility, transceiver, managed_process, reader |
| `using std::condition_variable` | 4 | transceiver, coordinator, managed_process, reader |
| `using std::lock_guard` | 2 | utility, coordinator |
| *(total ~40 declarations)* | | |

**Options:**

1. **Centralize:** Create `std_imports.h` included by all internal headers.
2. **Eliminate:** Switch to explicit `std::` qualification everywhere (more
   verbose but avoids namespace pollution for downstream includers of the
   header-only library).

Option 2 is the cleaner long-term choice for a public header-only library,
though it increases verbosity.

### 4C. `static inline thread_local` linkage conflict

**File:** `process_message_reader.h`, lines ~48–55

```cpp
static inline thread_local Managed_process* s_mproc = nullptr;
```

`static` gives internal linkage; `inline` gives external linkage; `static`
wins. If these variables are meant to be shared across TUs (which the usage
pattern suggests — they're in a header), `static` should be removed, leaving
`inline thread_local`. This is a correctness issue for multi-TU consumers: the
request-reader thread sets TLS in one TU, while callers in another TU read a
different `static` TLS slot.

### 4D. `spinlocked_containers.h` unsafe iterators

Methods like `begin()`, `end()`, `find()`, `front()`, `back()` each acquire
and release the lock independently, returning an unprotected iterator or
reference. The safe `scoped_access` pattern coexists with these unsafe methods.

**Suggestion:** Consider deprecating or removing the non-scoped accessors.
This is a correctness concern, not a LOC concern, but if the unsafe methods
are removed, the class shrinks by ~30 lines.

### 4E. `variable_buffer::Statics<void>` simplification

**File:** `message.h`, lines ~83–89

This template wrapper can be replaced with direct `inline thread_local`
variables since the project already targets C++17.

### 4F. Test choreography scaffolding

Five choreography tests total ~4,055 LOC:

| File | LOC |
|------|-----|
| `complex_choreography_test.cpp` | 737 |
| `complex_choreography_stress_test.cpp` | 924 |
| `choreography_extreme_test.cpp` | 977 |
| `extreme_choreography_test.cpp` | 896 |
| `barrier_complex_choreography_test.cpp` | 521 |

They share: barrier-name generation functions, round-based phase loops,
conductor/worker/aggregator/verifier actor patterns, and file-based result
verification. Each tests genuinely different synchronization patterns, so they
aren't pure duplicates.

**Suggestion:** Create `tests/test_choreography_utils.h` extracting:

- `make_barrier_name(prefix, phase, round)` — consolidates 6+ independently
  defined barrier-name functions.
- `write_choreography_result()` / `read_choreography_result()` — the result
  file format used by all five tests.
- A phase-loop helper struct managing per-round barrier naming and stop-signal
  propagation.

Estimated saving: ~200 lines.

---

## Summary

| Tier | Items | Est. LOC Saved |
|------|-------|---------------|
| **Tier 1** (5 items) | 1A–1E | ~900–1,020 |
| **Tier 2** (10 items) | 2A–2J | ~230 |
| **Tier 3** (13 items) | 3A–3M | ~350 |
| **Tier 4** (6 items) | 4A–4F | structural |
| **Grand Total** | | **~1,480–1,600** (conservative) |

With ASCII art removal (~120 lines) and Tier 4F (~200 lines), the total
reaches **~2,200–2,500 lines**, or roughly **6% of the full codebase**.
