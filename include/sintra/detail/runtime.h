// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "managed_process.h"
#include "utility.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
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
        std::fprintf(stderr, "Cleanup_guard destructor executing (after main exit)\n");
        m_callback();
        std::fprintf(stderr, "Cleanup_guard destructor completed successfully\n");
    }

private:
    std::function<void()> m_callback;
};

} // namespace detail

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

inline bool finalize()
{
    if (!s_mproc) {
        return false;
    }

    sequence_counter_type flush_seq = invalid_sequence;

    if (s_coord) {
        flush_seq = s_coord->begin_process_draining(s_mproc_id);
    }
    else {
        std::atomic<bool> done{false};
        std::thread watchdog([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!done.load(std::memory_order_acquire)) {
                s_mproc->unblock_rpc(process_of(s_coord_id));
            }
        });

        try {
            flush_seq = Coordinator::rpc_begin_process_draining(s_coord_id, s_mproc_id);
        }
        catch (...) {
            flush_seq = invalid_sequence;
        }

        done.store(true, std::memory_order_release);
        watchdog.join();
    }

    if (!s_coord && flush_seq != invalid_sequence) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    s_mproc->deactivate_all();
    s_mproc->unpublish_all_transceivers();

    s_mproc->pause();

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
    const auto piid = make_process_instance_id();

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
        if (s_mproc->spawn_swarm_process(spawn_args)) {
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

inline instance_id_type spawn_branch(int branch_index)
{
    if (!s_mproc) {
        return invalid_instance_id;
    }

    return s_mproc->spawn_branch(branch_index);
}

inline size_t spawn_registered_branch(int branch_index, size_t multiplicity)
{
    if (!s_mproc) {
        return 0;
    }

    return s_mproc->spawn_registered_branch(branch_index, multiplicity);
}

} // namespace sintra
