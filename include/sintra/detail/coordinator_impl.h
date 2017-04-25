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

#ifndef __SINTRA_COORDINATOR_IMPL_H__
#define __SINTRA_COORDINATOR_IMPL_H__


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
    Transceiver("", make_instance_id())
{
}



inline
Coordinator::~Coordinator()
{
    coord::s     = nullptr;
    coord_id::s = 0;
}



// EXPORTED FOR RPC
inline
type_id_type Coordinator::resolve_type(const string& name)
{
    auto& koko = mproc::s->m_type_id_of_name;// PORT
    auto it = koko.find(name);
    if (it != mproc::s->m_type_id_of_name.end()) {
        return it->second;
    }

    // a type is always assumed to exist
    return mproc::s->m_type_id_of_name[name] = make_type_id();
}



// EXPORTED FOR RPC
inline
instance_id_type Coordinator::resolve_instance(const string& name)
{
    auto it = mproc::s->m_instance_id_of_name.find(name);
    if (it != mproc::s->m_instance_id_of_name.end()) {
        return it->second;
    }

    // unlike types, instances need explicit allocation
    return invalid_instance_id;
}



// EXPORTED FOR RPC
inline
bool Coordinator::publish_transceiver(instance_id_type instance_id, const string& name)
{
    lock_guard<mutex> lock(m_publish_mutex);

    // if the name is already assigned...
    if (!name.empty() && resolve_instance(name) != invalid_instance_id) {
        return false;
    }

    auto process_index = instance_id >> detail::pid_shift;
    if (m_published[process_index].size() < max_public_transceivers_per_proc &&
        !m_published[process_index].count(instance_id))
    {
        if (!name.empty()) {
            mproc::s->m_instance_id_of_name[name] = instance_id;
            m_name_of_instance_id[instance_id] = name;
        }
        m_published[process_index].insert(instance_id);
        return true;
    }

    return false;
}



// EXPORTED FOR RPC
inline
bool Coordinator::unpublish_transceiver(instance_id_type instance_id)
{
    lock_guard<mutex> lock(m_publish_mutex);

    auto it = m_name_of_instance_id.find(instance_id);
    if (it != m_name_of_instance_id.end()) {
        mproc::s->m_instance_id_of_name.erase(it->second);
        m_name_of_instance_id.erase(it);
    }

    if (is_process(instance_id)) {
        Message_prefix* m = tl_current_message::s;
        instance_id_type process_id = m ? m->sender_instance_id : mproc::s->m_instance_id;

        lock_guard<mutex> lock(m_all_other_processes_done_mutex);

        auto removed_process = process_of(instance_id);

        // forget about all transceivers that live in the removed process

        for (auto it = mproc::s->m_instance_id_of_name.begin();
            it != mproc::s->m_instance_id_of_name.end(); )
        {
            if (process_of(it->second) == removed_process) {
                mproc::s->m_instance_id_of_name.erase(it++);
            }
            else {
                ++it;
            }
        }

        for (auto it = m_name_of_instance_id.begin();
            it != m_name_of_instance_id.end(); )
        {
            if (process_of(it->first) == removed_process) {
                m_name_of_instance_id.erase(it++);
            }
            else {
                ++it;
            }
        }

        // remove the process from all groups it was member of
        for (auto group : m_groups_of_process[process_id]) {
            m_processes_of_group[group].erase(process_id);
        }
        m_groups_of_process.erase(process_id);
        if (m_processes_of_group[All_processes::id()].size() == 1) {
            m_all_other_processes_done_condition.notify_all();
        }
    }

    send<instance_invalidated, any_local_or_remote>(instance_id);

    return true;
}



// EXPORTED FOR RPC
inline
bool Coordinator::barrier(type_id_type process_group_id)
{
    Message_prefix* m = tl_current_message::s;
    instance_id_type process_id = m ? m->sender_instance_id : mproc::s->m_instance_id;

    m_barrier_mutex.lock();

    auto &b = m_barriers[process_group_id];
    unique_lock<mutex> lock(b.m);

    m_barrier_mutex.unlock();

    if (++b.processes_reached == m_processes_of_group[process_group_id].size()) {
        b.processes_reached = 0;
        lock.unlock();
        b.cv.notify_all();
    }
    else {
        do {
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
    if (mproc::s->m_branched)
        return false;

    Message_prefix* m = tl_current_message::s;
    instance_id_type process_id = m ? m->sender_instance_id : mproc::s->m_instance_id;
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
    while (m_processes_of_group[All_processes::id()].size() != 1) {
        m_all_other_processes_done_condition.wait(lock);
    }
}



} // sintra


#endif
