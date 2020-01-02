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


#include <iostream>
#include <mutex>

#include "managed_process.h"


namespace sintra {


using std::cout;
using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;


inline
Coordinator::Coordinator():
    Derived_transceiver<Coordinator>("", make_instance_id())
{
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



// EXPORTED FOR RPC
inline
bool Coordinator::publish_transceiver(type_id_type tid, instance_id_type iid, const string& assigned_name)
{
    lock_guard<mutex> lock(m_publish_mutex);

    // empty strings are not valid names
    if (assigned_name.empty()) {
        return false;
    }

    auto process_iid = process_of(iid);
    auto pr_it = m_transceiver_registry.find(process_iid);
    auto entry = tn_type{ tid, assigned_name };

    if (pr_it != m_transceiver_registry.end()) { // the transceiver's process is known
        
        auto& pr = pr_it->second; // process registry

        // observe the limit of transceivers per process
        if (pr.size() >= max_public_transceivers_per_proc) {
            return false;
        }

        // the transceiver must not have been already published
        if (pr.find(iid) != pr.end()) {
            return false;
        }

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return false;
        }

        s_mproc->m_instance_id_of_assigned_name[entry.name] = iid;
        pr[iid] = entry;

        emit_global<instance_published>(tid, iid, assigned_name);
        return true;
    }
    else
    if (iid == process_iid) { // the transceiver is a Managed_process

        // the assigned_name should not be taken
        if (resolve_instance(assigned_name) != invalid_instance_id) {
            return false;
        }

        s_mproc->m_instance_id_of_assigned_name[entry.name] = iid;
        m_transceiver_registry[iid][iid] = entry;

        emit_global<instance_published>(tid, iid, assigned_name);
        return true;
    }
    else {
        return false;
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
        //Message_prefix* m = s_tl_current_message;
        //instance_id_type process_id = m ? m->sender_instance_id : s_mproc->m_instance_id;

        lock_guard<mutex> lock(m_all_other_processes_done_mutex);

        // remove all name lookup entries resolving to the unpublished process
        for (auto it = s_mproc->m_instance_id_of_assigned_name.begin();
            it != s_mproc->m_instance_id_of_assigned_name.end(); )
        {
            if (process_of(it->second) == process_iid) {
                s_mproc->m_instance_id_of_assigned_name.erase(it++);
            }
            else {
                ++it;
            }
        }

        // remove the unpublished process's registry entry
        m_transceiver_registry.erase(pr_it);

        // remove the process from all groups it was member of
        for (auto group : m_groups_of_process[process_iid]) {
            m_processes_of_group[group].erase(process_iid);
        }
        m_groups_of_process.erase(process_iid);
        if (m_processes_of_group[Externally_coordinated::id()].size() == 0) {
            m_all_other_processes_done_condition.notify_all();
        }

        // remove all group associations of unpublished process
        auto groups_it = m_groups_of_process.find(iid);
        if (groups_it != m_groups_of_process.end()) {
            for (auto& group : groups_it->second) {
                m_processes_of_group.erase(group);
                m_group_sizes[group]--;
            }
            m_groups_of_process.erase(groups_it);
        }
    }

    emit_global<instance_unpublished>(tn.type_id, iid, tn.name);

    return true;
}



// EXPORTED FOR RPC
inline
bool Coordinator::barrier(type_id_type process_group_id)
{
    m_barrier_mutex.lock();

    auto &b = m_barriers[process_group_id];
    unique_lock<mutex> lock(b.m);

    m_barrier_mutex.unlock();

    if (++b.processes_reached == m_group_sizes[process_group_id]) {
        b.processes_reached = 0;
        lock.unlock();
        b.cv.notify_all();
    }
    else {
        do {
            // this code might be reached either if the other processes have
            // not called barrier, or if the group size has not been set (yet).
            b.cv.wait(lock);
        }
        while (b.processes_reached);
    }

    return true;
}



// EXPORTED FOR RPC
inline
bool Coordinator::add_this_process_into_group(type_id_type process_group_id)
{
    if (s_mproc->m_branched)
        return false;

    Message_prefix* m = s_tl_current_message;
    instance_id_type process_id = m ? m->sender_instance_id : s_mproc->m_instance_id;
    add_process_into_group(process_id, process_group_id);
    return true;
}



// EXPORTED FOR RPC
inline
void Coordinator::print(const string& str)
{
    cout << str;
}



inline
void Coordinator::set_group_size(type_id_type process_group_id, size_t size)
{
    m_group_sizes[process_group_id] = (uint32_t)size;
    m_barriers[process_group_id].cv.notify_all();
}



inline
bool Coordinator::add_process_into_group(instance_id_type process_id, type_id_type process_group_id)
{
    m_processes_of_group[process_group_id].insert(process_id);
    m_groups_of_process[process_id].insert(process_group_id);
    return true;
}



inline
void Coordinator::wait_until_all_other_processes_are_done()
{
    // if the coordinator lives in this process, we have to postpone destruction until
    // every other process is finished.
    unique_lock<mutex> lock(m_all_other_processes_done_mutex);
    while (m_processes_of_group[Externally_coordinated::id()].size() != 0) {
        m_all_other_processes_done_condition.wait(lock);
    }
}



} // sintra


#endif
