// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../logging.h"
#include "../process/coordinator.h"
#include "../process/managed_process.h"
#include "../process/dispatch_wait_guard.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>


namespace sintra {


using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;



// EXPORTED EXCLUSIVELY FOR RPC
inline
sequence_counter_type Process_group::barrier(
    const string& barrier_name)
{
    std::unique_lock basic_lock(m_call_mutex);
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;
    if (m_process_ids.find(caller_piid) == m_process_ids.end()) {
        throw std::logic_error("The caller is not a member of the process group.");
    }

    auto& barrier_entry = m_barriers[barrier_name];
    if (!barrier_entry) {
        barrier_entry = std::make_shared<Barrier>();
    }

    auto barrier = barrier_entry; // keep the barrier alive even if the map rehashes
    Barrier& b = *barrier;
    // Lock ordering: m_call_mutex before barrier->m to prevent deadlock.
    b.m.lock();

    // Atomically snapshot membership and filter draining processes while holding m_call_mutex.
    // This ensures a consistent view: no process can be added/removed or change draining state
    // between the membership snapshot and the draining filter.
    if (b.processes_pending.empty()) {
        // new or reused barrier (may have failed previously)
        b.processes_pending = m_process_ids;
        b.processes_arrived.clear();
        b.failed = false;
        b.common_function_iid = make_instance_id();

        // Filter out draining processes while still holding m_call_mutex for atomicity
        if (auto* coord = s_coord) {
            for (auto it = b.processes_pending.begin();
                 it != b.processes_pending.end(); )
            {
                if (coord->is_process_draining(*it)) {
                    it = b.processes_pending.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
    }

    // Now safe to release m_call_mutex - barrier state is consistent and other threads
    // need to be able to arrive at the barrier concurrently
    basic_lock.unlock();

    b.processes_arrived.insert(caller_piid);
    b.processes_pending.erase(caller_piid);

    if (b.processes_pending.size() == 0) {
        // Last arrival
        auto additional_pids = b.processes_arrived;
        additional_pids.erase(caller_piid);

        assert(s_tl_common_function_iid == invalid_instance_id);
        assert(s_tl_additional_piids_size == 0);
        assert(additional_pids.size() < max_process_index);
        s_tl_additional_piids_size = 0;
        for (auto& e : additional_pids) {
            s_tl_additional_piids[s_tl_additional_piids_size++] = e;
        }

        const auto current_common_fiid = b.common_function_iid;
        s_tl_common_function_iid = current_common_fiid;
        b.m.unlock();

        // Re-lock m_call_mutex to safely erase from m_barriers
        basic_lock.lock();
        auto it = m_barriers.find(barrier_name);
        if (it != m_barriers.end() &&
            it->second &&
            it->second.get() == barrier.get() &&
            it->second->common_function_iid == current_common_fiid)
        {
            m_barriers.erase(it);
        }
        // basic_lock will unlock m_call_mutex on return
        // Use reply ring watermark (m_out_rep_c) since barrier completion messages
        // are sent on the reply channel. Get it at return time for the calling process.
        return s_mproc->m_out_rep_c->get_leading_sequence();
    }
    else {
        // Not last arrival - emit a deferral message now and return without a normal reply
        auto* current_message = s_tl_current_message;
        assert(current_message);

        deferral* placed_msg = s_mproc->m_out_rep_c->write<deferral>(0, b.common_function_iid);
        Transceiver::finalize_rpc_write(
            placed_msg,
            current_message->sender_instance_id,
            current_message->function_instance_id,
            this,
            (type_id_type)detail::reserved_id::deferral);

        mark_rpc_reply_deferred();
        b.m.unlock();
        return 0;
    }
}


inline void Process_group::drop_from_inflight_barriers(
    instance_id_type process_iid,
    std::vector<Barrier_completion>& completions)
{
    std::lock_guard basic_lock(m_call_mutex);

    for (auto barrier_it = m_barriers.begin(); barrier_it != m_barriers.end(); ) {
        auto barrier = barrier_it->second;
        if (!barrier) {
            barrier_it = m_barriers.erase(barrier_it);
            continue;
        }

        std::unique_lock barrier_lock(barrier->m);

        const bool touched_pending = barrier->processes_pending.erase(process_iid) > 0;
        const bool touched_arrived = barrier->processes_arrived.erase(process_iid) > 0;

        if (!touched_pending && !touched_arrived) {
            ++barrier_it;
            continue;
        }

        if (!barrier->processes_pending.empty()) {
            ++barrier_it;
            continue;
        }

        Barrier_completion completion;
        completion.common_function_iid = barrier->common_function_iid;
        completion.recipients.assign(
            barrier->processes_arrived.begin(),
            barrier->processes_arrived.end());

        if (touched_arrived) {
            completion.recipients.push_back(process_iid);
        }

        barrier->processes_arrived.clear();
        barrier->common_function_iid = invalid_instance_id;

        barrier_lock.unlock();

        barrier_it = m_barriers.erase(barrier_it);
        completions.push_back(std::move(completion));
    }
}

inline void Process_group::emit_barrier_completions(
    const std::vector<Barrier_completion>& completions)
{
    using return_message_type = Message<Enclosure<sequence_counter_type>, void, not_defined_type_id>;

    for (const auto& completion : completions) {
        if (completion.common_function_iid == invalid_instance_id) {
            continue;
        }

        if (completion.recipients.empty()) {
            continue;
        }

        // Get per-recipient flush token: compute it INSIDE the loop so each recipient
        // gets a watermark that's valid for their specific message write time.
        // This prevents hangs where a global token is ahead of some recipient's channel.
        for (auto recipient : completion.recipients) {
            const auto flush_sequence = s_mproc->m_out_rep_c->get_leading_sequence();

            auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
                vb_size<return_message_type>(flush_sequence), flush_sequence);

            Transceiver::finalize_rpc_write(
                placed_msg,
                recipient,
                completion.common_function_iid,
                this,
                not_defined_type_id);
        }
    }
}


inline
Coordinator::Coordinator():
    Derived_transceiver<Coordinator>("", make_service_instance_id())
{
    // Pre-initialize/ all draining states to 0 (ACTIVE) so that concurrent reads from
    // barrier paths are safe without additional locking. The array is fixed-size and
    // never grows, eliminating data races from container mutation.
    for (auto& draining_state : m_draining_process_states) {
        draining_state = 0;
    }
}



inline
Coordinator::~Coordinator()
{
    m_shutdown.store(true, std::memory_order_release);
    {
        std::lock_guard<mutex> lock(m_recovery_threads_mutex);
        for (auto& thread : m_recovery_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    s_coord     = nullptr;
    s_coord_id  = 0;
}



// EXPORTED FOR RPC
inline
type_id_type Coordinator::resolve_type(const string& pretty_name)
{
    lock_guard<mutex> lock(m_type_resolution_mutex);
    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
    {
        auto scoped_map = s_mproc->m_type_id_of_type_name.scoped();
        auto it = scoped_map.get().find(pretty_name);
        if (it != scoped_map.get().end()) {
            return it->second;
        }
        // Spinlock released here automatically when scoped_map goes out of scope
    }

    // a type is always assumed to exist
    return s_mproc->m_type_id_of_type_name[pretty_name] = make_type_id();
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::resolve_instance(const string& assigned_name)
{
    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
    auto scoped_map = s_mproc->m_instance_id_of_assigned_name.scoped();
    auto it = scoped_map.get().find(assigned_name);
    if (it != scoped_map.get().end()) {
        return it->second;
    }

    // unlike types, instances need explicit allocation
    return invalid_instance_id;
}



// EXPORTED EXCLUSIVELY FOR RPC
inline
instance_id_type Coordinator::wait_for_instance(const string& assigned_name)
{
    // This works similarly to a barrier. The difference is that
    // a barrier operates in a defined set of process instances, whereas
    // waiting on an instance is not restricted.
    // A caller waiting for an instance will be unblocked when the
    // instance is created and will not block at all if it exists already.
    // Waiting for an instance does not influence creation/deletion of
    // the instance, thus using it for synchronization may not always be
    // applicable.

    m_publish_mutex.lock();
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;

    auto iid = resolve_instance(assigned_name);
    if (iid != invalid_instance_id) {
        m_publish_mutex.unlock();
        return iid;
    }

    auto& waited_info = m_instances_waited[assigned_name];
    waited_info.waiters.insert(caller_piid);

    instance_id_type common_function_iid = waited_info.common_function_iid;
    if (common_function_iid == invalid_instance_id) {
        common_function_iid = waited_info.common_function_iid = make_instance_id();
    }

    auto* current_message = s_tl_current_message;
    assert(current_message);

    deferral* placed_msg = s_mproc->m_out_rep_c->write<deferral>(0, common_function_iid);
    Transceiver::finalize_rpc_write(
        placed_msg,
        current_message->sender_instance_id,
        current_message->function_instance_id,
        this,
        (type_id_type)detail::reserved_id::deferral);

    mark_rpc_reply_deferred();
    m_publish_mutex.unlock();
    return invalid_instance_id;
}



// EXPORTED EXCLUSIVELY FOR RPC
inline
instance_id_type Coordinator::publish_transceiver(type_id_type tid, instance_id_type iid, const string& assigned_name)
{
    lock_guard<mutex> lock(m_publish_mutex);

    // empty strings are not valid names
    if (assigned_name.empty()) {
        return invalid_instance_id;
    }

    auto process_iid = process_of(iid);
    auto pr_it = m_transceiver_registry.find(process_iid);
    auto entry = tn_type{ tid, assigned_name };

    auto true_sequence = [&](bool allow_notification_delay) {
        bool queued_notification = false;
        if (allow_notification_delay) {
            std::lock_guard<mutex> init_lock(m_init_tracking_mutex);
            if (!m_processes_in_initialization.empty()) {
                // Delay instance_published while startup is still in progress to prevent
                // circular RPC activation between not-yet-ready processes.
                m_delayed_instance_publications.push_back(
                    Pending_instance_publication{tid, iid, assigned_name});
                queued_notification = true;
            }
        }

        if (!queued_notification) {
            emit_global<instance_published>(tid, iid, assigned_name);
        }
        assert(s_tl_additional_piids_size == 0);
        assert(s_tl_common_function_iid == invalid_instance_id);

        s_tl_additional_piids_size = 0;

        instance_id_type notified_common_fiid = invalid_instance_id;

        if (auto waited_node = m_instances_waited.extract(assigned_name)) {
            auto waited_info = std::move(waited_node.mapped());

            assert(waited_info.waiters.size() < max_process_index);
            for (auto& e : waited_info.waiters) {
                s_tl_additional_piids[s_tl_additional_piids_size++] = e;
            }

            notified_common_fiid = waited_info.common_function_iid;
        }

        s_tl_common_function_iid = notified_common_fiid;

        return iid;
    };

    if (pr_it != m_transceiver_registry.end()) { // the transceiver's process is known
        
        auto& pr = pr_it->second; // process registry

        // observe the limit of transceivers per process
        if (pr.size() >= max_public_transceivers_per_proc) {
            return invalid_instance_id;
        }

        // the transceiver must not have been already published
        if (pr.find(iid) != pr.end()) {
            return invalid_instance_id;
        }

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return invalid_instance_id;
        }

        s_mproc->m_instance_id_of_assigned_name[entry.name] = iid;
        pr[iid] = entry;

        // Do NOT reset draining state here - only reset when publishing a NEW PROCESS (Managed_process),
        // not when publishing a regular transceiver. Resetting here could interfere with shutdown.

        return true_sequence(false);
    }
    else
    if (iid == process_iid) { // the transceiver is a Managed_process

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return invalid_instance_id;
        }

        s_mproc->m_instance_id_of_assigned_name[entry.name] = iid;
        m_transceiver_registry[iid][iid] = entry;

        // Reset draining state to 0 (ACTIVE) when publishing a Managed_process.
        // This handles recovery/restart scenarios where the process slot might still be marked as draining.
        const auto draining_index = get_process_index(process_iid);
        if (draining_index > 0 && draining_index <= static_cast<uint64_t>(max_process_index)) {
            const auto slot = static_cast<size_t>(draining_index);
            m_draining_process_states[slot] = 0;
        }

        return true_sequence(true);
    }
    else {
        return invalid_instance_id;
    }
}



// EXPORTED FOR RPC
inline
bool Coordinator::unpublish_transceiver(instance_id_type iid)
{
    lock_guard<mutex> lock(m_publish_mutex);

    // the process of the transceiver must have been registered
    auto process_iid = process_of(iid);
    auto pr_it = m_transceiver_registry.find(process_iid);
    if (pr_it == m_transceiver_registry.end()) {
        return false;
    }

    auto& pr = pr_it->second; // process registry

    // the transceiver must have been published
    auto it = pr.find(iid);
    if (it == pr.end()) {
        return false;
    }

    // the transceiver is assumed to have a name, not an empty string
    // (it shouldn't have made it through publish_transceiver otherwise)
    assert(!it->second.name.empty());

    // delete the reverse name lookup entry
    s_mproc->m_instance_id_of_assigned_name.erase(it->second.name);

    // keep a copy of the assigned name before deleting it
    auto tn = it->second;

    std::vector<Pending_instance_publication> ready_notifications;

    // if it is a Managed_process is being unpublished, more cleanup is required
    if (iid == process_iid) {

        // Cleanup in-flight join tracking for this process/branch, if any.
        if (auto branch_it = m_joined_process_branch.find(process_iid); branch_it != m_joined_process_branch.end()) {
            m_inflight_joins.erase(branch_it->second);
            m_joined_process_branch.erase(branch_it);
        }

        ready_notifications = finalize_initialization_tracking(process_iid);
        if (!ready_notifications.empty()) {
            ready_notifications.erase(
                std::remove_if(
                    ready_notifications.begin(),
                    ready_notifications.end(),
                    [&](const Pending_instance_publication& publication) {
                        return process_of(publication.instance_id) == process_iid;
                    }),
                ready_notifications.end());
        }

        // remove all name lookup entries resolving to the unpublished process
        auto name_map = s_mproc->m_instance_id_of_assigned_name.scoped();
        for (auto name_it = name_map.begin(); name_it != name_map.end(); )
        {
            if (process_of(name_it->second) == process_iid) {
                name_it = name_map.erase(name_it);
            }
            else {
                ++name_it;
            }
        }

        // remove the unpublished process's registry entry
        m_transceiver_registry.erase(pr_it);

        if (s_mproc) {
            s_mproc->release_lifeline(process_iid);
        }

        // CRITICAL: Mark as draining BEFORE removing from groups to prevent barriers
        // from including this process. This handles both graceful shutdown (where
        // begin_process_draining was already called) and crash scenarios (where it wasn't).
        const auto draining_index = get_process_index(process_iid);
        bool was_draining = false;
        if (draining_index > 0 && draining_index <= static_cast<uint64_t>(max_process_index)) {
            const auto slot = static_cast<size_t>(draining_index);
            was_draining = (m_draining_process_states[slot].load() != 0);
            m_draining_process_states[slot] = 1;
        }

        struct Pending_completion
        {
            std::string group_name;
            std::vector<Process_group::Barrier_completion> completions;
        };

        std::vector<Pending_completion> pending_completions;

        {
            lock_guard<mutex> groups_lock(m_groups_mutex);
            pending_completions.reserve(m_groups.size());

            for (auto& [name, group] : m_groups) {
                std::vector<Process_group::Barrier_completion> completions;
                group.drop_from_inflight_barriers(process_iid, completions);
                if (!completions.empty()) {
                    pending_completions.push_back({name, std::move(completions)});
                }
                group.remove_process(process_iid);
            }

            m_groups_of_process.erase(process_iid);
        }

        if (!pending_completions.empty() && s_mproc) {
            s_mproc->run_after_current_handler(
                [this, pending = std::move(pending_completions)]() mutable {
                    std::lock_guard<mutex> groups_lock(m_groups_mutex);
                    for (auto& entry : pending) {
                        auto group_it = m_groups.find(entry.group_name);
                        if (group_it != m_groups.end()) {
                            group_it->second.emit_barrier_completions(entry.completions);
                        }
                    }
                });
        }

        // Keep the draining bit set; it will be re-initialized to 0 (ACTIVE)
        // when a new process is published into this slot. This prevents the race
        // where resetting too early allows concurrent barriers to include a dying process.

        //// and finally, if the process was being read, stop reading from it
        if (iid != s_mproc_id) {
            Dispatch_lock_guard<std::shared_lock<std::shared_mutex>> readers_lock(s_mproc->m_readers_mutex);
            auto reader_it = s_mproc->m_readers.find(process_iid);
            if (reader_it != s_mproc->m_readers.end()) {
                if (auto& reader = reader_it->second) {
                    reader->stop_nowait();
                }
            }
        }

        // If a caller is waiting for all processes to reach the draining state,
        // notify so it can re-evaluate the aggregate draining predicate. The
        // predicate itself is computed inside wait_for_all_draining() to avoid
        // re-entrancy and lock ordering issues here.
        if (m_waiting_for_all_draining.load(std::memory_order_acquire)) {
            m_all_draining_cv.notify_all();
        }

        bool crash_seen = false;
        int crash_status = 0;
        {
            std::lock_guard<mutex> crash_lock(m_crash_mutex);
            auto crash_it = m_recent_crash_status.find(process_iid);
            if (crash_it != m_recent_crash_status.end()) {
                crash_seen = true;
                crash_status = crash_it->second;
                m_recent_crash_status.erase(crash_it);
            }
        }

        if (!crash_seen) {
            process_lifecycle_event event;
            event.process_iid = process_iid;
            event.process_slot = static_cast<uint32_t>(draining_index);
            event.status = 0;
            event.why = was_draining
                ? process_lifecycle_event::reason::normal_exit
                : process_lifecycle_event::reason::unpublished;
            emit_lifecycle_event(event);

            if (!was_draining) {
                Crash_info info;
                info.process_iid = process_iid;
                info.process_slot = static_cast<uint32_t>(draining_index);
                info.status = 0;
                recover_if_required(info);
            }
        }
    }

    emit_global<instance_unpublished>(tn.type_id, iid, tn.name);

    if (!ready_notifications.empty()) {
        for (auto& publication : ready_notifications) {
            emit_global<instance_published>(
                publication.type_id,
                publication.instance_id,
                publication.assigned_name);
        }
    }

    return true;
}


inline sequence_counter_type Coordinator::begin_process_draining(instance_id_type process_iid)
{
    const auto draining_index = get_process_index(process_iid);
    if (draining_index > 0 && draining_index <= static_cast<uint64_t>(max_process_index)) {
        const auto slot = static_cast<size_t>(draining_index);
        m_draining_process_states[slot] = 1;
    }

    struct Pending_completion
    {
        std::string group_name;
        std::vector<Process_group::Barrier_completion> completions;
    };

    std::vector<Pending_completion> pending_completions;
    {
        lock_guard<mutex> lock(m_groups_mutex);
        pending_completions.reserve(m_groups.size());

        for (auto& [name, group] : m_groups) {
            std::vector<Process_group::Barrier_completion> completions;
            group.drop_from_inflight_barriers(process_iid, completions);
            if (!completions.empty()) {
                pending_completions.push_back({name, std::move(completions)});
            }
        }
    }

    if (!pending_completions.empty() && s_mproc) {
        s_mproc->run_after_current_handler(
            [this, pending = std::move(pending_completions)]() mutable {
                for (auto& entry : pending) {
                    std::lock_guard<mutex> groups_lock(m_groups_mutex);
                    auto it = m_groups.find(entry.group_name);
                    if (it != m_groups.end()) {
                        it->second.emit_barrier_completions(entry.completions);
                    }
                }
            });

        // Note: No explicit event-loop "pump" is required here; queued completions
        // run via run_after_current_handler() and the request loop's post-handler hook.
    }

    // Use reply ring watermark (m_out_rep_c) since barrier completion messages
    // are sent on the reply channel. Get it at return time for the draining process.
    auto watermark = s_mproc->m_out_rep_c->get_leading_sequence();

    // If a caller (typically the coordinator during shutdown) is waiting for all
    // processes to enter the draining state, notify so it can re-evaluate the
    // aggregate draining predicate. The predicate itself is computed inside
    // wait_for_all_draining() to avoid lock layering here.
    if (m_waiting_for_all_draining.load(std::memory_order_acquire)) {
        m_all_draining_cv.notify_all();
    }

    return watermark;
}


inline bool Coordinator::is_process_draining(instance_id_type process_iid) const
{
    const auto draining_index = get_process_index(process_iid);
    if (draining_index == 0 || draining_index > static_cast<uint64_t>(max_process_index)) {
        return false;
    }

    const auto slot = static_cast<size_t>(draining_index);
    return m_draining_process_states[slot].load() != 0;
}


inline bool Coordinator::all_known_processes_draining_unlocked(instance_id_type /*self_process*/)
{
    // Snapshot known processes from the registry and initialization tracking.
    // Processes that have never registered any transceivers are not represented
    // here and therefore do not participate in coordinated draining.
    std::vector<instance_id_type> candidates;
    candidates.reserve(m_transceiver_registry.size() + m_processes_in_initialization.size());

    {
        std::lock_guard<mutex> publish_lock(m_publish_mutex);
        for (const auto& entry : m_transceiver_registry) {
            candidates.push_back(entry.first);
        }
    }

    {
        std::lock_guard<mutex> init_lock(m_init_tracking_mutex);
        for (const auto& piid : m_processes_in_initialization) {
            candidates.push_back(piid);
        }
    }

    if (candidates.empty()) {
        return true;
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (auto piid : candidates) {
        // Process IDs in registry/initialization tracking must be valid (non-zero).
        if (piid == 0) {
            assert(false && "Invalid process ID in draining candidates");
            continue;
        }
        if (!is_process_draining(piid)) {
            return false;
        }
    }

    return true;
}


inline void Coordinator::wait_for_all_draining(instance_id_type self_process)
{
    (void)self_process; // currently unused; kept for potential future refinements.

    m_waiting_for_all_draining.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lk(m_draining_state_mutex);

    m_all_draining_cv.wait(lk, [&] {
        return all_known_processes_draining_unlocked(self_process);
    });

    m_waiting_for_all_draining.store(false, std::memory_order_release);
}


inline void Coordinator::unpublish_transceiver_notify(instance_id_type transceiver_iid)
{
    unpublish_transceiver(transceiver_iid);
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::make_process_group(
    const string& name,
    const unordered_set<instance_id_type>& member_process_ids)
{
    lock_guard<mutex> lock(m_groups_mutex);

    // check if it exists
    if (m_groups.count(name)) {
        return invalid_instance_id;
    }

    m_groups[name].set(member_process_ids);
    auto ret = m_groups[name].m_instance_id;

    for (auto& e : member_process_ids) {
        m_groups_of_process[e].insert(ret);
    }

    m_groups[name].assign_name(name);
    return ret;
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::join_swarm(
    const string& binary_name,
    int32_t branch_index)
{
    if (!s_mproc || !s_coord) {
        return invalid_instance_id;
    }

    instance_id_type new_instance_id = invalid_instance_id;
    {
        lock_guard<mutex> guard(m_publish_mutex);

        // Safety: refuse joins when the process space is nearly exhausted to avoid
        // runaway spawning that would otherwise trip hard asserts.
        const auto current_processes = m_transceiver_registry.size();
        const auto initializing = m_processes_in_initialization.size();
        if (current_processes + initializing >= static_cast<size_t>(max_process_index)) {
            return invalid_instance_id;
        }

        // If a join for this branch is already underway, avoid spawning another
        // process and return the pending instance id instead.
        if (auto it = m_inflight_joins.find(branch_index); it != m_inflight_joins.end()) {
            return it->second;
        }

        new_instance_id = make_process_instance_id();
        m_inflight_joins.emplace(branch_index, new_instance_id);
        m_joined_process_branch.emplace(new_instance_id, branch_index);
    }

    if (branch_index < 1 || branch_index >= max_process_index) {
        return invalid_instance_id;
    }

    // Add to groups BEFORE spawning so the process is already a group member
    // when it starts executing. This prevents a race where the spawned process
    // calls barrier() before being added to the group, causing it to be excluded
    // from the barrier's processes_pending set.
    {
        lock_guard<mutex> groups_lock(m_groups_mutex);
        auto add_to_group = [&](const string& name) {
            auto it = m_groups.find(name);
            if (it != m_groups.end()) {
                it->second.add_process(new_instance_id);
                m_groups_of_process[new_instance_id].insert(it->second.m_instance_id);
            }
        };
        add_to_group("_sintra_all_processes");
        add_to_group("_sintra_external_processes");
    }

    Managed_process::Spawn_swarm_process_args spawn_args;
    spawn_args.binary_name = binary_name.empty() ? s_mproc->m_binary_name : binary_name;
    spawn_args.piid = new_instance_id;
    spawn_args.occurrence = 1; // mark as non-initial to skip startup barrier
    spawn_args.args = {
        spawn_args.binary_name,
        "--branch_index",   std::to_string(branch_index),
        "--swarm_id",       std::to_string(s_mproc->m_swarm_id),
        "--instance_id",    std::to_string(new_instance_id),
        "--coordinator_id", std::to_string(s_coord_id)
    };
    auto result = s_mproc->spawn_swarm_process(spawn_args);

    if (!result.success) {
        // Roll back group insertion on spawn failure.
        lock_guard<mutex> groups_lock(m_groups_mutex);
        auto remove_from_group = [&](const string& name) {
            auto it = m_groups.find(name);
            if (it != m_groups.end()) {
                it->second.remove_process(new_instance_id);
            }
        };
        remove_from_group("_sintra_all_processes");
        remove_from_group("_sintra_external_processes");
        m_groups_of_process.erase(new_instance_id);

        lock_guard<mutex> guard(m_publish_mutex);
        m_inflight_joins.erase(branch_index);
        m_joined_process_branch.erase(new_instance_id);
        return invalid_instance_id;
    }

    return new_instance_id;
}



// EXPORTED FOR RPC
inline
void Coordinator::print(const string& str)
{
    Log_stream(log_level::info) << str;
}



// EXPORTED FOR RPC
inline
void Coordinator::enable_recovery(instance_id_type piid)
{
    // enable crash recovery for the calling process
    assert(is_process(piid));
    m_requested_recovery.insert(piid);
}

inline
void Coordinator::recover_if_required(const Crash_info& info)
{
    assert(is_process(info.process_iid));

    if (!m_requested_recovery.count(info.process_iid)) {
        return;
    }

    Recovery_policy policy;
    Recovery_runner runner;
    {
        std::lock_guard<mutex> lock(m_lifecycle_mutex);
        policy = m_recovery_policy;
        runner = m_recovery_runner;
    }

    bool should_recover = true;
    if (policy) {
        should_recover = policy(info);
    }

    if (!should_recover) {
        return;
    }

    auto should_cancel = [this]() {
        return m_shutdown.load(std::memory_order_acquire);
    };

    auto spawned = std::make_shared<std::atomic<bool>>(false);
    auto spawn_now = [this, info, should_cancel, spawned]() {
        if (should_cancel()) {
            return;
        }
        if (spawned->exchange(true)) {
            return;
        }
        auto& s = s_mproc->m_cached_spawns[info.process_iid];
        s_mproc->spawn_swarm_process(s);
    };

    if (!runner) {
        spawn_now();
        return;
    }

    Recovery_control control;
    control.should_cancel = should_cancel;
    control.spawn = spawn_now;
    {
        std::lock_guard<mutex> lock(m_recovery_threads_mutex);
        m_recovery_threads.emplace_back([info, runner, control]() mutable {
            runner(info, control);
        });
    }
}

inline
void Coordinator::set_recovery_policy(Recovery_policy policy)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_recovery_policy = std::move(policy);
}

inline
void Coordinator::set_recovery_runner(Recovery_runner runner)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_recovery_runner = std::move(runner);
}

inline
void Coordinator::set_lifecycle_handler(Lifecycle_handler handler)
{
    std::lock_guard<mutex> lock(m_lifecycle_mutex);
    m_lifecycle_handler = std::move(handler);
}

inline
void Coordinator::begin_shutdown()
{
    m_shutdown.store(true, std::memory_order_release);
}

inline
void Coordinator::note_process_crash(const Crash_info& info)
{
    {
        std::lock_guard<mutex> lock(m_crash_mutex);
        m_recent_crash_status[info.process_iid] = info.status;
    }
    process_lifecycle_event event;
    event.process_iid = info.process_iid;
    event.process_slot = info.process_slot;
    event.why = process_lifecycle_event::reason::crash;
    event.status = info.status;
    emit_lifecycle_event(event);
}

inline
void Coordinator::emit_lifecycle_event(const process_lifecycle_event& event)
{
    Lifecycle_handler handler;
    {
        std::lock_guard<mutex> lock(m_lifecycle_mutex);
        handler = m_lifecycle_handler;
    }
    if (handler) {
        handler(event);
    }
}

inline
std::vector<Coordinator::Pending_instance_publication>
Coordinator::finalize_initialization_tracking(instance_id_type process_iid)
{
    std::vector<Pending_instance_publication> ready_notifications;
    {
        std::lock_guard<mutex> lock(m_init_tracking_mutex);
        if (process_iid != invalid_instance_id) {
            m_processes_in_initialization.erase(process_iid);
        }
        if (m_processes_in_initialization.empty() && !m_delayed_instance_publications.empty()) {
            ready_notifications.swap(m_delayed_instance_publications);
        }
    }
    return ready_notifications;
}

inline
void Coordinator::mark_initialization_complete(instance_id_type process_iid)
{
    {
        std::lock_guard<mutex> publish_lock(m_publish_mutex);
        if (auto branch_it = m_joined_process_branch.find(process_iid); branch_it != m_joined_process_branch.end()) {
            m_inflight_joins.erase(branch_it->second);
            m_joined_process_branch.erase(branch_it);
        }
    }

    auto ready_notifications = finalize_initialization_tracking(process_iid);
    if (ready_notifications.empty()) {
        return;
    }

    for (auto& publication : ready_notifications) {
        emit_global<instance_published>(
            publication.type_id,
            publication.instance_id,
            publication.assigned_name);
    }
}



} // sintra


