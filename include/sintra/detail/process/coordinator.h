// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../barrier_types.h"
#include "../id_types.h"
#include "../resolvable_instance.h"
#include "../resolve_type.h"
#include "../transceiver.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>
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


    void set(const unordered_set<instance_id_type>& member_process_ids);

    // barriers are deferred functions and return twice (this refers to the implementation,
    // which is transparent to the caller - for the caller they only return once).
    // the first returns 0 if it is expected to fail, or a serial number, if it is expected
    // to succeed.
    // once completed, a message is sent, that should be picked up and read only by the
    // processes waiting on the barrier. This message would contain
    // - the serial number, for identification purposes
    // - the sequence counter (i.e. at which ring sequence was the barrier completed)
    barrier_completion_payload barrier(const string& barrier_name,
                                       uint32_t request_flags,
                                       boot_id_type boot_id);


    struct Barrier
    {
        mutex                                   m;
        condition_variable                      cv;
        unordered_set<instance_id_type>         processes_pending;
        unordered_set<instance_id_type>         processes_arrived;
        bool                                    failed = false;
        instance_id_type                        common_function_iid = invalid_instance_id;
        uint32_t                                requirement_mask = std::numeric_limits<uint32_t>::max();
        uint64_t                                barrier_epoch = 0;
        sequence_counter_type                   rendezvous_sequence = invalid_sequence;
        bool                                    rendezvous_complete = false;
        barrier_completion_payload              completion_template = make_barrier_completion_payload();
    };

    struct Barrier_completion
    {
        instance_id_type                        common_function_iid = invalid_instance_id;
        std::vector<instance_id_type>           recipients;
        barrier_completion_payload              payload = make_barrier_completion_payload();
    };

    unordered_map<string, shared_ptr<Barrier>>  m_barriers;
    unordered_set<instance_id_type>             m_process_ids;

    mutex m_call_mutex;
    SINTRA_RPC_STRICT_EXPLICIT(barrier)

public:
    void drop_from_inflight_barriers(
        instance_id_type process_iid,
        std::vector<Barrier_completion>& completions);
    void emit_barrier_completions(
        const std::vector<Barrier_completion>& completions);

private:
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
    Coordinator();
    ~Coordinator();

    // EXPORTED FOR RPC
    type_id_type resolve_type(const string& pretty_name);
    instance_id_type resolve_instance(const string& assigned_name);

    instance_id_type wait_for_instance(const string& assigned_name);

    instance_id_type publish_transceiver(
        type_id_type type_id, instance_id_type instance_id, const string& assigned_name);
    bool unpublish_transceiver(instance_id_type instance_id);
    sequence_counter_type begin_process_draining(instance_id_type process_iid);
    void unpublish_transceiver_notify(instance_id_type transceiver_iid);

    //bool add_process_into_group(instance_id_type process_id, type_id_type process_group_id);


    instance_id_type make_process_group(
        const string& name,
        const unordered_set<instance_id_type>& member_process_ids);


    void enable_recovery(instance_id_type piid);
    void recover_if_required(instance_id_type piid);

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

    std::array<std::atomic<uint8_t>, max_process_index + 1> m_draining_process_states{};

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

    bool is_process_draining(instance_id_type process_iid) const;

    SINTRA_SIGNAL_EXPLICIT(instance_published,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)
    SINTRA_SIGNAL_EXPLICIT(instance_unpublished,
        type_id_type type_id, instance_id_type instance_id, message_string assigned_name)

    friend struct Managed_process;
    friend struct Transceiver;
    friend bool finalize();
};

}


