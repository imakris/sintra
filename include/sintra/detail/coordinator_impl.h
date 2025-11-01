// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "coordinator.h"
#include "managed_process.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>


namespace sintra {


inline Process_group::Process_group()
    : Derived_transceiver<Process_group>("", make_service_instance_id())
{
    activate<Managed_process>(
        [this](const Managed_process::barrier_ack_notify& msg) {
            if (msg.group_instance_id != this->instance_id()) {
                return;
            }
            barrier_ack_response(msg.response);
        },
        any_remote);
}

using std::cout;
using std::lock_guard;
using std::mutex;
using std::string;
using std::unique_lock;



// EXPORTED EXCLUSIVELY FOR RPC
inline
detail::barrier_completion_payload Process_group::barrier(
    const string& barrier_name,
    std::uint32_t request_flags)
{
    detail::barrier_completion_payload result_payload = detail::make_barrier_completion_payload();
    result_payload.request_flags = request_flags;

    std::unique_lock basic_lock(m_call_mutex);
    instance_id_type caller_piid = s_tl_current_message->sender_instance_id;
    if (m_process_ids.find(caller_piid) == m_process_ids.end()) {
        throw std::logic_error("The caller is not a member of the process group.");
    }

    auto& barrier_entry = m_barriers[barrier_name];
    if (!barrier_entry) {
        barrier_entry = std::make_shared<Barrier>();
    }

    auto barrier = barrier_entry; // keep the barrier alive even if the map rehashes
    Barrier& b = *barrier;
    std::unique_lock barrier_lock(b.m); // main barrier lock

    // Atomically snapshot membership and filter draining processes while holding m_call_mutex.
    // This ensures a consistent view: no process can be added/removed or change draining state
    // between the membership snapshot and the draining filter.
    if (b.processes_pending.empty()) {
        // new or reused barrier (may have failed previously)
        b.processes_pending = m_process_ids;
        b.processes_arrived.clear();
        b.failed = false;
        b.common_function_iid = make_instance_id();
        b.group_requirement_mask = 0;
        b.per_process_flags.clear();
        b.outbound_waiters.clear();
        b.processing_waiters.clear();
        b.completion_payloads.clear();
        b.outbound_state = detail::barrier_state::not_requested;
        b.outbound_failure = detail::barrier_failure::none;
        b.outbound_offender = invalid_instance_id;
        b.processing_state = detail::barrier_state::not_requested;
        b.processing_failure = detail::barrier_failure::none;
        b.processing_offender = invalid_instance_id;
        b.outbound_deadline = std::chrono::steady_clock::time_point{};
        b.processing_deadline = std::chrono::steady_clock::time_point{};
        b.rendezvous_sequence = invalid_sequence;
        b.awaiting_outbound = false;
        b.awaiting_processing = false;
        b.timeout_monitor_active = false;
        b.async_completion_pending = false;
        b.completion_dispatched = false;

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
    }

    // Now safe to release m_call_mutex - barrier state is consistent and other threads
    // need to be able to arrive at the barrier concurrently
    basic_lock.unlock();

    b.processes_arrived.insert(caller_piid);
    b.processes_pending.erase(caller_piid);
    b.per_process_flags[caller_piid] = request_flags;
    b.group_requirement_mask |= request_flags;

    if (b.processes_pending.empty()) {
        const auto current_common_fiid = b.common_function_iid;
        b.rendezvous_complete = true;
        const auto sequence = s_mproc->m_out_rep_c->get_leading_sequence();
        b.rendezvous_sequence = sequence;

        auto populate_payload = [&](instance_id_type pid, std::uint32_t flags) {
            auto& payload = b.completion_payloads[pid];
            payload = detail::make_barrier_completion_payload();
            payload.common_function_iid = current_common_fiid;
            payload.request_flags = flags;
            payload.barrier_sequence = sequence;
            payload.rendezvous.state = detail::barrier_state::satisfied;
            payload.rendezvous.sequence = sequence;

            if (flags & detail::barrier_flag_inbound) {
                payload.inbound.state = detail::barrier_state::satisfied;
                payload.inbound.sequence = sequence;
            }
            else {
                payload.inbound.state = detail::barrier_state::not_requested;
                payload.inbound.sequence = invalid_sequence;
            }
        };

        for (auto pid : b.processes_arrived) {
            auto flags_it = b.per_process_flags.find(pid);
            std::uint32_t flags = (flags_it != b.per_process_flags.end()) ? flags_it->second : 0;
            populate_payload(pid, flags);
        }

        const bool needs_outbound = (b.group_requirement_mask & detail::barrier_flag_outbound) != 0;
        const bool needs_processing = (b.group_requirement_mask & detail::barrier_flag_processing) != 0;
        const bool needs_ack = needs_outbound || needs_processing;

        if (needs_outbound) {
            b.awaiting_outbound = true;
            b.outbound_waiters = b.processes_arrived;
            b.outbound_state = detail::barrier_state::downgraded;
            for (auto& [pid, payload] : b.completion_payloads) {
                (void)pid;
                payload.outbound.state = detail::barrier_state::downgraded;
                payload.outbound.failure_code = detail::barrier_failure::none;
                payload.outbound.offender = invalid_instance_id;
                payload.outbound.sequence = sequence;
            }
        }
        else {
            b.awaiting_outbound = false;
            b.outbound_waiters.clear();
            b.outbound_state = detail::barrier_state::not_requested;
        }

        if (needs_processing) {
            b.awaiting_processing = true;
            b.processing_waiters = b.processes_arrived;
            b.processing_state = detail::barrier_state::downgraded;
            for (auto& [pid, payload] : b.completion_payloads) {
                (void)pid;
                payload.processing.state = detail::barrier_state::downgraded;
                payload.processing.failure_code = detail::barrier_failure::none;
                payload.processing.offender = invalid_instance_id;
                payload.processing.sequence = sequence;
            }
        }
        else {
            b.awaiting_processing = false;
            b.processing_waiters.clear();
            b.processing_state = detail::barrier_state::not_requested;
        }

        if (needs_ack) {
            const auto now = std::chrono::steady_clock::now();
            constexpr auto outbound_timeout = std::chrono::seconds(30);
            constexpr auto processing_timeout = std::chrono::seconds(60);

            if (needs_outbound) {
                b.outbound_deadline = now + outbound_timeout;
            }
            if (needs_processing) {
                b.processing_deadline = now + processing_timeout;
            }

            auto handle_rpc_failure = [&](detail::barrier_ack_type type,
                                          instance_id_type offender,
                                          detail::barrier_failure reason)
            {
                auto state_for_reason = [&](detail::barrier_failure failure_reason) {
                    return (failure_reason == detail::barrier_failure::peer_draining)
                        ? detail::barrier_state::downgraded
                        : detail::barrier_state::failed;
                };

                if (type == detail::barrier_ack_type::outbound && b.awaiting_outbound) {
                    b.outbound_state = state_for_reason(reason);
                    b.outbound_failure = reason;
                    b.outbound_offender = offender;
                    b.outbound_waiters.clear();
                    b.awaiting_outbound = false;
                }
                if (type == detail::barrier_ack_type::processing && b.awaiting_processing) {
                    b.processing_state = state_for_reason(reason);
                    b.processing_failure = reason;
                    b.processing_offender = offender;
                    b.processing_waiters.clear();
                    b.awaiting_processing = false;
                }
            };

            for (auto pid : b.processes_arrived) {
                if (needs_outbound && b.awaiting_outbound) {
                    detail::barrier_ack_request req = detail::make_barrier_ack_request();
                    req.group_instance_id = this->instance_id();
                    req.barrier_sequence = sequence;
                    req.common_function_iid = current_common_fiid;
                    req.ack_type = detail::barrier_ack_type::outbound;
                    req.target_sequence = sequence;
                    if (detail::barrier_trace_enabled()) {
                        sintra::detail::log_barrier_event("[ack_request_send] type=outbound pid=", pid,
                            " barrier_seq=", req.barrier_sequence,
                            " target_seq=", req.target_sequence,
                            " fiid=", req.common_function_iid);
                    }
                    try {
                        Managed_process::rpc_barrier_ack_request(pid, req);
                    }
                    catch (...) {
                        if (detail::barrier_trace_enabled()) {
                            sintra::detail::log_barrier_event("[ack_request_failure] type=outbound pid=", pid);
                        }
                        handle_rpc_failure(detail::barrier_ack_type::outbound, pid, detail::barrier_failure::peer_lost);
                    }
                }
                if (needs_processing && b.awaiting_processing) {
                    detail::barrier_ack_request req = detail::make_barrier_ack_request();
                    req.group_instance_id = this->instance_id();
                    req.barrier_sequence = sequence;
                    req.common_function_iid = current_common_fiid;
                    req.ack_type = detail::barrier_ack_type::processing;
                    req.target_sequence = sequence;
                    if (detail::barrier_trace_enabled()) {
                        sintra::detail::log_barrier_event("[ack_request_send] type=processing pid=", pid,
                            " barrier_seq=", req.barrier_sequence,
                            " target_seq=", req.target_sequence,
                            " fiid=", req.common_function_iid);
                    }
                    try {
                        Managed_process::rpc_barrier_ack_request(pid, req);
                    }
                    catch (...) {
                        if (detail::barrier_trace_enabled()) {
                            sintra::detail::log_barrier_event("[ack_request_failure] type=processing pid=", pid);
                        }
                        handle_rpc_failure(detail::barrier_ack_type::processing, pid, detail::barrier_failure::peer_lost);
                    }
                }
            }
        }

        auto* current_message = s_tl_current_message;
        assert(current_message);

        if (detail::barrier_trace_enabled()) {
            sintra::detail::log_barrier_event("[barrier_deferral] barrier=", barrier_name,
                " fiid=", b.common_function_iid,
                " needs_ack=", needs_ack,
                " awaiting_outbound=", b.awaiting_outbound,
                " awaiting_processing=", b.awaiting_processing);
        }

        deferral* placed_msg = s_mproc->m_out_rep_c->write<deferral>(0, b.common_function_iid);
        Transceiver::finalize_rpc_write(
            placed_msg,
            current_message->sender_instance_id,
            current_message->function_instance_id,
            this,
            (type_id_type)detail::reserved_id::deferral);

        mark_rpc_reply_deferred();

        b.async_completion_pending = needs_ack;

        const bool still_waiting =
            (b.awaiting_outbound && !b.outbound_waiters.empty()) ||
            (b.awaiting_processing && !b.processing_waiters.empty());

        if (needs_ack && !b.timeout_monitor_active) {
            b.timeout_monitor_active = true;
            auto weak_barrier = std::weak_ptr<Barrier>(barrier);
            std::thread([this, weak_barrier, group_name = barrier_name]() mutable {
                monitor_barrier_timeouts(std::move(group_name), std::move(weak_barrier));
            }).detach();
        }

        if (!still_waiting) {
            if (!basic_lock.owns_lock()) {
                basic_lock.lock();
            }
            finalize_barrier_locked(barrier_name, b, barrier_lock, basic_lock);
            return result_payload;
        }

        barrier_lock.unlock();
        return result_payload;
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
        barrier_lock.unlock();
        return result_payload;
    }
}

inline void Process_group::barrier_ack_response(
    const detail::barrier_ack_response& response)
{
    std::unique_lock basic_lock(m_call_mutex);

    std::shared_ptr<Barrier> barrier_ptr;
    std::string barrier_name;
    for (auto& [name, entry] : m_barriers) {
        if (entry && entry->common_function_iid == response.common_function_iid) {
            barrier_ptr = entry;
            barrier_name = name;
            break;
        }
    }

    if (!barrier_ptr) {
        return;
    }

    Barrier& b = *barrier_ptr;
    std::unique_lock barrier_lock(b.m);

    auto responder = response.responder;

    if (detail::barrier_trace_enabled()) {
        sintra::detail::log_barrier_event("[ack_response_recv] barrier=", barrier_name,
            " type=", static_cast<int>(response.ack_type),
            " success=", response.success,
            " responder=", responder,
            " observed=", response.observed_sequence);
    }

    // Check if the barrier is still awaiting acks for this phase BEFORE touching containers
    // Late-arriving responses must not modify completion_payloads after the phase completes
    const bool is_outbound = (response.ack_type == detail::barrier_ack_type::outbound);
    const bool is_processing = (response.ack_type == detail::barrier_ack_type::processing);

    if (is_outbound && !b.awaiting_outbound) {
        return; // Phase already completed, ignore late response
    }
    if (is_processing && !b.awaiting_processing) {
        return; // Phase already completed, ignore late response
    }

    auto& payload = b.completion_payloads[responder];
    payload.common_function_iid = b.common_function_iid;

    auto update_phase_state = [](bool success,
                                 detail::barrier_phase_status& phase_payload,
                                 detail::barrier_state& phase_state,
                                 detail::barrier_failure& failure_code,
                                 instance_id_type& offender,
                                 instance_id_type responder_iid,
                                 detail::barrier_failure failure_reason,
                                 sequence_counter_type observed_sequence)
    {
        if (success) {
            phase_payload.state = detail::barrier_state::satisfied;
            phase_payload.failure_code = detail::barrier_failure::none;
            phase_payload.offender = invalid_instance_id;
            phase_payload.sequence = observed_sequence;
            if (phase_state != detail::barrier_state::failed &&
                phase_state != detail::barrier_state::downgraded)
            {
                phase_state = detail::barrier_state::downgraded; // temporary until waiters drain
                failure_code = detail::barrier_failure::none;
                offender = invalid_instance_id;
            }
            return;
        }

        const bool downgrade = (failure_reason == detail::barrier_failure::peer_draining);
        phase_payload.state = downgrade ? detail::barrier_state::downgraded
                                        : detail::barrier_state::failed;
        phase_payload.failure_code = failure_reason;
        phase_payload.offender = responder_iid;
        phase_payload.sequence = invalid_sequence;

        phase_state = downgrade ? detail::barrier_state::downgraded
                                : detail::barrier_state::failed;
        failure_code = failure_reason;
        offender = responder_iid;
    };

    if (response.ack_type == detail::barrier_ack_type::outbound && b.awaiting_outbound) {
        b.outbound_waiters.erase(responder);

        if (!response.success) {
            // Treat participant refusal as peer_draining downgrade.
            b.outbound_waiters.clear();
        }

        update_phase_state(
            response.success,
            payload.outbound,
            b.outbound_state,
            b.outbound_failure,
            b.outbound_offender,
            responder,
            detail::barrier_failure::peer_draining,
            response.observed_sequence);

        if (b.outbound_waiters.empty()) {
            b.awaiting_outbound = false;
            if (b.outbound_state == detail::barrier_state::downgraded &&
                b.outbound_failure == detail::barrier_failure::none)
            {
                b.outbound_state = detail::barrier_state::satisfied;
            }
        }
    }
    else if (response.ack_type == detail::barrier_ack_type::processing && b.awaiting_processing) {
        b.processing_waiters.erase(responder);

        if (!response.success) {
            b.processing_waiters.clear();
        }

        update_phase_state(
            response.success,
            payload.processing,
            b.processing_state,
            b.processing_failure,
            b.processing_offender,
            responder,
            detail::barrier_failure::peer_draining,
            response.observed_sequence);

        if (b.processing_waiters.empty()) {
            b.awaiting_processing = false;
            if (b.processing_state == detail::barrier_state::downgraded &&
                b.processing_failure == detail::barrier_failure::none)
            {
                b.processing_state = detail::barrier_state::satisfied;
            }
        }
    }

    const bool ready = !b.awaiting_outbound && !b.awaiting_processing;
    const bool should_finalize =
        ready && b.async_completion_pending && !b.completion_dispatched;

    if (should_finalize) {
        if (!basic_lock.owns_lock()) {
            basic_lock.lock();
        }
        finalize_barrier_locked(barrier_name, b, barrier_lock, basic_lock);
        return;
    }

    barrier_lock.unlock();
    if (ready) {
        b.completion_cv.notify_all();
    }
}


inline void Process_group::drop_from_inflight_barriers(
    instance_id_type process_iid,
    std::vector<Barrier_completion>& completions)
{
    std::lock_guard basic_lock(m_call_mutex);

    const bool coordinator_stopping = (process_iid == process_of(this->instance_id()));
    const auto coordinator_offender = coordinator_stopping ? process_iid : invalid_instance_id;
    const auto rendezvous_failure = coordinator_stopping
        ? detail::barrier_failure::coordinator_stop
        : detail::barrier_failure::peer_draining;

    for (auto barrier_it = m_barriers.begin(); barrier_it != m_barriers.end(); ) {
        auto barrier = barrier_it->second;
        if (!barrier) {
            barrier_it = m_barriers.erase(barrier_it);
            continue;
        }

        std::unique_lock barrier_lock(barrier->m);

        const bool touched_pending = barrier->processes_pending.erase(process_iid) > 0;
        const bool touched_arrived = barrier->processes_arrived.erase(process_iid) > 0;

        if (!touched_pending && !touched_arrived) {
            ++barrier_it;
            continue;
        }

        if (!barrier->processes_pending.empty()) {
            ++barrier_it;
            continue;
        }

        Barrier_completion completion;
        completion.common_function_iid = barrier->common_function_iid;
        completion.recipients.assign(
            barrier->processes_arrived.begin(),
            barrier->processes_arrived.end());

        if (touched_arrived) {
            completion.recipients.push_back(process_iid);
        }

        completion.recipient_payloads.reserve(completion.recipients.size());

        auto adjust_payload = [&](detail::barrier_completion_payload& payload,
                                  instance_id_type recipient)
        {
            auto flags_it = barrier->per_process_flags.find(recipient);
            if (flags_it != barrier->per_process_flags.end()) {
                payload.request_flags = flags_it->second;
            }

            payload.barrier_sequence = invalid_sequence;
            payload.rendezvous.state = detail::barrier_state::failed;
            payload.rendezvous.failure_code = rendezvous_failure;
            payload.rendezvous.offender = coordinator_stopping ? coordinator_offender : process_iid;
            payload.rendezvous.sequence = invalid_sequence;

            auto apply_phase = [&](detail::barrier_phase_status& phase,
                                   bool requested)
            {
                if (!requested) {
                    phase.state = detail::barrier_state::not_requested;
                    phase.failure_code = detail::barrier_failure::none;
                    phase.offender = invalid_instance_id;
                    phase.sequence = invalid_sequence;
                    return;
                }

                if (coordinator_stopping) {
                    phase.state = detail::barrier_state::failed;
                    phase.failure_code = detail::barrier_failure::coordinator_stop;
                    phase.offender = coordinator_offender;
                }
                else {
                    phase.state = detail::barrier_state::downgraded;
                    phase.failure_code = detail::barrier_failure::peer_draining;
                    phase.offender = process_iid;
                }
                phase.sequence = invalid_sequence;
            };

            const auto flags = payload.request_flags;
            apply_phase(payload.inbound,    (flags & detail::barrier_flag_inbound) != 0);
            apply_phase(payload.outbound,   (flags & detail::barrier_flag_outbound) != 0);
            apply_phase(payload.processing, (flags & detail::barrier_flag_processing) != 0);
        };

        for (auto recipient : completion.recipients) {
            auto payload_it = barrier->completion_payloads.find(recipient);
            if (payload_it != barrier->completion_payloads.end()) {
                detail::barrier_completion_payload payload = payload_it->second;
                adjust_payload(payload, recipient);
                completion.recipient_payloads.push_back(std::move(payload));
            }
            else {
                detail::barrier_completion_payload payload = detail::make_barrier_completion_payload();
                payload.common_function_iid = barrier->common_function_iid;
                payload.request_flags = barrier->group_requirement_mask;
                adjust_payload(payload, recipient);
                completion.recipient_payloads.push_back(payload);
            }
        }

        barrier->processes_arrived.clear();
        barrier->common_function_iid = invalid_instance_id;
        barrier->async_completion_pending = false;
        barrier->completion_dispatched = true;
        barrier->timeout_monitor_active = false;

        barrier_lock.unlock();

        barrier_it = m_barriers.erase(barrier_it);
        completions.push_back(std::move(completion));
    }
}

inline void Process_group::emit_barrier_completions(
    const std::vector<Barrier_completion>& completions)
{
    using return_message_type =
        Message<Enclosure<detail::barrier_completion_payload>, void, not_defined_type_id>;

    for (const auto& completion : completions) {
        if (completion.common_function_iid == invalid_instance_id) {
            continue;
        }

        const auto recipient_count = completion.recipients.size();
        if (recipient_count == 0) {
            continue;
        }

        for (std::size_t idx = 0; idx < recipient_count; ++idx) {
            auto recipient = completion.recipients[idx];
            detail::barrier_completion_payload payload {};
            if (idx < completion.recipient_payloads.size()) {
                payload = completion.recipient_payloads[idx];
            }
            if (payload.common_function_iid == invalid_instance_id) {
                payload.common_function_iid = completion.common_function_iid;
            }

            const auto flush_sequence = s_mproc->m_out_rep_c->get_leading_sequence();
            if (payload.barrier_sequence == invalid_sequence) {
                payload.barrier_sequence = flush_sequence;
            }

            if (payload.inbound.state == detail::barrier_state::satisfied &&
                payload.inbound.sequence == invalid_sequence)
            {
                payload.inbound.sequence = payload.barrier_sequence;
            }

            if (payload.outbound.state == detail::barrier_state::satisfied &&
                payload.outbound.sequence == invalid_sequence)
            {
                payload.outbound.sequence = payload.barrier_sequence;
            }

            if (payload.processing.state == detail::barrier_state::satisfied &&
                payload.processing.sequence == invalid_sequence)
            {
                payload.processing.sequence = payload.barrier_sequence;
            }

            auto* placed_msg = s_mproc->m_out_rep_c->write<return_message_type>(
                vb_size<return_message_type>(payload), payload);

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
void Process_group::update_barrier_payload_states_locked(Barrier& barrier)
{
    for (auto& [pid, payload] : barrier.completion_payloads) {
        (void)pid;

        if (payload.barrier_sequence == invalid_sequence) {
            payload.barrier_sequence = barrier.rendezvous_sequence;
        }

        if ((payload.request_flags & detail::barrier_flag_outbound) == 0) {
            payload.outbound.state = detail::barrier_state::not_requested;
            payload.outbound.failure_code = detail::barrier_failure::none;
            payload.outbound.offender = invalid_instance_id;
            payload.outbound.sequence = invalid_sequence;
        } else {
            if (barrier.outbound_state == detail::barrier_state::failed ||
                barrier.outbound_state == detail::barrier_state::downgraded)
            {
                payload.outbound.state = barrier.outbound_state;
                payload.outbound.failure_code = barrier.outbound_failure;
                payload.outbound.offender = barrier.outbound_offender;
                payload.outbound.sequence = invalid_sequence;
            }
            else {
                payload.outbound.state = detail::barrier_state::satisfied;
                payload.outbound.failure_code = detail::barrier_failure::none;
                payload.outbound.offender = invalid_instance_id;
                if (payload.outbound.sequence == invalid_sequence) {
                    payload.outbound.sequence = payload.barrier_sequence;
                }
            }
        }

        if ((payload.request_flags & detail::barrier_flag_processing) == 0) {
            payload.processing.state = detail::barrier_state::not_requested;
            payload.processing.failure_code = detail::barrier_failure::none;
            payload.processing.offender = invalid_instance_id;
            payload.processing.sequence = invalid_sequence;
        } else {
            if (barrier.processing_state == detail::barrier_state::failed ||
                barrier.processing_state == detail::barrier_state::downgraded)
            {
                payload.processing.state = barrier.processing_state;
                payload.processing.failure_code = barrier.processing_failure;
                payload.processing.offender = barrier.processing_offender;
                payload.processing.sequence = invalid_sequence;
            }
            else {
                payload.processing.state = detail::barrier_state::satisfied;
                payload.processing.failure_code = detail::barrier_failure::none;
                payload.processing.offender = invalid_instance_id;
                if (payload.processing.sequence == invalid_sequence) {
                    payload.processing.sequence = payload.barrier_sequence;
                }
            }
        }
    }
}

inline
void Process_group::finalize_barrier_locked(
    const std::string& barrier_name,
    Barrier& barrier,
    std::unique_lock<std::mutex>& barrier_lock,
    std::unique_lock<std::mutex>& basic_lock)
{
    if (barrier.completion_dispatched) {
        if (barrier_lock.owns_lock()) {
            barrier_lock.unlock();
        }
        if (basic_lock.owns_lock()) {
            basic_lock.unlock();
        }
        return;
    }

    update_barrier_payload_states_locked(barrier);
    barrier.async_completion_pending = false;
    barrier.completion_dispatched = true;
    barrier.timeout_monitor_active = false;

    if (detail::barrier_trace_enabled()) {
        sintra::detail::log_barrier_event("[barrier_finalize] barrier=", barrier_name,
            " fiid=", barrier.common_function_iid,
            " outbound_state=", static_cast<int>(barrier.outbound_state),
            " processing_state=", static_cast<int>(barrier.processing_state));
    }

    Barrier_completion completion;
    completion.common_function_iid = barrier.common_function_iid;
    completion.recipients.reserve(barrier.completion_payloads.size());
    completion.recipient_payloads.reserve(barrier.completion_payloads.size());

    for (auto& [pid, payload] : barrier.completion_payloads) {
        completion.recipients.push_back(pid);
        completion.recipient_payloads.push_back(payload);
    }

    if (barrier_lock.owns_lock()) {
        barrier_lock.unlock();
    }

    if (!completion.recipients.empty()) {
        std::vector<Barrier_completion> completions;
        completions.push_back(std::move(completion));
        emit_barrier_completions(completions);
    }

    if (!basic_lock.owns_lock()) {
        basic_lock.lock();
    }
    auto it = m_barriers.find(barrier_name);
    if (it != m_barriers.end() &&
        it->second &&
        it->second.get() == &barrier &&
        it->second->common_function_iid == barrier.common_function_iid)
    {
        m_barriers.erase(it);
    }
    basic_lock.unlock();
}


inline
void Process_group::monitor_barrier_timeouts(std::string barrier_name, std::weak_ptr<Barrier> barrier_wp)
{
    constexpr auto poll_interval = std::chrono::milliseconds(10);

    while (true) {
        std::this_thread::sleep_for(poll_interval);

        auto barrier = barrier_wp.lock();
        if (!barrier) {
            if (detail::barrier_trace_enabled()) {
                sintra::detail::log_barrier_event("[timeout_monitor] barrier expired reference barrier=", barrier_name);
            }
            return;
        }

        std::unique_lock<std::mutex> basic_lock(m_call_mutex);
        auto it = m_barriers.find(barrier_name);
        if (it == m_barriers.end() || it->second.get() != barrier.get()) {
            barrier->timeout_monitor_active = false;
            basic_lock.unlock();
            if (detail::barrier_trace_enabled()) {
                sintra::detail::log_barrier_event("[timeout_monitor] barrier gone barrier=", barrier_name);
            }
            return;
        }

        std::unique_lock<std::mutex> barrier_lock(barrier->m);
        if (!barrier->awaiting_outbound && !barrier->awaiting_processing) {
            barrier->timeout_monitor_active = false;
            barrier_lock.unlock();
            basic_lock.unlock();
            if (detail::barrier_trace_enabled()) {
                sintra::detail::log_barrier_event("[timeout_monitor] nothing awaiting barrier=", barrier_name);
            }
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        bool state_changed = false;

        if (barrier->awaiting_outbound && now >= barrier->outbound_deadline) {
            barrier->outbound_state = detail::barrier_state::failed;
            barrier->outbound_failure = detail::barrier_failure::timeout;
            barrier->outbound_offender = barrier->outbound_waiters.empty()
                ? invalid_instance_id
                : *barrier->outbound_waiters.begin();
            barrier->outbound_waiters.clear();
            barrier->awaiting_outbound = false;
            state_changed = true;
            if (detail::barrier_trace_enabled()) {
                sintra::detail::log_barrier_event("[timeout_monitor] outbound timeout barrier=", barrier_name,
                    " offender=", barrier->outbound_offender);
            }
        }

        if (barrier->awaiting_processing && now >= barrier->processing_deadline) {
            barrier->processing_state = detail::barrier_state::failed;
            barrier->processing_failure = detail::barrier_failure::timeout;
            barrier->processing_offender = barrier->processing_waiters.empty()
                ? invalid_instance_id
                : *barrier->processing_waiters.begin();
            barrier->processing_waiters.clear();
            barrier->awaiting_processing = false;
            state_changed = true;
            if (detail::barrier_trace_enabled()) {
                sintra::detail::log_barrier_event("[timeout_monitor] processing timeout barrier=", barrier_name,
                    " offender=", barrier->processing_offender);
            }
        }

        bool ready = !barrier->awaiting_outbound && !barrier->awaiting_processing;
        bool should_finalize = ready && barrier->async_completion_pending && !barrier->completion_dispatched;

        if (should_finalize) {
            barrier->timeout_monitor_active = false;
            finalize_barrier_locked(barrier_name, *barrier, barrier_lock, basic_lock);
            return;
        }

        barrier_lock.unlock();
        basic_lock.unlock();

        if (!state_changed) {
            continue;
        }
        if (detail::barrier_trace_enabled()) {
            sintra::detail::log_barrier_event("[timeout_monitor] state_changed barrier=", barrier_name,
                " awaiting_outbound=", barrier->awaiting_outbound,
                " awaiting_processing=", barrier->awaiting_processing);
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

        struct Pending_completion
        {
            std::string group_name;
            std::vector<Process_group::Barrier_completion> completions;
        };

        std::vector<Pending_completion> pending_completions;

        {
            lock_guard<mutex> lock(m_groups_mutex);
            pending_completions.reserve(m_groups.size());

            for (auto& [name, group] : m_groups) {
                std::vector<Process_group::Barrier_completion> completions;
                group.drop_from_inflight_barriers(process_iid, completions);
                if (!completions.empty()) {
                    pending_completions.push_back({name, std::move(completions)});
                }
                group.remove_process(process_iid);
            }

            m_groups_of_process.erase(process_iid);
        }

        if (!pending_completions.empty() && s_mproc) {
            s_mproc->run_after_current_handler(
                [this, pending = std::move(pending_completions)]() mutable {
                    std::lock_guard<mutex> groups_lock(m_groups_mutex);
                    for (auto& entry : pending) {
                        auto it = m_groups.find(entry.group_name);
                        if (it != m_groups.end()) {
                            it->second.emit_barrier_completions(entry.completions);
                        }
                    }
                });
        }

        // Keep the draining bit set; it will be re-initialized to 0 (ACTIVE)
        // when a new process is published into this slot. This prevents the race
        // where resetting too early allows concurrent barriers to include a dying process.

        // remove all group associations of unpublished process
        {
            std::lock_guard<mutex> groups_lock(m_groups_mutex);
            auto groups_it = m_groups_of_process.find(iid);
            if (groups_it != m_groups_of_process.end()) {
                m_groups_of_process.erase(groups_it);
            }
        }

        //// and finally, if the process was being read, stop reading from it
        if (iid != s_mproc_id) {
            std::shared_lock<std::shared_mutex> readers_lock(s_mproc->m_readers_mutex);
            auto it = s_mproc->m_readers.find(process_iid);
            if (it != s_mproc->m_readers.end()) {
                if (auto& reader = it->second) {
                    reader->stop_nowait();
                }
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

    struct Pending_completion
    {
        std::string group_name;
        std::vector<Process_group::Barrier_completion> completions;
    };

    std::vector<Pending_completion> pending_completions;
    {
        lock_guard<mutex> lock(m_groups_mutex);
        pending_completions.reserve(m_groups.size());

        for (auto& [name, group] : m_groups) {
            std::vector<Process_group::Barrier_completion> completions;
            group.drop_from_inflight_barriers(process_iid, completions);
            if (!completions.empty()) {
                pending_completions.push_back({name, std::move(completions)});
            }
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
    lock_guard<mutex> lock(m_groups_mutex);

    // check if it exists
    if (m_groups.count(name)) {
        return invalid_instance_id;
    }

    m_groups[name].set(member_process_ids);
    auto ret = m_groups[name].m_instance_id;

    for (auto& e : member_process_ids) {
        m_groups_of_process[e].insert(ret);
    }

    m_groups[name].assign_name(name);
    return ret;
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



