# Dynamic swarm branch review

This document captures a side-by-side analysis of the dynamic swarm feature
branches fetched from `origin`.

## Branch scope overview

| Branch | Coordinator APIs | Managed process/runtime | Example & tests |
| --- | --- | --- | --- |
| `codex/add-test-for-example-6-w2kzoc` | No coordinator changes beyond master.【F:docs/dynamic_swarm_branch_review.md†L116-L124】 | Introduces branch registry (`auto_start`, `spawn_registered_branch`) enabling deferred spawns, but without RPC hooks back to the coordinator.【F:docs/dynamic_swarm_branch_review.md†L131-L143】 | Ships the first dynamic swarm example/test pair with controller-driven join/leave flow.【F:docs/dynamic_swarm_branch_review.md†L116-L124】 |
| `codex/implement-dynamic-process-entry-and-exit` | Adds a write-only `add_process_to_group` RPC so manually spawned branches re-register into `_sintra_*` groups.【F:docs/dynamic_swarm_branch_review.md†L39-L49】 | Retains per-branch spawn templates and exposes `spawn_branch` returning an instance id, but lacks cleanup for departures.【F:docs/dynamic_swarm_branch_review.md†L56-L77】 | Example/test rely on controller logic similar to later branches, yet expect RPC assistance for group membership on join only. |
| `codex/implement-dynamic-process-entry-and-exit-dnyuom` | Reuses coordinator publish path to auto-enroll late joiners into `_sintra_*` groups without new RPCs.【F:docs/dynamic_swarm_branch_review.md†L96-L110】 | No managed process/runtime changes; relies entirely on coordinator bookkeeping. | Bundles the dynamic swarm example/test but without runtime helpers (requires manual coordination). |
| `codex/implement-dynamic-process-entry-and-exit-doentf` | Extends coordinator RPC surface with `add_process_to_group`/`remove_process_from_group` so dynamic spawns can be rolled back cleanly.【F:docs/dynamic_swarm_branch_review.md†L152-L176】 | Combines branch registry + RPC helpers, providing `spawn_branch` count helpers and rollback on failure.【F:docs/dynamic_swarm_branch_review.md†L182-L216】 | Most complete example/test pair demonstrating spawn, readiness, and departure orchestration via controller events.【F:docs/dynamic_swarm_branch_review.md†L223-L238】 |

## Branch details

### `codex/implement-dynamic-process-entry-and-exit`

```
$ git diff --stat origin/master..origin/codex/implement-dynamic-process-entry-and-exit
 docs/architecture.md                              |  11 +++
 example/sintra/CMakeLists.txt                     |   3 +
 example/sintra/sintra_example_6_dynamic_swarm.cpp | 289 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 include/sintra/detail/coordinator.h               |   4 +
 include/sintra/detail/coordinator_impl.h          |  12 +++
 include/sintra/detail/id_types.h                  |   1 +
 include/sintra/detail/managed_process.h           |  10 +++
 include/sintra/detail/managed_process_impl.h      | 121 +++++++++++++++++++++++-----
 include/sintra/detail/runtime.h                   |   9 +++
 tests/CMakeLists.txt                              |   1 +
 tests/dynamic_swarm_test.cpp                      | 307 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 tests/run_tests.py                                | 276 --------------------------------------------------------------
```

The coordinator gains a minimal helper so the runtime can manually append late
joiners into default groups during `spawn_branch`:

```cpp
// include/sintra/detail/coordinator_impl.h (origin/codex/implement-dynamic-process-entry-and-exit)
inline void Coordinator::add_process_to_group(const string& name, instance_id_type process_iid)
{
    lock_guard<mutex> lock(m_groups_mutex);
    auto it = m_groups.find(name);
    if (it == m_groups.end()) {
        return;
    }
    it->second.add_process(process_iid);
    m_groups_of_process[process_iid].insert(it->second.m_instance_id);
}
```

