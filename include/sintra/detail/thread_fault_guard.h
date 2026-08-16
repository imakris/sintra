// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "sintra_windows.h"

#include <atomic>
#include <type_traits>

#if defined(_WIN32)
  // Only the watchdog needs these, and this header reaches nearly every
  // translation unit through logging.h.
  #include <mutex>
  #include <thread>
#endif

// Windows raises a C signal for a hardware fault only through the UCRT's
// __scrt_common_main_seh filter, which exists on the main thread alone. A fault
// on any other thread - including the ring reader threads, where application
// message handlers run - therefore never reaches the CRT signal path, and the
// abnormal-termination dispatch that tells the coordinator a peer has died
// never runs. Peers then remain published, and barriers keep waiting for a
// participant that no longer exists.
//
// This guard closes that gap by placing a structured-exception frame on the
// threads sintra owns. POSIX needs none of it: a hardware fault there is
// delivered as a signal on the faulting thread and the process-wide handler
// runs, whichever thread it was.
//
// SINTRA_HAS_SEH selects whether that frame can be built at all.
//
// __try/__except is a Microsoft language extension:
//   - MSVC and clang-cl support it, and both define _MSC_VER.
//   - GCC does not support it in any mode. MinGW GCC 13.1 rejects `__try` with
//     "'__try' was not declared in this scope", with and without
//     -fms-extensions, so those builds compile the passthrough below.
//   - clang targeting *-w64-windows-gnu does support it under -fms-extensions,
//     but advertises no macro that distinguishes that mode (it defines neither
//     _MSC_VER nor _MSC_EXTENSIONS), so such builds must opt in explicitly:
//         -DSINTRA_HAS_SEH=1 -fms-extensions
//
// Where the frame is unavailable, sintra behaves exactly as it did before: the
// host's unhandled-exception filter still sees the fault, and a host that wants
// the coordinator informed calls announce_fatal_windows_exception from it.
#if !defined(SINTRA_HAS_SEH)
  #if defined(_WIN32) && defined(_MSC_VER)
    #define SINTRA_HAS_SEH 1
  #else
    #define SINTRA_HAS_SEH 0
  #endif
#endif

// Normalise before use, so that -DSINTRA_HAS_SEH with no value degrades to 0
// instead of turning every test below into a preprocessor syntax error, and so
// that a typo is rejected rather than silently treated as "on".
#if (SINTRA_HAS_SEH + 0) == 0
  #undef SINTRA_HAS_SEH
  #define SINTRA_HAS_SEH 0
#elif (SINTRA_HAS_SEH + 0) == 1
  #undef SINTRA_HAS_SEH
  #define SINTRA_HAS_SEH 1
#else
  #error "SINTRA_HAS_SEH must be defined as 0 or 1"
#endif

#if SINTRA_HAS_SEH && !defined(_WIN32)
  #error "SINTRA_HAS_SEH=1 requires a Windows target"
#endif

// Fail with a sentence rather than with "'__try' was not declared in this
// scope" a hundred lines further down.
#if SINTRA_HAS_SEH && defined(__GNUC__) && !defined(__clang__)
  #error "SINTRA_HAS_SEH=1 requires MSVC, clang-cl, or clang with -fms-extensions; GCC does not implement __try/__except"
#endif

