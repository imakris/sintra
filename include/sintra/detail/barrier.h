// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "managed_process.h"
#include "process_message_reader_impl.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace sintra {
namespace detail {

class request_thread_delivery_fence_waiter
{
public:
    void wait(Managed_process& process, Process_message_reader* reader)
    {
        if (!reader) {
            process.wait_for_delivery_fence();
            return;
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_worker.joinable()) {
            m_worker = std::thread([this]() { worker_loop(); });
        }

        m_cv.wait(lock, [this]() { return m_state == State::Idle; });

        m_process = &process;
        m_reader = reader;
        m_error = nullptr;
        m_state = State::Running;
        m_cv.notify_one();

        auto pump_post_handlers = []() {
            if (tl_post_handler_function) {
                auto post_handler = std::move(tl_post_handler_function);
                tl_post_handler_function = {};
                post_handler();
                return;
            }

            std::this_thread::yield();
        };

        while (m_state == State::Running) {
            lock.unlock();
            pump_post_handlers();
            lock.lock();

            if (m_state == State::Running) {
                m_cv.wait_for(lock, std::chrono::milliseconds(1));
            }
        }

        auto error = m_error;
        m_error = nullptr;
        m_state = State::Idle;
        lock.unlock();
        m_cv.notify_all();

        if (error) {
            std::rethrow_exception(error);
        }
    }

    ~request_thread_delivery_fence_waiter()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }

        m_cv.notify_all();

        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

private:
    enum class State {
        Idle,
        Running,
        Completed
    };

    void worker_loop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        while (true) {
            m_cv.wait(lock, [this]() {
                return m_state == State::Running || m_stop;
            });

            if (m_stop) {
                return;
            }

            auto* process = m_process;
            auto* reader = m_reader;

            lock.unlock();

            auto* previous_reader = s_tl_current_request_reader;
            const bool previous_is_request_thread = tl_is_req_thread;

            s_tl_current_request_reader = reader;
            tl_is_req_thread = true;

            std::exception_ptr local_error;

            try {
                process->wait_for_delivery_fence();
            }
            catch (...) {
                local_error = std::current_exception();
            }

            tl_is_req_thread = previous_is_request_thread;
            s_tl_current_request_reader = previous_reader;

            lock.lock();

            m_error = std::move(local_error);
            m_state = State::Completed;
            m_cv.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    Managed_process* m_process = nullptr;
    Process_message_reader* m_reader = nullptr;
    std::exception_ptr m_error;
    bool m_stop = false;
    State m_state = State::Idle;
};

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

    auto* current_reader = s_tl_current_request_reader;
    if (!current_reader) {
        s_mproc->wait_for_delivery_fence();
        return;
    }

    thread_local request_thread_delivery_fence_waiter waiter;
    waiter.wait(*s_mproc, current_reader);
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
