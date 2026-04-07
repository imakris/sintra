// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "logging.h"
#include "process/managed_process.h"
#include "messaging/process_message_reader_impl.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sintra {
namespace detail {

constexpr std::string_view k_reserved_barrier_prefix = "_sintra_";
constexpr int32_t k_barrier_mode_rendezvous = 0;
constexpr int32_t k_barrier_mode_delivery = 1;
constexpr int32_t k_barrier_mode_processing = 2;
constexpr int32_t k_barrier_mode_processing_phase = 3;

inline void validate_user_barrier_name(const std::string& barrier_name)
{
    if (barrier_name.rfind(k_reserved_barrier_prefix.data(), 0) == 0) {
        throw std::invalid_argument(
            "Barrier names beginning with '_sintra_' are reserved for internal use.");
    }
}

/// Reject user-facing barrier calls on the all-processes group while a standard
/// shutdown protocol is active.  This catches the common footgun of layering
/// extra final barriers on top of shutdown().
inline void validate_no_barrier_during_shutdown(const std::string& group_name)
{
    if (group_name != "_sintra_all_processes") {
        return;
    }
    const auto state = s_shutdown_state.load(std::memory_order_acquire);
    if (state != shutdown_protocol_state::idle) {
        throw std::logic_error(
            "User barrier on '_sintra_all_processes' called while a standard shutdown "
            "protocol is active (state=" + std::to_string(static_cast<int>(state)) + "). "
            "Do not layer extra barriers on top of shutdown().");
    }
}

inline std::string make_processing_phase_barrier_name(const std::string& barrier_name)
{
    return "_sintra_processing_phase/" + barrier_name;
}

inline void log_barrier_bypass(const std::string& barrier_name,
                               const std::string& group_name,
                               const char* reason)
{
    Log_stream(log_level::warning)
        << "Barrier '" << barrier_name
        << "' in group '" << group_name
        << "' was treated as satisfied during shutdown/drain handling ("
        << reason << ").\n";
}

inline bool should_treat_rpc_failure_as_satisfied()
{
    return s_mproc &&
           (s_mproc->m_communication_state != Managed_process::COMMUNICATION_RUNNING ||
            s_mproc->m_must_stop.load(std::memory_order_acquire) ||
            s_shutdown_state.load(std::memory_order_acquire) !=
                shutdown_protocol_state::idle);
}

inline void wait_for_processing_quiescence();

inline sequence_counter_type rendezvous_barrier(const std::string& barrier_name,
                                                const std::string& group_name,
                                                int32_t barrier_mode_tag)
{
    sequence_counter_type flush_seq = invalid_sequence;
    try {
        flush_seq = Process_group::rpc_barrier(group_name, barrier_name, barrier_mode_tag);
    }
    catch (const rpc_cancelled&) {
        if (should_treat_rpc_failure_as_satisfied()) {
            log_barrier_bypass(barrier_name, group_name, "rpc cancelled");
            return invalid_sequence;
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
            log_barrier_bypass(barrier_name, group_name, message.c_str());
            return invalid_sequence;
        }
        throw;
    }
    catch (const std::exception& e) {
        // Catches exceptions not derived from std::runtime_error (e.g.
        // std::logic_error from Process_group::barrier() when the caller
        // has been removed from the group during draining/shutdown).
        if (should_treat_rpc_failure_as_satisfied()) {
            log_barrier_bypass(barrier_name, group_name, e.what());
            return invalid_sequence;
        }
        throw;
    }

    if (flush_seq == invalid_sequence) {
        return invalid_sequence;
    }

    if (!s_coord) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    return flush_seq;
}

inline sequence_counter_type barrier_dispatch(rendezvous_t,
                                              const std::string& barrier_name,
                                              const std::string& group_name)
{
    validate_user_barrier_name(barrier_name);
    validate_no_barrier_during_shutdown(group_name);
    return rendezvous_barrier(barrier_name, group_name, k_barrier_mode_rendezvous);
}

inline sequence_counter_type barrier_dispatch(delivery_fence_t,
                                              const std::string& barrier_name,
                                              const std::string& group_name)
{
    validate_user_barrier_name(barrier_name);
    validate_no_barrier_during_shutdown(group_name);
    const auto rendezvous_seq = rendezvous_barrier(barrier_name, group_name, k_barrier_mode_delivery);
    if (rendezvous_seq == invalid_sequence) {
        return invalid_sequence;
    }

    if (!s_mproc) {
        return rendezvous_seq;
    }

    s_mproc->wait_for_delivery_fence();
    return rendezvous_seq;
}

inline sequence_counter_type barrier_dispatch(processing_fence_t,
                                              const std::string& barrier_name,
                                              const std::string& group_name)
{
    validate_user_barrier_name(barrier_name);
    validate_no_barrier_during_shutdown(group_name);
    const auto rendezvous_seq = rendezvous_barrier(barrier_name, group_name, k_barrier_mode_processing);
    if (rendezvous_seq == invalid_sequence) {
        return invalid_sequence;
    }

    wait_for_processing_quiescence();

    const auto processing_phase_name = make_processing_phase_barrier_name(barrier_name);
    return rendezvous_barrier(
        processing_phase_name,
        group_name,
        k_barrier_mode_processing_phase);
}

inline sequence_counter_type internal_processing_fence_barrier(const std::string& barrier_name,
                                                               const std::string& group_name)
{
    const auto rendezvous_seq = rendezvous_barrier(barrier_name, group_name, k_barrier_mode_processing);
    if (rendezvous_seq == invalid_sequence) {
        return invalid_sequence;
    }

    wait_for_processing_quiescence();

    const auto processing_phase_name = make_processing_phase_barrier_name(barrier_name);
    return rendezvous_barrier(
        processing_phase_name,
        group_name,
        k_barrier_mode_processing_phase);
}

inline void wait_for_processing_quiescence()
{
    if (!s_mproc) {
        return;
    }

    // Waiting directly on the delivery fence keeps request-thread state intact and
    // allows the Managed_process to service any queued post-handlers without
    // paying the cost of an additional helper thread.
    s_mproc->wait_for_delivery_fence();
}

} // namespace detail

template <>
inline sequence_counter_type barrier<rendezvous_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(rendezvous_t{}, barrier_name, group_name);
}

template <>
inline sequence_counter_type barrier<delivery_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(delivery_fence_t{}, barrier_name, group_name);
}

template <>
inline sequence_counter_type barrier<processing_fence_t>(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(processing_fence_t{}, barrier_name, group_name);
}

template <typename BarrierMode>
inline sequence_counter_type barrier(const std::string& barrier_name, const std::string& group_name)
{
    return detail::barrier_dispatch(BarrierMode{}, barrier_name, group_name);
}

} // namespace sintra
