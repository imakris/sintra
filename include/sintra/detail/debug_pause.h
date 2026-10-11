// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "logging.h"
#include "process/process_id.h"
#include "sintra_windows.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef SINTRA_DEBUG_PAUSE_ON_EXIT
#define SINTRA_DEBUG_PAUSE_ON_EXIT 0
#endif

namespace sintra {
namespace detail {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
using Debug_pause_callback = void (*)(const char*) noexcept;
inline std::atomic<Debug_pause_callback> s_debug_pause_entered{nullptr};
#if !defined(_WIN32)
inline std::atomic<int> s_native_backoff_error{0};
inline std::atomic<unsigned> s_native_backoff_attempts{0};
inline std::atomic<unsigned> s_native_backoff_fallbacks{0};
#endif
}
#endif

// Debug pause functionality - only enabled when SINTRA_DEBUG_PAUSE_ON_EXIT is non-zero.
inline bool is_debug_pause_requested()
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const bool requested = (SINTRA_DEBUG_PAUSE_ON_EXIT != 0);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return requested;
}

inline std::atomic<bool> s_debug_pause_active{false};
static_assert(std::atomic<bool>::is_always_lock_free,
    "installed fatal handlers require a lock-free debug flag");

inline std::atomic<bool>& debug_pause_state() { return s_debug_pause_active; }

inline void set_debug_pause_active(bool active) { debug_pause_state() = active;      }
inline bool is_debug_pause_active()             { return debug_pause_state().load(); }

// These paths also run under shared inspection locks and in fatal handlers.
// Keep their formatting and output independent of the application's logger.
inline void native_diagnostic(
    const char* prefix, uint64_t value = 0, const char* suffix = "\n", bool number = true) noexcept
{
    char buffer[1024];
    size_t length = 0;
    const auto append = [&](const char* text) {
        while (*text && length < sizeof(buffer)) {
            buffer[length++] = *text++;
        }
    };
    append(prefix);
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (number && count != 0 && length < sizeof(buffer)) {
        buffer[length++] = digits[--count];
    }
    append(suffix);
    size_t written = 0;
    while (written < length) {
#if defined(_WIN32)
        DWORD bytes = 0;
        if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), buffer + written,
                DWORD(length - written), &bytes, nullptr) || bytes == 0)
        {
            break;
        }
#else
        const auto bytes = ::write(STDERR_FILENO, buffer + written, length - written);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            break;
        }
#endif
        written += size_t(bytes);
    }
}

inline void native_error_backoff() noexcept
{
#if defined(_WIN32)
    Sleep(1);
#else
    timespec interval{0, 1'000'000};
    int reported_error = 0;
    int reported_fallback_error = 0;
    for (;;) {
        timespec remaining{};
        int result;
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        ++test_hooks::s_native_backoff_attempts;
        const int injected = test_hooks::s_native_backoff_error.exchange(0);
        if (injected) { errno = injected; remaining = interval; result = -1; }
        else
#endif
        { result = ::nanosleep(&interval, &remaining); }
        if (result == 0) { return; }
        const int error = errno;
        if (error == EINTR) { interval = remaining; continue; }
        if (error != reported_error) {
            native_diagnostic("[sintra] native cleanup backoff error ", uint64_t(error),
                "; retaining cleanup duty and native parking.\n");
            reported_error = error;
        }
        // Use a descriptor-free native delay after an unexpected failure.
        // Unlike sleep, poll does not have unspecified SIGALRM interactions.
        // Check interruptions and errors before retrying the original duty.
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        ++test_hooks::s_native_backoff_fallbacks;
#endif
        pollfd unused{};
        for (;;) {
            if (::poll(&unused, 0, 1) == 0) { return; }
            const int fallback_error = errno;
            if (fallback_error == EINTR) { continue; }
            if (fallback_error != reported_fallback_error) {
                native_diagnostic("[sintra] native cleanup fallback error ",
                    uint64_t(fallback_error), "; retrying native parking with cleanup duty retained.\n");
                reported_fallback_error = fallback_error;
            }
            break;
        }
        interval = {0, 1'000'000};
    }
#endif
}

