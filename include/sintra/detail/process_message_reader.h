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

#ifndef SINTRA_PROCESS_MESSAGE_READER_H
#define SINTRA_PROCESS_MESSAGE_READER_H


#include "globals.h"
#include "message.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>


namespace sintra {


using std::atomic;
using std::atomic_flag;
using std::string;
using std::mutex;
using std::condition_variable;
using std::thread;

struct Outstanding_rpc_control;
struct Process_message_reader;

// Note: this should be a specialization of Message_reader (which does not exist), but for the sake
// of simplicity and code coverage, the Message_reader was not implemented.
// The idea is that a Message_reader would support the exchange of messages between individual
// transceivers, whereas the Process_message_reader routes the messages of resident transceivers
// to other processes, in addition to its own.
// It seems however that there is no substantial benefit in implementing point to point transceiver
// messaging with the use of this mechanism.
// - If faster communication is sought, then setting up a ring between the two transceivers is
//   probably the best solution (Ring_W/Ring_R). This does not support message handlers by design,
//   just data exchange.
// - Messages with handlers are by definition slower than plain data rings, but they are meant to be
//   used as part of the program's logic in less-time critical scopes, certainly not for large
//   data transfers. The mechanism however works completely out of the box - does not require any
//   setup or configuration.



static inline thread_local Message_prefix* s_tl_current_message = nullptr;
static inline thread_local instance_id_type s_tl_common_function_iid = invalid_instance_id;

static inline thread_local Process_message_reader* s_tl_current_request_reader = nullptr;

static inline thread_local instance_id_type s_tl_additional_piids[max_process_index];
static inline thread_local size_t s_tl_additional_piids_size = 0;
static inline thread_local bool s_tl_rpc_reply_deferred = false;

inline void clear_rpc_reply_deferred()
{
    s_tl_rpc_reply_deferred = false;
}

inline void mark_rpc_reply_deferred()
{
    s_tl_rpc_reply_deferred = true;
}

inline bool rpc_reply_is_deferred()
{
    return s_tl_rpc_reply_deferred;
}

// This exists because it may occur that there are multiple outstanding RPC calls
// from different threads.
inline mutex& s_outstanding_rpcs_mutex()
{
    static mutex& m = *new mutex();
    return m;
}

inline std::set<Outstanding_rpc_control*>& s_outstanding_rpcs()
{
    static auto& set_ref = *new std::set<Outstanding_rpc_control*>();
    return set_ref;
}


struct Process_message_reader
{
    enum State
    {
        READER_NORMAL,      // full functionality
        READER_SERVICE,     // only basic functionality
        READER_STOPPING
    };

    struct Delivery_progress
    {
        struct Stream_state
        {
            std::atomic<sequence_counter_type> delivered{invalid_sequence};
            std::atomic<sequence_counter_type> processed{invalid_sequence};
            std::atomic<bool> stopped{false};
        };

        Stream_state request;
        Stream_state reply;

        std::atomic<uint32_t> waiter_count{0};
        std::mutex             wait_mutex;
        std::condition_variable wait_condition;

        void notify_waiters()
        {
            if (waiter_count.load(std::memory_order_acquire) > 0) {
                wait_condition.notify_all();
            }
        }
    };

    using Delivery_progress_ptr = std::shared_ptr<Delivery_progress>;

    enum class Delivery_stream
    {
        Request,
        Reply
    };

    enum class Fence_mode
    {
        Delivery,
        Processing
    };

    struct Delivery_target
    {
        Delivery_progress_ptr progress;
        Delivery_stream stream = Delivery_stream::Request;
        Fence_mode mode = Fence_mode::Delivery;
        sequence_counter_type target = invalid_sequence;
        bool wait_needed = false;
    };

    inline
    Process_message_reader(instance_id_type process_instance_id,
        Delivery_progress_ptr delivery_progress,
        uint32_t occurrence = 0);

    inline
    ~Process_message_reader();


    void pause() { m_reader_state.store(READER_SERVICE, std::memory_order_release); }


    inline
    void stop_nowait();


    inline
    bool stop_and_wait(double waiting_period);  // waiting period in seconds


    // This implementation of the following functions assumes the following:
    // We have two types of messages: rpc messages and events
    // - RPC messages are only addressed to specific receivers. All receivers able to handle this
    //   type of message use the same handler, thus the handler map is static.
    // - Events are only addressed "to whom it may concern" (i.e. any_*).
    //   Their handlers are registered at process level, but they are assigned dynamically, thus
    //   they may differ across different instances of the same type of receiver type.


    inline
    void request_reader_function();


    inline
    void reply_reader_function();


    // this is only meant to be called when the reader is started, to assure that
    // no messages are sent and lost before the thread is ready to process them
    inline
    void wait_until_ready();


    instance_id_type get_process_instance_id() const
    {
        return m_process_instance_id;
    }

    sequence_counter_type get_request_reading_sequence() const
    {
        return m_in_req_c->get_message_reading_sequence();
    }

    sequence_counter_type get_request_leading_sequence() const
    {
        return m_in_req_c->get_leading_sequence();
    }

    sequence_counter_type get_reply_leading_sequence() const
    {
        return m_in_rep_c->get_leading_sequence();
    }

    sequence_counter_type get_reply_reading_sequence() const
    {
        return m_in_rep_c->get_message_reading_sequence();
    }

    Delivery_target prepare_fence_target(
        Delivery_stream stream,
        Fence_mode mode,
        sequence_counter_type target_sequence) const;

    Delivery_progress_ptr delivery_progress() const { return m_delivery_progress; }

    State state() const { return m_reader_state.load(std::memory_order_acquire); }

private:

    atomic<State>           m_reader_state          = READER_NORMAL;

    instance_id_type        m_process_instance_id;

    std::shared_ptr<Message_ring_R> m_in_req_c;
    std::shared_ptr<Message_ring_R> m_in_rep_c;

    Delivery_progress_ptr            m_delivery_progress;

    thread*                 m_request_reader_thread = nullptr;
    thread*                 m_reply_reader_thread   = nullptr;
    
    atomic<bool>            m_req_running           = false;
    atomic<bool>            m_rep_running           = false;
    mutex                   m_ready_mutex;
    condition_variable      m_ready_condition;
    mutex                   m_stop_mutex;
    condition_variable      m_stop_condition;

};


}


#endif
