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

#ifndef SINTRA_ID_TYPES_H
#define SINTRA_ID_TYPES_H

#include "config.h"
#include "globals.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>


namespace sintra {

using std::atomic;


// type id
//========


constexpr type_id_type        invalid_type_id = 0;
constexpr type_id_type        not_defined_type_id = ~0ull;


namespace detail {

    enum class reserved_id: type_id_type
    {
        invalid_type_id = invalid_type_id,

        // EXPLICITLY DEFINED RPC
        resolve_type,
        resolve_instance,
        wait_for_instance,
        publish_transceiver,
        unpublish_transceiver,
        make_process_group,
        print,
        barrier,
        enable_recovery,

        // EXPLICITLY DEFINED SIGNALS
        //instance_invalidated, // sent by Transceiver on destruction
        instance_published,   // sent by Coordinator when a transceiver is named
                              // and can be subsequently looked up by its name.
        instance_unpublished, // sent by Coordinator, always before the
                              // Transceiver sends instance_invalidated

        // SPECIAL MESSAGE IDENTIFIERS
        exception,
        deferral,

        // EXCEPTION TYPES
        std_invalid_argument,
        std_domain_error,
        std_length_error,
        std_out_of_range,
        std_range_error,
        std_overflow_error,
        std_underflow_error,
        std_ios_base_failure,
        std_logic_error,
        std_runtime_error,
        std_exception,
        unknown_exception,

        // EXPLICITLY DEFINED SIGNALS HANDLED BY COORDINATOR
        base_of_messages_handled_by_coordinator = 0x80000000,
        terminated_abnormally,

