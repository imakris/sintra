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
    // difference between a usable diagnostic and a truncated one. The loop
    // blocks for as long as the reader on the other end makes it: bounding it
    // would need overlapped I/O on a handle sintra does not own, or a second
    // thread, and a fault path may have neither.
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
// Thread and event are created while the process is still healthy, so arming
// from a fault path is a handful of atomics and a bare SetEvent: no allocation,
// and no dependency on a heap the fault may have corrupted.
//
// Arming is *counted*, not signalled. Faults are concurrent - the gate below
// admits one thread at a time into the host's filter, but every other faulted
// thread is already armed and queued behind it - and an event cannot express
// how many. Two SetEvents on an already-signalled auto-reset event are one
// signal, so a third fault would come out of the arithmetic with no bound at
// all and make an unbounded host call: exactly the hang this watchdog exists to
// prevent. A counter is conserved under concurrency; a signal is not.
//
// Arming is scoped to one host call and is released again when the host turns
// out to have repaired the fault, so a recovered process is never killed by a
// watchdog left over from its recovery.
//
// The grace period bounds how long a host crash reporter may take. A minidump
// of a large multi-process address space can take a while, so it is overridable.
#if !defined(SINTRA_CRASH_WATCHDOG_GRACE_MS)
  #define SINTRA_CRASH_WATCHDOG_GRACE_MS 5000
#endif
constexpr DWORD k_crash_watchdog_grace_ms = SINTRA_CRASH_WATCHDOG_GRACE_MS;

// How often the watchdog re-reads the deadline while faults are in flight.
// Coarse enough that waiting costs nothing, fine enough to add no meaningful
// slack to a grace period measured in seconds.
constexpr DWORD k_crash_watchdog_poll_ms = 50;

inline HANDLE& crash_watchdog_armed_event()
{
    static HANDLE evt = NULL;
    return evt;
}

inline std::atomic<unsigned>& crash_watchdog_exit_status()
{
    static std::atomic<unsigned> status{0xC0000005u};
    return status;
}

// Faults currently inside a host call. Zero means there is nothing to bound.
inline std::atomic<unsigned>& crash_watchdog_armed_faults()
{
    static std::atomic<unsigned> armed{0};
    return armed;
}

// The GetTickCount64 value past which the armed faults have taken too long.
// Ticks never run backwards and the grace period is a constant, so a later arm
// always computes a later deadline: the bound can only ever be pushed out. That
// is what gives every newly armed fault its own full interval, and it is also
// why a release needs to touch nothing but the count - it cannot shorten the
// bound another fault is still relying on.
//
// One deadline is shared by every armed fault, so a fault that arrived first
// waits out the newest arrival's interval too. Per-fault deadlines would need a
// table indexed by something a faulted thread can produce without allocating,
// and the swarm gains nothing from the finer bound: the guarantee owed is that a
// faulted peer disappears within a grace period of the last thread to fault, not
// of the first.
inline std::atomic<ULONGLONG>& crash_watchdog_deadline()
{
    static std::atomic<ULONGLONG> deadline{0};
    return deadline;
}

// Set only once the watchdog thread is actually running. Creating the event is
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
        // Awake because something armed. Watch the count rather than wait for a
        // matching release: arms and releases interleave freely across faulted
        // threads, and only the count says whether anything is still in flight.
        //
        // Every iteration reads both halves of the answer and there is exactly
        // one place that decides, so termination always rests on a fault being
        // in flight *and* the deadline it was armed with having passed. A count
        // watched by the loop and a deadline tested outside it would let a
        // non-zero count stand on its own for an expired deadline, which it
        // never is: a release and a fresh arm can both land while the watchdog
        // sits between the two reads, and the newly faulted thread would then be
        // killed at the start of its grace period instead of at the end of it.
        for (;;) {
            if (crash_watchdog_armed_faults().load(std::memory_order_acquire)
                    == 0)
            {
                break;              // nothing in flight; back to the long wait
            }
            // Read after the count, because arm_crash_watchdog writes the
            // deadline before it publishes the count and publishes it with
            // release: an acquire load that observes an arm therefore also
            // observes that arm's deadline. Reading the deadline first would
            // pair a fault with its predecessor's bound, which has by then
            // expired.
            if (GetTickCount64() <
                crash_watchdog_deadline().load(std::memory_order_acquire))
            {
                Sleep(k_crash_watchdog_poll_ms);
                continue;
            }
            // Blocking here would defeat the bound this thread exists to
            // enforce - see the note on write_stderr_raw. It is accepted because
            // without this line a watchdog kill is indistinguishable from an
            // ordinary fault exit: both carry the same status.
            write_stderr_raw(
                "[sintra] crash path did not complete - forcing termination\n");
            TerminateProcess(
                GetCurrentProcess(),
                crash_watchdog_exit_status().load(std::memory_order_acquire));
            return;
        }
    }
}

inline void ensure_crash_watchdog() noexcept
{
    static std::once_flag once;
    std::call_once(once, [] {
        crash_watchdog_armed_event() = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (crash_watchdog_armed_event() == NULL) {
            return;
        }
        try {
            std::thread(crash_watchdog_loop).detach();
            crash_watchdog_ready().store(true, std::memory_order_release);
        }
        catch (...) {
            // No watchdog. Callers see that through arm_crash_watchdog and can
            // choose not to make an unbounded host call. Failing to create one
            // thread must not fail runtime initialisation - but the event has no
            // waiter now and no second chance to acquire one, so give it back.
            CloseHandle(crash_watchdog_armed_event());
            crash_watchdog_armed_event() = NULL;
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
    crash_watchdog_exit_status().store(exit_status, std::memory_order_relaxed);
    crash_watchdog_deadline().store(
        GetTickCount64() + k_crash_watchdog_grace_ms, std::memory_order_relaxed);

    // The count is published last, and with release, so that the watchdog's
    // acquire load of it also brings the deadline written above. Publishing the
    // count first would let the watchdog pair a fault it has only just seen with
    // the previous fault's deadline, which has by then expired - an immediate
    // kill of a process still inside its own grace period.
    crash_watchdog_armed_faults().fetch_add(1, std::memory_order_release);

    if (SetEvent(crash_watchdog_armed_event()) == 0) {
        // Nothing is guaranteed to wake for this bound, so take it back rather
        // than leave a count no release will ever balance: the watchdog would
        // then terminate the process on some later fault's deadline.
        crash_watchdog_armed_faults().fetch_sub(1, std::memory_order_release);
        return false;
    }
    return true;
}

// Paired with an arm_crash_watchdog that returned true, and reached only from
// there. An unbounded host call has no bound to give back.
inline void release_crash_watchdog() noexcept
{
    crash_watchdog_armed_faults().fetch_sub(1, std::memory_order_release);
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
//
// The floating-point codes share one precondition: the UCRT masks every FP
// class, so none of them is raised unless the application unmasked it with
// _controlfp_s. They are therefore listed as a complete class rather than a
// selection - covering an unmasked invalid operation but not an unmasked
// overflow would make sintra's reporting depend on which classes an application
// happened to unmask. A code outside this set leaves thread_fault_filter
// returning EXCEPTION_CONTINUE_SEARCH, and the process then dies through the
// host with no dispatch at all.
//
// EXCEPTION_FLT_INEXACT_RESULT is deliberately absent: unmasking it faults on
// ordinary floating-point arithmetic, so no working program has it unmasked.
// So are EXCEPTION_FLT_STACK_CHECK, which reports an x87 register-stack
// condition, and EXCEPTION_INT_OVERFLOW, which needs an explicit INTO or BOUND
// that C++ code does not generate.
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
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
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
