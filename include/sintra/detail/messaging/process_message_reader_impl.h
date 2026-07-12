// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../logging.h"
#include "../transceiver_impl.h"
#include "../tls_post_handler.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace sintra {

using std::function;
using std::lock_guard;
using std::recursive_mutex;
using std::thread;

void install_signal_handler();


inline bool thread_local tl_is_req_thread = false;

inline const char* Process_message_reader::reader_state_name(State state)
{
    switch (state) {
        case READER_NORMAL:   return "normal";
        case READER_SERVICE:  return "service";
        case READER_STOPPING: return "stopping";
        default:              return "unknown";
    }
}

inline const char* Process_message_reader::delivery_stream_name(Delivery_stream stream)
{
    switch (stream) {
        case Delivery_stream::Request: return "request";
        case Delivery_stream::Reply:   return "reply";
        default:                       return "unknown";
    }
}

inline const char* Process_message_reader::communication_state_name(int communication_state)
{
    switch (communication_state) {
        case Managed_process::COMMUNICATION_STOPPED: return "stopped";
        case Managed_process::COMMUNICATION_PAUSED:  return "paused";
        case Managed_process::COMMUNICATION_RUNNING: return "running";
        default:                                     return "unknown";
    }
}

inline void append_instance_id_value(std::ostringstream& out, instance_id_type value)
{
    if (value == invalid_instance_id) {
        out << "unknown";
        return;
    }

    out << static_cast<unsigned long long>(value);
}

inline void append_sequence_value(std::ostringstream& out, sequence_counter_type value)
{
    if (value == invalid_sequence) {
        out << "invalid";
        return;
    }

    out << value;
}

inline void append_sequence_lag(
    std::ostringstream&    out,
    sequence_counter_type  reading_sequence,
    sequence_counter_type  leading_sequence)
{
    out << " lag=";
    if (reading_sequence == invalid_sequence || leading_sequence == invalid_sequence) {
        out << "unknown";
        return;
    }

    if (leading_sequence < reading_sequence) {
        out << "regressed";
        return;
    }

    out << (leading_sequence - reading_sequence);
}

inline void append_current_communication_state(std::ostringstream& out)
{
    if (!s_mproc) {
        out << " communication_state=unreachable";
        return;
    }

    out << " communication_state="
        << Process_message_reader::communication_state_name(s_mproc->m_communication_state);
}

inline void append_message_ring_summary(
    std::ostringstream&                         out,
    const char*                                 label,
    const std::shared_ptr<Message_ring_R>&      ring,
    bool                                        running,
    bool                                        progress_available,
    sequence_counter_type                       published_sequence,
    bool                                        progress_stopped)
{
    out << " " << label
        << "{running=" << (running ? 1 : 0)
        << " progress_stopped=" << (progress_stopped ? 1 : 0);

    out << " published=";
    if (progress_available) {
        append_sequence_value(out, published_sequence);
    }
    else {
        out << "missing";
    }

    if (!ring) {
        out << " ring=missing}";
        return;
    }

    const auto reading_sequence = ring->get_message_reading_sequence();
    const auto leading_sequence = ring->get_leading_sequence();
    const auto ring_diagnostics = ring->get_diagnostics();

    out << " ring_stopping=" << (ring->is_stopping() ? 1 : 0)
        << " read=";
    append_sequence_value(out, reading_sequence);
    out << " lead=";
    append_sequence_value(out, leading_sequence);
    append_sequence_lag(out, reading_sequence, leading_sequence);
    out << " capacity=" << message_ring_size
        << " max_lag=" << ring_diagnostics.max_reader_lag
        << " overflow_count=" << ring_diagnostics.reader_lag_overflow_count
        << " worst_overflow_lag=" << ring_diagnostics.worst_overflow_lag
        << " evictions=" << ring_diagnostics.reader_eviction_count
        << " regressions=" << ring_diagnostics.reader_sequence_regressions
        << "}";
}

inline const char* Process_message_reader::reader_condition_name() const
{
    if (m_reader_state.load() == READER_STOPPING) {
        return "stopping";
    }

    if (m_req_running.load() || m_rep_running.load()) {
        return "running";
    }

    return "stopped";
}

