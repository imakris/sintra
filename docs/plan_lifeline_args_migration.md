# Implementation Plan: Migrate Lifeline from Environment Variables to Command-Line Arguments

## Overview

This plan migrates the lifeline configuration mechanism from environment variables to command-line arguments. This change improves debuggability (arguments are visible in process listings), avoids environment pollution, and aligns with how other sintra options are already passed.

## Motivation

- **Visibility**: Command-line arguments appear in `ps`, Task Manager, and debugger process views
- **Consistency**: Other sintra options (`--branch_index`, `--swarm_id`, etc.) already use arguments
- **Isolation**: Environment variables can leak to grandchild processes; arguments do not
- **Simplicity**: Single mechanism for all inter-process configuration

## Files to Modify

| File | Purpose |
|------|---------|
| `include/sintra/detail/process/managed_process.h` | Update `Lifetime_policy` struct |
| `include/sintra/detail/process/managed_process_impl.h` | Spawn-side and parse-side changes |
| `README.md` | Update documentation |
| `docs/process_lifecycle_notes.md` | Update manual launch instructions |

## Detailed Changes

### 1. `managed_process.h` — Simplify `Lifetime_policy` struct

**Location**: Lines 137-145

**Current**:
```cpp
struct Lifetime_policy
{
    bool enable_lifeline = true;
    int hard_exit_code = 99;
    int hard_exit_timeout_ms = 100;
    const char* env_handle_key_win = "SINTRA_LIFELINE_HANDLE";
    const char* env_fd_key_posix = "SINTRA_LIFELINE_FD";
    const char* disable_env = "SINTRA_LIFELINE_DISABLE";
};
```

**New**:
```cpp
struct Lifetime_policy
{
    bool enable_lifeline = true;
    int hard_exit_code = 99;
    int hard_exit_timeout_ms = 100;
};
```

**Rationale**:
- Lifeline argument names are fixed internally; per-spawn customization would not be visible to the child during `init()`
- Removes unused Windows/POSIX env keys and disable env field entirely
- Exit code and timeout remain per-spawn fields as before

---

### 2. `managed_process_impl.h` — Argument Constants

**Location**: Lines 404-434 (replace env var constants)

**Current**:
```cpp
constexpr const char* k_lifeline_exit_code_env = "SINTRA_LIFELINE_EXIT_CODE";
constexpr const char* k_lifeline_timeout_env = "SINTRA_LIFELINE_TIMEOUT_MS";
constexpr const char* k_lifeline_handle_env_default = "SINTRA_LIFELINE_HANDLE";
constexpr const char* k_lifeline_fd_env_default = "SINTRA_LIFELINE_FD";

inline bool env_flag_enabled(const char* name) { ... }
inline int env_int_value(const char* name, int fallback) { ... }
```

**New**:
```cpp
// Argument names for lifeline configuration (internal, fixed)
constexpr const char* k_lifeline_handle_arg = "--lifeline_handle";
constexpr const char* k_lifeline_exit_code_arg = "--lifeline_exit_code";
constexpr const char* k_lifeline_timeout_arg = "--lifeline_timeout_ms";
constexpr const char* k_lifeline_disable_arg = "--lifeline_disable";

// Storage for parsed lifeline values (set during init, read by start_lifeline_watcher)
static inline std::string s_lifeline_handle_value;
static inline int s_lifeline_exit_code = 99;
static inline int s_lifeline_timeout_ms = 100;
static inline bool s_lifeline_disabled = false;
```

**Rationale**:
- Use `static inline` to avoid per-translation-unit duplication in a header
- Remove `env_flag_enabled()` and `env_int_value()` — they are only used for lifeline and will be unused

---

### 3. `managed_process_impl.h` — Argument Parsing

**Location**: Lines 1378-1450 (within the argument parsing loop in `init()`)

**Add after the `--recovery_occurrence` block (around line 1443)**:

