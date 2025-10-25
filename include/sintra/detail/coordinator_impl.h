// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "coordinator.h"
#include "managed_process.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>


namespace sintra {

namespace detail {

struct Coordinator_group_membership
{
    template <typename GroupsOfProcess, typename Group>
    static bool add_process(GroupsOfProcess& groups_of_process, Group& group, instance_id_type process_iid)
    {
        auto [entry, inserted] = groups_of_process[process_iid].insert(group.instance_id());
        if (inserted) {
            group.add_process(process_iid);
        }
        return inserted;
    }

    template <typename GroupsOfProcess, typename Group>
    static void remove_process(GroupsOfProcess& groups_of_process, Group& group, instance_id_type process_iid)
    {
        auto map_it = groups_of_process.find(process_iid);
        if (map_it != groups_of_process.end()) {
            map_it->second.erase(group.instance_id());
            if (map_it->second.empty()) {
                groups_of_process.erase(map_it);
            }
        }

        group.remove_process(process_iid);
    }

    template <typename GroupsOfProcess, typename GroupsMap>
    static bool enroll_in_default_groups(
        GroupsOfProcess& groups_of_process,
        GroupsMap& groups,
        instance_id_type process_iid,
        bool include_in_external_group)
    {
        auto all_it = groups.find("_sintra_all_processes");
        if (all_it == groups.end()) {
            return true;
        }

        const bool added_to_all = add_process(groups_of_process, all_it->second, process_iid);
        (void)added_to_all;

        if (!include_in_external_group) {
            return true;
        }

        auto external_it = groups.find("_sintra_external_processes");
        if (external_it == groups.end()) {
            if (added_to_all) {
                remove_process(groups_of_process, all_it->second, process_iid);
            }
            return false;
        }

        add_process(groups_of_process, external_it->second, process_iid);
        return true;
    }
};

} // namespace detail


using std::cout;
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

    Barrier& b = m_barriers[barrier_name];
    b.m.lock(); // main barrier lock

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
        if (it != m_barriers.end() && it->second.common_function_iid == current_common_fiid) {
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
        auto& barrier = barrier_it->second;
        std::unique_lock barrier_lock(barrier.m);

        const bool touched_pending = barrier.processes_pending.erase(process_iid) > 0;
        const bool touched_arrived = barrier.processes_arrived.erase(process_iid) > 0;

        if (!touched_pending && !touched_arrived) {
            ++barrier_it;
            continue;
        }

        if (!barrier.processes_pending.empty()) {
            ++barrier_it;
            continue;
        }

        Barrier_completion completion;
        completion.common_function_iid = barrier.common_function_iid;
        completion.recipients.assign(
            barrier.processes_arrived.begin(),
            barrier.processes_arrived.end());

        if (touched_arrived) {
            completion.recipients.push_back(process_iid);
        }

        barrier.processes_arrived.clear();
        barrier.common_function_iid = invalid_instance_id;

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
        draining_state.store(0, std::memory_order_relaxed);
    }
}



inline
Coordinator::~Coordinator()
{
    s_coord     = nullptr;
    s_coord_id  = 0;
}


inline bool Coordinator::add_process_to_group_locked(
    std::unique_lock<std::mutex>& groups_lock,
    Process_group& group,
    instance_id_type process_iid)
{
    assert(groups_lock.owns_lock());
    return detail::Coordinator_group_membership::add_process(m_groups_of_process, group, process_iid);
}


inline void Coordinator::remove_process_from_group_locked(
    std::unique_lock<std::mutex>& groups_lock,
    Process_group& group,
    instance_id_type process_iid)
{
    assert(groups_lock.owns_lock());
    detail::Coordinator_group_membership::remove_process(m_groups_of_process, group, process_iid);
}


inline bool Coordinator::enroll_process_in_default_groups_locked(
    std::unique_lock<std::mutex>& groups_lock,
    instance_id_type process_iid,
    bool include_in_external_group)
{
    assert(groups_lock.owns_lock());
    return detail::Coordinator_group_membership::enroll_in_default_groups(
        m_groups_of_process,
        m_groups,
        process_iid,
        include_in_external_group);
}


