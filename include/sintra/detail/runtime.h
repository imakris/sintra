// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "globals.h"
#include "process/managed_process.h"
#include "utility.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
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

inline bool& init_once_flag()
{
    static bool once = false;
    return once;
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

// Debug pause functionality - only enabled when SINTRA_DEBUG_PAUSE_ON_EXIT is set
inline bool is_debug_pause_enabled()
{
    const char* env = std::getenv("SINTRA_DEBUG_PAUSE_ON_EXIT");
    return env && *env && (*env != '0');
}

inline void debug_pause_forever(const char* reason)
{
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif

    std::fprintf(stderr, "\n[SINTRA_DEBUG_PAUSE] Process %d paused: %s\n", pid, reason);
    std::fprintf(stderr, "[SINTRA_DEBUG_PAUSE] Attach debugger to PID %d to capture stacks\n", pid);
    std::fflush(stderr);

    // Infinite loop to keep process alive for debugger attachment
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}

#ifdef _WIN32
inline LONG WINAPI debug_vectored_exception_handler(EXCEPTION_POINTERS* exception_info)
{
    if (!exception_info || !exception_info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only handle actual crashes, not debugging events
    DWORD code = exception_info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const char* exception_name = "Unknown exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            exception_name = "Access violation";
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            exception_name = "Array bounds exceeded";
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            exception_name = "Datatype misalignment";
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            exception_name = "Float divide by zero";
            break;
        case EXCEPTION_FLT_INVALID_OPERATION:
            exception_name = "Float invalid operation";
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            exception_name = "Illegal instruction";
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            exception_name = "Integer divide by zero";
            break;
        case EXCEPTION_STACK_OVERFLOW:
            exception_name = "Stack overflow";
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    debug_pause_forever(exception_name);
    return EXCEPTION_CONTINUE_SEARCH; // Never reached due to infinite loop
}
#else
inline void debug_signal_handler(int signum)
{
    const char* signal_name = "Unknown signal";
    switch (signum) {
        case SIGABRT:
            signal_name = "SIGABRT (abort)";
            break;
        case SIGSEGV:
            signal_name = "SIGSEGV (segmentation fault)";
            break;
        case SIGFPE:
            signal_name = "SIGFPE (floating point exception)";
            break;
        case SIGILL:
            signal_name = "SIGILL (illegal instruction)";
            break;
        case SIGBUS:
            signal_name = "SIGBUS (bus error)";
            break;
        case SIGTERM:
            signal_name = "SIGTERM (termination)";
            break;
        default:
            break;
    }

    debug_pause_forever(signal_name);
}
#endif

inline void debug_signal_handler_win(int signum)
{
    const char* signal_name = "Unknown signal";
    switch (signum) {
        case SIGABRT:
            signal_name = "SIGABRT (abort)";
            break;
        case SIGFPE:
            signal_name = "SIGFPE (floating point exception)";
            break;
        case SIGILL:
            signal_name = "SIGILL (illegal instruction)";
            break;
        case SIGSEGV:
            signal_name = "SIGSEGV (segmentation fault)";
            break;
        case SIGTERM:
            signal_name = "SIGTERM (termination)";
            break;
        default:
            break;
    }

    debug_pause_forever(signal_name);
}

inline void install_debug_pause_handlers()
{
    if (!is_debug_pause_enabled()) {
        return;
    }

    std::fprintf(stderr, "[SINTRA_DEBUG_PAUSE] Handlers installed\n");
    std::fflush(stderr);

#ifdef _WIN32
    // AddVectoredExceptionHandler for exceptions (more reliable than SetUnhandledExceptionFilter)
    // First parameter: 1 = add as first handler in chain
    AddVectoredExceptionHandler(1, debug_vectored_exception_handler);

    // signal() for abort(), etc.
    std::signal(SIGABRT, debug_signal_handler_win);
    std::signal(SIGFPE, debug_signal_handler_win);
    std::signal(SIGILL, debug_signal_handler_win);
    std::signal(SIGSEGV, debug_signal_handler_win);
    std::signal(SIGTERM, debug_signal_handler_win);

    std::fprintf(stderr, "[SINTRA_DEBUG_PAUSE] Windows handlers configured (VEH)\n");
    std::fflush(stderr);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = debug_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

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

    // Install debug pause handlers if enabled via SINTRA_DEBUG_PAUSE_ON_EXIT
    detail::install_debug_pause_handlers();

    bool& once = detail::init_once_flag();
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

    detail::init_once_flag() = false;

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

} // namespace sintra