```cpp
// Lifeline arguments - these are internal and not shown in help
// Long-option only to avoid collisions with user args
if (auto value = option_value(arg, k_lifeline_handle_arg, '\0', true, i)) {
    s_lifeline_handle_value = *value;
    continue;
}

if (auto value = option_value(arg, k_lifeline_exit_code_arg, '\0', true, i)) {
    try {
        s_lifeline_exit_code = std::stoi(*value);
    }
    catch (...) {
        // Invalid value - use default
    }
    continue;
}

if (auto value = option_value(arg, k_lifeline_timeout_arg, '\0', true, i)) {
    try {
        s_lifeline_timeout_ms = std::stoi(*value);
        if (s_lifeline_timeout_ms < 0) {
            s_lifeline_timeout_ms = 0;
        }
    }
    catch (...) {
        // Invalid value - use default
    }
    continue;
}

// Note: --lifeline_disable is a flag (no value required)
if (auto value = option_value(arg, k_lifeline_disable_arg, '\0', false, i)) {
    s_lifeline_disabled = true;
    continue;
}
```

**Rationale**:
- Long-option only prevents collisions with user-provided short flags
- Parse errors silently fall back to defaults (matching current env var behavior)
- The disable flag uses `option_value()` with `requires_value=false` for consistency

---

### 4. `managed_process_impl.h` — Update `start_lifeline_watcher()`

**Location**: Lines 535-595

**Current** (abbreviated):
```cpp
inline void start_lifeline_watcher(const Lifetime_policy& policy, bool lifeline_required)
{
    if (!policy.enable_lifeline) {
        return;
    }

    if (env_flag_enabled(policy.disable_env)) {
        // ... disabled via env var
        return;
    }

#ifdef _WIN32
    const char* env_value = policy.env_handle_key_win
        ? std::getenv(policy.env_handle_key_win)
        : nullptr;
#else
    const char* env_value = policy.env_fd_key_posix
        ? std::getenv(policy.env_fd_key_posix)
        : nullptr;
#endif

    // ... parse env_value and start watcher
}
```

**New**:
```cpp
inline void start_lifeline_watcher(const Lifetime_policy& policy, bool lifeline_required)
{
    if (!policy.enable_lifeline) {
        return;
    }

    // Check if lifeline was disabled via argument
    if (s_lifeline_disabled) {
#ifndef NDEBUG
        log_lifeline_message(
            detail::log_level::warning,
            std::string("[sintra] Lifeline disabled via ") + k_lifeline_disable_arg + "\n");
#endif
        return;
    }

    // Check if handle/fd was provided via argument
    if (s_lifeline_handle_value.empty()) {
        if (lifeline_required) {
            log_lifeline_message(
                detail::log_level::error,
                "[sintra] Lifeline missing - terminating\n");
            lifeline_hard_exit(policy.hard_exit_code);
        }
        return;
    }

    // Parse the handle value
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(s_lifeline_handle_value.c_str(), &end, 10);
    if (!end || end == s_lifeline_handle_value.c_str() || errno != 0) {
        if (lifeline_required) {
            log_lifeline_message(
                detail::log_level::error,
                "[sintra] Lifeline parse failure - terminating\n");
            lifeline_hard_exit(policy.hard_exit_code);
        }
        return;
    }

    // Use parsed argument values (already validated during init)
    const int exit_code = s_lifeline_exit_code;
    const int timeout_ms = s_lifeline_timeout_ms;

#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parsed));
    std::thread(lifeline_watch_loop, handle, timeout_ms, exit_code).detach();
#else
    const int fd = static_cast<int>(parsed);
    std::thread(lifeline_watch_loop, fd, timeout_ms, exit_code).detach();
#endif
}
```

**Rationale**:
- No more `std::getenv()` calls
- Uses the static variables populated during argument parsing
- Maintains same error handling semantics (hard exit if required but missing/invalid)

---

### 5. `managed_process_impl.h` — Update Spawn Side and Argument Construction

**Location**: Lines 1794-1859