inline std::string Process_message_reader::diagnostic_summary() const
{
    std::ostringstream out;

    out << "condition=" << reader_condition_name()
        << " state=" << reader_state_name(m_reader_state.load())
        << " target_process_id=";
    append_instance_id_value(out, m_process_instance_id);
    append_current_communication_state(out);

    const auto progress           = m_delivery_progress;
    const bool progress_available = static_cast<bool>(progress);

    const auto request_published = progress_available
        ? progress->request_sequence.load()
        : invalid_sequence;
    const auto reply_published = progress_available
        ? progress->reply_sequence.load()
        : invalid_sequence;

    const bool request_stopped = progress_available && progress->request_stopped.load();
    const bool reply_stopped   = progress_available && progress->reply_stopped.load();

    append_message_ring_summary(
        out,
        "request",
        m_in_req_c,
        m_req_running.load(),
        progress_available,
        request_published,
        request_stopped);
    append_message_ring_summary(
        out,
        "reply",
        m_in_rep_c,
        m_rep_running.load(),
        progress_available,
        reply_published,
        reply_stopped);

    return out.str();
}

inline std::string Process_message_reader::delivery_target_summary(
    Delivery_stream        stream,
    sequence_counter_type  target_sequence) const
{
    std::ostringstream out;

    out << "stream=" << delivery_stream_name(stream)
        << " target_sequence=";
    append_sequence_value(out, target_sequence);

    const auto progress = m_delivery_progress;
    if (!progress) {
        out << " observed=missing stream_stopped=unknown ";
        out << diagnostic_summary();
        return out.str();
    }

    const auto observed_sequence = (stream == Delivery_stream::Request)
        ? progress->request_sequence.load()
        : progress->reply_sequence.load();
    const bool stream_stopped = (stream == Delivery_stream::Request)
        ? progress->request_stopped.load()
        : progress->reply_stopped.load();

    out << " observed=";
    append_sequence_value(out, observed_sequence);
    out << " stream_stopped=" << (stream_stopped ? 1 : 0)
        << " target_lag=";
    if (target_sequence == invalid_sequence || observed_sequence == invalid_sequence) {
        out << "unknown";
    }
    else
    if (target_sequence > observed_sequence) {
        out << (target_sequence - observed_sequence);
    }
    else {
        out << 0;
    }

    out << " " << diagnostic_summary();
    return out.str();
}

inline std::string Process_message_reader::missing_reader_summary(instance_id_type target_process_id)
{
    std::ostringstream out;

    out << "condition=missing target_process_id=";
    append_instance_id_value(out, target_process_id);
    append_current_communication_state(out);
    return out.str();
}

inline bool validate_relay_sender(
    Message_prefix&                  message,
    instance_id_type                 ring_owner,
    const char*                      ring_name,
    const Process_message_reader&    reader)
{
    const instance_id_type sender_process      = process_of(message.sender_instance_id);
    const instance_id_type coordinator_process = process_of(s_coord_id);
    if (ring_owner == sender_process || ring_owner == coordinator_process) {
        return true;
    }

    Log_stream(log_level::warning)
        << "Sintra dropped a message on the " << ring_name
        << " ring because sender_instance_id="
        << static_cast<unsigned long long>(message.sender_instance_id)
        << " does not belong to ring owner "
        << static_cast<unsigned long long>(ring_owner)
        << " or coordinator process "
        << static_cast<unsigned long long>(coordinator_process)
        << ". " << reader.diagnostic_summary() << "\n";
    return false;
}

inline bool validate_request_message(
    Message_prefix&                  message,
    instance_id_type                 ring_owner,
    const Process_message_reader&    reader)
{
    if (!validate_relay_sender(message, ring_owner, "request", reader)) {
        return false;
    }

    if (message.message_type_id == not_defined_type_id) {
        Log_stream(log_level::warning)
            << "Sintra dropped a request message with undefined message_type_id from sender_instance_id="
            << static_cast<unsigned long long>(message.sender_instance_id)
            << ". " << reader.diagnostic_summary() << "\n";
        return false;
    }

    return true;
}

inline bool validate_reply_message(
    Message_prefix&                  message,
    instance_id_type                 ring_owner,
    const Process_message_reader&    reader)
{
    if (!validate_relay_sender(message, ring_owner, "reply", reader)) {
        return false;
    }

    if (message.receiver_instance_id == any_local) {
        Log_stream(log_level::warning)
            << "Sintra dropped a reply message with invalid any_local receiver from sender_instance_id="
            << static_cast<unsigned long long>(message.sender_instance_id)
            << ". " << reader.diagnostic_summary() << "\n";
        return false;
    }

    return true;
}

namespace detail
{

class Collected_handler_slot
{
public:
    Collected_handler_slot(
        const function<void(const Message_prefix&)>& handler,
        std::shared_ptr<Handler_slot_state>          state)
    :
        m_handler(handler),
        m_state(std::move(state))
    {}