The runtime keeps per-branch spawn templates and exposes a `spawn_branch`
helper that returns the spawned instance id after forcing group membership:

```cpp
// include/sintra/detail/managed_process_impl.h (origin/codex/implement-dynamic-process-entry-and-exit)
inline instance_id_type Managed_process::spawn_branch(size_t branch_index)
{
    if (!s_coord) { return invalid_instance_id; }
    if (branch_index == 0 || branch_index > m_branch_spawn_templates.size()) { return invalid_instance_id; }
    const auto& spawn_template = m_branch_spawn_templates[branch_index - 1];
    const auto assigned_instance_id = make_process_instance_id();
    std::vector<std::string> sintra_options = spawn_template.base_sintra_options;
    sintra_options.push_back("--swarm_id");
    sintra_options.push_back(std::to_string(m_swarm_id));
    sintra_options.push_back("--instance_id");
    sintra_options.push_back(std::to_string(assigned_instance_id));
    sintra_options.push_back("--coordinator_id");
    sintra_options.push_back(std::to_string(s_coord_id));
    std::vector<std::string> all_args = {spawn_template.binary_name};
    all_args.insert(all_args.end(), sintra_options.begin(), sintra_options.end());
    all_args.insert(all_args.end(), spawn_template.user_options.begin(), spawn_template.user_options.end());
    if (!spawn_swarm_process({spawn_template.binary_name, all_args, assigned_instance_id})) { return invalid_instance_id; }
    Coordinator::rpc_add_process_to_group(s_coord_id, "_sintra_all_processes", assigned_instance_id);
    Coordinator::rpc_add_process_to_group(s_coord_id, "_sintra_external_processes", assigned_instance_id);
    return assigned_instance_id;
}
```

### `codex/implement-dynamic-process-entry-and-exit-dnyuom`

```
$ git diff --stat origin/master..origin/codex/implement-dynamic-process-entry-and-exit-dnyuom
 example/sintra/CMakeLists.txt                     |   3 +
 example/sintra/sintra_example_6_dynamic_swarm.cpp | 348 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 include/sintra/detail/coordinator_impl.h          |  34 +++++++
 tests/CMakeLists.txt                              |   1 +
 tests/dynamic_swarm_test.cpp                      | 376 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 tests/run_tests.py                                | 276 --------------------------------------------------
```

Instead of new RPCs, this branch wires the coordinator’s publish path to enroll
late joiners automatically:

```cpp
// include/sintra/detail/coordinator_impl.h (origin/codex/implement-dynamic-process-entry-and-exit-dnyuom)
if (process_iid != s_mproc_id) {
    const auto coordinator_iid = s_mproc->m_instance_id;
    if (s_mproc->m_group_all == invalid_instance_id) {
        std::unordered_set<instance_id_type> members = {coordinator_iid, process_iid};
        s_mproc->m_group_all = make_process_group("_sintra_all_processes", members);
    } else {
        std::lock_guard<mutex> lock(m_groups_mutex);
        if (auto it = m_groups.find("_sintra_all_processes"); it != m_groups.end()) {
            it->second.add_process(process_iid);
            m_groups_of_process[process_iid].insert(it->second.m_instance_id);
        }
    }
    // analogous logic for "_sintra_external_processes"
}
```

### `codex/add-test-for-example-6-w2kzoc`

```
$ git diff --stat origin/master..origin/codex/add-test-for-example-6-w2kzoc
 example/sintra/CMakeLists.txt                     |   3 +
 example/sintra/sintra_example_6_dynamic_swarm.cpp | 430 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 include/sintra/detail/managed_process.h           |  34 ++++--
 include/sintra/detail/managed_process_impl.h      | 113 ++++++++++++++----
 include/sintra/detail/runtime.h                   |   9 ++
 tests/CMakeLists.txt                              |   1 +
 tests/dynamic_swarm_test.cpp                      | 450 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 tests/run_tests.py                                | 276 ------------------------------------------
```

