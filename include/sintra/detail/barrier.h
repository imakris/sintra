// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "barrier_types.h"
#include "process/managed_process.h"
#include "messaging/process_message_reader_impl.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace sintra {
namespace detail {

inline bool should_treat_rpc_failure_as_satisfied()
{
    return s_mproc &&
           s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING;
}

inline void wait_for_processing_quiescence();

inline barrier_completion_payload rendezvous_barrier_payload(const std::string& barrier_name,
                                                             const std::string& group_name,
                                                             uint32_t request_flags,
                                                             boot_id_type boot_id)
{
    auto payload = make_barrier_completion_payload();
    try {
        payload = Process_group::rpc_barrier(group_name, barrier_name, request_flags, boot_id);
    }
    catch (const rpc_cancelled&) {
        if (should_treat_rpc_failure_as_satisfied()) {
            payload.rendezvous.state = barrier_state::satisfied;
            payload.rendezvous.failure_code = barrier_failure::none;
            return payload;
        }
        throw;
    }
    catch (const std::runtime_error& e) {
        const std::string message = e.what();
        const bool rpc_unavailable =
            (message == "RPC failed") ||
            (message.find("no longer available") != std::string::npos) ||
            (message.find("shutting down") != std::string::npos);
        if (rpc_unavailable && should_treat_rpc_failure_as_satisfied()) {
            payload.rendezvous.state = barrier_state::satisfied;
            payload.rendezvous.failure_code = barrier_failure::none;
            return payload;
        }
        throw;
    }

    if (payload.barrier_sequence != invalid_sequence && !s_coord) {
        s_mproc->flush(process_of(s_coord_id), payload.barrier_sequence);
    }

    return payload;
}

inline barrier_result barrier_dispatch(rendezvous_t,
                                       const std::string& barrier_name,
                                       const std::string& group_name)
{
    auto payload = rendezvous_barrier_payload(barrier_name,
                                             group_name,
                                             barrier_flag_none,
                                             0);
    return barrier_result{std::move(payload)};
}

inline barrier_result barrier_dispatch(delivery_fence_t,
                                       const std::string& barrier_name,
                                       const std::string& group_name)
{
    auto payload = rendezvous_barrier_payload(barrier_name,
                                             group_name,
                                             barrier_flag_inbound,
                                             0);
    barrier_result result{std::move(payload)};
    if (result.payload.rendezvous.state != barrier_state::satisfied) {
        return result;
    }

    if (!s_mproc) {
        result.payload.outbound = make_phase_status(barrier_state::satisfied,
                                                    barrier_failure::none,
                                                    invalid_sequence);
        return result;
    }

    s_mproc->wait_for_delivery_fence();
    result.payload.outbound = make_phase_status(barrier_state::satisfied,
                                                barrier_failure::none,
                                                invalid_sequence);
    return result;
}

inline barrier_result barrier_dispatch(processing_fence_t,
                                       const std::string& barrier_name,
                                       const std::string& group_name)
{
    auto payload = rendezvous_barrier_payload(barrier_name,
                                             group_name,
                                             barrier_flag_inbound | barrier_flag_processing,
                                             0);
    barrier_result result{std::move(payload)};
    if (result.payload.rendezvous.state != barrier_state::satisfied) {
        return result;
    }

    wait_for_processing_quiescence();

    const std::string processing_phase_name = barrier_name + "/processing";
    auto processing_result = barrier_dispatch(rendezvous_t{}, processing_phase_name, group_name);
    result.local_processing_satisfied = static_cast<bool>(processing_result);
    result.payload.processing = processing_result.payload.rendezvous;
    result.payload.outbound = make_phase_status(barrier_state::satisfied,
                                                barrier_failure::none,
                                                invalid_sequence);
    return result;
}

inline void wait_for_processing_quiescence()
{
    if (!s_mproc ||
        s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING)
    {
        return;
    }

    // Waiting directly on the delivery fence keeps request-thread state intact and
    // allows the Managed_process to service any queued post-handlers without
    // paying the cost of an additional helper thread.
    s_mproc->wait_for_delivery_fence();
}

} // namespace detail

template <>
inline barrier_result barrier<rendezvous_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(rendezvous_t{}, barrier_name, group_name);
}

template <>
inline barrier_result barrier<delivery_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(delivery_fence_t{}, barrier_name, group_name);
}

template <>
inline barrier_result barrier<processing_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(processing_fence_t{}, barrier_name, group_name);
}

template <typename BarrierMode>
inline barrier_result barrier(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(BarrierMode{}, barrier_name, group_name);
}

} // namespace sintra
