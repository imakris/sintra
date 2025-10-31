// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "barrier_protocol.h"
#include "managed_process.h"
#include "process_message_reader_impl.h"

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

inline thread_local barrier_completion_payload s_tl_last_barrier_payload = make_barrier_completion_payload();

inline bool rendezvous_barrier(const std::string& barrier_name,
                               const std::string& group_name,
                               std::uint32_t request_flags = 0)
{
    detail::barrier_completion_payload payload = detail::make_barrier_completion_payload();
    try {
        payload = Process_group::rpc_barrier(group_name, barrier_name, request_flags);
    }
    catch (const rpc_cancelled&) {
        if (should_treat_rpc_failure_as_satisfied()) {
            return true;
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
            return true;
        }
        throw;
    }

    s_tl_last_barrier_payload = payload;

    if (payload.rendezvous.state == barrier_state::failed) {
        return false;
    }

    const sequence_counter_type flush_seq = payload.barrier_sequence;
    if (flush_seq == invalid_sequence) {
        return false;
    }

    if (!s_coord) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    return true;
}

inline bool barrier_dispatch(rendezvous_t,
                             const std::string& barrier_name,
                             const std::string& group_name)
{
    return rendezvous_barrier(barrier_name, group_name);
}

inline bool barrier_dispatch(delivery_fence_t,
                             const std::string& barrier_name,
                             const std::string& group_name)
{
    const bool rendezvous_completed = rendezvous_barrier(
        barrier_name,
        group_name,
        barrier_flag_inbound);
    if (!rendezvous_completed) {
        return false;
    }

    const auto payload = s_tl_last_barrier_payload;
    if ((payload.request_flags & barrier_flag_inbound) &&
        payload.inbound.state == barrier_state::failed)
    {
        return false;
    }

    if (!s_mproc) {
        return true;
    }

    s_mproc->wait_for_delivery_fence();
    return true;
}

inline bool barrier_dispatch(processing_fence_t,
                             const std::string& barrier_name,
                             const std::string& group_name)
{
    const bool rendezvous_completed = rendezvous_barrier(
        barrier_name,
        group_name,
        barrier_flag_inbound | barrier_flag_processing);
    if (!rendezvous_completed) {
        return false;
    }

    wait_for_processing_quiescence();

    const auto payload = s_tl_last_barrier_payload;
    if ((payload.request_flags & barrier_flag_processing) &&
        payload.processing.state == barrier_state::failed)
    {
        return false;
    }

    const std::string processing_phase_name = barrier_name + "/processing";
    return barrier_dispatch(rendezvous_t{}, processing_phase_name, group_name);
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
inline bool barrier<rendezvous_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(rendezvous_t{}, barrier_name, group_name);
}

template <>
inline bool barrier<delivery_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(delivery_fence_t{}, barrier_name, group_name);
}

template <>
inline bool barrier<processing_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(processing_fence_t{}, barrier_name, group_name);
}

template <typename BarrierMode>
inline bool barrier(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(BarrierMode{}, barrier_name, group_name);
}

} // namespace sintra
