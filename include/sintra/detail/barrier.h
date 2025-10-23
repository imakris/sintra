// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "managed_process.h"
#include "process_message_reader_impl.h"

#include <stdexcept>
#include <string>
#include <thread>

namespace sintra {
namespace detail {

inline bool should_treat_rpc_failure_as_satisfied()
{
    return s_mproc &&
           s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING;
}

inline bool rendezvous_barrier(const std::string& barrier_name, const std::string& group_name)
{
    sequence_counter_type flush_seq = invalid_sequence;
    try {
        flush_seq = Process_group::rpc_barrier(group_name, barrier_name);
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

    if (flush_seq == invalid_sequence) {
        return false;
    }

    if (!s_coord) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    return true;
}

inline void wait_for_processing_quiescence()
{
    if (!s_mproc ||
        s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING)
    {
        return;
    }

    if (!on_request_reader_thread())
    {
        s_mproc->wait_for_delivery_fence();
        return;
    }

    std::thread waiter([]() {
        // Execute the wait on a separate thread so that the request reader can
        // continue making progress.  Do not rebind s_tl_current_request_reader
        // here – wait_for_delivery_fence() must observe that we're *not* on the
        // request reader thread so it waits for the in-flight handler to
        // complete before returning.
        s_mproc->wait_for_delivery_fence();
    });
    waiter.join();
}

} // namespace detail

template <>
inline bool barrier<rendezvous_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::rendezvous_barrier(barrier_name, group_name);
}

template <>
inline bool barrier<delivery_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    const bool rendezvous_completed = barrier<rendezvous_t>(barrier_name, group_name);
    if (!rendezvous_completed) {
        return false;
    }

    if (!s_mproc) {
        return true;
    }

    s_mproc->wait_for_delivery_fence();
    return true;
}

template <>
inline bool barrier<processing_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    const bool rendezvous_completed = barrier<rendezvous_t>(barrier_name, group_name);
    if (!rendezvous_completed) {
        return false;
    }

    detail::wait_for_processing_quiescence();

    const std::string processing_phase_name = barrier_name + "/processing";
    return barrier<rendezvous_t>(processing_phase_name, group_name);
}

template <typename BarrierMode>
inline bool barrier(const std::string& barrier_name, const std::string& group_name)
{
    return barrier<rendezvous_t>(barrier_name, group_name);
}

} // namespace sintra