    Collected_handler_slot(Collected_handler_slot&& other) noexcept
    :
        m_handler(std::move(other.m_handler)),
        m_state(std::move(other.m_state))
    {}

    Collected_handler_slot& operator=(Collected_handler_slot&& other) noexcept
    {
        if (this != &other) {
            m_handler = std::move(other.m_handler);
            m_state   = std::move(other.m_state);
        }
        return *this;
    }

    ~Collected_handler_slot() noexcept = default;

    Collected_handler_slot(const Collected_handler_slot&) = delete;
    Collected_handler_slot& operator=(const Collected_handler_slot&) = delete;

    void clear_handler() noexcept
    {
        m_handler = nullptr;
    }

    bool active() const noexcept
    {
        return m_state && m_state->active.load(std::memory_order_acquire);
    }

    const std::shared_ptr<Handler_slot_state>& state() const noexcept
    {
        return m_state;
    }

    void invoke(Message_prefix& message)
    {
        m_handler(message);
    }

private:
    function<void(const Message_prefix&)>   m_handler;
    std::shared_ptr<Handler_slot_state>     m_state;
};


class Handler_dispatch_depth_scope
{
public:
    explicit Handler_dispatch_depth_scope(bool registered) noexcept
    :
        m_registered(registered)
    {}

    ~Handler_dispatch_depth_scope() noexcept
    {
        release();
    }

    Handler_dispatch_depth_scope(const Handler_dispatch_depth_scope&) = delete;
    Handler_dispatch_depth_scope& operator=(const Handler_dispatch_depth_scope&) = delete;

    void release() noexcept
    {
        if (!m_registered) {
            return;
        }

        s_mproc->m_handlers_dispatch_depth.fetch_sub(1, std::memory_order_acq_rel);
        m_registered = false;
    }

private:
    bool m_registered;
};


class Handler_slot_invocation_scope
{
public:
    explicit Handler_slot_invocation_scope(const std::shared_ptr<Handler_slot_state>& state) noexcept
    :
        m_state(state),
        m_previous_handler_state(tl_current_handler_slot_state),
        m_previous_in_handler_dispatch(tl_in_handler_dispatch)
    {
        m_state->invocations.fetch_add(1, std::memory_order_acq_rel);
        tl_current_handler_slot_state             = m_state.get();
        tl_in_handler_dispatch                    = true;
    }

    ~Handler_slot_invocation_scope() noexcept
    {
        tl_in_handler_dispatch        = m_previous_in_handler_dispatch;
        tl_current_handler_slot_state = m_previous_handler_state;
        m_state->invocations.fetch_sub(1, std::memory_order_acq_rel);
    }

    Handler_slot_invocation_scope(const Handler_slot_invocation_scope&) = delete;
    Handler_slot_invocation_scope& operator=(const Handler_slot_invocation_scope&) = delete;

private:
    std::shared_ptr<Handler_slot_state>  m_state;
    const Handler_slot_state*            m_previous_handler_state;
    bool                                 m_previous_in_handler_dispatch;
};

} // namespace detail

inline void dispatch_event_handlers(
    Message_prefix&                            message,
    std::initializer_list<instance_id_type>    scope_ids)
{
    // Collect matching handlers under the lock, then invoke them after releasing
    // it.  On Windows, CRITICAL_SECTION (backing std::recursive_mutex) has unfair
    // locking: the most recent releaser is favored for the next acquisition.
    // When a reader thread rapidly processes many messages it can starve the main
    // thread trying to register a handler via activate_slot() / receive<T>().
    // Releasing before invocation gives waiting threads a chance to acquire the
    // lock between messages.
    std::vector<detail::Collected_handler_slot> collected;
    bool dispatch_registered = false;

    {
        lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

        auto it_mt = s_mproc->m_active_handlers.find(message.message_type_id);
        if (it_mt == s_mproc->m_active_handlers.end()) {
            return;
        }
        auto* sender_map = &it_mt->second;

        for (auto sid : scope_ids) {
            auto shl = sender_map->find(sid);
            if (shl != sender_map->end()) {
                for (auto& handler : shl->second) {
                    collected.emplace_back(handler, detail::handler_slot_state_for(handler));
                }
            }
        }

        // Preserve the existing dispatch-depth bookkeeping while callbacks run
        // outside the handler mutex.
        if (!collected.empty()) {
            s_mproc->m_handlers_dispatch_depth.fetch_add(1, std::memory_order_acq_rel);
            dispatch_registered = true;
        }
    }

    if (!dispatch_registered) {
        return;
    }
    detail::Handler_dispatch_depth_scope dispatch_depth_scope(dispatch_registered);

    try {
        for (auto& collected_slot : collected) {
            detail::Collected_handler_slot slot = std::move(collected_slot);
            if (!slot.active()) {
                continue;
            }

            std::unique_lock<std::mutex> slot_invocation_lock(slot.state()->invocation_mutex);
            if (!slot.active()) {
                continue;
            }

            detail::Handler_slot_invocation_scope invocation(slot.state());
            try {
                if (slot.active()) {
                    slot.invoke(message);
                }
            }
            catch (...) {
                slot.clear_handler();
                throw;
            }
            slot.clear_handler();
        }
    }
    catch (...) {
        dispatch_depth_scope.release();
        throw;
    }

    dispatch_depth_scope.release();
}

