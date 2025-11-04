// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../process/coordinator.h"
#include "../process/managed_process.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>


namespace sintra {


using std::cout;
using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;



// EXPORTED EXCLUSIVELY FOR RPC
inline
barrier_completion_payload Process_group::barrier(
    const string& barrier_name,
    uint32_t request_flags,
    boot_id_type boot_id)
{
    std::unique_lock basic_lock(m_call_mutex);
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;
    if (m_process_ids.find(caller_piid) == m_process_ids.end()) {
        throw std::logic_error("The caller is not a member of the process group.");
    }

    auto* current_message = s_tl_current_message;
    assert(current_message);
    mark_rpc_reply_deferred();

    auto send_payload = [&](instance_id_type recipient,
                            instance_id_type function_iid,
                            const barrier_completion_payload& payload) {
        using return_message_type = Message<Enclosure<barrier_completion_payload>, void, not_defined_type_id>;
        auto copy = payload;
        auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
            vb_size<return_message_type>(copy), copy);
        Transceiver::finalize_rpc_write(
            placed_msg,
            recipient,
            function_iid,
            this,
            not_defined_type_id);
    };

    // Update the coordinator's best-effort boot id snapshot for the caller. The
    // architecture expects rendezvous membership to key on `(iid, boot_id)`; in
    // the absence of a dedicated registry feed we learn the boot id from the
    // caller itself.
    m_process_boot_ids[caller_piid] = boot_id;

    auto& barrier_entry = m_barriers[barrier_name];
    if (!barrier_entry) {
        barrier_entry = std::make_shared<Barrier>();
    }

    auto barrier = barrier_entry; // keep the barrier alive even if the map rehashes
    Barrier& b = *barrier;
    std::unique_lock barrier_lock(b.m);

    auto make_fast_fail_payload = [&](barrier_failure reason) {
        auto payload = make_barrier_completion_payload();
        payload.barrier_epoch = b.barrier_epoch;
        const auto frozen_mask = b.requirement_mask;
        payload.requirement_mask = (frozen_mask == std::numeric_limits<uint32_t>::max())
                                       ? request_flags
                                       : frozen_mask;
        payload.rendezvous.state = barrier_state::failed;
        payload.rendezvous.failure_code = reason;
        payload.rendezvous.sequence = invalid_sequence;
        return payload;
    };

