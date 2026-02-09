# Spinlocked Container API Proposal

## Problem Statement

The `cleanup-refactoring` branch removed the unsafe direct accessors (`operator[]`, `find()`,
`begin()`, `end()`, etc.) from `spinlocked_containers.h` because they return references/iterators
that escape the lock scope -- a footgun that defeats the container's safety guarantees.

However, the replacement API (`scoped()`, `with_lock()`, `contains_key()`, `copy_value()`,
`set_value()`) introduced two issues:

1. **Verbosity**: Call sites like `activate_impl` went from ~7 lines to ~25 lines.
2. **Performance regression**: `dispatch_event_handlers` now copies the entire handler function
   list into a `std::vector` on every single message dispatch via `copy_value()`. This is the
   hottest path in the library.

## Design Constraints

- **No escaping iterators/references**: Methods like `find()`, `begin()`, `end()`, `operator[]`
  must NOT be exposed at the top level, as they return handles that outlive the lock.
- **Defense in depth**: Spinlocks must remain even on containers guarded by outer mutexes.
- **Readable call sites**: The API should keep call sites concise.
- **No performance regressions**: Hot paths must not allocate or copy unnecessarily.

## Proposed API

### Base `spinlocked<CT, Args...>`

Keep the current restricted base unchanged:

```cpp
// Compound access (for multi-step operations)
template <typename Fn> decltype(auto) with_lock(Fn&& fn);
scoped_access scoped() noexcept;
const_scoped_access scoped() const noexcept;

// Simple atomic operations
void clear() noexcept;
bool empty() const noexcept;
auto size() const noexcept;
auto pop_front();
template <typename... FArgs> auto push_back(FArgs&&...);

// Bulk copy/assignment
operator CT<Args...>() const;
spinlocked& operator=(const CT<Args...>&);
spinlocked& operator=(CT<Args...>&&);
```

### Additions to `spinlocked_umap<Key, T>` (and `spinlocked_map`)

```cpp
// --- Existing (keep) ---
bool     contains_key(const Key&) const;
void     set_value(K&&, V&&);

// --- NEW: find-and-copy (replaces ~12 verbose scoped blocks) ---
template <typename V = T>
std::optional<V> get_value(const Key& key) const;

// --- NEW: erase by key (replaces ~5 verbose scoped blocks) ---
size_t   erase_key(const Key& key);

// --- NEW: call fn(const value&) under lock if key exists ---
// CRITICAL: fixes dispatch_event_handlers performance regression.
// No copy, no allocation, handler list iterated under lock.
template <typename Fn>
bool     with_value(const Key& key, Fn&& fn) const;

// --- Remove ---
// copy_value() -- superseded by with_value() (zero-copy alternative)
```

### `copy_value` vs `with_value`

`copy_value` copies all elements from a list/set value into a `std::vector`. This is wasteful on
the handler dispatch hot path. `with_value` calls a lambda with `const T&` under the lock,
allowing direct iteration of the handler list with zero allocation.

`copy_value` can be removed entirely; every usage is better served by either `with_value` or
`get_value`.

## Call Site Changes

### 1. `dispatch_event_handlers` (process_message_reader_impl.h) -- HOT PATH FIX

**Before** (performance regression):
```cpp
for (auto sid : scope_ids) {
    std::vector<function<void(const Message_prefix&)>> handlers;
    if (!sender_map->copy_value(sid, handlers)) {
        continue;
    }
    for (auto& handler : handlers) {
        handler(message);
    }
}
```

**After** (zero-copy, safe):
```cpp
for (auto sid : scope_ids) {
    sender_map->with_value(sid, [&](const auto& handler_list) {
        for (const auto& handler : handler_list) {
            handler(message);
        }
    });
}
```

This is safe because `m_handlers_mutex` is already held by the caller, preventing any concurrent
modification. The spinlock is held during iteration purely as defense in depth.

### 2. `rpc_handler` (transceiver_impl.h:748-754) -- pointer lookup

**Before**:
```cpp
typename RPCTC::o_type* obj = nullptr;
{
    auto scoped_map = get_instance_to_object_map<RPCTC>().scoped();
    auto it = scoped_map.get().find(untyped_msg.receiver_instance_id);
    if (it != scoped_map.get().end()) {
        obj = it->second;
    }
}
```

**After**:
```cpp
auto obj = get_instance_to_object_map<RPCTC>()
    .get_value(untyped_msg.receiver_instance_id)
    .value_or(nullptr);
```

### 3. `rpc_impl` local fast path (transceiver_impl.h:917-924) -- pointer lookup + throw

**Before**:
```cpp
typename RPCTC::o_type* object = nullptr;
{
    auto scoped_map = get_instance_to_object_map<RPCTC>().scoped();
    auto it = scoped_map.get().find(instance_id);
    if (it == scoped_map.get().end()) {
        throw std::runtime_error("Local RPC target no longer available.");
    }
    object = it->second;
}
```

**After**:
```cpp
auto object = get_instance_to_object_map<RPCTC>()
    .get_value(instance_id)
    .value_or(nullptr);
if (!object) {
    throw std::runtime_error("Local RPC target no longer available.");
}
```

### 4. `export_rpc_impl` cleanup lambda (transceiver_impl.h:1133-1134) -- erase

**Before**:
```cpp
auto scoped_map = get_instance_to_object_map<RPCTC>().scoped();
scoped_map.get().erase(instance_id);
```

**After**:
```cpp
get_instance_to_object_map<RPCTC>().erase_key(instance_id);
```

### 5. `destroy` (transceiver_impl.h:293-295) -- erase from local pointer map

**Before**:
```cpp
auto scoped_map = s_mproc->m_local_pointer_of_instance_id.scoped();
scoped_map.get().erase(m_instance_id);
```

