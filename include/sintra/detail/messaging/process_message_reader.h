// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../globals.h"
#include "../messaging/message.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
namespace sintra {

using std::atomic;
using std::condition_variable;
using std::mutex;
using std::thread;

namespace detail {
namespace test_hooks {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Process_reader_rpc_unblock_callback =
    void (*)(const char*, instance_id_type, uint32_t);
inline std::atomic<Process_reader_rpc_unblock_callback>
    s_process_reader_rpc_unblock{nullptr};
#endif

inline constexpr const char* k_process_reader_rpc_unblock_entered =
    "process_reader_rpc_unblock_entered";
inline constexpr const char* k_process_reader_rpc_unblock_claimed =
    "process_reader_rpc_unblock_claimed";
inline constexpr const char* k_process_reader_rpc_unblock_complete =
    "process_reader_rpc_unblock_complete";

} // namespace test_hooks

inline void process_reader_rpc_unblock_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_process_reader_rpc_unblock.load(
            std::memory_order_acquire))
    {
        callback(stage, process_instance_id, occurrence);
    }
#else
    (void)stage;
    (void)process_instance_id;
    (void)occurrence;
#endif
}

} // namespace detail


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



inline thread_local Message_prefix*  s_tl_current_message     = nullptr;
inline thread_local instance_id_type s_tl_common_function_iid = invalid_instance_id;

inline thread_local Process_message_reader* s_tl_current_request_reader = nullptr;

inline thread_local instance_id_type s_tl_additional_piids[max_process_index];
inline thread_local size_t s_tl_additional_piids_size = 0;
inline thread_local bool   s_tl_rpc_reply_deferred    = false;

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
        std::atomic<sequence_counter_type> request_sequence{invalid_sequence};
        std::atomic<sequence_counter_type> reply_sequence{invalid_sequence};
        std::atomic<bool>                  request_stopped{false};
        std::atomic<bool>                  reply_stopped{false};
    };

    using Delivery_progress_ptr      = std::shared_ptr<Delivery_progress>;
    using Delivery_progress_weak_ptr = std::weak_ptr<Delivery_progress>;

    enum class Delivery_stream
    {
        Request,
        Reply
    };

    struct Delivery_target
    {
        Delivery_progress_weak_ptr progress;
        instance_id_type           process_instance_id = invalid_instance_id;
        Delivery_stream            stream              = Delivery_stream::Request;
        sequence_counter_type      target              = invalid_sequence;
        bool                       wait_needed         = false;
    };

    inline
    Process_message_reader(instance_id_type process_instance_id,
        Delivery_progress_ptr delivery_progress,
        uint32_t occurrence = 0,
        uint64_t managed_child_custody_identity = 0);

    inline
    ~Process_message_reader();


    void pause() { m_reader_state = READER_SERVICE; }


    inline
    void stop_nowait();


    inline
    // `waiting_period` is the total stop budget in seconds across all phases.
    bool stop_and_wait(double waiting_period);

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    bool running_for_test() const
    {
        return m_req_running.load() || m_rep_running.load();
    }

    void set_running_for_test(bool request_running, bool reply_running)
    {
        m_req_running.store(request_running);
        m_rep_running.store(reply_running);
        m_stop_condition.notify_all();
    }

    void unblock_rpc_for_test()
    {
        unblock_rpc_once();
    }
#endif


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

    uint32_t get_occurrence() const
    {
        return m_occurrence;
    }

    uint64_t get_managed_child_custody_identity() const
    {
        return m_managed_child_custody_identity;
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

    Delivery_target prepare_delivery_target(
        Delivery_stream        stream,
        sequence_counter_type  target_sequence) const;

    Delivery_progress_ptr delivery_progress() const { return m_delivery_progress; }

    State state() const { return m_reader_state.load(); }

    static const char* reader_state_name(State state);
    static const char* delivery_stream_name(Delivery_stream stream);
    static const char* communication_state_name(int communication_state);

    const char* reader_condition_name() const;
    std::string diagnostic_summary() const;
    std::string delivery_target_summary(
        Delivery_stream        stream,
        sequence_counter_type  target_sequence) const;

    static std::string missing_reader_summary(instance_id_type target_process_id);

private:

    void unblock_rpc_once();

    void begin_reading_session(
        const std::shared_ptr<Message_ring_R>& ring,
        std::atomic<bool>&                     running_flag);

    void end_reading_session(
        const std::shared_ptr<Message_ring_R>& ring,
        std::atomic<bool>&                     running_flag);

    atomic<State>                       m_reader_state              = READER_NORMAL;

    instance_id_type                    m_process_instance_id;
    uint32_t                            m_occurrence                       = 0;
    uint64_t                            m_managed_child_custody_identity   = 0;

    std::shared_ptr<Message_ring_R>     m_in_req_c;
    std::shared_ptr<Message_ring_R>     m_in_rep_c;

    Delivery_progress_ptr               m_delivery_progress;

    std::unique_ptr<thread>             m_request_reader_thread;
    std::unique_ptr<thread>             m_reply_reader_thread;
    
    atomic<bool>                        m_req_running               = false;
    atomic<bool>                        m_rep_running               = false;
    std::once_flag                      m_rpc_unblock_once;
    mutex                               m_ready_mutex;
    condition_variable                  m_ready_condition;
    mutex                               m_stop_mutex;
    condition_variable                  m_stop_condition;

};


}