// Historical note: mingw 11.2.0 had issues with inline thread_local non-POD objects.
// Keep the callable in a heap object to avoid TLS destructor crashes.

inline
Process_message_reader::Process_message_reader(
    instance_id_type process_instance_id,
    Delivery_progress_ptr delivery_progress,
    uint32_t occurrence,
    uint64_t managed_child_custody_identity):
    m_reader_state(READER_NORMAL),
    m_process_instance_id(process_instance_id),
    m_occurrence(occurrence),
    m_managed_child_custody_identity(managed_child_custody_identity),
    m_delivery_progress(std::move(delivery_progress))
{
    if (!m_delivery_progress) {
        m_delivery_progress = std::make_shared<Delivery_progress>();
    }

    m_in_req_c              = std::make_shared<Message_ring_R>(
        s_mproc->m_directory,
        "req",
        m_process_instance_id,
        occurrence);
    m_in_rep_c              = std::make_shared<Message_ring_R>(
        s_mproc->m_directory,
        "rep",
        m_process_instance_id,
        occurrence);
    m_request_reader_thread = std::make_unique<thread>([&] () { request_reader_function(); });
    m_request_reader_thread->detach();
    m_reply_reader_thread   = std::make_unique<thread>([&] () { reply_reader_function();   });
    m_reply_reader_thread->detach();
}


inline
void Process_message_reader::wait_until_ready()
{
    std::unique_lock<std::mutex> lk(m_ready_mutex);
    m_ready_condition.wait(lk, [&]() {
        const bool req_started = m_req_running.load();
        const bool rep_started = m_rep_running.load();
        const bool stopping    = m_reader_state.load() == READER_STOPPING;
        return (req_started && rep_started) || stopping;
    });
}



inline
void Process_message_reader::stop_nowait()
{
    m_reader_state = READER_STOPPING;
    m_ready_condition.notify_all();

    if (auto progress = m_delivery_progress) {
        const auto req_seq = m_in_req_c ? m_in_req_c->get_message_reading_sequence() : invalid_sequence;
        progress->request_sequence = req_seq;
        progress->request_stopped  = true;

        const auto rep_seq = m_in_rep_c ? m_in_rep_c->get_message_reading_sequence() : invalid_sequence;
        progress->reply_sequence   = rep_seq;
        progress->reply_stopped    = true;

        if (s_mproc) {
            s_mproc->notify_delivery_progress();
        }
    }

    m_in_req_c->done_reading();
    m_in_req_c->request_stop();

    // if there are outstanding RPC calls waiting for reply, they should be
    // unblocked (and fail), to avoid a deadlock
    s_mproc->unblock_rpc();


    if (!tl_is_req_thread || tl_in_post_handler()) {
        m_in_rep_c->done_reading();
        m_in_rep_c->request_stop();
    }
    else {
        // The purpose of the lambda below is the following scenario:
        // 1. This function is called from a handler, thus from within the request
        //    reading function.
        // 2. The very same handler also calls some RPC, whose reply is sent through
        //    the reply ring, in the corresponding thread.
        // If we force the reply ring to exit, there will be no ring to get the
        // reply from, leading to a deadlock. Thus if the function is determined
        // to be called from within the request reading thread, forcing the reply
        // reading thread to exit will happen only after the request reading loop
        // exits.

        auto rep_ring = m_in_rep_c;
        if (!tl_post_handler_function_ready()) {
            tl_post_handler_function_ref() = [rep_ring]() {
                if (!rep_ring) {
                    return;
                }
                rep_ring->done_reading();
                rep_ring->request_stop();
            };
        }
        else {
            auto previous = std::move(*tl_post_handler_function);
            tl_post_handler_function_ref() = [prev = std::move(previous), rep_ring]() mutable {
                if (prev) {
                    prev();
                }
                if (!rep_ring) {
                    return;
                }
                rep_ring->done_reading();
                rep_ring->request_stop();
            };
        }
    }
}



