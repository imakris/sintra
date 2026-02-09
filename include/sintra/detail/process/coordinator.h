// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../id_types.h"
#include "lifecycle_types.h"
#include "../resolvable_instance.h"
#include "../resolve_type.h"
#include "../transceiver.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>


namespace sintra {


using std::condition_variable;
using std::mutex;
using std::shared_ptr;
using std::string;
using std::unordered_map;
using std::unordered_set;



struct Process_group: Derived_transceiver<Process_group>
{

    Process_group():
        Derived_transceiver<Process_group>("", make_service_instance_id())
    {
    }


    // Replace the membership set for this group; serialized with barrier state.
    void set(const unordered_set<instance_id_type>& member_process_ids);

    // barriers are deferred functions and return twice (this refers to the implementation,
    // which is transparent to the caller - for the caller they only return once).
    // the first returns 0 if it is expected to fail, or a serial number, if it is expected
    // to succeed.
    // once completed, a message is sent, that should be picked up and read only by the
    // processes waiting on the barrier. This message would contain
    // - the serial number, for identification purposes
    // - the sequence counter (i.e. at which ring sequence was the barrier completed)
    sequence_counter_type barrier(const string& barrier_name);


    struct Barrier
    {
        mutex                                   m;
        condition_variable                      cv;
        unordered_set<instance_id_type>         processes_pending;
        unordered_set<instance_id_type>         processes_arrived;
        //sequence_counter_type                   flush_sequence = 0;
        bool                                    failed = false;
        instance_id_type                        common_function_iid = invalid_instance_id;
    };

    struct Barrier_completion
    {
        instance_id_type                        common_function_iid = invalid_instance_id;
        std::vector<instance_id_type>           recipients;
    };

    unordered_map<string, shared_ptr<Barrier>>  m_barriers;
    unordered_set<instance_id_type>             m_process_ids;

    mutex m_call_mutex;
    SINTRA_RPC_STRICT_EXPLICIT(barrier)

public:
    // Remove a draining process from pending barriers and collect completions
    // for any barriers that become satisfied as a result.
    void drop_from_inflight_barriers(
        instance_id_type process_iid,
        std::vector<Barrier_completion>& completions);
    // Emit barrier completion messages for the recorded recipients (per-recipient
    // reply tokens are computed at write time).
    void emit_barrier_completions(
        const std::vector<Barrier_completion>& completions);

private:
    // Add/remove a process from the group membership (guards barrier snapshots).
    void add_process(instance_id_type process_iid);
    void remove_process(instance_id_type process_iid);

    friend struct Coordinator;
};


inline
void Process_group::set(const unordered_set<instance_id_type>& member_process_ids)
{
    std::lock_guard lock(m_call_mutex);
    m_process_ids = unordered_set<instance_id_type>(
        member_process_ids.begin(), member_process_ids.end()
    );
}


inline
void Process_group::add_process(instance_id_type process_iid)
{
    std::lock_guard lock(m_call_mutex);
    m_process_ids.insert(process_iid);
}


inline
void Process_group::remove_process(instance_id_type process_iid)
{
    std::lock_guard lock(m_call_mutex);
    m_process_ids.erase(process_iid);
}



struct Coordinator: public Derived_transceiver<Coordinator>
{

private:
    struct Pending_completion
    {
        std::string group_name;
        std::vector<Process_group::Barrier_completion> completions;
    };

    Coordinator();
    ~Coordinator();

    // EXPORTED FOR RPC
    // Resolve or allocate a type id for a pretty type name. Unknown names are
    // assigned a new id, since types are assumed to exist when referenced.
    type_id_type resolve_type(const string& pretty_name);

    // Resolve an assigned transceiver name to an instance id; returns invalid
    // if the name has not been published.
    instance_id_type resolve_instance(const string& assigned_name);

    // Block until a named instance is published. If it is not yet available,
    // registers the caller for a deferred reply and returns invalid_instance_id
    // to trigger the deferral path.
    instance_id_type wait_for_instance(const string& assigned_name);

    // Publish a transceiver instance and assigned name in the registry. Enforces
    // name uniqueness and per-process limits, emits instance_published (or
    // queues it during startup), and unblocks waiters. For Managed_process,
    // resets the draining bit for the process slot.
    instance_id_type publish_transceiver(
        type_id_type type_id, instance_id_type instance_id, const string& assigned_name);

