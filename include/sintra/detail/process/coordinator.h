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
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>


namespace sintra {

// Forward declaration for friend access from Coordinator.
namespace detail {
inline bool finalize_impl();
struct Managed_child_readiness_access;
}

using std::condition_variable;
using std::map;
using std::mutex;
using std::set;
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
    sequence_counter_type barrier(const string& barrier_name, int32_t barrier_mode_tag);


    struct Barrier
    {
        mutex                                   m;
        condition_variable                      cv;
        unordered_set<instance_id_type>         processes_pending;
        unordered_set<instance_id_type>         processes_arrived;
        //sequence_counter_type                   flush_sequence = 0;
        bool                                    failed = false;
        int32_t                                 mode_tag = 0;
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
        instance_id_type                   process_iid,
        std::vector<Barrier_completion>&   completions,
        bool                               user_barriers_only = false);
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


namespace detail {

enum class Coordinator_mutex_rank: unsigned
{
    external_process_invitations = 1,
    groups                       = 2,
    publish                      = 3,
    init_tracking                = 4,
};

#ifndef NDEBUG

inline thread_local unsigned s_coordinator_mutex_rank = 0;

template <Coordinator_mutex_rank Rank>
class Coordinator_ranked_mutex
{
public:
    Coordinator_ranked_mutex() = default;
    Coordinator_ranked_mutex(const Coordinator_ranked_mutex&) = delete;
    Coordinator_ranked_mutex& operator=(const Coordinator_ranked_mutex&) = delete;

    void lock()
    {
        assert(s_coordinator_mutex_rank < rank_value &&
            "coordinator mutex acquired out of rank order");
        m_mutex.lock();
        m_previous_rank = s_coordinator_mutex_rank;
        s_coordinator_mutex_rank = rank_value;
    }

    bool try_lock()
    {
        assert(s_coordinator_mutex_rank < rank_value &&
            "coordinator mutex acquired out of rank order");
        if (!m_mutex.try_lock()) {
            return false;
        }
        m_previous_rank = s_coordinator_mutex_rank;
        s_coordinator_mutex_rank = rank_value;
        return true;
    }

    void unlock()
    {
        assert(s_coordinator_mutex_rank == rank_value &&
            "coordinator mutex unlocked out of rank stack order");
        s_coordinator_mutex_rank = m_previous_rank;
        m_mutex.unlock();
    }

private:
    static constexpr auto rank_value = static_cast<unsigned>(Rank);

    std::mutex m_mutex;
    unsigned   m_previous_rank = 0;
};

using Coordinator_condition_variable = std::condition_variable_any;

#else

template <Coordinator_mutex_rank>
using Coordinator_ranked_mutex = std::mutex;

using Coordinator_condition_variable = std::condition_variable;

#endif

using Coordinator_external_process_invitations_mutex =
    Coordinator_ranked_mutex<Coordinator_mutex_rank::external_process_invitations>;
using Coordinator_groups_mutex =
    Coordinator_ranked_mutex<Coordinator_mutex_rank::groups>;
using Coordinator_publish_mutex =
    Coordinator_ranked_mutex<Coordinator_mutex_rank::publish>;
using Coordinator_init_tracking_mutex =
    Coordinator_ranked_mutex<Coordinator_mutex_rank::init_tracking>;

} // namespace detail



struct Coordinator: public Derived_transceiver<Coordinator>
{

private:
    struct Pending_completion
    {
        std::string group_name;
        std::vector<Process_group::Barrier_completion> completions;
    };

    enum class External_process_invitation_state
    {
        pending,
        rejecting,
        removing,
    };

    struct External_process_invitation_record
    {
        std::string                            token;
        std::chrono::steady_clock::time_point  expires_at;
        External_process_invitation_state      state = External_process_invitation_state::pending;
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

    struct Managed_child_publication_identity
    {
        uint64_t          custody_identity = 0;
        instance_id_type  process_iid      = invalid_instance_id;
        uint32_t          occurrence        = 0;

        bool matches(
            uint64_t          expected_custody_identity,
            instance_id_type  expected_process_iid,
            uint32_t          expected_occurrence) const noexcept
        {
            return custody_identity == expected_custody_identity &&
                process_iid == expected_process_iid &&
                occurrence == expected_occurrence;
        }

        explicit operator bool() const noexcept
        {
            return custody_identity != 0 &&
                process_iid != invalid_instance_id;
        }