inline
bool Process_message_reader::stop_and_wait(double waiting_period)
{
    std::unique_lock<std::mutex> lk(m_stop_mutex);
    stop_nowait();

    // `waiting_period` is a total budget for the complete stop transaction,
    // not a budget that each fallback phase may spend independently.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(waiting_period > 0.0 ? waiting_period : 0.0);

    auto no_readers = [&]() {
        const bool req_running = m_req_running.load();
        const bool rep_running = m_rep_running.load();
        return !(req_running || rep_running);
    };

    auto wait_for_readers = [&]() {
        if (!no_readers()) {
            m_stop_condition.wait_until(lk, deadline, no_readers);
        }
    };

    wait_for_readers();

    if (!no_readers()) {
        // We might get here, if the coordinator is gone already.
        // In this case, we unblock pending RPC calls and do some more waiting.
        s_mproc->unblock_rpc(m_process_instance_id);
        wait_for_readers();
    }

    if (!no_readers()) {
        m_in_req_c->done_reading();
        m_in_req_c->request_stop();
        m_in_rep_c->done_reading();
        m_in_rep_c->request_stop();
        wait_for_readers();
        if (!no_readers()) {
            Log_stream(log_level::warning)
                << "Process_message_reader::stop_and_wait timeout: "
                << diagnostic_summary() << "\n";
        }
    }
    return no_readers();
}


inline
Process_message_reader::~Process_message_reader()
{
    if (!stop_and_wait(22.)) {
        // 'wait_for' timed out. To avoid hanging, we exit.
        // If we get here, something must have probably gone wrong.
        // Weird things might happen while exiting.
        exit(1);
    }

    m_request_reader_thread.reset();
    m_in_req_c.reset();
    m_reply_reader_thread.reset();
    m_in_rep_c.reset();
}

inline
void Process_message_reader::begin_reading_session(
    const std::shared_ptr<Message_ring_R>& ring,
    std::atomic<bool>&                     running_flag)
{
    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers++;
    s_mproc->m_num_active_readers_mutex.unlock();

    ring->start_reading();
    {
        std::lock_guard<std::mutex> ready_guard(m_ready_mutex);
        running_flag = true;
    }
    m_ready_condition.notify_all();
}

inline
void Process_message_reader::end_reading_session(
    const std::shared_ptr<Message_ring_R>& ring,
    std::atomic<bool>&                     running_flag)
{
    ring->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();
    if (s_coord) {
        // Reader exit can satisfy the coordinator drain predicate; route it
        // through the coordinator drain-state gate so waiters cannot miss it.
        s_coord->note_draining_state_change();
    }

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    running_flag = false;
    m_ready_condition.notify_all();
    m_stop_condition.notify_one();
}


// This implementation of the following functions assumes the following:
// We have two types of messages: rpc messages and events
// - RPC messages are only addressed to specific receivers. All receivers able to handle this
//   type of message use the same handler, thus the handler map is static.
// - Events are only addressed "to whom it may concern" (i.e. any_*).
//   Their handlers are registered at process level, but they are assigned dynamically, thus
//   they may differ across different instances of the same type of receiver type.


