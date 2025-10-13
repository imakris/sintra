/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef SINTRA_COORDINATOR_IMPL_H
#define SINTRA_COORDINATOR_IMPL_H


#include "coordinator.h"
#include "managed_process.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>


namespace sintra {


using std::cout;
using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;
using std::vector;



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
    basic_lock.unlock();
    
    if (b.processes_pending.empty()) {
        // new or reused barrier (may have failed previously)
        b.processes_pending = m_process_ids;
        if (s_coord) {
            for (auto it = b.processes_pending.begin(); it != b.processes_pending.end(); ) {
                if (static_cast<Coordinator*>(s_coord)->is_process_draining(*it)) {
                    it = b.processes_pending.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        b.failed = false;
        b.common_function_iid = make_instance_id();
    }

    b.processes_pending.erase(caller_piid);

    if (b.processes_pending.size() == 0) {
        auto additional_pids = m_process_ids;
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

        basic_lock.lock();
        auto it = m_barriers.find(barrier_name);
        if (it != m_barriers.end() && it->second.common_function_iid == current_common_fiid) {
            m_barriers.erase(it);
        }
        return s_mproc->m_out_req_c->get_leading_sequence();
    }
    else {
        std::pair<deferral, function<void()> > ret;
        ret.first.new_fiid = b.common_function_iid;
        ret.second = [&](){ b.m.unlock(); };
        throw ret;
    }
}



inline
sequence_counter_type Process_group::release_process_from_barriers(instance_id_type process_iid)
{
    sequence_counter_type max_sequence = 0;
    vector<string> barrier_names;

    {
        lock_guard<mutex> lock(m_call_mutex);
        barrier_names.reserve(m_barriers.size());
        for (auto& entry : m_barriers) {
            if (entry.second.processes_pending.find(process_iid) != entry.second.processes_pending.end()) {
                barrier_names.emplace_back(entry.first);
            }
        }
    }

    if (barrier_names.empty()) {
        return s_mproc->m_out_req_c->get_leading_sequence();
    }

    Message_prefix synthetic_message{};
    synthetic_message.sender_instance_id = process_iid;
    synthetic_message.receiver_instance_id = instance_id();

    Message_prefix* saved_message = s_tl_current_message;

    for (const auto& barrier_name : barrier_names) {
        bool still_pending = false;
        {
            lock_guard<mutex> lock(m_call_mutex);
            auto it = m_barriers.find(barrier_name);
            if (it != m_barriers.end() &&
                it->second.processes_pending.find(process_iid) != it->second.processes_pending.end()) {
                still_pending = true;
            }
        }

        if (!still_pending) {
            continue;
        }

        Message_prefix* previous = s_tl_current_message;
        s_tl_current_message = &synthetic_message;

        try {
            auto seq = barrier(barrier_name);
            if (seq > max_sequence) {
                max_sequence = seq;
            }
        }
        catch (std::pair<deferral, function<void()>>& deferred) {
            deferred.second();
        }

        s_tl_current_message = previous;
    }

    s_tl_current_message = saved_message;

    if (max_sequence == 0) {
        max_sequence = s_mproc->m_out_req_c->get_leading_sequence();
    }

    return max_sequence;
}


inline
Coordinator::Coordinator():
    Derived_transceiver<Coordinator>("", make_service_instance_id())
{
    for (auto& state : m_process_states) {
        state.store(static_cast<uint8_t>(process_state::active), std::memory_order_relaxed);
    }
    // Leave it empty. The coordinator is constructed within the Mnanaged_process
    // constructor, while still building the basic infrastructure.
}



inline
size_t Coordinator::state_index(instance_id_type piid) const
{
    auto idx = get_process_index(piid);
    if (idx == 0 || idx > max_process_index) {
        return process_state_capacity; // invalid
    }
    return static_cast<size_t>(idx);
}


inline
void Coordinator::set_process_state(instance_id_type piid, process_state state)
{
    auto idx = state_index(piid);
    if (idx >= process_state_capacity) {
        return;
    }
    m_process_states[idx].store(static_cast<uint8_t>(state), std::memory_order_release);
}


inline
bool Coordinator::is_process_draining(instance_id_type piid) const
{
    auto idx = state_index(piid);
    if (idx >= process_state_capacity) {
        return false;
    }
    return m_process_states[idx].load(std::memory_order_acquire) == static_cast<uint8_t>(process_state::draining);
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

    std::pair<deferral, std::function<void()>> ret;
    ret.first.new_fiid = common_function_iid;
    ret.second = [&](){
        m_publish_mutex.unlock();
    };
    throw ret;
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

        set_process_state(process_iid, process_state::active);

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

        set_process_state(process_iid, process_state::active);

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

        lock_guard<mutex> lock(m_groups_mutex);

        for (auto& group_entry : m_groups) {
            group_entry.second.remove_process(process_iid);
        }

        m_groups_of_process.erase(process_iid);

        // remove all group associations of unpublished process
        auto groups_it = m_groups_of_process.find(iid);
        if (groups_it != m_groups_of_process.end()) {
            m_groups_of_process.erase(groups_it);
        }

        //// and finally, if the process was being read, stop reading from it
        if (iid != s_mproc_id) {
            auto it = s_mproc->m_readers.find(process_iid);
            if (it != s_mproc->m_readers.end()) {
                it->second.stop_nowait();
            }
        }
    }

    emit_global<instance_unpublished>(tn.type_id, iid, tn.name);

    return true;
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
sequence_counter_type Coordinator::begin_process_draining(instance_id_type process_iid)
{
    set_process_state(process_iid, process_state::draining);

    sequence_counter_type max_sequence = 0;

    {
        lock_guard<mutex> lock(m_groups_mutex);
        for (auto& entry : m_groups) {
            auto seq = entry.second.release_process_from_barriers(process_iid);
            if (seq > max_sequence) {
                max_sequence = seq;
            }
        }
    }

    if (max_sequence == 0) {
        max_sequence = s_mproc->m_out_req_c->get_leading_sequence();
    }

    return max_sequence;
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


#endif