        bool operator==(const Managed_child_publication_identity&) const = default;
    };

    struct Transceiver_publication
    {
        type_id_type                       type_id = 0;
        string                             name;
        Managed_child_publication_identity managed_child_identity;

        Transceiver_publication() = default;

        Transceiver_publication(
            type_id_type                              published_type_id,
            const string&                             assigned_name,
            const Managed_child_publication_identity& identity)
            : type_id(published_type_id),
              name(assigned_name),
              managed_child_identity(identity)
        {
        }

        Transceiver_publication(const Tn_type& publication)
            : type_id(publication.type_id),
              name(publication.name)
        {
        }

        Transceiver_publication& operator=(const Tn_type& publication)
        {
            type_id = publication.type_id;
            name = publication.name;
            managed_child_identity = {};
            return *this;
        }
    };

    instance_id_type resolve_managed_child_instance_locked(
        const string&     assigned_name,
        uint64_t          custody_identity,
        instance_id_type  process_iid,
        uint32_t          occurrence,
        const std::atomic<bool>& cancelled);
    instance_id_type resolve_managed_child_instance(
        const string&     assigned_name,
        uint64_t          custody_identity,
        instance_id_type  process_iid,
        uint32_t          occurrence,
        const std::atomic<bool>& cancelled);
    instance_id_type wait_for_managed_child_instance(
        const string&     assigned_name,
        uint64_t          custody_identity,
        instance_id_type  process_iid,
        uint32_t          occurrence,
        const std::atomic<bool>& cancelled);
    void notify_managed_child_readiness_cancelled();

    // Publish a transceiver instance and assigned name in the registry. Enforces
    // name uniqueness and per-process limits, emits instance_published (or
    // queues it during startup), and unblocks waiters. For Managed_process,
    // resets the draining bit for the process slot.
    instance_id_type publish_transceiver(
        type_id_type       type_id,
        instance_id_type   instance_id,
        const string&      assigned_name);
    instance_id_type publish_transceiver_with_managed_child_identity(
        type_id_type                              type_id,
        instance_id_type                          instance_id,
        const string&                             assigned_name,
        const Managed_child_publication_identity& managed_child_identity);

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

    // Claim a coordinator-created external-process invitation during init().
    // Returns false when the invitation is absent, canceled, expired, or the
    // caller does not present the recorded token.
    bool claim_external_process_invitation(
        instance_id_type   process_iid,
        const string&      token);

