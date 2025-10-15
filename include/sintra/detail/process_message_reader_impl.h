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

#ifndef SINTRA_PROCESS_MESSAGE_READER_IMPL_H
#define SINTRA_PROCESS_MESSAGE_READER_IMPL_H


#include "transceiver_impl.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

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

// Historical note: mingw 11.2.0 had issues with inline thread_local non-POD objects
// (January 2022). We now store the callable directly and rely on the fixed runtime
// behaviour to avoid manual allocation.
inline thread_local function<void()> tl_post_handler_function;

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
        const bool req_started = m_req_running.load(std::memory_order_acquire);
        const bool rep_started = m_rep_running.load(std::memory_order_acquire);
        const bool stopping = m_reader_state.load(std::memory_order_acquire) == READER_STOPPING;
        return (req_started && rep_started) || stopping;
    });
}



inline
void Process_message_reader::stop_nowait()
{
    m_reader_state.store(READER_STOPPING, std::memory_order_release);
    m_ready_condition.notify_all();

    if (auto progress = m_delivery_progress) {
        const auto req_seq = m_in_req_c ? m_in_req_c->get_message_reading_sequence() : invalid_sequence;
        progress->request.delivered.store(req_seq, std::memory_order_release);
        progress->request.processed.store(req_seq, std::memory_order_release);
        progress->request.stopped.store(true, std::memory_order_release);

        const auto rep_seq = m_in_rep_c ? m_in_rep_c->get_message_reading_sequence() : invalid_sequence;
        progress->reply.delivered.store(rep_seq, std::memory_order_release);
        progress->reply.processed.store(rep_seq, std::memory_order_release);
        progress->reply.stopped.store(true, std::memory_order_release);

        progress->notify_waiters();
    }

    m_in_req_c->done_reading();
    m_in_req_c->request_stop();

    // if there are outstanding RPC calls waiting for reply, they should be
    // unblocked (and fail), to avoid a deadlock
    s_mproc->unblock_rpc();


    if (!tl_is_req_thread) {
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
        tl_post_handler_function = [rep_ring]() {
            if (!rep_ring) {
                return;
            }
            rep_ring->done_reading();
            rep_ring->request_stop();
        };
    }
}



