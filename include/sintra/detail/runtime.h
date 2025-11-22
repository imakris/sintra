// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "debug_pause.h"
#include "globals.h"
#include "process/managed_process.h"
#include "utility.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sintra {
namespace detail {

inline void append_branch(
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    int multiplicity)
{
    if (multiplicity <= 0) {
        return;
    }

    branches.reserve(branches.size() + static_cast<std::size_t>(multiplicity));
    for (int i = 0; i < multiplicity; ++i) {
        branches.push_back(descriptor);
    }
}

inline void collect_branches(std::vector<Process_descriptor>&) {}

template <typename... Args>
void collect_branches(
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    append_branch(branches, descriptor, 1);
    collect_branches(branches, std::forward<Args>(rest)...);
}

template <typename... Args>
void collect_branches(
    std::vector<Process_descriptor>& branches,
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    append_branch(branches, descriptor, multiplicity);
    collect_branches(branches, std::forward<Args>(rest)...);
}

class Cleanup_guard {
public:
    explicit Cleanup_guard(std::function<void()> callback)
        : m_callback(std::move(callback))
    {}

    Cleanup_guard(const Cleanup_guard&) = delete;
    Cleanup_guard& operator=(const Cleanup_guard&) = delete;

    ~Cleanup_guard()
    {
        m_callback();
    }

private:
    std::function<void()> m_callback;
};

} // namespace detail

inline void disable_debug_pause_for_current_process()
{
    detail::set_debug_pause_active(false);
}

inline std::vector<Process_descriptor> make_branches()
{
    return {};
}

inline std::vector<Process_descriptor> make_branches(std::vector<Process_descriptor>& branches)
{
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches,
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    detail::collect_branches(branches, multiplicity, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    std::vector<Process_descriptor> branches;
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    std::vector<Process_descriptor> branches;
    detail::collect_branches(branches, multiplicity, descriptor, std::forward<Args>(rest)...);
    return branches;
}

inline bool finalize();

inline void init(
    int argc,
    const char* const* argv,
    std::vector<Process_descriptor> branches = std::vector<Process_descriptor>())
{
#ifndef _WIN32
    struct sigaction noaction;
    std::memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &noaction, nullptr);
    setsid();
#endif

    // Install debug pause handlers if enabled via SINTRA_DEBUG_PAUSE_ON_EXIT
    detail::install_debug_pause_handlers();

    static bool once = false;
    assert(!once); // init() may only be run once.
    once = true;

    static detail::Cleanup_guard cleanup_guard([]() {
        if (s_mproc) {
            finalize();
        }
    });

    s_mproc = new Managed_process;
    s_mproc->init(argc, argv);
    if (!branches.empty()) {
        s_mproc->branch(branches);
    }
    s_mproc->go();
}

template <typename... Args>
void init(int argc, const char* const* argv, Args&&... args)
{
#ifndef NDEBUG
    const auto cache_line_size = get_cache_line_size();
    if (assumed_cache_line_size != cache_line_size) {
        std::cerr
            << "WARNING: assumed_cache_line_size is set to " << assumed_cache_line_size
            << ", but on this system it is actually " << cache_line_size << "." << std::endl;
    }
#endif

    init(argc, argv, make_branches(std::forward<Args>(args)...));
}

inline bool join(const std::string& swarm_directory, const std::string& name)
{
    if (s_mproc) {
        return false;
    }

    s_mproc = new Managed_process;
    s_mproc->m_directory = swarm_directory;

    detail::Swarm_registry* registry_ptr = nullptr;
    instance_id_type my_id = invalid_instance_id;

    try {
        auto& registry = s_mproc->registry();
        registry_ptr = &registry;
        my_id = registry.allocate_process_id();
        std::fprintf(stderr, "[sintra::join] allocated process_iid=%llu\n",
            static_cast<unsigned long long>(my_id));

        s_mproc_id = my_id;
        s_coord_id = compose_instance(2u, 2ull);

        s_mproc->m_out_req_c = new Message_ring_W(swarm_directory, "req", my_id, s_recovery_occurrence);
        s_mproc->m_out_rep_c = new Message_ring_W(swarm_directory, "rep", my_id, s_recovery_occurrence);
        std::fprintf(stderr, "[sintra::join] rings created for process_iid=%llu\n",
            static_cast<unsigned long long>(my_id));

        auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
        auto coord_reader = std::make_shared<Process_message_reader>(my_id, progress, 0u, "req");
        {
            std::unique_lock<std::shared_mutex> readers_lock(s_mproc->m_readers_mutex);
            s_mproc->m_readers.emplace(my_id, coord_reader);
        }
        coord_reader->wait_until_ready();

        auto ack_promise = std::make_shared<std::promise<bool>>();
        auto ack_future = ack_promise->get_future();
        auto ack_delivered = std::make_shared<std::atomic<bool>>(false);
        auto ack_handler = [ack_promise, ack_delivered](const Coordinator::join_ack& ack) {
            bool expected = false;
            if (!ack_delivered->compare_exchange_strong(expected, true)) {
                return;
            }
            try {
                ack_promise->set_value(ack.success);
            }
            catch (const std::future_error&) {
            }
        };
        auto ack_deactivator = s_mproc->activate<Coordinator>(ack_handler, Typed_instance_id<Coordinator>(s_coord_id));

        registry.lock_lobby();
        detail::Cleanup_guard lobby_guard([&registry]() {
            registry.unlock_lobby();
        });

        Message_ring_W lobby_writer(swarm_directory, "lobby_req", LOBBY_INSTANCE_ID, 0u);
        auto* join_msg = lobby_writer.write<Coordinator::join_request>(vb_size<Coordinator::join_request>());
        join_msg->process_iid = my_id;
        join_msg->pid = static_cast<uint32_t>(detail::get_current_process_id());
        std::memset(join_msg->name, 0, sizeof(join_msg->name));
        std::strncpy(join_msg->name, name.c_str(), sizeof(join_msg->name) - 1);
        join_msg->sender_instance_id   = my_id;
        join_msg->receiver_instance_id = any_local_or_remote;
        lobby_writer.done_writing();
        std::fprintf(stderr, "[sintra::join] join request sent for process_iid=%llu\n",
            static_cast<unsigned long long>(my_id));

        const auto wait_status = ack_future.wait_for(std::chrono::seconds(5));
        if (wait_status != std::future_status::ready || !ack_future.get()) {
            std::fprintf(stderr, "[sintra::join] join ack timeout/failure for process_iid=%llu\n",
                static_cast<unsigned long long>(my_id));
            if (ack_deactivator) {
                ack_deactivator();
            }
            registry.free_process_id(my_id);
            delete s_mproc;
            s_mproc = nullptr;
            return false;
        }

        if (ack_deactivator) {
            ack_deactivator();
        }
        s_mproc->construct_with_id(my_id, "");
        if (!name.empty()) {
            s_mproc->assign_name(name);
        }
        s_mproc->m_communication_state = Managed_process::COMMUNICATION_RUNNING;
        std::fprintf(stderr, "[sintra::join] join ack received and process ready for process_iid=%llu\n",
            static_cast<unsigned long long>(my_id));
        return true;
    }
    catch (...) {
        if (registry_ptr && my_id != invalid_instance_id) {
            registry_ptr->free_process_id(my_id);
        }
        delete s_mproc;
        s_mproc = nullptr;
        return false;
    }
}