    // Create a process group transceiver with explicit membership (used for
    // built-in groups like _sintra_all_processes).
    instance_id_type make_process_group(
        const string&                          name,
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

    void external_process_invitation_cleanup_loop();
    void transition_expired_external_process_invitations(
        std::vector<instance_id_type>&         readers_to_remove,
        std::chrono::steady_clock::time_point& next_deadline);

    void remove_external_process_invitation_readers(
        const std::vector<instance_id_type>&   process_ids);
    void cancel_all_external_process_invitations();
    bool process_id_is_known_for_external_invitation(instance_id_type process_iid);
    bool external_process_invitation_exists_unlocked(instance_id_type process_iid) const;
    bool publish_process_group(Process_group& group, const string& name);
    void unpublish_process_group(Process_group& group);
    bool add_external_process_to_standard_groups(instance_id_type process_iid);

    detail::Coordinator_condition_variable      m_managed_child_publication_changed;

public:
    // Helpers (not exported for RPC).
    // ================================================

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    instance_id_type publish_transceiver_for_test(
        type_id_type tid,
        instance_id_type iid,
        const string& assigned_name)
    {
        return publish_transceiver(tid, iid, assigned_name);
    }

    instance_id_type publish_managed_child_transceiver_for_test(
        type_id_type       tid,
        instance_id_type   iid,
        const string&      assigned_name,
        uint64_t           custody_identity,
        instance_id_type   process_iid,
        uint32_t           occurrence)
    {
        return publish_transceiver_with_managed_child_identity(
            tid,
            iid,
            assigned_name,
            Managed_child_publication_identity{
                custody_identity,
                process_iid,
                occurrence});
    }
#endif

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
    void set_drain_timeout(
        std::chrono::seconds   timeout);

    bool reserve_external_process_invitation(
        instance_id_type                       process_iid,
        const string&                          token,
        std::chrono::steady_clock::time_point  expires_at,
        uint32_t&                              occurrence_out);

    bool cancel_external_process_invitation(
        instance_id_type       process_iid);

    bool cancel_external_process_invitation(
        instance_id_type       process_iid,
        const string&          token);

    bool external_process_invitation_exists(
        instance_id_type       process_iid);

    bool group_has_non_external_peer(
        const string&          group_name,
        instance_id_type       self_process_iid);

    // Mark a process as inside collective shutdown. Ordinary user barriers
    // stop waiting for it, but internal shutdown barriers still count it.
    sequence_counter_type begin_collective_shutdown(instance_id_type process_iid);

    // Blocks until all processes identified by process_group_id have called the function.
    // num_absences may be used by a caller to specify that it is aware that other callers will
    // not make it to the barrier, thus prevent a deadlock.
    // NOTE: If more than one callers are aware of the absence of some other caller, only one
    // of them may notify of its absence.
    // Returns the leading sequence of the coordinator process' request ring.

    void print(const string& str);

    // Coordinator lock ordering (outermost first). A thread may skip levels,
    // but must never acquire an earlier-listed mutex while holding a
    // later-listed one:
    //   1. m_external_process_invitations_mutex
    //      (held across the "is this process known" checks during invitation
    //      reservation)
    //   2. m_groups_mutex
    //   3. m_publish_mutex
    //   4. m_init_tracking_mutex
    // Process_group locks nest below m_groups_mutex:
    //   m_groups_mutex -> Process_group::m_call_mutex -> Barrier::m
    // The remaining coordinator mutexes are leaves; no other coordinator
    // mutex may be acquired while holding one of them:
    //   m_type_resolution_mutex, m_lifecycle_mutex, m_crash_mutex,
    //   m_recovery_threads_mutex, m_draining_state_mutex
    mutex                                          m_type_resolution_mutex;
    detail::Coordinator_publish_mutex             m_publish_mutex;
    detail::Coordinator_groups_mutex              m_groups_mutex;
    detail::Coordinator_init_tracking_mutex       m_init_tracking_mutex;

    // access only after acquiring m_publish_mutex
    map<
        instance_id_type,                       // process instance id
        map<
            instance_id_type,                   // transceiver instance id (within the process)
            Transceiver_publication             // publication and exact managed-child identity
        >
    >                                              m_transceiver_registry;
    // access only after acquiring m_groups_mutex
    map<
        instance_id_type,
        unordered_set< instance_id_type >
    >                                              m_groups_of_process;
    map<string, Process_group>                     m_groups;


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
    >                                              m_instances_waited;

    // access only after acquiring m_lifecycle_mutex
    set<instance_id_type>                          m_requested_recovery;

    // access only after acquiring m_publish_mutex
    unordered_set<instance_id_type>                m_external_attached_processes;
    mutable std::mutex                             m_lifecycle_mutex;
    Recovery_policy                                m_recovery_policy;
    Recovery_runner                                m_recovery_runner;
    Lifecycle_handler                              m_lifecycle_handler;
    mutable std::mutex                             m_crash_mutex;
    std::unordered_map<instance_id_type, int>      m_recent_crash_status;
    std::mutex                                     m_recovery_threads_mutex;
    std::vector<std::thread>                       m_recovery_threads;
    std::atomic<bool>                              m_shutdown{false};

    detail::Coordinator_external_process_invitations_mutex
                                                   m_external_process_invitations_mutex;
    detail::Coordinator_condition_variable         m_external_process_invitations_cv;
    std::unordered_map<
        instance_id_type,
        External_process_invitation_record
    >                                              m_external_process_invitations;
    std::unordered_map<instance_id_type, uint32_t> m_external_process_invitation_next_occurrence;
    std::thread                                    m_external_process_invitation_cleanup_thread;
    bool                                           m_external_process_invitation_cleanup_stop = false;

    // Track in-flight join_swarm requests keyed by branch_index to avoid
    // spawning multiple processes when callers retry the RPC. Cleared once
    // the corresponding process completes initialization.
    std::unordered_map<int32_t, instance_id_type>  m_inflight_joins;
    std::unordered_map<instance_id_type, int32_t>  m_joined_process_branch;

    std::array<std::atomic<uint8_t>, max_process_index + 1>
                                                   m_draining_process_states{};
    std::array<std::atomic<uint8_t>, max_process_index + 1>
                                                   m_collective_shutdown_process_states{};

    unordered_set<instance_id_type>                m_processes_in_initialization;
    struct Pending_instance_publication
    {
        type_id_type       type_id     = not_defined_type_id;
        instance_id_type   instance_id = invalid_instance_id;
        string             assigned_name;
    };

    std::vector<Pending_instance_publication>      m_delayed_instance_publications;

    // Remove a process from init tracking; return delayed publications now ready
    // to emit once startup coordination has finished.
    std::vector<Pending_instance_publication> finalize_initialization_tracking(
        instance_id_type   process_iid);

    std::vector<Pending_completion> collect_pending_barrier_completions(
        instance_id_type   process_iid,
        bool               remove_process,
        bool               user_barriers_only = false);

    // Same as collect_pending_barrier_completions, for callers that already
    // hold m_groups_mutex.
    std::vector<Pending_completion> collect_pending_barrier_completions_unlocked(
        instance_id_type   process_iid,
        bool               remove_process,
        bool               user_barriers_only = false);

    void emit_pending_barrier_completions(
        const std::vector<Pending_completion>& pending_completions);

    void collect_and_schedule_barrier_completions(
        instance_id_type   process_iid,
        bool               remove_process);

    bool draining_slot_of_index(
        uint64_t           draining_index,
        size_t&            slot) const;

    // Writes value into the draining-state slot for process_iid (if any).
    // Returns true when a slot exists.
    bool set_draining_state(instance_id_type process_iid, int value);

    bool set_collective_shutdown_state(instance_id_type process_iid, int value);

    // Like set_draining_state, but also reports the prior value through was_draining.
    bool exchange_draining_state(
        instance_id_type   process_iid,
        int                value,
        bool&              was_draining);

    // Draining coordination -------------------------------------------------
    //
    // The draining state is tracked per-process via m_draining_process_states.
    // To tighten shutdown semantics, the coordinator can optionally wait until
    // every known process has entered the draining state (or been scavenged)
    // before allowing its own shutdown to proceed. This is driven by
    // wait_for_all_draining(), which blocks the caller until the condition
    // holds. The flag is_process_draining() remains the canonical per-slot
    // predicate; these helpers simply aggregate it over all known processes.
    // m_draining_state_generation is bumped after any state transition that can
    // satisfy a drain predicate. Waiters snapshot it before re-checking the
    // predicate and sleep only until the next generation change, preventing
    // missed wakeups without polling.
    mutable std::mutex                             m_draining_state_mutex;
    std::condition_variable                        m_all_draining_cv;
    std::atomic<bool>                              m_waiting_for_all_draining{false};
    uint64_t                                       m_draining_state_generation = 0;

    // Configurable timeout for wait_for_all_draining(). A value of 0 means
    // wait indefinitely (no timeout). Default is 20 seconds.
    std::chrono::seconds                           m_drain_timeout{20};

    // Aggregate draining state for all known processes based on the registry and
    // initialization tracking (caller holds lock).
    void collect_known_process_candidates_unlocked(std::vector<instance_id_type>& candidates);
    bool all_known_processes_draining_unlocked(instance_id_type self_process);
    bool is_sole_known_process_unlocked(instance_id_type self_process);
    void note_draining_state_change();
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
    SINTRA_RPC_STRICT_EXPLICIT(begin_collective_shutdown)
    SINTRA_RPC_EXPLICIT(make_process_group)
    SINTRA_RPC_EXPLICIT(print)
    SINTRA_RPC_EXPLICIT(enable_recovery)
    SINTRA_RPC_EXPLICIT(mark_initialization_complete)
    SINTRA_RPC_STRICT_EXPLICIT(join_swarm)
    SINTRA_RPC_STRICT_EXPLICIT(claim_external_process_invitation)

    // Read the draining bit for a process slot; does not validate liveness.
    bool is_process_draining(instance_id_type process_iid) const;
    bool is_process_in_collective_shutdown(instance_id_type process_iid) const;
    bool is_sole_known_process(instance_id_type self_process);

    SINTRA_MESSAGE_RESERVED(instance_published,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)
    SINTRA_MESSAGE_RESERVED(instance_unpublished,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)

    friend struct Managed_process;
    friend struct detail::Managed_child_readiness_access;
    friend struct Transceiver;
    friend bool finalize();
    friend bool detail::finalize_impl();
};

}