    if (!b.rendezvous_active) {
        b.pending.clear();
        b.arrivals.clear();
        b.waiter_function_ids.clear();
        b.membership_snapshot.clear();
        b.rendezvous_active = true;
        b.requirement_mask = std::numeric_limits<uint32_t>::max();
        b.rendezvous_complete = false;
        b.rendezvous_sequence = invalid_sequence;
        ++b.barrier_epoch;
        b.completion_template = make_barrier_completion_payload();
        b.completion_template.barrier_epoch = b.barrier_epoch;
        b.completion_template.requirement_mask = 0;
        b.completion_template.rendezvous = make_phase_status(
            barrier_state::not_requested,
            barrier_failure::none,
            invalid_sequence);

        for (auto iid : m_process_ids) {
            boot_id_type member_boot = 0;
            auto boot_it = m_process_boot_ids.find(iid);
            if (boot_it != m_process_boot_ids.end()) {
                member_boot = boot_it->second;
            }
            Participant_id participant{iid, member_boot};
            b.pending.insert(participant);
            b.membership_snapshot.emplace(iid, participant);
        }

        if (auto* coord = s_coord) {
            for (auto it = b.pending.begin(); it != b.pending.end(); ) {
                if (coord->is_process_draining(it->iid)) {
                    it = b.pending.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
    }

    basic_lock.unlock();

    Participant_id arrival_key{caller_piid, boot_id};
    Participant_id expected_key{};
    if (auto snapshot_it = b.membership_snapshot.find(caller_piid);
        snapshot_it != b.membership_snapshot.end())
    {
        expected_key = snapshot_it->second;
    }

    auto fail_rendezvous = [&](barrier_failure reason,
                               const Participant_id& failing_participant,
                               instance_id_type failing_function) {
        auto payload = make_fast_fail_payload(reason);
        const auto requirement_mask_snapshot = b.requirement_mask;
        auto waiters = b.waiter_function_ids;

        b.waiter_function_ids.clear();
        b.pending.clear();
        b.arrivals.clear();
        b.membership_snapshot.clear();
        b.rendezvous_active = false;
        b.rendezvous_complete = true;
        b.rendezvous_sequence = invalid_sequence;
        b.completion_template.barrier_sequence = invalid_sequence;
        b.completion_template.rendezvous = payload.rendezvous;

        barrier_lock.unlock();

        if (payload.requirement_mask == 0 &&
            requirement_mask_snapshot != std::numeric_limits<uint32_t>::max())
        {
            payload.requirement_mask = requirement_mask_snapshot;
        }

        auto dispatch = [&](const Participant_id& participant, instance_id_type function_iid) {
            if (function_iid == invalid_instance_id) {
                return;
            }
            send_payload(participant.iid, function_iid, payload);
        };

        dispatch(failing_participant, failing_function);

        for (const auto& [participant, function_iid] : waiters) {
            if (participant.iid == failing_participant.iid &&
                function_iid == failing_function) {
                continue;
            }
            dispatch(participant, function_iid);
        }

        return make_barrier_completion_payload();
    };

    if (b.requirement_mask == std::numeric_limits<uint32_t>::max()) {
        b.requirement_mask = request_flags;
        b.completion_template.requirement_mask = request_flags;
    }
    else if (b.requirement_mask != request_flags) {
        return fail_rendezvous(barrier_failure::incompatible_request,
                               arrival_key,
                               current_message->function_instance_id);
    }

    if (expected_key.iid == 0 && expected_key.boot_id == 0) {
        auto payload = make_fast_fail_payload(barrier_failure::incompatible_request);
        const auto function_iid = current_message->function_instance_id;
        barrier_lock.unlock();
        send_payload(caller_piid, function_iid, payload);
        return make_barrier_completion_payload();
    }

    if (b.arrivals.find(arrival_key) != b.arrivals.end() ||
        b.arrivals.find(expected_key) != b.arrivals.end())
    {
        auto payload = make_fast_fail_payload(barrier_failure::incompatible_request);
        const auto function_iid = current_message->function_instance_id;
        barrier_lock.unlock();
        send_payload(caller_piid, function_iid, payload);
        return make_barrier_completion_payload();
    }

    auto pending_it = b.pending.find(expected_key);
    if (pending_it == b.pending.end()) {
        auto payload = make_fast_fail_payload(barrier_failure::incompatible_request);
        const auto function_iid = current_message->function_instance_id;
        barrier_lock.unlock();
        send_payload(caller_piid, function_iid, payload);
        return make_barrier_completion_payload();
    }

    const bool boot_mismatch = expected_key.boot_id != boot_id;
    if (boot_mismatch) {
        if (b.completion_template.rendezvous.state == barrier_state::not_requested) {
            b.completion_template.rendezvous.state = barrier_state::downgraded;
            b.completion_template.rendezvous.failure_code = barrier_failure::peer_lost;
        }
        else if (b.completion_template.rendezvous.state == barrier_state::satisfied) {
            b.completion_template.rendezvous.state = barrier_state::downgraded;
            b.completion_template.rendezvous.failure_code = barrier_failure::peer_lost;
        }
        else if (b.completion_template.rendezvous.state == barrier_state::downgraded &&
                 b.completion_template.rendezvous.failure_code == barrier_failure::none)
        {
            b.completion_template.rendezvous.failure_code = barrier_failure::peer_lost;
        }

        b.membership_snapshot[caller_piid] = arrival_key;
        b.arrivals.erase(expected_key);
        b.waiter_function_ids.erase(expected_key);
    }

    b.pending.erase(pending_it);
    b.arrivals.insert(arrival_key);
    b.waiter_function_ids[arrival_key] = current_message->function_instance_id;

    if (!b.pending.empty()) {
        barrier_lock.unlock();
        return make_barrier_completion_payload();
    }

    const auto flush_sequence = s_mproc->m_out_rep_c->get_leading_sequence();
    b.rendezvous_sequence = flush_sequence;
    b.rendezvous_complete = true;
    b.rendezvous_active = false;
    b.completion_template.barrier_sequence = flush_sequence;
    if (b.completion_template.rendezvous.state == barrier_state::not_requested) {
        b.completion_template.rendezvous.state = barrier_state::satisfied;
        b.completion_template.rendezvous.failure_code = barrier_failure::none;
    }
    b.completion_template.requirement_mask = b.requirement_mask;
    b.completion_template.rendezvous.sequence = flush_sequence;

    auto payload = b.completion_template;
    auto waiters = b.waiter_function_ids;
    const auto requirement_mask = b.requirement_mask;
    b.waiter_function_ids.clear();
    b.arrivals.clear();
    b.membership_snapshot.clear();
    barrier_lock.unlock();

    if (payload.requirement_mask == 0 &&
        requirement_mask != std::numeric_limits<uint32_t>::max())
    {
        payload.requirement_mask = requirement_mask;
    }

    for (const auto& [participant, function_iid] : waiters) {
        send_payload(participant.iid, function_iid, payload);
    }

    return make_barrier_completion_payload();
}


inline void Process_group::drop_from_inflight_barriers(instance_id_type process_iid)
{
    std::lock_guard basic_lock(m_call_mutex);

    using return_message_type = Message<Enclosure<barrier_completion_payload>, void, not_defined_type_id>;

    for (auto barrier_it = m_barriers.begin(); barrier_it != m_barriers.end(); ) {
        auto barrier = barrier_it->second;
        if (!barrier) {
            barrier_it = m_barriers.erase(barrier_it);
            continue;
        }

        std::unique_lock barrier_lock(barrier->m);

        auto erase_from_set = [&](auto& container) {
            bool erased = false;
            for (auto it = container.begin(); it != container.end(); ) {
                if (it->iid == process_iid) {
                    it = container.erase(it);
                    erased = true;
                }
                else {
                    ++it;
                }
            }
            return erased;
        };

        auto erase_from_waiters = [&](auto& container) {
            bool erased = false;
            for (auto it = container.begin(); it != container.end(); ) {
                if (it->first.iid == process_iid) {
                    it = container.erase(it);
                    erased = true;
                }
                else {
                    ++it;
                }
            }
            return erased;
        };

        const bool touched_pending = erase_from_set(barrier->pending);
        const bool touched_arrived = erase_from_set(barrier->arrivals);
        const bool touched_waiter = erase_from_waiters(barrier->waiter_function_ids);

        if (!touched_pending && !touched_arrived && !touched_waiter) {
            ++barrier_it;
            continue;
        }

        barrier->membership_snapshot.erase(process_iid);
        barrier->pending.clear();
        barrier->arrivals.clear();
        barrier->rendezvous_active = false;
        barrier->completion_template.barrier_sequence = invalid_sequence;
        barrier->completion_template.rendezvous.state = barrier_state::failed;
        barrier->completion_template.rendezvous.failure_code = barrier_failure::peer_lost;
        barrier->completion_template.rendezvous.sequence = invalid_sequence;
        barrier->rendezvous_complete = true;

        auto waiters = barrier->waiter_function_ids;
        barrier->waiter_function_ids.clear();
        auto payload = barrier->completion_template;
        const auto requirement_mask = barrier->requirement_mask;

        barrier_lock.unlock();

        if (payload.requirement_mask == 0 &&
            requirement_mask != std::numeric_limits<uint32_t>::max())
        {
            payload.requirement_mask = requirement_mask;
        }

        for (const auto& [participant, function_iid] : waiters) {
            auto copy = payload;
            auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
                vb_size<return_message_type>(copy), copy);
            Transceiver::finalize_rpc_write(
                placed_msg,
                participant.iid,
                function_iid,
                this,
                not_defined_type_id);
        }

        ++barrier_it;
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

        {
            lock_guard<mutex> lock(m_groups_mutex);
            for (auto& [name, group] : m_groups) {
                (void)name;
                group.drop_from_inflight_barriers(process_iid);
                group.remove_process(process_iid);
            }

            m_groups_of_process.erase(process_iid);
        }

        // Keep the draining bit set; it will be re-initialized to 0 (ACTIVE)
        // when a new process is published into this slot. This prevents the race
        // where resetting too early allows concurrent barriers to include a dying process.

        // remove all group associations of unpublished process
        {
            std::lock_guard<mutex> groups_lock(m_groups_mutex);
            auto groups_it = m_groups_of_process.find(iid);
            if (groups_it != m_groups_of_process.end()) {
                m_groups_of_process.erase(groups_it);
            }
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

    {
        lock_guard<mutex> lock(m_groups_mutex);
        for (auto& [name, group] : m_groups) {
            (void)name;
            group.drop_from_inflight_barriers(process_iid);
        }
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