        num_reserved_type_ids
    };
}

inline
type_id_type make_type_id()
{
    static_assert(sizeof(type_id_type) == 8, "type_id_type must be 64-bit");
    static atomic<type_id_type> counter((type_id_type)detail::reserved_id::num_reserved_type_ids);
    return ++counter;
}

inline
type_id_type make_type_id(uint64_t v)
{
    assert(v <= (type_id_type)detail::reserved_id::num_reserved_type_ids || v == not_defined_type_id);
    assert(v > 0);
    return v;
}


/*

instance id
===========

An instance id is a two part variable. The first part (p) is in the higher bits and
identifies the process, while the second part (i) the transceiver within process p.
The number of bits for each part is determined by num_process_index_bits, which is
defined in config.h.
The meaning is as follows:

process index               | transceiver index
----------------------------|-------------------------------
 a : process a              |  b : transceiver b
~a : any process except a   | ~b : any transceiver except b
 0 : no processes           |  0 : no transceivers
~0 : all processes          | ~0 : all transceivers
 1 : the local process      |  1 : the local Managed_process
~1 : all remote processes   | ~1 : all transceivers except the local Managed_process

Examples:
p =  2, i =  4 -> Transceiver instance 4 of process 2
p =  2, i =  1 -> Managed_process instance of process 2
p =  0, i =  0 -> no process, no transceiver (invalid)
p =  1, i =  4 -> Transceiver instance 4 in the local process
p =  1, i = ~4 -> All transceivers except 4 in the local process
p = ~3, i = ~4 -> All transceivers except 4, in all processes except process 3

*/


constexpr int num_transceiver_index_bits =
    sizeof(instance_id_type) * 8 - num_process_index_bits;

static constexpr
uint64_t flag_most_significant_bits(uint64_t v)
{
    return (v == (v | (v << 1))) ? v : flag_most_significant_bits(v | (v << 1));
}

constexpr uint64_t pid_mask =
    flag_most_significant_bits(uint64_t(1) << num_transceiver_index_bits);

static_assert(num_process_index_bits < 16);
constexpr int max_process_index =       int(uint64_t(1) << (num_process_index_bits     - 1)) - 1;

constexpr uint64_t max_instance_index = (uint64_t(1) << (num_transceiver_index_bits - 1)) - 1;
constexpr uint64_t num_reserved_service_instances = 0x1000;
constexpr uint64_t all_remote_processess_wildcard = (max_process_index << 1);
constexpr uint64_t all_processes_wildcard = all_remote_processess_wildcard | 1;
constexpr uint64_t all_transceivers_except_mproc_wildcard = (max_instance_index << 1);
constexpr uint64_t all_transceivers_wildcard = all_transceivers_except_mproc_wildcard | 1;

struct decomposed_instance_id {
    uint32_t process;
    uint64_t transceiver;
    uint32_t process_complement;
    uint64_t transceiver_complement;
};

[[nodiscard]] constexpr decomposed_instance_id decompose_instance(instance_id_type instance) noexcept
{
    constexpr uint64_t transceiver_mask = (uint64_t(1) << num_transceiver_index_bits) - 1;
    const auto process = static_cast<uint32_t>(instance >> num_transceiver_index_bits);
    const auto transceiver = static_cast<uint64_t>(instance & transceiver_mask);
    return {
        process,
        transceiver,
        static_cast<uint32_t>(~process),
        static_cast<uint64_t>(~transceiver)
    };
}

[[nodiscard]] constexpr instance_id_type compose_instance(uint32_t process, uint64_t transceiver) noexcept
{
    return (instance_id_type(process) << num_transceiver_index_bits) | instance_id_type(transceiver);
}


inline
instance_id_type get_instance_id_type(uint64_t process_index, uint64_t transceiver_index)
{
    return compose_instance(static_cast<uint32_t>(process_index), static_cast<uint64_t>(transceiver_index));
};


inline
uint64_t get_process_index(instance_id_type instance_id)
{
    return decompose_instance(instance_id).process;
}


inline
uint64_t get_transceiver_index(instance_id_type instance_id)
{
    return decompose_instance(instance_id).transceiver;
}


inline
uint64_t get_process_complement(instance_id_type instance_id)
{
    return decompose_instance(instance_id).process_complement;
}


inline
uint64_t get_transceiver_complement(instance_id_type instance_id)
{
    return decompose_instance(instance_id).transceiver_complement;
}


constexpr instance_id_type any_local_or_remote   =
    (all_processes_wildcard         << num_transceiver_index_bits) | all_transceivers_wildcard;
constexpr instance_id_type any_remote            =
    (all_remote_processess_wildcard << num_transceiver_index_bits) | all_transceivers_wildcard;
constexpr instance_id_type any_local             =
    (                          1ull << num_transceiver_index_bits) | all_transceivers_wildcard;
constexpr instance_id_type invalid_instance_id   = 0;


inline
instance_id_type make_instance_id()
{
    static atomic<uint64_t> instance_index_counter(2 + num_reserved_service_instances);
    assert(instance_index_counter.load(std::memory_order_relaxed) < max_instance_index);
    auto d = decompose_instance(s_mproc_id);
    const auto index = static_cast<uint64_t>(instance_index_counter++);
    return compose_instance(d.process, index);
}


inline
instance_id_type make_service_instance_id()
{
    // 1 is always the transceiver index of the local Managed_process,
    // thus other transceivers start from 2
    static atomic<uint64_t> instance_index_counter(2);
    assert(instance_index_counter.load(std::memory_order_relaxed) <
           2 + num_reserved_service_instances);
    auto d = decompose_instance(s_mproc_id);
    const auto index = static_cast<uint64_t>(instance_index_counter++);
    return compose_instance(d.process, index);
}


inline
instance_id_type is_service_instance(instance_id_type instance_id)
{
    return
        (instance_id & ~pid_mask) >=2 &&
        (instance_id & ~pid_mask) < (2+num_reserved_service_instances);
}


inline
instance_id_type make_process_instance_id()
{
    // 1 is the wildcard for the local process
    // thus process indices start from 2
    static atomic<uint64_t> process_index_counter(2);
    assert(process_index_counter.load(std::memory_order_relaxed) <= max_process_index);
    const auto index = static_cast<uint32_t>(process_index_counter++);
    return compose_instance(index, 1ull);
}


inline
instance_id_type make_process_instance_id(uint32_t process_index)
{
    assert(process_index>0 && process_index <= max_process_index);
    return compose_instance(process_index, 1ull);
}


// returns true if iid refers to one local instance
inline
bool is_local_instance(instance_id_type iid)
{
    const auto di = decompose_instance(iid);
    const auto process_index = di.process;
    return
        // local process wildcard, always matches
        (process_index == 1) ||
        // explicitly specified process index match
        (process_index == get_process_index(s_mproc_id));
}


// returns true if iid refers to one or multiple local instances
inline
bool is_local(instance_id_type iid)
{
    const auto di = decompose_instance(iid);
    const auto process_index = di.process;
    return
        is_local_instance(iid) ||
        // complement of explicitly specified process, matches this process implicitly
        (process_index > max_process_index && ((~static_cast<uint64_t>(process_index)) >> num_transceiver_index_bits) != get_process_index(s_mproc_id)) ||
        // all processes wildcard, always matches
        (process_index == all_processes_wildcard);
}


inline
bool is_process(instance_id_type iid)
{
    const auto di = decompose_instance(iid);
    assert (di.process > 0 && di.process <= max_process_index);

    return di.transceiver == 1;
}


inline
instance_id_type process_of(instance_id_type iid)
{
    const auto di = decompose_instance(iid);
    assert(di.process > 0 && di.process <= max_process_index);
    return compose_instance(di.process, 1ull);
}



}


#endif