inline
void Process_message_reader::request_reader_function()
{
    install_signal_handler();

    tl_is_req_thread = true;
    s_tl_current_request_reader = this;

    auto progress = m_delivery_progress;
    if (progress) {
        progress->request_stopped = false;
    }

    auto publish_request_progress = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->request_sequence.exchange(seq);
        if (previous != seq && s_mproc) {
            s_mproc->notify_delivery_progress();
        }
    };

    begin_reading_session(m_in_req_c, m_req_running);

    publish_request_progress(m_in_req_c->get_message_reading_sequence());

    while (true) {
        const State reader_state = m_reader_state.load();
        if (reader_state == READER_STOPPING) {
            break;
        }
        s_tl_current_message = nullptr;

        // NOTE: Barrier completion messages (and the corresponding flush tokens)
        // travel on the *reply* ring. Do not drain m_flush_sequence in the
        // request reader. The reply reader thread is responsible for popping
        // tokens when the *reply* reading sequence reaches them.

        Message_prefix* m = m_in_req_c->fetch_message();
        s_tl_current_message = m;
        if (m == nullptr) {
            break;
        }
        if (!validate_request_message(*m, m_in_req_c->m_id, *this)) {
            publish_request_progress(m_in_req_c->get_message_reading_sequence());
            continue;
        }

        if (is_local_instance(m->receiver_instance_id)) {
            if (m->receiver_instance_id == any_local) {
                // Local event: only handle on the originating process ring.
                const bool reading_local_ring =
                    has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c);
                if (reading_local_ring &&
                    ((reader_state == READER_NORMAL) ||
                     (s_coord && m->message_type_id >
                         (type_id_type)detail::reserved_id::base_of_messages_handled_by_coordinator)))
                {
                    dispatch_event_handlers(
                        *m,
                        {m->sender_instance_id, any_local, any_local_or_remote});
                }
            }
            else {
                // If the coordinator is local and this request targets a *service* instance
                // (e.g., Coordinator), relay it to the coordinator's ring and *skip* local
                // dispatch to avoid double-processing (local dispatch + relay).
                if (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c) &&
                    is_service_instance(m->receiver_instance_id))
                {
                    s_mproc->m_out_req_c->relay(
                        *m,
                        m_managed_child_custody_identity,
                        m_occurrence);
                    publish_request_progress(m_in_req_c->get_message_reading_sequence());
                    continue;
                }

                const bool receiver_exists =
                    s_mproc->m_local_pointer_of_instance_id.contains_key(m->receiver_instance_id);
                // Targeted local RPCs can legitimately race with receiver
                // destruction after name resolution or enqueue. Treat a missing
                // receiver as a normal unavailable-target reply instead of as an
                // invariant violation.
                if (!receiver_exists) {
                    if (m->function_instance_id == invalid_instance_id) {
                        publish_request_progress(m_in_req_c->get_message_reading_sequence());
                        continue;
                    }

                    const std::string reason     = "RPC target is no longer available.";
                    auto*             placed_msg =
                        s_mproc->m_out_rep_c->write<Transceiver::exception>(
                            vb_size<Transceiver::exception>(reason),
                            reason);
                    placed_msg->sender_instance_id   = m->receiver_instance_id;
                    placed_msg->receiver_instance_id = m->sender_instance_id;
                    placed_msg->function_instance_id = m->function_instance_id;
                    placed_msg->exception_type_id =
                        (type_id_type)detail::reserved_id::sintra_rpc_unavailable;
                    s_mproc->m_out_rep_c->done_writing();
                    publish_request_progress(m_in_req_c->get_message_reading_sequence());
                    continue;
                }

                if ((reader_state == READER_NORMAL) ||
                    (is_service_instance(m->receiver_instance_id) && s_coord) ||
                    (m->sender_instance_id == s_coord_id))
                {
                    // If addressed to a specified local receiver, this may only be an RPC call,
                    // thus the named receiver must exist.

                    // if the receiver registered handler, call the handler
                    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
                    void (*handler_fn)(Message_prefix&);
                    {
                        auto scoped_map = Transceiver::get_rpc_handler_map().scoped();
                        auto it         = scoped_map.get().find(m->message_type_id);
                        if (it == scoped_map.get().end()) {
                            Log_stream(log_level::warning)
                                << "Received RPC for unknown message type "
                                << static_cast<unsigned long long>(m->message_type_id)
                                << "; rejecting the request. "
                                << diagnostic_summary() << "\n";

                            const std::string reason     = "RPC function is not available.";
                            auto*             placed_msg =
                                s_mproc->m_out_rep_c->write<Transceiver::exception>(
                                    vb_size<Transceiver::exception>(reason),
                                    reason);
                            placed_msg->sender_instance_id   = m->receiver_instance_id;
                            placed_msg->receiver_instance_id = m->sender_instance_id;
                            placed_msg->function_instance_id = m->function_instance_id;
                            placed_msg->exception_type_id =
                                (type_id_type)detail::reserved_id::sintra_rpc_unavailable;
                            s_mproc->m_out_rep_c->done_writing();
                            publish_request_progress(m_in_req_c->get_message_reading_sequence());
                            continue;
                        }

                        // Copy the function pointer while holding the lock
                        handler_fn = it->second;
                        // Spinlock released here automatically when scoped_map goes out of scope
                    }

                    (*handler_fn)(*m);
                }
            }
        }
        else
        if (m->receiver_instance_id >= any_remote) {

            // this is an interprocess event message.

            if ((reader_state == READER_NORMAL) ||
                (s_coord && m->message_type_id > (type_id_type)detail::reserved_id::base_of_messages_handled_by_coordinator))
            {
                // Avoid double-handling on the coordinator: when the coordinator is
                // reading a remote ring, it will relay below to its own ring as well.
                // In that case, skip local event handling here and let the relayed
                // copy be handled when reading the coordinator's ring.
                const bool coordinator_reading_remote = (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c));
                const bool sender_is_local = is_local(m->sender_instance_id);
                const bool skip_local_sender =
                    (m->receiver_instance_id == any_remote) && sender_is_local;

                if (!coordinator_reading_remote && !skip_local_sender) {
                    dispatch_event_handlers(
                        *m,
                        {m->sender_instance_id, any_remote, any_local_or_remote});
                }
            }

            // if the coordinator is in this process, relay
            if (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c)) {
                s_mproc->m_out_req_c->relay(*m);
            }
        }
        else {
            // a local event has no place in interprocess messages
            // this would be a bug.
            assert(m->receiver_instance_id != any_local);

            // a specific non-local receiver means an rpc to another process.
            // if the coordinator is in this process, relay
            if (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c)) {
                // the message type is specified, thus it is a request
                s_mproc->m_out_req_c->relay(*m);
            }
        }

        if (tl_post_handler_function_ready()) {
            auto post_handler = std::move(*tl_post_handler_function);
            tl_post_handler_function_clear();
            Post_handler_guard post_guard;
            post_handler();
        }

        publish_request_progress(m_in_req_c->get_message_reading_sequence());
    }

    // Update delivery progress BEFORE end_reading_session, which sets
    // m_req_running = false.  Once that flag is cleared the destructor's
    // stop_and_wait() may return and reset m_in_req_c, so all accesses
    // to the ring must happen while the flag still indicates "running".
    if (progress) {
        const auto seq = m_in_req_c->get_message_reading_sequence();
        progress->request_sequence = seq;
        progress->request_stopped  = true;
    }
    if (s_mproc) {
        s_mproc->notify_delivery_progress();
    }

    end_reading_session(m_in_req_c, m_req_running);

    s_tl_current_request_reader = nullptr;
    tl_is_req_thread = false;

    tl_post_handler_function_clear();
    tl_post_handler_function_release();
}