**After**:
```cpp
s_mproc->m_local_pointer_of_instance_id.erase_key(m_instance_id);
```

### 6. `destroy` (transceiver_impl.h:283-285) -- erase from name map

**Before**:
```cpp
auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
scoped_map.get().erase(m_cache_name);
```

**After**:
```cpp
s_mproc->m_instance_id_of_assigned_name.erase_key(m_cache_name);
```

### 7. `assign_name` (transceiver_impl.h:215-217) -- insert

**Before**:
```cpp
auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
auto rvp = scoped_map.get().insert(cache_entry);
assert(rvp.second == true);
```

**After** (use `set_value`, or keep `scoped()` if the assert is important):
```cpp
// If the assert matters for debugging:
auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
auto rvp = scoped_map.get().insert(cache_entry);
assert(rvp.second == true);

// If the assert can be dropped:
s_mproc->m_instance_id_of_assigned_name.set_value(name, m_instance_id);
```

### 8. `request_reader_function` (process_message_reader_impl.h:396-403) -- rpc handler lookup

**Before**:
```cpp
void (*handler_fn)(Message_prefix&);
{
    auto scoped_map = Transceiver::get_rpc_handler_map().scoped();
    auto it = scoped_map.get().find(m->message_type_id);
    assert(it != scoped_map.get().end());
    handler_fn = it->second;
}
```

**After**:
```cpp
auto handler_fn = Transceiver::get_rpc_handler_map()
    .get_value(m->message_type_id)
    .value_or(nullptr);
assert(handler_fn);
```

### 9. `resolve_type` (coordinator_impl.h:252-259) -- find-or-return

**Before**:
```cpp
{
    auto scoped_map = s_mproc->m_type_id_of_type_name.scoped();
    auto it = scoped_map.get().find(pretty_name);
    if (it != scoped_map.get().end()) {
        return it->second;
    }
}
```

**After**:
```cpp
if (auto found = s_mproc->m_type_id_of_type_name.get_value(pretty_name)) {
    return *found;
}
```

### 10. `resolve_instance` (coordinator_impl.h:276-281) -- find-or-return

**Before**:
```cpp
auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
auto it = scoped_map.get().find(assigned_name);
if (it != scoped_map.get().end()) {
    return it->second;
}
return invalid_instance_id;
```

**After**:
```cpp
return s_mproc->m_instance_id_of_assigned_name
    .get_value(assigned_name)
    .value_or(invalid_instance_id);
```

### 11. `cached_resolve` (managed_process_impl.h:944-952) -- find-or-miss

**Before**:
```cpp
{
    auto scoped_map = cache.scoped();
    auto it = scoped_map.get().find(key);
    if (it != scoped_map.get().end()) {
        return it->second;
    }
}
```

**After**:
```cpp
if (auto found = cache.get_value(key)) {
    return *found;
}
```

### 12. `unpublish_transceiver` (coordinator_impl.h:471-473) -- erase by name

**Before**:
```cpp
{
    auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
    scoped_map.get().erase(it->second.name);
}
```

**After**:
```cpp
s_mproc->m_instance_id_of_assigned_name.erase_key(it->second.name);
```

### 13. `reply_reader_function` (process_message_reader_impl.h:618-634) -- multi-step lookup

This site looks up a `Transceiver*`, then accesses its `m_return_handlers_mutex` and
`m_active_return_handlers` under the spinlock. This is a compound operation that genuinely needs
`scoped()` because we access multiple fields of the found value. **Keep `scoped()` here.**

Alternatively, `with_value` could work:
```cpp
s_mproc->m_local_pointer_of_instance_id.with_value(m->receiver_instance_id,
    [&](Transceiver* const& transceiver) {
        object_found = true;
        std::lock_guard<std::mutex> guard(transceiver->m_return_handlers_mutex);
        auto it2 = transceiver->m_active_return_handlers.find(m->function_instance_id);
        if (it2 != transceiver->m_active_return_handlers.end()) {
            handler_copy = it2->second;
            have_handler = true;
        }
    });
```

This holds the spinlock during the inner `m_return_handlers_mutex` acquisition. Since the inner
mutex is for return handlers (separate from handlers_mutex), this doesn't create a deadlock risk.
But it does hold the spinlock longer. The current `scoped()` approach is fine here; the new API
wouldn't improve it much.

### Sites that stay with `scoped()` / `with_lock()`

These compound operations genuinely need the extended lock scope and won't benefit from the
simple accessor methods:

- `activate_impl` (transceiver_impl.h:341-401) -- nested map insert/find with iterator capture
- `unpublish_transceiver` process cleanup (coordinator_impl.h:502-511) -- iterate + conditional erase
- `unpublished_handler` (managed_process_impl.h:1707-1715) -- iterate + conditional erase
- `unpublish_all_transceivers` (managed_process_impl.h:2430-2442) -- iterate + collect
- `call_on_availability` and related (managed_process_impl.h) -- compound with_lock operations
- `make_process_group` / `join_swarm` -- `m_groups_of_process` scoped insert
- `reply_reader_function` -- multi-field access on found value

## Summary of Impact

| Metric | Before | After |
|--------|--------|-------|
| `scoped()` blocks for simple lookups | ~12 | 0 |
| `scoped()` blocks for simple erases | ~5 | 0 |
| `copy_value()` on hot path | 1 (allocates per msg) | 0 |
| `scoped()` blocks for compound ops | ~8 | ~8 (unchanged) |
| New API methods | 0 | 3 (`get_value`, `erase_key`, `with_value`) |
| Lines saved (estimated) | | ~60-80 |

The three new methods handle ~17 of the ~35 call sites, eliminating the boilerplate scoped blocks
for the common patterns while keeping `scoped()` available for the genuinely compound operations.