namespace sintra {
namespace detail {

#if defined(_WIN32)

// --- primitives usable from a faulted thread -------------------------------
//
// No heap, no stdio locks, no formatting machinery: these run after a fault,
// sometimes with only a few dozen bytes of stack left.

inline void write_stderr_raw(const char* text) noexcept
{
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    // A pipe can accept less than asked for; the whole line or nothing is the
    // difference between a usable diagnostic and a truncated one.
    DWORD offset = 0;
    while (offset < length) {
        DWORD written = 0;
        if (!WriteFile(handle, text + offset, length - offset, &written, nullptr) ||
            written == 0)
        {
            return;
        }
        offset += written;
    }
}

// Renders "0x" followed by eight hex digits into ten bytes of caller storage.
inline void format_hex32(unsigned long value, char* out10) noexcept
{
    static const char digits[] = "0123456789ABCDEF";
    out10[0] = '0';
    out10[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        out10[2 + i] = digits[(value >> ((7 - i) * 4)) & 0xFu];
    }
}

// --- crash watchdog --------------------------------------------------------
//
// Both fault paths hand control to the host's unhandled-exception filter, which
// is arbitrary application code: a crash reporter writing a minidump to a dead
// network share, or one that waits on a lock the faulted thread was holding. A
// faulted peer must never be allowed to hang there, so the call is bounded.
//
// Thread and events are created while the process is still healthy, so arming
// from a fault path is a bare SetEvent: no allocation, and no dependency on a
// heap the fault may have corrupted.
//
// Both events are auto-reset. Arming is scoped to one host call and is released
// again when the host turns out to have repaired the fault, so a recovered
// process is never killed by a watchdog left over from its recovery.
//
// The grace period bounds how long a host crash reporter may take. A minidump
// of a large multi-process address space can take a while, so it is overridable.
#if !defined(SINTRA_CRASH_WATCHDOG_GRACE_MS)
  #define SINTRA_CRASH_WATCHDOG_GRACE_MS 5000
#endif
constexpr DWORD k_crash_watchdog_grace_ms = SINTRA_CRASH_WATCHDOG_GRACE_MS;

inline HANDLE& crash_watchdog_armed_event()
{
    static HANDLE evt = NULL;
    return evt;
}

inline HANDLE& crash_watchdog_release_event()
{
    static HANDLE evt = NULL;
    return evt;
}

inline std::atomic<unsigned>& crash_watchdog_exit_status()
{
    static std::atomic<unsigned> status{0xC0000005u};
    return status;
}

// Set only once the watchdog thread is actually running. Creating the events is
// not enough: an armed event with no waiter is not a backstop.
inline std::atomic<bool>& crash_watchdog_ready()
{
    static std::atomic<bool> ready{false};
    return ready;
}

inline void crash_watchdog_loop()
{
    for (;;) {
        if (WaitForSingleObject(crash_watchdog_armed_event(), INFINITE)
                != WAIT_OBJECT_0)
        {
            return;
        }
        if (WaitForSingleObject(crash_watchdog_release_event(),
                                k_crash_watchdog_grace_ms) == WAIT_OBJECT_0)
        {
            // The fault was repaired and the process is healthy again.
            continue;
        }
        write_stderr_raw(
            "[sintra] crash path did not complete - forcing termination\n");
        TerminateProcess(
            GetCurrentProcess(),
            crash_watchdog_exit_status().load(std::memory_order_acquire));
        return;
    }
}

inline void ensure_crash_watchdog() noexcept
{
    static std::once_flag once;
    std::call_once(once, [] {
        crash_watchdog_armed_event()   = CreateEventW(NULL, FALSE, FALSE, NULL);
        crash_watchdog_release_event() = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (crash_watchdog_armed_event() == NULL ||
            crash_watchdog_release_event() == NULL)
        {
            return;
        }
        try {
            std::thread(crash_watchdog_loop).detach();
            crash_watchdog_ready().store(true, std::memory_order_release);
        }
        catch (...) {
            // No watchdog. Callers see that through arm_crash_watchdog and can
            // choose not to make an unbounded host call. Failing to create one
            // thread must not fail runtime initialisation.
        }
    });
}

// Bounds the host call that follows. Returns false when no watchdog is running,
// so callers can decide whether an unbounded host call is acceptable.
inline bool arm_crash_watchdog(unsigned exit_status) noexcept
{
    if (!crash_watchdog_ready().load(std::memory_order_acquire)) {
        return false;
    }
    crash_watchdog_exit_status().store(exit_status, std::memory_order_release);
    return SetEvent(crash_watchdog_armed_event()) != 0;
}

inline void release_crash_watchdog() noexcept
{
    if (crash_watchdog_ready().load(std::memory_order_acquire)) {
        SetEvent(crash_watchdog_release_event());
    }
}

// Installed by the runtime once it can report a death. Called only after the
// host has been consulted and the fault established as terminal, and never
// returns.
using Thread_fault_action = void (*)(unsigned long exception_code);

inline std::atomic<Thread_fault_action>& thread_fault_action()
{
    static std::atomic<Thread_fault_action> action{nullptr};
    return action;
}

// Only codes that mean the faulting thread cannot continue. Everything an
// application may legitimately raise and handle is excluded, in particular C++
// exceptions (0xE06D7363), breakpoints and single steps, and the MSVC
// thread-naming exception (0x406D1388).
inline bool is_terminal_hardware_exception(DWORD code) noexcept
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
            return true;
        default:
            return false;
    }
}

// The status the watchdog uses if it has to end the process itself. Only a
// backstop: on every normal path the exception code is used directly.
inline unsigned consult_exit_status(DWORD code) noexcept
{
    return code != 0 ? static_cast<unsigned>(code) : 0xC0000005u;
}

// Asks the host's unhandled-exception filter what should happen, bounded by the
// watchdog and serialised across threads. Used by both fault paths - the CRT
// signal handler and the per-thread guard - so that they cannot disagree about
// who decides first.
//
// Serialisation matters because two threads can fault at once, and most crash
// reporters assume a single entry. The loser waits rather than parking forever:
// the winner normally terminates the process, but if it repaired the fault the
// loser must still get its turn.
inline std::atomic<bool>& host_consult_in_progress()
{
    static std::atomic<bool> busy{false};
    return busy;
}

