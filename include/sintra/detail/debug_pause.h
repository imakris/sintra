// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sintra {
namespace detail {

// Debug pause functionality - only enabled when SINTRA_DEBUG_PAUSE_ON_EXIT is set
inline bool is_debug_pause_requested()
{
    const char* env = std::getenv("SINTRA_DEBUG_PAUSE_ON_EXIT");
    return env && *env && (*env != '0');
}

inline std::atomic<bool>& debug_pause_state()
{
    static std::atomic<bool> active{false};
    return active;
}

inline void set_debug_pause_active(bool active) { debug_pause_state() = active;      }
inline bool is_debug_pause_active()             { return debug_pause_state().load(); }

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
    if (!is_debug_pause_active()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

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
        case EXCEPTION_ACCESS_VIOLATION:      exception_name = "Access violation";        break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: exception_name = "Array bounds exceeded";   break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: exception_name = "Datatype misalignment";   break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    exception_name = "Float divide by zero";    break;
        case EXCEPTION_FLT_INVALID_OPERATION: exception_name = "Float invalid operation"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   exception_name = "Illegal instruction";     break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    exception_name = "Integer divide by zero";  break;
        case EXCEPTION_STACK_OVERFLOW:        exception_name = "Stack overflow";          break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }

    debug_pause_forever(exception_name);
    return EXCEPTION_CONTINUE_SEARCH; // Never reached due to infinite loop
}
#else
inline void debug_signal_handler(int signum)
{
    if (!is_debug_pause_active()) {
        std::signal(signum, SIG_DFL);
        std::raise(signum);
        return;
    }

    const char* signal_name = "Unknown signal";
    switch (signum) {
        case SIGABRT: signal_name = "SIGABRT (abort)";                   break;
        case SIGSEGV: signal_name = "SIGSEGV (segmentation fault)";      break;
        case SIGFPE:  signal_name = "SIGFPE (floating point exception)"; break;
        case SIGILL:  signal_name = "SIGILL (illegal instruction)";      break;
        case SIGBUS:  signal_name = "SIGBUS (bus error)";                break;
    }

    debug_pause_forever(signal_name);
}
#endif

inline void debug_signal_handler_win(int signum)
{
    if (!is_debug_pause_active()) {
        std::signal(signum, SIG_DFL);
        std::raise(signum);
        return;
    }

    const char* signal_name = "Unknown signal";
    switch (signum) {
        case SIGABRT: signal_name = "SIGABRT (abort)";                   break;
        case SIGFPE:  signal_name = "SIGFPE (floating point exception)"; break;
        case SIGILL:  signal_name = "SIGILL (illegal instruction)";      break;
        case SIGSEGV: signal_name = "SIGSEGV (segmentation fault)";      break;
    }

    debug_pause_forever(signal_name);
}

inline void install_debug_pause_handlers()
{
    const bool requested = is_debug_pause_requested();
    set_debug_pause_active(requested);

    if (!requested) {
        return;
    }

    std::fprintf(stderr, "[SINTRA_DEBUG_PAUSE] Handlers installed\n");
    std::fflush(stderr);

#ifdef _WIN32
    // AddVectoredExceptionHandler for exceptions (more reliable than SetUnhandledExceptionFilter)
    // First parameter: 1 = add as first handler in chain
    AddVectoredExceptionHandler(1, debug_vectored_exception_handler);

    // Also install signal handlers for abort() etc.
    std::signal(SIGABRT, debug_signal_handler_win);
    std::signal(SIGFPE,  debug_signal_handler_win);
    std::signal(SIGILL,  debug_signal_handler_win);
    std::signal(SIGSEGV, debug_signal_handler_win);
#else
    std::signal(SIGABRT, debug_signal_handler);
    std::signal(SIGSEGV, debug_signal_handler);
    std::signal(SIGFPE,  debug_signal_handler);
    std::signal(SIGILL,  debug_signal_handler);
    std::signal(SIGBUS,  debug_signal_handler);
#endif
}

} // namespace detail
} // namespace sintra