This branch keeps the branch registry and deferred spawn hooks but omits the
coordinator RPC updates. The runtime changes mirror the `doentf` branch (see
below) minus the success/failure rollback logic.

```cpp
// include/sintra/detail/managed_process.h (origin/codex/add-test-for-example-6-w2kzoc)
struct Process_descriptor
{
    Entry_descriptor entry;
    vector<string> sintra_options;
    vector<string> user_options;
    int num_children = 0;
    instance_id_type assigned_instance_id = invalid_instance_id;
    int branch_index = 0;
    bool auto_start = true;
    // ... constructors mirror the base type ...
};
```

### `codex/implement-dynamic-process-entry-and-exit-doentf`

```
$ git diff --stat origin/master..origin/codex/implement-dynamic-process-entry-and-exit-doentf
 example/sintra/CMakeLists.txt                     |   3 +
 example/sintra/sintra_example_6_dynamic_swarm.cpp | 430 ++++++++++++++++++++++++++++++++++++++++++++++++++
 include/sintra/detail/coordinator.h               |   6 +
 include/sintra/detail/coordinator_impl.h          |  39 +++++
 include/sintra/detail/id_types.h                  |   2 +
 include/sintra/detail/managed_process.h           |  34 +++-
 include/sintra/detail/managed_process_impl.h      | 132 +++++++++++++---
 include/sintra/detail/runtime.h                   |   9 ++
 tests/CMakeLists.txt                              |   1 +
 tests/dynamic_swarm_test.cpp                      | 599 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 tests/run_tests.py                                | 276 -------------------------------
```

The coordinator grows balanced RPCs so dynamic spawns can be inserted and
rolled back deterministically:

```cpp
// include/sintra/detail/coordinator_impl.h (origin/codex/implement-dynamic-process-entry-and-exit-doentf)
inline bool Coordinator::add_process_to_group(const string& name, instance_id_type process_iid)
{
    lock_guard<mutex> lock(m_groups_mutex);
    auto group_it = m_groups.find(name);
    if (group_it == m_groups.end()) { return false; }
    group_it->second.add_process(process_iid);
    m_groups_of_process[process_iid].insert(group_it->second.m_instance_id);
    return true;
}
inline bool Coordinator::remove_process_from_group(const string& name, instance_id_type process_iid)
{
    lock_guard<mutex> lock(m_groups_mutex);
    auto group_it = m_groups.find(name);
    if (group_it == m_groups.end()) { return false; }
    group_it->second.remove_process(process_iid);
    auto groups_it = m_groups_of_process.find(process_iid);
    if (groups_it != m_groups_of_process.end()) {
        groups_it->second.erase(group_it->second.m_instance_id);
        if (groups_it->second.size() == 0) {
            m_groups_of_process.erase(groups_it);
        }
    }
    return true;
}
```

The runtime pairs these RPCs with a branch registry so late spawns can reuse
stored command lines, opt out of auto-start, and clean up on failure:

```cpp
// include/sintra/detail/managed_process_impl.h (origin/codex/implement-dynamic-process-entry-and-exit-doentf)
std::map<int, Process_descriptor>  m_branch_registry;
mutable std::mutex                 m_branch_registry_mutex;
inline size_t Managed_process::spawn_registered_branch(int branch_index, size_t multiplicity)
{
    if (!s_coord || multiplicity == 0) { return 0; }
    std::optional<Process_descriptor> descriptor;
    {
        std::lock_guard<std::mutex> lock(m_branch_registry_mutex);
        auto it = m_branch_registry.find(branch_index);
        if (it != m_branch_registry.end()) { descriptor = it->second; }
    }
    if (!descriptor) { return 0; }
    size_t spawned = 0;
    for (size_t i = 0; i < multiplicity; ++i) {
        auto spawn_args = make_spawn_args(*descriptor, branch_index);
        const auto process_iid = spawn_args.piid;
        const bool added_all = s_coord->add_process_to_group("_sintra_all_processes", process_iid);
        const bool added_external = s_coord->add_process_to_group("_sintra_external_processes", process_iid);
        if (!added_all || !added_external) {
            if (added_all) { s_coord->remove_process_from_group("_sintra_all_processes", process_iid); }
            if (added_external) { s_coord->remove_process_from_group("_sintra_external_processes", process_iid); }
            continue;
        }
        if (spawn_swarm_process(spawn_args)) {
            ++spawned;
        }
        else {
            s_coord->remove_process_from_group("_sintra_all_processes", process_iid);
            s_coord->remove_process_from_group("_sintra_external_processes", process_iid);
        }
    }
    return spawned;
}
```