inline bool finalize()
{
    if (!s_mproc) {
        return false;
    }

    sequence_counter_type flush_seq = invalid_sequence;

    if (!s_coord) {
        try {
            auto& reg = s_mproc->registry();
            reg.lock_lobby();
            detail::Cleanup_guard lobby_guard([&reg]() { reg.unlock_lobby(); });

            Message_ring_W lobby_writer(s_mproc->m_directory, "lobby_req", LOBBY_INSTANCE_ID, 0u);
            auto* leave = lobby_writer.write<Coordinator::leave_request>(vb_size<Coordinator::leave_request>());
            leave->process_iid = s_mproc_id;
            leave->sender_instance_id   = s_mproc_id;
            leave->receiver_instance_id = any_local_or_remote;
            lobby_writer.done_writing();
            std::fprintf(stderr, "[sintra::finalize] leave request sent for process_iid=%llu\n",
                static_cast<unsigned long long>(s_mproc_id));
        }
        catch (...) {
        }
    }

    if (s_coord) {
        flush_seq = s_coord->begin_process_draining(s_mproc_id);
    }
    else {
        std::atomic<bool> done{false};
        std::thread watchdog([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!done.load()) {
                s_mproc->unblock_rpc(process_of(s_coord_id));
            }
        });

        try {
            flush_seq = Coordinator::rpc_begin_process_draining(s_coord_id, s_mproc_id);
        }
        catch (...) {
            flush_seq = invalid_sequence;
        }

        done = true;
        watchdog.join();
    }

    if (!s_coord && flush_seq != invalid_sequence) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    s_mproc->deactivate_all();
    s_mproc->unpublish_all_transceivers();

    s_mproc->pause();

    try {
        s_mproc->registry().free_process_id(s_mproc_id);
    }
    catch (...) {
    }

    delete s_mproc;
    s_mproc = nullptr;

    return true;
}

inline size_t spawn_swarm_process(
    const std::string& binary_name,
    std::vector<std::string> args = {},
    size_t multiplicity = 1)
{
    size_t spawned = 0;
    instance_id_type piid = invalid_instance_id;
    if (s_mproc) {
        try {
            piid = s_mproc->registry().allocate_process_id();
        }
        catch (...) {
        }
    }
    if (piid == invalid_instance_id) {
        piid = make_process_instance_id();
        if (s_mproc) {
            try {
                s_mproc->registry().reserve_process_id(piid);
            }
            catch (...) {
            }
        }
    }

    args.insert(args.end(), {
        "--swarm_id",       std::to_string(s_mproc->m_swarm_id),
        "--instance_id",    std::to_string(piid),
        "--coordinator_id", std::to_string(s_coord_id)
    });

    Managed_process::Spawn_swarm_process_args spawn_args;
    spawn_args.binary_name = binary_name;
    spawn_args.args = args;

    for (size_t i = 0; i < multiplicity; ++i) {
        spawn_args.piid = piid;
        auto result = s_mproc->spawn_swarm_process(spawn_args);
        if (result.success) {
            ++spawned;
        }
    }

    return spawned;
}

inline int process_index()
{
    return s_branch_index;
}

template <typename FT, typename SENDER_T>
auto activate_slot(const FT& slot_function, Typed_instance_id<SENDER_T> sender_id)
{
    return s_mproc->activate(slot_function, sender_id);
}

inline void deactivate_all_slots()
{
    s_mproc->deactivate_all();
}

inline void enable_recovery()
{
    s_mproc->enable_recovery();
}

} // namespace sintra