**Current** (abbreviated):
```cpp
if (s.lifetime.enable_lifeline) {
#ifdef _WIN32
    const char* env_key = s.lifetime.env_handle_key_win;
#else
    const char* env_key = s.lifetime.env_fd_key_posix;
#endif
    // ... create pipe, then:
    spawn_options.env_overrides.push_back(
        std::string(env_key) + "=" + handle_value);
    spawn_options.env_overrides.push_back(
        std::string(k_lifeline_exit_code_env) + "=" + ...);
    spawn_options.env_overrides.push_back(
        std::string(k_lifeline_timeout_env) + "=" + ...);
}
else {
    if (s.lifetime.disable_env && *s.lifetime.disable_env) {
        spawn_options.env_overrides.push_back(std::string(s.lifetime.disable_env) + "=1");
    }
}
```

**New**:
```cpp
if (s.lifetime.enable_lifeline) {
#ifdef _WIN32
    if (!create_lifeline_pipe(lifeline_read_handle, lifeline_write_handle_win, &spawn_error)) {
        spawn_ready = false;
        spawn_error_message = "Failed to create lifeline pipe";
    }
    else {
        lifeline_write_handle =
            static_cast<Managed_process::lifeline_handle_type>(
                reinterpret_cast<uintptr_t>(lifeline_write_handle_win));
        spawn_options.inherit_handles.push_back(lifeline_read_handle);
        const std::string handle_value =
            std::to_string(reinterpret_cast<uintptr_t>(lifeline_read_handle));

        // Add lifeline arguments (inserted into args, not env)
        args.push_back(k_lifeline_handle_arg);
        args.push_back(handle_value);
        args.push_back(k_lifeline_exit_code_arg);
        args.push_back(std::to_string(s.lifetime.hard_exit_code));
        args.push_back(k_lifeline_timeout_arg);
        args.push_back(std::to_string(s.lifetime.hard_exit_timeout_ms));
    }
#else
    if (!create_lifeline_pipe(lifeline_read_fd, lifeline_write_fd, &spawn_error)) {
        spawn_ready = false;
        spawn_error_message = "Failed to create lifeline pipe";
    }
    else {
        lifeline_write_handle =
            static_cast<Managed_process::lifeline_handle_type>(lifeline_write_fd);
        const std::string handle_value = std::to_string(lifeline_read_fd);

        // Add lifeline arguments (inserted into args, not env)
        args.push_back(k_lifeline_handle_arg);
        args.push_back(handle_value);
        args.push_back(k_lifeline_exit_code_arg);
        args.push_back(std::to_string(s.lifetime.hard_exit_code));
        args.push_back(k_lifeline_timeout_arg);
        args.push_back(std::to_string(s.lifetime.hard_exit_timeout_ms));
    }
#endif
}
else {
    // Lifeline disabled - add disable flag
    args.push_back(k_lifeline_disable_arg);
}

// Build argv after all internal arguments are appended
cstring_vector cargs(std::move(args));
```

**Note on argument ordering**: `s.args` can already include user args, so append lifeline args after `--recovery_occurrence` and only build `cstring_vector` once all internal args are added. Ordering is not semantically important as long as the long options are present.

---

### 6. `README.md` — Update Documentation

**Location**: Lines 216-225

**Current**:
```markdown
Environment variables used by the lifeline:
- `SINTRA_LIFELINE_HANDLE` (Windows handle value)
- `SINTRA_LIFELINE_FD` (POSIX file descriptor value)
- `SINTRA_LIFELINE_EXIT_CODE`
- `SINTRA_LIFELINE_TIMEOUT_MS`
- `SINTRA_LIFELINE_DISABLE` (set to `1` to disable lifeline in the child)

Note: spawned processes require a lifeline by default. If you launch a process
manually (outside `spawn_swarm_process`), either provide the lifeline env var or
set `SINTRA_LIFELINE_DISABLE=1` for that process.
```