// EXPORTED FOR RPC
inline
type_id_type Coordinator::resolve_type(const string& pretty_name)
{
    lock_guard<mutex> lock(m_type_resolution_mutex);
    auto it = s_mproc->m_type_id_of_type_name.find(pretty_name);
    if (it != s_mproc->m_type_id_of_type_name.end()) {
        return it->second;
    }

    // a type is always assumed to exist
    return s_mproc->m_type_id_of_type_name[pretty_name] = make_type_id();
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::resolve_instance(const string& assigned_name)
{
    auto it = s_mproc->m_instance_id_of_assigned_name.find(assigned_name);
    if (it != s_mproc->m_instance_id_of_assigned_name.end()) {
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

    auto true_sequence = [&]() {
        emit_global<instance_published>(tid, iid, assigned_name);
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

        return true_sequence();
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
            m_draining_process_states[slot].store(0, std::memory_order_release);
        }

        return true_sequence();
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

    // if it is a Managed_process is being unpublished, more cleanup is required
    if (iid == process_iid) {

        // remove all name lookup entries resolving to the unpublished process
        auto name_map = s_mproc->m_instance_id_of_assigned_name.scoped();
        for (auto it = name_map.begin(); it != name_map.end(); )
        {
            if (process_of(it->second) == process_iid) {
                it = name_map.erase(it);
            }
            else {
                ++it;
            }
        }

        // remove the unpublished process's registry entry
        m_transceiver_registry.erase(pr_it);

        // CRITICAL: Mark as draining BEFORE removing from groups to prevent barriers
        // from including this process. This handles both graceful shutdown (where
        // begin_process_draining was already called) and crash scenarios (where it wasn't).
        const auto draining_index = get_process_index(process_iid);
        if (draining_index > 0 && draining_index <= static_cast<uint64_t>(max_process_index)) {
            const auto slot = static_cast<size_t>(draining_index);
            m_draining_process_states[slot].store(1, std::memory_order_release);
        }

        lock_guard<mutex> lock(m_groups_mutex);

        for (auto& group_entry : m_groups) {
            group_entry.second.remove_process(process_iid);
        }

        m_groups_of_process.erase(process_iid);

        // Keep the draining bit set; it will be re-initialized to 0 (ACTIVE)
        // when a new process is published into this slot. This prevents the race
        // where resetting too early allows concurrent barriers to include a dying process.

        // remove all group associations of unpublished process
        auto groups_it = m_groups_of_process.find(iid);
        if (groups_it != m_groups_of_process.end()) {
            m_groups_of_process.erase(groups_it);
        }

        //// and finally, if the process was being read, stop reading from it
        if (iid != s_mproc_id) {
            std::shared_lock<std::shared_mutex> readers_lock(s_mproc->m_readers_mutex);
            auto it = s_mproc->m_readers.find(process_iid);
            if (it != s_mproc->m_readers.end()) {
                if (auto& reader = it->second) {
                    reader->stop_nowait();
                }
            }
        }
    }

    emit_global<instance_unpublished>(tn.type_id, iid, tn.name);

    return true;
}


inline sequence_counter_type Coordinator::begin_process_draining(instance_id_type process_iid)
{
    const auto draining_index = get_process_index(process_iid);
    if (draining_index > 0 && draining_index <= static_cast<uint64_t>(max_process_index)) {
        const auto slot = static_cast<size_t>(draining_index);
        m_draining_process_states[slot].store(1, std::memory_order_release);
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
    return s_mproc->m_out_rep_c->get_leading_sequence();
}


inline bool Coordinator::is_process_draining(instance_id_type process_iid) const
{
    const auto draining_index = get_process_index(process_iid);
    if (draining_index == 0 || draining_index > static_cast<uint64_t>(max_process_index)) {
        return false;
    }

    const auto slot = static_cast<size_t>(draining_index);
    return m_draining_process_states[slot].load(std::memory_order_acquire) != 0;
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
void Coordinator::print(const string& str)
{
    cout << str;
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
void Coordinator::recover_if_required(instance_id_type piid)
{
    assert(is_process(piid));
    if (m_requested_recovery.count(piid)) {
        // respawn
        auto& s = s_mproc->m_cached_spawns[piid];
        s_mproc->spawn_swarm_process(s);
    }
    else {
        // remove traces
        // ... [implement]
    }
}



} // sintra