The sample controller orchestrates join/leave cycles using `spawn_branch` and
custom events:

```cpp
// example/sintra/sintra_example_6_dynamic_swarm.cpp (origin/codex/implement-dynamic-process-entry-and-exit-doentf)
if (request_spawn) {
    console() << "[Controller] inviting bench player after " << hit_number << " hits\n";
    if (spawn_branch(bench_branch_index) == 0) {
        console() << "[Controller] failed to spawn bench player\n";
        std::lock_guard<std::mutex> lock(state_mutex);
        bench_spawning = false;
    }
}
if (request_leave) {
    world() << LeaveSwarm{bench_branch_index};
}
if (request_stop) {
    console() << "[Controller] stopping rally after cooldown\n";
    world() << StopPlay{};
}
```

## Test & example execution (`codex/implement-dynamic-process-entry-and-exit-doentf`)

Commands were executed after checking out `origin/codex/implement-dynamic-process-entry-and-exit-doentf` locally and building a Debug configuration.

* `ninja -C build sintra_dynamic_swarm_test_debug` — builds succeeded but the test binary exits with `rpc_cancelled` exceptions and status 1.【4442e1†L1-L23】
* `ninja -C build sintra_example_6_dynamic_swarm` followed by multiple runs of the example showed deterministic completion messages but repeated `rpc_cancelled` noise on stderr.【06ad14†L1-L18】【e861c0†L1-L16】
* Python resource profiling recorded ~0.53s user / 0.41s system time and ~12MB RSS for the example, and ~0.59s user / 0.32s system time / ~12MB RSS for the failing test.【1f805c†L1-L27】【4442e1†L1-L23】

The absence of `/usr/bin/time` in the environment prevented use of the standard
`time` CLI; Python’s `resource` module was used instead.【544e69†L1-L1】

## Merge plan

1. **Coordinator contract** — Start from the `doentf` branch RPCs so dynamic
   spawns can be inserted and rolled back deterministically. Layer in the
   automatic enrollment logic from the `dnyuom` branch so processes spawned
   outside `spawn_branch` still join `_sintra_*` groups without RPC calls.【80828a†L1-L37】【5affb7†L1-L33】
2. **Runtime helpers** — Retain the branch registry + rollback support from
   `doentf` but slim the interface to return instance ids like the base branch
   so controllers can track explicit participants. Ensure departure paths call
   `remove_process_from_group` to avoid the lingering membership that triggers
   the observed `rpc_cancelled` shutdown noise.【70e5d0†L1-L45】【4442e1†L1-L23】
3. **Example & test** — Keep the richer controller choreography (join, ready,
   depart) from `doentf`, but audit shutdown sequencing to avoid late RPCs to
   departed peers. The current test fails reliably with `rpc_cancelled`; use
   deterministic barriers around `StopPlay` and bench departures to quiet the
   errors.【4976e2†L1-L36】【4442e1†L1-L23】
4. **Tooling** — The rewritten `tests/run_tests.py` diverges heavily from the
   existing harness. Before merging, reconcile its feature set with the current
   script or limit the change scope to dynamic-swarm additions so regression
   risk stays bounded.

Following this plan yields a combined branch that preserves the most complete
runtime API while incorporating the lightweight coordinator bookkeeping and
addressing the stability issues surfaced during test execution.