    // Unpublish a transceiver, erase name mappings, and emit instance_unpublished.
    // If the unpublished instance is a Managed_process, this also drops it from
    // groups, marks draining, stops readers, emits lifecycle events, and may
    // trigger recovery.
    bool unpublish_transceiver(instance_id_type instance_id);

    // Mark a process as draining, remove it from in-flight barriers, and return
    // the coordinator reply-ring watermark for the caller to flush.
    sequence_counter_type begin_process_draining(instance_id_type process_iid);

    // Coordinator-local unpublish used during teardown when RPCs are unsafe
    // (e.g., service mode or coordinator-local cleanup).
    void unpublish_transceiver_notify(instance_id_type transceiver_iid);

    // Spawn a swarm branch with a stable branch index. Tracks in-flight joins
    // to avoid duplicate spawns when callers retry and inserts the process into
    // core groups before spawn.
    instance_id_type join_swarm(const string& binary_name, int32_t branch_index);

    // Create a process group transceiver with explicit membership (used for
    // built-in groups like _sintra_all_processes).
    instance_id_type make_process_group(
        const string& name,
        const unordered_set<instance_id_type>& member_process_ids);
 
    // Record that a process opted into crash recovery; recover_if_required()
    // ignores processes that did not call enable_recovery().
    void enable_recovery(instance_id_type piid);

    // Apply recovery policy and spawn replacement if required. Delegates any
    // delay/logic to the recovery runner and obeys shutdown state.
    void recover_if_required(const Crash_info& info);

    // Release join_swarm in-flight tracking and flush delayed publications once
    // all processes finish initialization.
    void mark_initialization_complete(instance_id_type process_iid);

    // Signal shutdown to cancel delayed recovery and future spawns. Recovery
    // threads check this flag before running or respawning.
    void begin_shutdown();

    // Track crash status so unpublish can distinguish crash vs normal exit,
    // then emit the crash lifecycle event.
    void note_process_crash(const Crash_info& info);

    // Dispatch lifecycle events to the configured handler (if any).
    void emit_lifecycle_event(const process_lifecycle_event& event);

public:
    // Helpers (not exported for RPC).
    // ================================================

    // Lifecycle/recovery hooks run during crash/unpublish handling and recovery
    // scheduling. See docs/process_lifecycle_notes.md for timing, threading, and
    // usage patterns (non-coordinator calls are no-ops).

    // Configure recovery policy. The policy runs on the coordinator thread
    // during recover_if_required().
    void set_recovery_policy(Recovery_policy policy);

    // Configure recovery runner. The runner runs on a recovery thread and
    // decides when to call spawn().
    void set_recovery_runner(Recovery_runner runner);

    // Configure lifecycle event handler (crash/normal exit/unpublished).
    void set_lifecycle_handler(Lifecycle_handler handler);

    // Configure the drain timeout for wait_for_all_draining(). A value of 0
    // means wait indefinitely. Default is 20 seconds.
    void set_drain_timeout(std::chrono::seconds timeout);

    // Blocks until all processes identified by process_group_id have called the function.
    // num_absences may be used by a caller to specify that it is aware that other callers will
    // not make it to the barrier, thus prevent a deadlock.
    // NOTE: If more than one callers are aware of the absence of some other caller, only one
    // of them may notify of its absence.
    // Returns the leading sequence of the coordinator process' request ring.

    void print(const string& str);

    mutex                                       m_type_resolution_mutex;
    mutex                                       m_publish_mutex;
    mutex                                       m_groups_mutex;
    mutex                                       m_init_tracking_mutex;

    // access only after acquiring m_publish_mutex
    map<
        instance_id_type,                       // process instance id
        map<
            instance_id_type,                   // transceiver instance id (within the process)
            tn_type                             // type id and assigned name
        >
    >                                           m_transceiver_registry;

    // access only after acquiring m_groups_mutex
    map<
        instance_id_type,
        spinlocked_uset< instance_id_type >
    >                                           m_groups_of_process;
    map<string, Process_group>                  m_groups;


    // access only after acquiring m_publish_mutex
    // (currently, only inside publish_transceiver() )
    struct waited_instance_info
    {
        unordered_set<instance_id_type> waiters;
        instance_id_type                common_function_iid = invalid_instance_id;
    };

    map<
        string,
        waited_instance_info
    >                                           m_instances_waited;

