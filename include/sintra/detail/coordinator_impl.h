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
#include "debug_log.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace sintra {


using std::cout;
using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;



namespace detail {

struct Bootstrap_group_state
{
    uint32_t expected = 0;
    std::atomic<uint32_t> joined{0};
    std::mutex m;
    std::condition_variable cv;
    std::unordered_set<instance_id_type> members;
    std::unordered_set<instance_id_type> accounted_absentees;
    std::uint64_t swarm_id = 0;
    std::string group_name;
    bool initialized = false;
};

inline std::mutex& bootstrap_group_states_mutex()
{
    static std::mutex m;
    return m;
}

inline std::unordered_map<std::uint64_t, std::unique_ptr<Bootstrap_group_state>>& bootstrap_group_states()
{
    static std::unordered_map<std::uint64_t, std::unique_ptr<Bootstrap_group_state>> states;
    return states;
}

inline std::string format_instance_set(const std::unordered_set<instance_id_type>& members)
{
    std::ostringstream oss;
    oss << '[';
    bool first = true;
    for (const auto member : members) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << member;
    }
    oss << ']';
    return oss.str();
}

inline std::uint64_t bootstrap_group_key(std::uint64_t swarm_id, const std::string& name)
{
    std::uint64_t h = 1469598103934665603ull ^ (swarm_id * 1099511628211ull);
    for (unsigned char c : name) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

inline Bootstrap_group_state* ensure_bootstrap_group_state(std::uint64_t swarm_id, const std::string& name)
{
    std::lock_guard<std::mutex> map_lock(bootstrap_group_states_mutex());
    auto key = bootstrap_group_key(swarm_id, name);
    auto& map = bootstrap_group_states();
    auto& entry = map[key];
    if (!entry) {
        entry = std::make_unique<Bootstrap_group_state>();
    }
    return entry.get();
}

inline void reset_bootstrap_group_state(
    std::uint64_t swarm_id,
    const std::string& group_name,
    uint32_t expected_members,
    instance_id_type coordinator_member)
{
    auto* state = ensure_bootstrap_group_state(swarm_id, group_name);

    uint32_t initial_joined = 0;
    {
        std::lock_guard<std::mutex> state_lock(state->m);
        state->initialized = false;
        state->expected = expected_members;
        state->members.clear();
        state->accounted_absentees.clear();
        state->swarm_id = swarm_id;
        state->group_name = group_name;

        if (expected_members > 0 && coordinator_member != invalid_instance_id) {
            state->members.insert(coordinator_member);
            initial_joined = 1;
        }

        state->joined.store(initial_joined, std::memory_order_release);
        state->initialized = true;
    }

    state->cv.notify_all();

    const auto members_snapshot = format_instance_set(state->members);
    const auto absentees_snapshot = format_instance_set(state->accounted_absentees);
    detail::trace_sync("coordinator.group.init", [&](auto& os) {
        os << "swarm=" << swarm_id
           << " name=" << group_name
           << " expected=" << expected_members
           << " initial_joined=" << initial_joined
           << " coordinator=" << coordinator_member
           << " members=" << members_snapshot
           << " absentees=" << absentees_snapshot;
    });
}

inline void account_bootstrap_absence(instance_id_type member_id, const char* reason)
{
    std::vector<Bootstrap_group_state*> states;
    {
        std::lock_guard<std::mutex> map_lock(bootstrap_group_states_mutex());
        auto& map = bootstrap_group_states();
        states.reserve(map.size());
        for (auto& entry : map) {
            states.push_back(entry.second.get());
        }
    }

    for (auto* state : states) {
        bool notify = false;
        uint32_t expected_after = 0;
        std::uint64_t swarm_id = 0;
        std::string group_name;

        {
            std::unique_lock<std::mutex> state_lock(state->m);
            if (!state->initialized) {
                continue;
            }

            swarm_id = state->swarm_id;
            group_name = state->group_name;

            if (state->members.find(member_id) != state->members.end()) {
                continue;
            }

            if (!state->accounted_absentees.insert(member_id).second) {
                continue;
            }

            if (state->expected > 0) {
                state->expected -= 1;
            }
            expected_after = state->expected;
            notify = true;
        }

        if (notify) {
            const auto members_snapshot = format_instance_set(state->members);
            const auto absentees_snapshot = format_instance_set(state->accounted_absentees);
            detail::trace_sync("coordinator.group.drop_absent", [&](auto& os) {
                os << "swarm=" << swarm_id
                   << " name=" << group_name
                   << " member=" << member_id
                   << " reason=" << reason
                   << " expected=" << expected_after
                   << " members=" << members_snapshot
                   << " absentees=" << absentees_snapshot;
            });
            state->cv.notify_all();
        }
    }
}

} // namespace detail



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

    detail::trace_sync("barrier.lock", [&](auto& os) {
        os << "name=" << barrier_name
           << " caller=" << caller_piid
           << " coordinator=" << s_coord_id
           << " local_instance=" << s_mproc_id
           << " pending_before=" << b.processes_pending.size()
           << " arrived_before=" << b.processes_arrived.size();
    });

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

        detail::trace_sync("barrier.snapshot", [&](auto& os) {
            os << "name=" << barrier_name
               << " caller=" << caller_piid
               << " membership=" << b.processes_pending.size();
        });
    }

    // Now safe to release m_call_mutex - barrier state is consistent and other threads
    // need to be able to arrive at the barrier concurrently
    basic_lock.unlock();

    b.processes_arrived.insert(caller_piid);
    b.processes_pending.erase(caller_piid);

    detail::trace_sync("barrier.arrive", [&](auto& os) {
        os << "name=" << barrier_name
           << " caller=" << caller_piid
           << " pending_remaining=" << b.processes_pending.size()
           << " arrived=" << b.processes_arrived.size()
           << " common_fiid=" << b.common_function_iid;
    });

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

        detail::trace_sync("barrier.complete", [&](auto& os) {
            os << "name=" << barrier_name
               << " caller=" << caller_piid
               << " recipients=" << additional_pids.size() + 1
               << " common_fiid=" << current_common_fiid
               << " reply_seq=" << s_mproc->m_out_rep_c->get_leading_sequence();
        });

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
        detail::trace_sync("barrier.defer", [&](auto& os) {
            os << "name=" << barrier_name
               << " caller=" << caller_piid
               << " pending_remaining=" << b.processes_pending.size()
               << " common_fiid=" << b.common_function_iid;
        });
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

        detail::trace_sync("barrier.drop", [&](auto& os) {
            os << "name=" << barrier_it->first
               << " removing=" << process_iid
               << " pending_before=" << barrier.processes_pending.size()
               << " arrived_before=" << barrier.processes_arrived.size();
        });

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

        detail::trace_sync("barrier.drop.complete", [&](auto& os) {
            os << "name=" << barrier_it->first
               << " removing=" << process_iid
               << " recipients=" << completion.recipients.size()
               << " common_fiid=" << completion.common_function_iid;
        });

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

        detail::trace_sync("barrier.emit", [&](auto& os) {
            os << "common_fiid=" << completion.common_function_iid
               << " recipients=" << completion.recipients.size();
        });

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

        detail::account_bootstrap_absence(process_iid, "unpublish");

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
                it->second.stop_nowait();
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

    detail::account_bootstrap_absence(process_iid, "drain");

    struct Group_reference
    {
        std::string name;
        Process_group* group = nullptr;
    };

    std::vector<Group_reference> groups_to_notify;
    {
        lock_guard<mutex> lock(m_groups_mutex);
        groups_to_notify.reserve(m_groups.size());
        for (auto& entry : m_groups) {
            groups_to_notify.push_back({entry.first, &entry.second});
        }
    }

    struct Pending_completion
    {
        std::string group_name;
        std::vector<Process_group::Barrier_completion> completions;
    };

    std::vector<Pending_completion> pending_completions;
    pending_completions.reserve(groups_to_notify.size());

    for (auto& reference : groups_to_notify) {
        if (!reference.group) {
            continue;
        }

        std::vector<Process_group::Barrier_completion> completions;
        reference.group->drop_from_inflight_barriers(process_iid, completions);
        if (!completions.empty()) {
            pending_completions.push_back({reference.name, std::move(completions)});
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
    instance_id_type ret = invalid_instance_id;
    bool coordinator_in_group = false;

    {
        lock_guard<mutex> lock(m_groups_mutex);

        auto it = m_groups.find(name);
        instance_id_type previous_group_instance = invalid_instance_id;
        bool previous_group_published = false;
        if (it != m_groups.end()) {
            auto& group = it->second;
            previous_group_instance = group.m_instance_id;
            previous_group_published = group.is_published();

            const auto previous_members = group.m_process_ids;
            for (const auto& member : previous_members) {
                auto map_it = m_groups_of_process.find(member);
                if (map_it == m_groups_of_process.end()) {
                    continue;
                }

                map_it->second.erase(previous_group_instance);
                if (map_it->second.empty()) {
                    m_groups_of_process.erase(map_it);
                }
            }

            m_groups.erase(it);
        }

        if (previous_group_published) {
            unpublish_transceiver(previous_group_instance);
        }

        auto& group = m_groups[name];
        group.set(member_process_ids);
        ret = group.m_instance_id;

        for (const auto& member : member_process_ids) {
            m_groups_of_process[member].insert(ret);
        }

        if (!group.assign_name(name)) {
            detail::trace_sync("coordinator.group_assign_failed", [&](auto& os) {
                os << "name=" << name << " instance=" << ret;
            });
        }

        coordinator_in_group = member_process_ids.count(s_coord_id) != 0;
    }

    const auto swarm_id = s_mproc ? s_mproc->m_swarm_id : 0u;
    const auto expected_members = static_cast<uint32_t>(member_process_ids.size());
    detail::reset_bootstrap_group_state(
        swarm_id,
        name,
        expected_members,
        coordinator_in_group ? s_coord_id : invalid_instance_id);

    const auto members_snapshot = detail::format_instance_set(member_process_ids);
    detail::trace_sync("coordinator.group.make", [&](auto& os) {
        os << "swarm=" << swarm_id
           << " name=" << name
           << " expected=" << expected_members
           << " coordinator_in_group=" << coordinator_in_group
           << " members=" << member_process_ids.size()
           << " member_ids=" << members_snapshot;
    });

    return ret;
}

inline instance_id_type Coordinator::join_and_wait_group(
    std::uint64_t swarm_id,
    const std::string& group_name,
    instance_id_type member_id)
{
    auto* state = detail::ensure_bootstrap_group_state(swarm_id, group_name);

    std::unique_lock<std::mutex> state_lock(state->m);
    detail::trace_sync("coordinator.group.rpc_entry", [&](auto& os) {
        os << "swarm=" << swarm_id
           << " name=" << group_name
           << " member=" << member_id
           << " initialized=" << static_cast<int>(state->initialized)
           << " expected=" << state->expected
           << " joined=" << state->joined.load(std::memory_order_acquire);
    });
    if (!state->initialized) {
        detail::trace_sync("coordinator.group.await_init", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " member=" << member_id;
        });

        state->cv.wait(state_lock, [&] {
            return state->initialized;
        });

        detail::trace_sync("coordinator.group.init_ready", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " member=" << member_id
               << " expected=" << state->expected;
        });
    }

    uint32_t joined_after_insert = state->joined.load(std::memory_order_acquire);
    if (state->members.insert(member_id).second) {
        joined_after_insert = state->joined.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    else {
        joined_after_insert = state->joined.load(std::memory_order_acquire);
    }

    const auto expected = state->expected;
    const auto members_snapshot = detail::format_instance_set(state->members);
    const auto absentees_snapshot = detail::format_instance_set(state->accounted_absentees);

    detail::trace_sync("coordinator.group.join", [&](auto& os) {
        os << "swarm=" << swarm_id
           << " name=" << group_name
           << " member=" << member_id
           << " joined=" << joined_after_insert
           << " expected=" << expected
           << " members=" << members_snapshot
           << " absentees=" << absentees_snapshot;
    });

    auto ready_predicate = [&]() {
        if (!state->initialized) {
            return false;
        }

        const auto expected_now = state->expected;
        if (expected_now == 0) {
            return true;
        }

        const auto joined_now = state->joined.load(std::memory_order_acquire);
        if (joined_now >= expected_now) {
            return true;
        }

        if (s_mproc && s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING) {
            return true;
        }

        return false;
    };

    auto log_wait_state = [&](const char* stage) {
        const auto expected_now = state->expected;
        const auto joined_now = state->joined.load(std::memory_order_acquire);
        const auto pending = expected_now > joined_now ? expected_now - joined_now : 0u;
        const auto members_now = detail::format_instance_set(state->members);
        const auto absentees_now = detail::format_instance_set(state->accounted_absentees);
        detail::trace_sync("coordinator.group.wait_ready", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " member=" << member_id
               << " stage=" << stage
               << " joined=" << joined_now
               << " expected=" << expected_now
               << " pending=" << pending
               << " members=" << members_now
               << " absentees=" << absentees_now;
        });
    };

    if (!ready_predicate()) {
        log_wait_state("start");
        while (!state->cv.wait_for(state_lock, std::chrono::milliseconds(250), ready_predicate)) {
            log_wait_state("tick");
        }
        log_wait_state("finish");
    }

    const auto final_expected = state->expected;
    const auto final_joined = state->joined.load(std::memory_order_acquire);
    const bool ready = final_expected == 0 || final_joined >= final_expected;
    const auto final_members = detail::format_instance_set(state->members);
    const auto final_absentees = detail::format_instance_set(state->accounted_absentees);
    if (ready) {
        detail::trace_sync("coordinator.group.ready", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " joined=" << final_joined
               << " expected=" << final_expected
               << " members=" << final_members
               << " absentees=" << final_absentees;
        });
    }
    else {
        detail::trace_sync("coordinator.group.not_ready", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " member=" << member_id
               << " joined=" << final_joined
               << " expected=" << final_expected
               << " terminating="
               << static_cast<int>(s_mproc ? s_mproc->m_communication_state : Managed_process::COMMUNICATION_RUNNING)
               << " members=" << final_members
               << " absentees=" << final_absentees;
        });
    }
    state->cv.notify_all();

    state_lock.unlock();

    instance_id_type group_instance = invalid_instance_id;
    {
        lock_guard<mutex> groups_lock(m_groups_mutex);
        auto it = m_groups.find(group_name);
        if (it != m_groups.end()) {
            group_instance = it->second.m_instance_id;
        }
    }

    if (!ready && group_instance == invalid_instance_id) {
        detail::trace_sync("coordinator.group.cancelled", [&](auto& os) {
            os << "swarm=" << swarm_id
               << " name=" << group_name
               << " member=" << member_id;
        });
    }

    return group_instance;
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


#endif
