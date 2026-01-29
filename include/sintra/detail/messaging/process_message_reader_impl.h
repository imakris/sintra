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
#include <memory>
#include <mutex>
#include <utility>

#ifndef SINTRA_TRACE_WORLD
#define SINTRA_TRACE_WORLD 0
#endif

namespace sintra {

void install_signal_handler();


using std::thread;
using std::unique_ptr;
using std::function;


// ============================================================================
// Defensive utilities for zero-copy + deferral safety
// ============================================================================
// These helpers provide a safe way to clone ring messages when storing
// references in deferred work. While the current implementation executes
// deferred work before advancing the ring, future code might capture message
// pointers in lambdas that outlive the ring's read cycle.
//
// Usage: If you need to capture a message pointer for deferred execution:
//   auto cloned = sintra_clone_message(m);
//   run_after_current_handler([buf = std::move(cloned)]() {
//       auto* msg = reinterpret_cast<const Message_prefix*>(buf.get());
//       // safely access msg
//   });

inline size_t sintra_message_total_size(const Message_prefix* m)
{
    return static_cast<size_t>(m->bytes_to_next_message);
}

inline std::unique_ptr<std::byte[]> sintra_clone_message(const Message_prefix* m)
{
    const size_t n = sintra_message_total_size(m);
    auto buf = std::unique_ptr<std::byte[]>(new std::byte[n]);
    std::memcpy(buf.get(), reinterpret_cast<const void*>(m), n);
    return buf;
}
// ============================================================================


inline bool thread_local tl_is_req_thread = false;

inline bool on_request_reader_thread()
{
    return tl_is_req_thread;
}

// Historical note: mingw 11.2.0 had issues with inline thread_local non-POD objects.
// Keep the callable in a heap object to avoid TLS destructor crashes.

inline
Process_message_reader::Process_message_reader(
    instance_id_type process_instance_id,
    Delivery_progress_ptr delivery_progress,
    uint32_t occurrence):
    m_reader_state(READER_NORMAL),
    m_process_instance_id(process_instance_id),
    m_delivery_progress(std::move(delivery_progress))
{
    if (!m_delivery_progress) {
        m_delivery_progress = std::make_shared<Delivery_progress>();
    }

    m_in_req_c = std::make_shared<Message_ring_R>(
        s_mproc->m_directory, "req", m_process_instance_id, occurrence);
    m_in_rep_c = std::make_shared<Message_ring_R>(
        s_mproc->m_directory, "rep", m_process_instance_id, occurrence);
    m_request_reader_thread = new thread([&] () { request_reader_function(); });
    m_request_reader_thread->detach();
    m_reply_reader_thread   = new thread([&] () { reply_reader_function();   });
    m_reply_reader_thread->detach();
}


inline
void Process_message_reader::wait_until_ready()
{
    std::unique_lock<std::mutex> lk(m_ready_mutex);
    m_ready_condition.wait(lk, [&]() {
        const bool req_started = m_req_running.load();
        const bool rep_started = m_rep_running.load();
        const bool stopping = m_reader_state.load() == READER_STOPPING;
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

    auto no_readers = [&]() {
        const bool req_running = m_req_running.load();
        const bool rep_running = m_rep_running.load();
        return !(req_running || rep_running);
    };

    m_stop_condition.wait_for(
        lk, std::chrono::duration<double>(waiting_period), no_readers);

    if (!no_readers()) {
        // We might get here, if the coordinator is gone already.
        // In this case, we unblock pending RPC calls and do some more waiting.
        s_mproc->unblock_rpc(m_process_instance_id);
        m_stop_condition.wait_for(
            lk, std::chrono::duration<double>(waiting_period), no_readers);
    }

    if (!no_readers()) {
        m_in_req_c->done_reading();
        m_in_req_c->request_stop();
        m_in_rep_c->done_reading();
        m_in_rep_c->request_stop();
        m_stop_condition.wait_for(
            lk, std::chrono::duration<double>(1.0), no_readers);
        if (!no_readers()) {
            Log_stream(log_level::warning)
                << "Process_message_reader::stop_and_wait timeout: pid="
                << static_cast<unsigned long long>(m_process_instance_id)
                << " req_running=" << m_req_running.load()
                << " rep_running=" << m_rep_running.load() << "\n";
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

    delete m_request_reader_thread;
    m_in_req_c.reset();
    delete m_reply_reader_thread;
    m_in_rep_c.reset();
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

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers++;
    s_mproc->m_num_active_readers_mutex.unlock();

    m_in_req_c->start_reading();
    {
        std::lock_guard<std::mutex> ready_guard(m_ready_mutex);
        m_req_running = true;
    }
    m_ready_condition.notify_all();

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

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        // TODO: If some process not being part of the core set of processes sends nonsense,
        // it might be a good idea to kill it. If it is in the core set of processes,
        // then it would be a bug.
        assert(m_in_req_c->m_id == process_of(m->sender_instance_id) ||
               m_in_req_c->m_id == process_of(s_coord_id));

        assert(m->message_type_id != not_defined_type_id);

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
                    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

                    auto& active_handlers = s_mproc->m_active_handlers;
                    auto it_mt = active_handlers.find(m->message_type_id);

                    if (it_mt != active_handlers.end()) {
                        instance_id_type sids[] = {
                            m->sender_instance_id,
                            any_local,
                            any_local_or_remote
                        };

                        for (auto sid : sids) {
                            auto shl = it_mt->second.find(sid);
                            if (shl != it_mt->second.end()) {
                                for (auto& e : shl->second) {
                                    e(*m);
                                }
                            }
                        }
                    }
                }
            }
            else {
                // If the coordinator is local and this request targets a *service* instance
                // (e.g., Coordinator), relay it to the coordinator's ring and *skip* local
                // dispatch to avoid double-processing (local dispatch + relay).
                if (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c) &&
                    is_service_instance(m->receiver_instance_id))
                {
                    s_mproc->m_out_req_c->relay(*m);
                    publish_request_progress(m_in_req_c->get_message_reading_sequence());
                    continue;
                }

                // If addressed to a specified local receiver, this may only be an RPC call,
                // thus the receiver must exist.
                assert(
                    reader_state == READER_NORMAL ?
                        s_mproc->m_local_pointer_of_instance_id.find(m->receiver_instance_id) !=
                        s_mproc->m_local_pointer_of_instance_id.end()
                    :
                        true
                );

                if ((reader_state == READER_NORMAL) ||
                    (is_service_instance(m->receiver_instance_id) && s_coord) ||
                    (m->sender_instance_id == s_coord_id) )
                {
                    // If addressed to a specified local receiver, this may only be an RPC call,
                    // thus the named receiver must exist.

                    // if the receiver registered handler, call the handler
                    // Hold spinlock while accessing the iterator to prevent use-after-invalidation
                    void (*handler_fn)(Message_prefix&);
                    {
                        auto scoped_map = Transceiver::get_rpc_handler_map().scoped();
                        auto it = scoped_map.get().find(m->message_type_id);
                        assert(it != scoped_map.get().end()); // this would be a library error

                        // Copy the function pointer while holding the lock
                        handler_fn = it->second;
                        // Spinlock released here automatically when scoped_map goes out of scope
                    }

                    (*handler_fn)(*m); // call the handler
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
                    lock_guard<recursive_mutex> sl(s_mproc->m_handlers_mutex);

                    // find handlers that operate with this type of message in this process
                    auto& active_handlers = s_mproc->m_active_handlers;
                    auto it_mt = active_handlers.find(m->message_type_id);

                    if (it_mt != active_handlers.end()) {
                        instance_id_type sids[] = {
                            m->sender_instance_id,
                            any_remote,
                            any_local_or_remote
                        };

                        // Optional debug tracing for world broadcasts to diagnose missing deliveries (SINTRA_TRACE_WORLD).
                        const bool trace_world = (SINTRA_TRACE_WORLD != 0);

                        if (trace_world) {
                            Log_stream(log_level::debug)
                                << "[sintra_trace_world] pid=" << static_cast<int>(getpid())
                                << " reader_state=" << static_cast<int>(reader_state)
                                << " msg_type=" << static_cast<unsigned long long>(m->message_type_id)
                                << " sender_iid=" << static_cast<unsigned long long>(m->sender_instance_id)
                                << " recv_iid=" << static_cast<unsigned long long>(m->receiver_instance_id)
                                << " proc_iid="
                                << static_cast<unsigned long long>(s_mproc ? s_mproc->m_instance_id : 0ULL) << "\n";
                        }

                        for (auto sid : sids) {
                            auto shl = it_mt->second.find(sid);
                            if (shl != it_mt->second.end()) {
                                if (trace_world) {
                                    Log_stream(log_level::debug)
                                        << "[sintra_trace_world] pid=" << static_cast<int>(getpid())
                                        << " sid_match=" << static_cast<unsigned long long>(sid)
                                        << " handlers=" << shl->second.size() << "\n";
                                }
                                for (auto& e : shl->second) {
                                    e(*m);
                                }
                            }
                        }
                    }
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

    m_in_req_c->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    m_req_running = false;
    m_ready_condition.notify_all();
    m_stop_condition.notify_one();

    if (progress) {
        const auto seq = m_in_req_c->get_message_reading_sequence();
        progress->request_sequence = seq;
        progress->request_stopped  = true;
    }
    if (s_mproc) {
        s_mproc->notify_delivery_progress();
    }

    s_tl_current_request_reader = nullptr;
    tl_is_req_thread = false;

    tl_post_handler_function_clear();
    tl_post_handler_function_release();
}



inline
Process_message_reader::Delivery_target Process_message_reader::prepare_delivery_target(
    Delivery_stream stream,
    sequence_counter_type target_sequence) const
{
    Delivery_target target;
    target.stream = stream;
    target.target = target_sequence;

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

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers++;
    s_mproc->m_num_active_readers_mutex.unlock();

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

    m_in_rep_c->start_reading();
    {
        std::lock_guard<std::mutex> ready_guard(m_ready_mutex);
        m_rep_running = true;
    }
    m_ready_condition.notify_all();

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
            auto reading_sequence = m_in_rep_c->get_message_reading_sequence();
            size_t notifications = 0;
            {
                std::unique_lock<std::mutex> flush_lock(s_mproc->m_flush_sequence_mutex);
                while (!s_mproc->m_flush_sequence.empty() &&
                       reading_sequence >= s_mproc->m_flush_sequence.front()) {
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

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        assert(m_in_rep_c->m_id == process_of(m->sender_instance_id) ||
               m_in_rep_c->m_id == process_of(s_coord_id));

        assert(m->receiver_instance_id != any_local);

        if (is_local_instance(m->receiver_instance_id)) {

            if ((reader_state == READER_NORMAL) ||
                (m->receiver_instance_id == s_coord_id && s_coord) ||
                (m->sender_instance_id   == s_coord_id) )
            {
                // Hold the spinlock for the entire critical section to prevent use-after-free.
                // The Transceiver destructor erases from this map, so holding the lock ensures
                // the pointer remains valid while we access it.
                Transceiver::Return_handler handler_copy;
                bool have_handler = false;
                bool object_found = false;
                {
                    auto scoped_map = s_mproc->m_local_pointer_of_instance_id.scoped();
                    auto it = scoped_map.get().find(m->receiver_instance_id);

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
                        if (s_mproc && s_mproc->m_communication_state == Managed_process::COMMUNICATION_RUNNING) {
                            Log_stream(log_level::warning)
                                << "Warning: Reply reader received message for function_instance_id="
                                << static_cast<unsigned long long>(m->function_instance_id)
                                << " but no active handler found (receiver_instance_id="
                                << static_cast<unsigned long long>(m->receiver_instance_id) << ")\n";
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
                            << ", function=" << static_cast<unsigned long long>(m->function_instance_id) << ")\n";
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

    m_in_rep_c->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    m_rep_running = false;
    m_ready_condition.notify_all();
    m_stop_condition.notify_one();

    publish_reply_progress(m_in_rep_c->get_message_reading_sequence());

    if (progress) {
        progress->reply_stopped = true;
    }
    if (s_mproc) {
        s_mproc->notify_delivery_progress();
    }

}


} // namespace sintra