inline
bool Process_message_reader::stop_and_wait(double waiting_period)
{
    std::unique_lock<std::mutex> lk(m_stop_mutex);
    stop_nowait();

    auto no_readers = [&]() {
        const bool req_running = m_req_running.load(std::memory_order_acquire);
        const bool rep_running = m_rep_running.load(std::memory_order_acquire);
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
            std::fprintf(stderr,
                "Process_message_reader::stop_and_wait timeout: pid=%llu req_running=%d rep_running=%d\n",
                static_cast<unsigned long long>(m_process_instance_id),
                m_req_running.load(std::memory_order_acquire),
                m_rep_running.load(std::memory_order_acquire));
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
        progress->request.stopped.store(false, std::memory_order_release);
    }

    auto publish_request_delivery = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->request.delivered.exchange(
            seq, std::memory_order_acq_rel);
        if (previous != seq) {
            progress->notify_waiters();
        }
    };

    auto publish_request_processing = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->request.processed.exchange(
            seq, std::memory_order_acq_rel);
        if (previous != seq) {
            progress->notify_waiters();
        }
    };

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers++;
    s_mproc->m_num_active_readers_mutex.unlock();

    m_in_req_c->start_reading();
    {
        std::lock_guard<std::mutex> ready_guard(m_ready_mutex);
        m_req_running.store(true, std::memory_order_release);
    }
    m_ready_condition.notify_all();

    const auto initial_sequence = m_in_req_c->get_message_reading_sequence();
    publish_request_delivery(initial_sequence);
    publish_request_processing(initial_sequence);

    while (true) {
        const State reader_state = m_reader_state.load(std::memory_order_acquire);
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

        publish_request_delivery(m_in_req_c->get_message_reading_sequence());

        // Only the process with the coordinator's instance is allowed to send messages on
        // someone else's behalf (for relay purposes).
        // TODO: If some process not being part of the core set of processes sends nonsense,
        // it might be a good idea to kill it. If it is in the core set of processes,
        // then it would be a bug.
        assert(m_in_req_c->m_id == process_of(m->sender_instance_id) ||
               m_in_req_c->m_id == process_of(s_coord_id));

        assert(m->message_type_id != not_defined_type_id);

        if (is_local_instance(m->receiver_instance_id)) {
            // If the coordinator is local and this request targets a *service* instance
            // (e.g., Coordinator), relay it to the coordinator's ring and *skip* local
            // dispatch to avoid double-processing (local dispatch + relay).
            if (s_coord && !has_same_mapping(*m_in_req_c, *s_mproc->m_out_req_c) &&
                is_service_instance(m->receiver_instance_id))
            {
                s_mproc->m_out_req_c->relay(*m);
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

                // if the receiver  registered handler, call the handler
                auto it = Transceiver::get_rpc_handler_map().find(m->message_type_id);
                assert(it != Transceiver::get_rpc_handler_map().end()); // this would be a library error
                (*it->second)(*m); // call the handler
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
                if (!coordinator_reading_remote) {
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

        if (tl_post_handler_function) {
            auto post_handler = std::move(tl_post_handler_function);
            tl_post_handler_function = {};
            post_handler();
        }

        publish_request_processing(m_in_req_c->get_message_reading_sequence());
    }

    m_in_req_c->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    if (m_reader_state.load(std::memory_order_acquire) == READER_STOPPING) {
        std::fprintf(stderr, "request_reader_function(pid=%llu) exiting normally after stop.\n",
            static_cast<unsigned long long>(m_process_instance_id));
    }
    m_req_running.store(false, std::memory_order_release);
    m_ready_condition.notify_all();
    m_stop_condition.notify_one();

    if (progress) {
        const auto seq = m_in_req_c->get_message_reading_sequence();
        progress->request.delivered.store(seq, std::memory_order_release);
        progress->request.processed.store(seq, std::memory_order_release);
        progress->request.stopped.store(true, std::memory_order_release);
        progress->notify_waiters();
    }

    s_tl_current_request_reader = nullptr;
    tl_is_req_thread = false;

    if (tl_post_handler_function) {
        tl_post_handler_function = {};
    }
}



inline
Process_message_reader::Delivery_target Process_message_reader::prepare_fence_target(
    Delivery_stream stream,
    Fence_mode mode,
    sequence_counter_type target_sequence) const
{
    Delivery_target target;
    target.stream = stream;
    target.mode = mode;
    target.target = target_sequence;

    if (target_sequence == invalid_sequence) {
        return target;
    }

    if (stream == Delivery_stream::Request && s_tl_current_request_reader == this) {
        return target;
    }

    auto progress = m_delivery_progress;
    if (!progress) {
        return target;
    }

    target.progress = std::move(progress);

    const auto& stream_state = (stream == Delivery_stream::Request)
        ? target.progress->request
        : target.progress->reply;

    const auto observed = (mode == Fence_mode::Delivery)
        ? stream_state.delivered.load(std::memory_order_acquire)
        : stream_state.processed.load(std::memory_order_acquire);

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
        progress->reply.stopped.store(false, std::memory_order_release);
    }

    auto publish_reply_delivery = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->reply.delivered.exchange(
            seq, std::memory_order_acq_rel);
        if (previous != seq) {
            progress->notify_waiters();
        }
    };

    auto publish_reply_processing = [progress](sequence_counter_type seq) {
        if (!progress) {
            return;
        }
        const auto previous = progress->reply.processed.exchange(
            seq, std::memory_order_acq_rel);
        if (previous != seq) {
            progress->notify_waiters();
        }
    };

    m_in_rep_c->start_reading();
    {
        std::lock_guard<std::mutex> ready_guard(m_ready_mutex);
        m_rep_running.store(true, std::memory_order_release);
    }
    m_ready_condition.notify_all();

    const auto initial_sequence = m_in_rep_c->get_message_reading_sequence();
    publish_reply_delivery(initial_sequence);
    publish_reply_processing(initial_sequence);

    while (true) {
        const State reader_state = m_reader_state.load(std::memory_order_acquire);
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

        publish_reply_delivery(m_in_rep_c->get_message_reading_sequence());

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
                auto it = s_mproc->m_local_pointer_of_instance_id.find(m->receiver_instance_id);

                if (it != s_mproc->m_local_pointer_of_instance_id.end()) {
                    auto &return_handlers = it->second->m_active_return_handlers;

                    Transceiver::Return_handler handler_copy;
                    bool have_handler = false;
                    {
                        std::lock_guard<std::mutex> guard(it->second->m_return_handlers_mutex);
                        auto it2 = return_handlers.find(m->function_instance_id);
                        if (it2 != return_handlers.end()) {
                            handler_copy = it2->second;
                            have_handler = true;
                        }
                    }

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
                        // No active return handler — can happen if the caller already cleaned up
                        // (e.g., after cancellation/shutdown) and a late/duplicate message arrived.
                        // Drop it quietly unless we're fully RUNNING; in RUNNING emit a diagnostic
                        // but do not hard-assert to avoid modal dialogs on Windows Debug.
                        if (s_mproc && s_mproc->m_communication_state == Managed_process::COMMUNICATION_RUNNING) {
                            std::fprintf(stderr,
                                "Warning: Reply reader received message for function_instance_id=%llu but no active handler found (receiver_instance_id=%llu)\n",
                                static_cast<unsigned long long>(m->function_instance_id),
                                static_cast<unsigned long long>(m->receiver_instance_id));
                        }
                    }
                }
                else {
                    // The target object no longer exists locally. During shutdown or after
                    // coordinator loss, late replies can legitimately arrive after objects
                    // have been torn down. Do not hard-assert; drop unless we're fully RUNNING.
                    if (s_mproc && s_mproc->m_communication_state == Managed_process::COMMUNICATION_RUNNING) {
                        std::fprintf(stderr,
                            "Warning: Reply reader received message for receiver_instance_id=%llu but object no longer exists (sender=%llu, function=%llu)\n",
                            static_cast<unsigned long long>(m->receiver_instance_id),
                            static_cast<unsigned long long>(m->sender_instance_id),
                            static_cast<unsigned long long>(m->function_instance_id));
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

        publish_reply_processing(m_in_rep_c->get_message_reading_sequence());
    }

    m_in_rep_c->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    m_rep_running.store(false, std::memory_order_release);
    m_ready_condition.notify_all();
    m_stop_condition.notify_one();

    const auto final_sequence = m_in_rep_c->get_message_reading_sequence();
    publish_reply_delivery(final_sequence);
    publish_reply_processing(final_sequence);

    if (progress) {
        progress->reply.stopped.store(true, std::memory_order_release);
        progress->notify_waiters();
    }
}


} // namespace sintra

#endif