[[noreturn]] inline void native_debug_pause_forever(const char* reason) noexcept
{
    native_diagnostic("\n[SINTRA_DEBUG_PAUSE] Process ",
        uint64_t(get_current_process_id()), " paused: ");
    native_diagnostic(reason, 0, "\n", false);
    native_diagnostic("[SINTRA_DEBUG_PAUSE] Attach debugger to PID ",
        uint64_t(get_current_process_id()), " to capture stacks\n");
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    // Fixtures installing this terminal observer must obey the same native-only
    // contract as the handler; ordinary application callbacks are never used.
    if (auto callback = test_hooks::s_debug_pause_entered.load(std::memory_order_acquire)) {
        callback(reason);
    }
#endif
    for (;;) {
#if defined(_WIN32)
        Sleep(3'600'000);
#else
        const timespec interval{3600, 0};
        ::nanosleep(&interval, nullptr);
#endif
    }
}

[[noreturn]] inline void native_debug_aware_abort() noexcept
{
    if (is_debug_pause_active()) {
        native_debug_pause_forever("abort");
    }
    std::abort();
}

// LCOV_EXCL_START - active when SINTRA_DEBUG_PAUSE_ON_EXIT evaluates nonzero
inline void debug_pause_forever(const char* reason)
{
    const auto pid = static_cast<unsigned long long>(get_current_process_id());

    Log_stream(log_level::info)
        << "\n[SINTRA_DEBUG_PAUSE] Process " << pid << " paused: " << reason
        << "\n";
    Log_stream(log_level::info)
        << "[SINTRA_DEBUG_PAUSE] Attach debugger to PID " << pid
        << " to capture stacks\n";

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_debug_pause_entered.load(std::memory_order_acquire)) {
        callback(reason);
    }
#endif

    // Infinite loop to keep process alive for debugger attachment
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
}

// Debug-aware abort that respects SINTRA_DEBUG_PAUSE_ON_EXIT.
// Use this instead of std::abort() in tests to ensure the test harness
// can attach debuggers and capture stack traces before process termination.
//
// Background: On Windows with MinGW, the managed_process signal handler
// intercepts SIGABRT and calls TerminateProcess() immediately, which prevents
// both the debug_pause signal handler and the test harness from capturing
// crash information. By calling debug_pause_forever() BEFORE abort(), we
// ensure the process pauses for debugging when SINTRA_DEBUG_PAUSE_ON_EXIT != 0.
[[noreturn]] inline void debug_aware_abort()
{
    if (is_debug_pause_active()) {
        debug_pause_forever("abort");
    }
    std::abort();
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
        default:                              return EXCEPTION_CONTINUE_SEARCH;
    }

    native_debug_pause_forever(exception_name);
    return EXCEPTION_CONTINUE_SEARCH; // Never reached due to infinite loop
}
#endif

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
#ifdef SIGBUS
        case SIGBUS:  signal_name = "SIGBUS (bus error)";                break;
#endif
    }

    native_debug_pause_forever(signal_name);
}
// LCOV_EXCL_STOP

inline void install_debug_pause_handlers()
{
    const bool requested = is_debug_pause_requested();
    set_debug_pause_active(requested);

    if (!requested) {
        return;
    }

    // LCOV_EXCL_START - only reached when SINTRA_DEBUG_PAUSE_ON_EXIT evaluates nonzero
    Log_stream(log_level::info) << "[SINTRA_DEBUG_PAUSE] Handlers installed\n";

#ifdef _WIN32
    // Add a first-chance vectored handler for debug-pause exceptions.
    // First parameter: 1 = add as first handler in chain
    AddVectoredExceptionHandler(1, debug_vectored_exception_handler);
#endif

    std::signal(SIGABRT, debug_signal_handler);
    std::signal(SIGSEGV, debug_signal_handler);
    std::signal(SIGFPE,  debug_signal_handler);
    std::signal(SIGILL,  debug_signal_handler);
#ifdef SIGBUS
    std::signal(SIGBUS,  debug_signal_handler);
#endif
}
// LCOV_EXCL_STOP

} // namespace detail
} // namespace sintra