    set<instance_id_type>                       m_requested_recovery;
    mutable std::mutex                          m_lifecycle_mutex;
    Recovery_policy                             m_recovery_policy;
    Recovery_runner                             m_recovery_runner;
    Lifecycle_handler                           m_lifecycle_handler;
    mutable std::mutex                          m_crash_mutex;
    std::unordered_map<instance_id_type, int>   m_recent_crash_status;
    std::mutex                                  m_recovery_threads_mutex;
    std::vector<std::thread>                    m_recovery_threads;
    std::atomic<bool>                           m_shutdown{false};

    // Track in-flight join_swarm requests keyed by branch_index to avoid
    // spawning multiple processes when callers retry the RPC. Cleared once
    // the corresponding process completes initialization.
    std::unordered_map<int32_t, instance_id_type> m_inflight_joins;
    std::unordered_map<instance_id_type, int32_t> m_joined_process_branch;

    std::array<std::atomic<uint8_t>, max_process_index + 1> m_draining_process_states{};

    unordered_set<instance_id_type>             m_processes_in_initialization;

    struct Pending_instance_publication
    {
        type_id_type     type_id = not_defined_type_id;
        instance_id_type instance_id = invalid_instance_id;
        string           assigned_name;
    };

    std::vector<Pending_instance_publication>   m_delayed_instance_publications;

    // Remove a process from init tracking; return delayed publications now ready
    // to emit once startup coordination has finished.
    std::vector<Pending_instance_publication> finalize_initialization_tracking(
        instance_id_type process_iid);

    void collect_and_schedule_barrier_completions(
        instance_id_type process_iid,
        bool remove_process,
        bool lock_groups_once);

    bool draining_slot_of_index(uint64_t draining_index, size_t& slot) const;

    // Draining coordination -------------------------------------------------
    //
    // The draining state is tracked per-process via m_draining_process_states.
    // To tighten shutdown semantics, the coordinator can optionally wait until
    // every known process has entered the draining state (or been scavenged)
    // before allowing its own shutdown to proceed. This is driven by
    // wait_for_all_draining(), which blocks the caller until the condition
    // holds. The flag is_process_draining() remains the canonical per-slot
    // predicate; these helpers simply aggregate it over all known processes.
    mutable std::mutex                         m_draining_state_mutex;
    std::condition_variable                    m_all_draining_cv;
    std::atomic<bool>                          m_waiting_for_all_draining{false};

    // Configurable timeout for wait_for_all_draining(). A value of 0 means
    // wait indefinitely (no timeout). Default is 20 seconds.
    std::chrono::seconds                       m_drain_timeout{20};

    // Aggregate draining state for all known processes based on the registry and
    // initialization tracking (caller holds lock).
    void collect_known_process_candidates_unlocked(std::vector<instance_id_type>& candidates);
    bool all_known_processes_draining_unlocked(instance_id_type self_process);
    // Block until all known processes are draining (or have been scavenged).
    // Returns true if all processes reached draining state, false if timed out.
    // A timeout of 0 (set via set_drain_timeout) means wait indefinitely.
    bool wait_for_all_draining(instance_id_type self_process);

public:
    SINTRA_RPC_EXPLICIT(resolve_type)
    SINTRA_RPC_EXPLICIT(resolve_instance)
    SINTRA_RPC_STRICT_EXPLICIT(wait_for_instance)
    SINTRA_RPC_STRICT_EXPLICIT(publish_transceiver)
    SINTRA_RPC_EXPLICIT(unpublish_transceiver)
    SINTRA_RPC_STRICT_EXPLICIT(begin_process_draining)
    SINTRA_RPC_EXPLICIT(make_process_group)
    SINTRA_RPC_EXPLICIT(print)
    SINTRA_RPC_EXPLICIT(enable_recovery)
    SINTRA_RPC_EXPLICIT(mark_initialization_complete)
    SINTRA_RPC_STRICT_EXPLICIT(join_swarm)

    // Read the draining bit for a process slot; does not validate liveness.
    bool is_process_draining(instance_id_type process_iid) const;

    SINTRA_MESSAGE_RESERVED(instance_published,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)
    SINTRA_MESSAGE_RESERVED(instance_unpublished,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)

    friend struct Managed_process;
    friend struct Transceiver;
    friend bool finalize();
};

}


