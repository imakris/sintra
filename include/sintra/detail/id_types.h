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

#ifndef __SINTRA_ID_TYPES_H__
#define __SINTRA_ID_TYPES_H__


#include "config.h"
#include "globals.h"

#include <cstdint>
#include <cassert>
#include <atomic>


namespace sintra
{

using std::atomic;

constexpr type_id_type        invalid_type_id       =  0;
constexpr type_id_type        not_defined_type_id   = ~0;
constexpr instance_id_type    any_local_or_remote   = ~0;
constexpr instance_id_type    any_remote            = ~0 - 1;
constexpr instance_id_type    any_local             = ~0 - 2;
constexpr instance_id_type    invalid_instance_id   =  0;


namespace detail {

    enum reserved_id
    {
        invalid_type_id = invalid_type_id,

        // EXPLICITLY DEFINED RPC
        resolve_type,
        resolve_instance,
        publish_transceiver,
        unpublish_transceiver,
        barrier,
        add_this_process_into_group,
        print,

        append,

        // EXPLICITLY DEFINED SIGNALS
        instance_invalidated,

        num_reserved_type_ids
    };


    constexpr
    int bits_required_to_represent(uint64_t v, int counter = 0)
    {
        return (v == (v >> (counter + 1))) ?
            counter :
            bits_required_to_represent(v >> 1, counter + 1);
    }

    constexpr
    uint64_t flag_n_most_significant_bits(uint64_t v)
    {
        return (v == (v | (v << 1))) ? v : flag_n_most_significant_bits(v | (v << 1));
    }

    constexpr uint64_t pid_shift =
        8 * sizeof(instance_id_type) - bits_required_to_represent(max_process_instance_id);

    constexpr uint64_t pid_mask = flag_n_most_significant_bits(uint64_t(1)<<pid_shift);

    constexpr uint64_t reserved_instance_ids = max_process_instance_id;
}

inline
type_id_type make_type_id()
{
    static atomic<uint64_t> counter(detail::reserved_id::num_reserved_type_ids);
    return ++counter;
}

inline
type_id_type make_type_id(uint64_t v)
{
    assert(v <= detail::reserved_id::num_reserved_type_ids || v == not_defined_type_id);
    assert(v > 0);
    return v;
}

inline
instance_id_type make_instance_id()
{
    static atomic<uint64_t> counter(detail::reserved_instance_ids);
    return (mproc_id::s << detail::pid_shift) | ++counter;
}

inline
instance_id_type make_process_instance_id()
{
    // 1 is reserved for the first process
    static atomic<uint64_t> counter(1);
    counter++;
    assert(counter <= detail::reserved_instance_ids);
    return (counter << detail::pid_shift) | counter;
}

inline
instance_id_type make_process_instance_id(uint64_t v)
{
    assert(v <= detail::reserved_instance_ids);
    assert(v > 0);
    return (v << detail::pid_shift) | v;
}

inline
bool is_local_instance(instance_id_type instance_id)
{
    return (instance_id >> detail::pid_shift) == (mproc_id::s & ~detail::pid_mask);
}

inline
bool is_not_any_transceiver(instance_id_type instance_id)
{
    return (instance_id & ~detail::pid_mask) != ~detail::pid_mask;
}

inline
bool is_process(instance_id_type instance_id)
{
    return (instance_id >> detail::pid_shift) == (instance_id & ~detail::pid_mask);
}

inline
instance_id_type process_of(instance_id_type instance_id)
{
    return (instance_id >> detail::pid_shift) | (instance_id & detail::pid_mask);
}

}


#endif