inline LONG consult_host_exception_filter(
    EXCEPTION_POINTERS* info,
    unsigned            watchdog_status) noexcept
{
    const bool bounded = arm_crash_watchdog(watchdog_status);

    while (host_consult_in_progress().exchange(true, std::memory_order_acquire)) {
        Sleep(1);
    }
    const LONG decision = UnhandledExceptionFilter(info);
    host_consult_in_progress().store(false, std::memory_order_release);

    if (decision != EXCEPTION_EXECUTE_HANDLER && bounded) {
        // Not a death after all: stand the watchdog down so it cannot kill a
        // process the host just repaired.
        release_crash_watchdog();
    }
    return decision;
}

inline LONG thread_fault_filter(EXCEPTION_POINTERS* info) noexcept
{
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!is_terminal_hardware_exception(code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const Thread_fault_action action =
        thread_fault_action().load(std::memory_order_acquire);
    if (!action) {
        // The runtime has not installed its reporter yet. There is nothing to
        // report, and nothing to gain by taking the fault from the host.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // A stack overflow is the one case where the host is deliberately not
    // consulted. What remains after the guard page is gone - measured at about
    // 6.9 KB - is enough for the dispatch and a WriteFile, but not for an
    // arbitrary crash reporter writing a minidump. A fault inside the host's
    // filter would lose the notification entirely, and the notification is the
    // thing the swarm cannot do without. So report first and accept the loss of
    // the host's dump for this fault class.
    if (code == EXCEPTION_STACK_OVERFLOW) {
        action(static_cast<unsigned long>(code));
        return EXCEPTION_EXECUTE_HANDLER;               // not reached
    }

    // The host owns the decision about whether execution continues. Sintra must
    // not pre-empt it, and must not declare a peer dead over a fault the host
    // repairs: broadcasting terminated_abnormally for a recovered fault, and
    // stopping every reader, is the rule stated by
    // handled_exception_survival_contract_test.
    const LONG host_decision =
        consult_host_exception_filter(info, consult_exit_status(code));
    if (host_decision != EXCEPTION_EXECUTE_HANDLER) {
        // EXCEPTION_CONTINUE_EXECUTION: the host repaired the fault, so this is
        // not a death - no dispatch, no termination, readers keep running.
        // EXCEPTION_CONTINUE_SEARCH: a debugger is attached, and
        // UnhandledExceptionFilter returns this without consulting the host at
        // all. Stay out of the way so the debugger sees the original fault; a
        // developer stepping through a crash does not want the swarm reacting
        // to it. The consequence is that the guard does not report while a
        // debugger is attached.
        return host_decision;
    }

    // The host decided the process will not continue. Report the death, then
    // make it certain.
    action(static_cast<unsigned long>(code));
    return EXCEPTION_EXECUTE_HANDLER;                   // not reached
}

#endif // _WIN32

// Everything whose *definition* depends on SINTRA_HAS_SEH lives in a namespace
// named after the mode. A program that compiles one translation unit with the
// guard and another without would otherwise hold two different definitions of
// the same external-linkage entity, which is undefined behaviour no diagnostic
// is required for. With the mode in the name, each translation unit simply
// links to the version it was compiled for.
#if SINTRA_HAS_SEH
inline namespace seh_enabled {
#else
inline namespace seh_disabled {
#endif

#if SINTRA_HAS_SEH

// The structured-exception frame lives in a function of its own on purpose.
// MSVC rejects __try in any function that also requires C++ object unwinding
// (C2712), so nothing here may own a destructor - hence the plain function
// pointer and void* rather than a callable.
//
// Deliberately not noexcept. A C++ exception reaching this frame is classified
// by the filter as non-terminal (code 0xE06D7363 is not in the terminal set),
// so it keeps propagating exactly as it would have done had the frame not been
// here, and the caller's own boundary decides what happens to it.
inline void invoke_fault_guarded(void (*body)(void*), void* context)
{
    __try {
        body(context);
    }
    __except (::sintra::detail::thread_fault_filter(GetExceptionInformation())) {
        // Unreachable. The filter either continues execution, declines, or
        // terminates the process; __except just requires a body.
    }
}

#endif

// Runs fn under the guard. fn may own objects with destructors and may throw:
// both live in the trampoline, one frame below the structured-exception frame.
// Accepts lvalues and temporaries alike - a temporary outlives the call, which
// is all the trampoline's pointer needs.
template <typename F>
inline void run_fault_guarded(F&& fn)
{
#if SINTRA_HAS_SEH
    using Body = std::remove_reference_t<F>;
    Body& body  = fn;
    invoke_fault_guarded(
        [](void* ctx) { (*static_cast<Body*>(ctx))(); },
        static_cast<void*>(&body));
#else
    // No frame to build: call straight through, adding nothing at all.
    fn();
#endif
}

#if SINTRA_HAS_SEH
} // inline namespace seh_enabled
#else
} // inline namespace seh_disabled
#endif

} // namespace detail
} // namespace sintra