**New**:
```markdown
Command-line arguments used by the lifeline:
- `--lifeline_handle <value>` — Windows HANDLE or POSIX file descriptor (numeric)
- `--lifeline_exit_code <value>` — exit code used on lifeline break (default: 99)
- `--lifeline_timeout_ms <value>` — grace period before hard exit (default: 100)
- `--lifeline_disable` — flag to disable lifeline monitoring

Note: spawned processes require a lifeline by default. If you launch a process
manually (outside `spawn_swarm_process`), either provide `--lifeline_handle` with
a valid readable pipe/handle, or pass `--lifeline_disable` to skip the check.
```

---

### 7. `docs/process_lifecycle_notes.md` — Update Manual Launch Note

**Location**: Lines 73-74

**Current**:
```markdown
- Manual launches must set `SINTRA_LIFELINE_DISABLE=1` or provide a lifeline
  env var, otherwise the process hard-exits immediately.
```

**New**:
```markdown
- Manual launches must pass `--lifeline_disable` or provide `--lifeline_handle`
  with a valid pipe/handle, otherwise the process hard-exits immediately.
```

---

## Data Flow Summary

### Parent (spawn) side:
1. Create pipe as before
2. Build argument list:
   ```
   [binary] [sintra_args...] [user_args...] --recovery_occurrence N --lifeline_handle 12345 --lifeline_exit_code 99 --lifeline_timeout_ms 100
   ```
3. Or if disabled:
   ```
   [binary] [sintra_args...] [user_args...] --recovery_occurrence N --lifeline_disable
   ```

### Child (init) side:
1. `Managed_process::init()` parses all arguments including lifeline args
2. Stores values in static variables: `s_lifeline_handle_value`, `s_lifeline_exit_code`, `s_lifeline_timeout_ms`, `s_lifeline_disabled`
3. Later, `start_lifeline_watcher()` reads these static variables instead of calling `getenv()`

Optional: filter `--lifeline_*` from `m_recovery_cmd` if recovery ever reuses the command line.

---

## Backward Compatibility

**This is a breaking change** for anyone manually launching sintra processes with the old environment variables. However:

- **Low impact**: The lifeline mechanism is internal; users normally use `spawn_swarm_process()` which handles everything
- **Clear migration**: Documentation updates make the new approach clear
- **Same behavior**: Processes spawned via sintra APIs work identically

If backward compatibility is required, a transitional period could check both args and env vars, but this adds complexity. Recommend clean break since this is internal machinery.

---

## Test Impact

### `tests/lifeline_basic_test.cpp`

| Test | Expected Behavior | Impact |
|------|-------------------|--------|
| `run_owner_case()` | Uses `spawn_swarm_process()` | No change needed — spawner adds args automatically |
| `run_missing_lifeline_case()` (line 936) | Spawns without lifeline args, expects exit 99 | Works — no `--lifeline_handle` means "missing", triggers hard exit |
| Disabled case | Uses policy with `enable_lifeline=false` | Works — spawner adds `--lifeline_disable` |

No test code changes expected. The test infrastructure uses `sintra::init()` which will parse the new arguments.

---

## Verification Steps

1. **Build**: `cmake --build build`
2. **Run lifeline tests**: `ctest -R lifeline --test-dir build`
3. **Run all tests**: `ctest --test-dir build`
4. **Verify no env vars remain**:
   ```bash
   grep -r "SINTRA_LIFELINE" include/ tests/
   # Should return no matches after changes
   ```
5. **Manual verification**: Launch a process with `--lifeline_disable` and verify it runs without a lifeline
6. **Process listing check**: Spawn a managed process and verify lifeline args appear in `ps aux` or Task Manager

---

## Implementation Order

1. Update `Lifetime_policy` struct in `managed_process.h`
2. Replace env var constants with arg constants in `managed_process_impl.h`
3. Add static storage variables for parsed lifeline values
4. Add argument parsing in `init()`
5. Update `start_lifeline_watcher()` to use parsed values
6. Update spawn side to emit arguments instead of env vars
7. Remove `env_flag_enabled()` and `env_int_value()` helper functions
8. Update `README.md`
9. Update `docs/process_lifecycle_notes.md`
10. Build and run tests
11. Verify with grep that no `SINTRA_LIFELINE` env var references remain