inline
Process_message_reader::Delivery_target Process_message_reader::prepare_delivery_target(
    Delivery_stream        stream,
    sequence_counter_type  target_sequence) const
{
    Delivery_target target;
    target.process_instance_id = m_process_instance_id;
    target.stream              = stream;
    target.target              = target_sequence;

    if (target_sequence == invalid_sequence) {
        return target;
    }

    auto progress = m_delivery_progress;
    if (!progress) {
        return target;
    }

    target.progress = progress;

    if (stream == Delivery_stream::Request && s_tl_current_request_reader == this) {
        return target;
    }

    const auto strong_progress = target.progress.lock();
    if (!strong_progress) {
        // Reader was destroyed after we captured the target; no wait necessary.
        return target;
    }

    const auto observed = (stream == Delivery_stream::Request)
        ? strong_progress->request_sequence.load()
        : strong_progress->reply_sequence.load();

    if (observed >= target_sequence) {
        return target;
    }

    target.wait_needed = true;
    return target;
}



inline
void Process_message_reader::reply_reader_function()
{
    install_signal_handler();

    auto progress = m_delivery_progress;
    if (progress) {
        progress->reply_stopped = false;
    }

    auto publish_reply_progress = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->reply_sequence.exchange(seq);
        if (previous != seq && s_mproc) {
            s_mproc->notify_delivery_progress();
        }
    };

    begin_reading_session(m_in_rep_c, m_rep_running);

    publish_reply_progress(m_in_rep_c->get_message_reading_sequence());

    while (true) {
        const State reader_state = m_reader_state.load();
        if (reader_state == READER_STOPPING) {
            break;
        }
        s_tl_current_message = nullptr;

        // Check if reply ring has reached any pending flush sequences.
        // Barrier completions are RPC responses sent on the reply ring, so
        // flush tokens for barriers refer to reply ring sequences.
        {
            auto   reading_sequence = m_in_rep_c->get_message_reading_sequence();
            size_t notifications    = 0;
            {
                std::unique_lock<std::mutex> flush_lock(s_mproc->m_flush_sequence_mutex);
                while (!s_mproc->m_flush_sequence.empty() &&
                    reading_sequence >= s_mproc->m_flush_sequence.front())
                {
                    s_mproc->m_flush_sequence.pop_front();
                    ++notifications;
                }
            }
            while (notifications > 0) {
                s_mproc->m_flush_sequence_condition.notify_one();
                --notifications;
            }
        }

        Message_prefix* m = m_in_rep_c->fetch_message();
        s_tl_current_message = m;

        if (m == nullptr) {
            break;
        }
        if (!validate_reply_message(*m, m_in_rep_c->m_id, *this)) {
            publish_reply_progress(m_in_rep_c->get_message_reading_sequence());
            continue;
        }

        if (is_local_instance(m->receiver_instance_id)) {

            if ((reader_state == READER_NORMAL) ||
                (m->receiver_instance_id == s_coord_id && s_coord) ||
                (m->sender_instance_id   == s_coord_id))
            {
                // Hold the spinlock for the entire critical section to prevent use-after-free.
                // The Transceiver destructor erases from this map, so holding the lock ensures
                // the pointer remains valid while we access it.
                Transceiver::Return_handler handler_copy;
                bool have_handler        = false;
                bool object_found        = false;
                bool handler_was_retired = false;
                {
                    auto scoped_map = s_mproc->m_local_pointer_of_instance_id.scoped();
                    auto it         = scoped_map.get().find(m->receiver_instance_id);

                    if (it != scoped_map.get().end()) {
                        object_found = true;
                        auto &return_handlers = it->second->m_active_return_handlers;

                        {
                            std::lock_guard<std::mutex> guard(it->second->m_return_handlers_mutex);
                            auto it2 = return_handlers.find(m->function_instance_id);
                            if (it2 != return_handlers.end()) {
                                handler_copy = it2->second;
                                have_handler = true;
                            }
                            else {
                                handler_was_retired =
                                    it->second->m_retired_return_handler_ids.find(m->function_instance_id) !=
                                    it->second->m_retired_return_handler_ids.end();
                            }
                        }
                    }
                    // Spinlock released here automatically when scoped_map goes out of scope
                }

                // Now invoke handlers with spinlock released to avoid holding it during
                // potentially long-running user code.
                if (object_found) {
                    if (have_handler) {
                        if (m->exception_type_id == not_defined_type_id) {
                            handler_copy.return_handler(*m);
                        }
                        else
                        if (m->exception_type_id != (type_id_type)detail::reserved_id::deferral) {
                            handler_copy.exception_handler(*m);
                        }
                        else {
                            handler_copy.deferral_handler(*m);
                        }
                    }
                    else {
                        // No active return handler - can happen if the caller already cleaned up
                        // (e.g., after cancellation/shutdown) and a late/duplicate message arrived.
                        // Drop it quietly unless we're fully RUNNING; in RUNNING emit a diagnostic
                        // but do not hard-assert to avoid modal dialogs on Windows Debug.
                        if (!handler_was_retired &&
                            s_mproc &&
                            s_mproc->m_communication_state == Managed_process::COMMUNICATION_RUNNING)
                        {
                            Log_stream(log_level::warning)
                                << "Warning: Reply reader received message for function_instance_id="
                                << static_cast<unsigned long long>(m->function_instance_id)
                                << " but no active handler found (receiver_instance_id="
                                << static_cast<unsigned long long>(m->receiver_instance_id)
                                << ") " << diagnostic_summary() << "\n";
                        }
                    }
                }
                else {
                    // The target object no longer exists locally. During shutdown or after
                    // coordinator loss, late replies can legitimately arrive after objects
                    // have been torn down. Do not hard-assert; drop unless we're fully RUNNING.
                    if (s_mproc && s_mproc->m_communication_state == Managed_process::COMMUNICATION_RUNNING) {
                        Log_stream(log_level::warning)
                            << "Warning: Reply reader received message for receiver_instance_id="
                            << static_cast<unsigned long long>(m->receiver_instance_id)
                            << " but object no longer exists (sender="
                            << static_cast<unsigned long long>(m->sender_instance_id)
                            << ", function=" << static_cast<unsigned long long>(m->function_instance_id)
                            << ") " << diagnostic_summary() << "\n";
                    }
                }
            }
        }
        else {

            // A specific non-local receiver implies an rpc call to another process,
            // thus if the coordinator is in the current process, relay -
            // unless the message originates from the ring we would relay to.
            if (s_coord && !has_same_mapping(*s_mproc->m_out_rep_c, *m_in_rep_c) ) {
                // the message type is not specified, thus it is a reply
                s_mproc->m_out_rep_c->relay(*m);
            }
        }

        publish_reply_progress(m_in_rep_c->get_message_reading_sequence());
    }

    // Update delivery progress BEFORE end_reading_session, which sets
    // m_rep_running = false.  Once that flag is cleared the destructor's
    // stop_and_wait() may return and reset m_in_rep_c, so all accesses
    // to the ring must happen while the flag still indicates "running".
    publish_reply_progress(m_in_rep_c->get_message_reading_sequence());

    if (progress) {
        progress->reply_stopped = true;
    }
    if (s_mproc) {
        s_mproc->notify_delivery_progress();
    }

    end_reading_session(m_in_rep_c, m_rep_running);

}


} // namespace sintra
