// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../utility.h"
#include "../type_utils.h"
#include "../exception.h"
#include "../ipc/file_utils.h"
#include "../ipc/process_utils.h"
#include "../logging.h"
#include "../tls_post_handler.h"
#include "dispatch_wait_guard.h"

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <csignal>
#include <fstream>
#include <functional>
#include <list>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <errno.h>
#endif
#ifndef _WIN32
#include <signal.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

namespace sintra {

extern thread_local bool tl_is_req_thread;

// Protects access to s_mproc during signal dispatch to prevent use-after-free.
// On POSIX: The signal dispatch thread takes a shared lock when accessing s_mproc.
// On Windows: The CRT signal handler thread takes a shared lock when accessing s_mproc.
// The Managed_process destructor takes an exclusive lock before clearing s_mproc.
inline std::shared_mutex dispatch_shutdown_mutex_instance;

inline std::once_flag& signal_handler_once_flag()
{
    static std::once_flag flag;
    return flag;
}

namespace {

    inline std::atomic<unsigned int>& pending_signal_mask()
    {
        static std::atomic<unsigned int> mask{0};
        return mask;
    }

    inline std::atomic<uint32_t>& dispatched_signal_counter()
    {
        static std::atomic<uint32_t> counter{0};
        return counter;
    }

    inline std::once_flag& signal_dispatcher_once_flag()
    {
        static std::once_flag flag;
        return flag;
    }

#ifdef _WIN32
    struct signal_slot {
        int sig;
        void (__cdecl* previous)(int) = SIG_DFL;
        bool has_previous = false;
    };

#ifdef SIGTRAP
    constexpr std::size_t k_signal_slot_count = 7;
#else
    constexpr std::size_t k_signal_slot_count = 6;
#endif

    inline std::array<signal_slot, k_signal_slot_count>& signal_slots()
    {
        static std::array<signal_slot, k_signal_slot_count> slot_table {{
            {SIGABRT}, {SIGFPE}, {SIGILL}, {SIGINT}, {SIGSEGV}, {SIGTERM}
#ifdef SIGTRAP
            , {SIGTRAP}
#endif
        }};
        return slot_table;
    }

    // Windows signal dispatcher infrastructure - matches POSIX architecture
    // to prevent blocking in signal handlers and provide timeout guarantees.

    // Forward declarations - defined after #endif in the common section
    inline std::size_t signal_index(int sig);
    inline void dispatch_signal_number(int sig_number);
    inline void drain_pending_signals();

    inline HANDLE& signal_event()
    {
        static HANDLE evt = NULL;
        return evt;
    }

    inline void wait_for_signal_dispatch_win(uint32_t expected_count);

    inline void queue_signal_dispatch_win(int sig_number, bool wait_for_dispatch = true)
    {
        auto* mproc = s_mproc;
        const bool should_wait_for_dispatch = mproc && mproc->m_out_req_c;
        const bool can_wait = can_wait_for_signal_dispatch();
        uint32_t dispatched_before = 0;
        if (should_wait_for_dispatch && can_wait) {
            dispatched_before = dispatched_signal_counter().load();
        }

        bool can_dispatch = false;
        if (signal_event() != NULL) {
            auto idx = signal_index(sig_number);
            if (idx < signal_slots().size()) {
                uint32_t bit = 1U << idx;
                pending_signal_mask().fetch_or(bit);
                can_dispatch = true;
            }
            SetEvent(signal_event());
        }

        if (should_wait_for_dispatch && wait_for_dispatch && can_wait && can_dispatch) {
            wait_for_signal_dispatch_win(dispatched_before);
        }
    }

    inline LONG WINAPI s_vectored_exception_handler(EXCEPTION_POINTERS* exception_info)
    {
        if (!exception_info || !exception_info->ExceptionRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = exception_info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        int sig_number = 0;
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_IN_PAGE_ERROR:
            case EXCEPTION_STACK_OVERFLOW:
                sig_number = SIGSEGV;
                break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
                sig_number = SIGILL;
                break;
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            case EXCEPTION_FLT_INVALID_OPERATION:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                sig_number = SIGFPE;
                break;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
        }

        queue_signal_dispatch_win(sig_number);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    inline void signal_dispatch_loop_win()
    {
        while (true) {
            DWORD result = WaitForSingleObject(signal_event(), INFINITE);
            if (result == WAIT_OBJECT_0) {
                drain_pending_signals();
                // Reset the event after processing
                ResetEvent(signal_event());
            }
            else {
                // Event was closed or error occurred - exit the loop
                break;
            }
        }
    }

    inline void ensure_signal_dispatcher_win()
    {
        std::call_once(signal_dispatcher_once_flag(), []() {
            // Manual-reset event, initially non-signaled
            signal_event() = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (signal_event() == NULL) {
                return;
            }
            std::thread(signal_dispatch_loop_win).detach();
        });
    }

    inline void wait_for_signal_dispatch_win(uint32_t expected_count)
    {
        const uint32_t target = expected_count + 1;
        // Wait up to 200ms (matching POSIX timeout)
        for (int spin = 0; spin < 200; ++spin) {
            if (dispatched_signal_counter().load() >= target) {
                return;
            }
            Sleep(1);  // 1 millisecond
        }
    }
#else
    struct signal_slot {
        int sig;
        struct sigaction previous {};
        bool has_previous = false;
    };

#ifdef SIGTRAP
    constexpr std::size_t k_signal_slot_count = 8;
#else
    constexpr std::size_t k_signal_slot_count = 7;
#endif

    inline std::array<signal_slot, k_signal_slot_count>& signal_slots()
    {
        static std::array<signal_slot, k_signal_slot_count> slot_table {{
            {SIGABRT}, {SIGFPE}, {SIGILL}, {SIGINT}, {SIGSEGV}, {SIGTERM},
#ifdef SIGTRAP
            {SIGTRAP},
#endif
            {SIGCHLD}
        }};
        return slot_table;
    }

    inline std::array<char, 64 * 1024>& alt_stack_storage()
    {
        static std::array<char, 64 * 1024> storage {};
        return storage;
    }

    inline bool& alt_stack_installed()
    {
        static bool installed = false;
        return installed;
    }

    inline int (&signal_pipe())[2]
    {
        static int pipefd[2] = {-1, -1};
        return pipefd;
    }

    // Forward declarations - defined after #endif in the common section
    inline std::size_t signal_index(int sig);
    inline void dispatch_signal_number(int sig_number);
    inline void drain_pending_signals();

    inline uint64_t signal_dispatch_now_ns() noexcept
    {
        timespec ts {};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            return 0;
        }
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
            + static_cast<uint64_t>(ts.tv_nsec);
    }

    inline void wait_for_signal_dispatch(uint32_t expected_count)
    {
        constexpr uint64_t k_wait_timeout_ns = 200'000'000ULL;
        uint64_t start_ns = signal_dispatch_now_ns();
        constexpr int k_fallback_rounds = 5000;
        int fallback_rounds = (start_ns == 0) ? k_fallback_rounds : -1;

        // Exponential backoff: start responsive (32 pauses), grow to max (1024).
        // This reduces CPU usage during longer waits while staying responsive
        // for the common case where dispatch completes quickly.
        constexpr int k_initial_pause_count = 32;
        constexpr int k_max_pause_count = 1024;
        int pause_count = k_initial_pause_count;

        for (;;) {
            const uint32_t current = dispatched_signal_counter().load(std::memory_order_acquire);
            if (static_cast<uint32_t>(current - expected_count) >= 1U) {
                return;
            }

            const uint64_t now_ns = signal_dispatch_now_ns();
            if (start_ns != 0 && now_ns != 0) {
                if ((now_ns - start_ns) >= k_wait_timeout_ns) {
                    return;
                }
            }
            else if (fallback_rounds > 0) {
                if (--fallback_rounds == 0) {
                    return;
                }
            }

            for (int pause = 0; pause < pause_count; ++pause) {
                sintra::detail::spin_pause();
            }

            // Double pause count each iteration up to maximum (exponential backoff).
            if (pause_count < k_max_pause_count) {
                pause_count *= 2;
            }
        }
    }

    inline void signal_dispatch_loop()
    {
        auto& pipefd = signal_pipe();

        while (true) {
            int sig_number = 0;
            ssize_t bytes_read = ::read(pipefd[0], &sig_number, sizeof(sig_number));

            if (bytes_read == sizeof(sig_number)) {
                dispatch_signal_number(sig_number);
                drain_pending_signals();
                continue;
            }

            if (bytes_read == -1) {
                if (errno == EINTR) {
                    drain_pending_signals();
                    continue;
                }

                if (errno == EAGAIN) {
                    std::this_thread::yield();
                    drain_pending_signals();
                    continue;
                }
            }

            drain_pending_signals();

            // Either the write end was closed (bytes_read == 0) or an unrecoverable
            // error occurred. Exit the loop to allow the thread to finish.
            break;
        }
    }

    inline void ensure_signal_dispatcher()
    {
        std::call_once(signal_dispatcher_once_flag(), []() {
            int pipefd_local[2];
            if (::pipe(pipefd_local) != 0) {
                return;
            }

            int flags = ::fcntl(pipefd_local[1], F_GETFL, 0);
            if (flags != -1) {
                ::fcntl(pipefd_local[1], F_SETFL, flags | O_NONBLOCK);
            }

            auto& pipefd = signal_pipe();
            pipefd[0] = pipefd_local[0];
            pipefd[1] = pipefd_local[1];

            std::thread(signal_dispatch_loop).detach();
        });
    }
#endif

    // --- Common signal infrastructure (shared across platforms) ---

    inline void drain_pending_signals()
    {
        auto mask = pending_signal_mask().exchange(0U);
        if (mask == 0U) {
            return;
        }

        auto& slot_table = signal_slots();
        for (std::size_t idx = 0; idx < slot_table.size(); ++idx) {
            if ((mask & (1U << idx)) != 0U) {
                dispatch_signal_number(slot_table[idx].sig);
            }
        }
    }

    inline std::size_t signal_index(int sig)
    {
        auto& slot_table = signal_slots();
        for (std::size_t idx = 0; idx < slot_table.size(); ++idx) {
            if (slot_table[idx].sig == sig) {
                return idx;
            }
        }
        return slot_table.size();
    }

    inline void dispatch_signal_number(int sig_number)
    {
        Dispatch_shared_lock dispatch_lock(dispatch_shutdown_mutex_instance);

        auto* mproc = s_mproc;
#ifndef _WIN32
        if (sig_number == SIGCHLD) {
            if (mproc) {
                mproc->reap_finished_children();
            }
            return;
        }
#endif
        if (mproc && mproc->m_out_req_c) {
            mproc->emit_remote<Managed_process::terminated_abnormally>(sig_number);

            Dispatch_shared_lock readers_lock(mproc->m_readers_mutex);
            for (auto& reader_entry : mproc->m_readers) {
                if (auto& reader = reader_entry.second) {
                    reader->stop_nowait();
                }
            }

            dispatched_signal_counter().fetch_add(1);
        }
    }

    template <std::size_t N>
    inline signal_slot* find_slot(std::array<signal_slot, N>& slot_table, int sig)
    {
        for (auto& candidate : slot_table) {
            if (candidate.sig == sig) {
                return &candidate;
            }
        }
        return nullptr;
    }

    inline std::atomic<bool>& lifeline_shutdown_flag()
    {
        static std::atomic<bool> flag{false};
        return flag;
    }

    // Argument names for lifeline configuration (internal, fixed)
    constexpr const char* k_lifeline_handle_arg = "--lifeline_handle";
    constexpr const char* k_lifeline_exit_code_arg = "--lifeline_exit_code";
    constexpr const char* k_lifeline_timeout_arg = "--lifeline_timeout_ms";
    constexpr const char* k_lifeline_disable_arg = "--lifeline_disable";

    // Storage for parsed lifeline values (set during init, read by start_lifeline_watcher)
    static inline std::string s_lifeline_handle_value;
    static inline int s_lifeline_exit_code = 99;
    static inline int s_lifeline_timeout_ms = 100;
    static inline bool s_lifeline_disabled = false;

    inline void log_lifeline_message(detail::log_level level, const std::string& message)
    {
        detail::log_raw(level, message.c_str());
    }

    [[noreturn]] inline void lifeline_hard_exit(int exit_code)
    {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exit_code));
        std::abort();
#else
        _exit(exit_code);
#endif
    }

    inline void schedule_lifeline_hard_exit(int timeout_ms, int exit_code)
    {
        std::thread([timeout_ms, exit_code]() {
            if (timeout_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            }
            log_lifeline_message(
                detail::log_level::error,
                std::string("[sintra] Hard exit with code ") + std::to_string(exit_code) + "\n");
            lifeline_hard_exit(exit_code);
        }).detach();
    }

    inline void signal_lifeline_shutdown(int timeout_ms, int exit_code)
    {
        if (lifeline_shutdown_flag().exchange(true)) {
            return;
        }

        log_lifeline_message(
            detail::log_level::error,
            std::string("[sintra] Lifeline broken - owner exited - terminating in ") +
                std::to_string(timeout_ms) + "ms\n");

        if (s_mproc) {
            s_mproc->m_must_stop.store(true, std::memory_order_release);
            s_mproc->stop();
            s_mproc->unblock_rpc();
        }

        schedule_lifeline_hard_exit(timeout_ms, exit_code);
    }

#ifdef _WIN32
    inline void lifeline_watch_loop(HANDLE handle, int timeout_ms, int exit_code)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE) {
            signal_lifeline_shutdown(timeout_ms, exit_code);
            return;
        }

        char buffer = 0;
        DWORD bytes_read = 0;
        while (true) {
            const BOOL ok = ReadFile(handle, &buffer, 1, &bytes_read, nullptr);
            if (!ok) {
                break;
            }
            if (bytes_read == 0) {
                break;
            }
        }

        CloseHandle(handle);
        signal_lifeline_shutdown(timeout_ms, exit_code);
    }
#else
    inline void lifeline_watch_loop(int fd, int timeout_ms, int exit_code)
    {
        if (fd < 0) {
            signal_lifeline_shutdown(timeout_ms, exit_code);
            return;
        }

        char buffer = 0;
        while (true) {
            const ssize_t bytes_read = ::read(fd, &buffer, 1);
            if (bytes_read > 0) {
                continue;
            }
            if (bytes_read == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        ::close(fd);
        signal_lifeline_shutdown(timeout_ms, exit_code);
    }
#endif

    inline void start_lifeline_watcher(const Lifetime_policy& policy, bool lifeline_required)
    {
        if (!policy.enable_lifeline) {
            return;
        }

        // Check if lifeline was disabled via argument
        if (s_lifeline_disabled) {
#ifndef NDEBUG
            log_lifeline_message(
                detail::log_level::warning,
                std::string("[sintra] Lifeline disabled via ") + k_lifeline_disable_arg + "\n");
#endif
            return;
        }

        // Check if handle/fd was provided via argument
        if (s_lifeline_handle_value.empty()) {
            if (lifeline_required) {
                log_lifeline_message(
                    detail::log_level::error,
                    "[sintra] Lifeline missing - terminating\n");
                lifeline_hard_exit(policy.hard_exit_code);
            }
            return;
        }

        // Parse the handle value
        errno = 0;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(s_lifeline_handle_value.c_str(), &end, 10);
        if (!end || end == s_lifeline_handle_value.c_str() || errno != 0) {
            if (lifeline_required) {
                log_lifeline_message(
                    detail::log_level::error,
                    "[sintra] Lifeline parse failure - terminating\n");
                lifeline_hard_exit(policy.hard_exit_code);
            }
            return;
        }

        // Use parsed argument values
        const int exit_code = s_lifeline_exit_code;
        int timeout_ms = s_lifeline_timeout_ms;
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }
#ifdef _WIN32
        const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parsed));
        std::thread(lifeline_watch_loop, handle, timeout_ms, exit_code).detach();
#else
        const int fd = static_cast<int>(parsed);
        std::thread(lifeline_watch_loop, fd, timeout_ms, exit_code).detach();
#endif
    }

    inline bool lifeline_handle_valid(Managed_process::lifeline_handle_type handle)
    {
#ifdef _WIN32
        return handle != 0 && handle != static_cast<Managed_process::lifeline_handle_type>(-1);
#else
        return handle != static_cast<Managed_process::lifeline_handle_type>(-1);
#endif
    }

    inline void close_lifeline_handle(Managed_process::lifeline_handle_type handle)
    {
        if (!lifeline_handle_valid(handle)) {
            return;
        }
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
        ::close(static_cast<int>(handle));
#endif
    }

#ifdef _WIN32
    inline bool create_lifeline_pipe(HANDLE& read_handle, HANDLE& write_handle, int* error_out)
    {
        read_handle = nullptr;
        write_handle = nullptr;

        SECURITY_ATTRIBUTES sa {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) {
            if (error_out) {
                *error_out = static_cast<int>(GetLastError());
            }
            return false;
        }

        if (!SetHandleInformation(write_handle, HANDLE_FLAG_INHERIT, 0)) {
            if (error_out) {
                *error_out = static_cast<int>(GetLastError());
            }
            CloseHandle(read_handle);
            CloseHandle(write_handle);
            read_handle = nullptr;
            write_handle = nullptr;
            return false;
        }

        return true;
    }
#else
    inline bool create_lifeline_pipe(int& read_fd, int& write_fd, int* error_out)
    {
        read_fd = -1;
        write_fd = -1;

        int pipefd[2] = {-1, -1};
        if (detail::call_pipe2(pipefd, O_CLOEXEC) != 0) {
            if (error_out) {
                *error_out = errno;
            }
            return false;
        }

        int flags = detail::fcntl_retry(pipefd[0], F_GETFD);
        if (flags == -1 || detail::fcntl_retry(pipefd[0], F_SETFD, flags & ~FD_CLOEXEC) == -1) {
            if (error_out) {
                *error_out = errno;
            }
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        read_fd = pipefd[0];
        write_fd = pipefd[1];
        return true;
    }
#endif
}

#ifdef _WIN32
inline
static void s_signal_handler(int sig)
{
    // Windows signal handler using dispatcher pattern (matching POSIX architecture).
    // This ensures potentially-blocking operations (emit_remote, mutex acquisition)
    // happen in the dispatcher thread, not in the signal handler, with a timeout
    // to guarantee the handler eventually completes even if IPC is broken.

    auto& slot_table = signal_slots();

    queue_signal_dispatch_win(sig);

    // Chain to previous handler (e.g., debug_pause handler)
    if (auto* slot = find_slot(slot_table, sig); slot && slot->has_previous) {
        auto prev = slot->previous;
        if (prev != SIG_DFL && prev != SIG_IGN && prev != s_signal_handler) {
            prev(sig);
            return;  // Previous handler took over (e.g., debug_pause entered infinite loop)
        }
    }

    // Default: terminate the process
    // Reader threads may be blocked on semaphores, and Windows shutdown waits for
    // all threads to exit. Since this is a crashing process (signal handler was called),
    // we don't need graceful shutdown - the coordinator will detect the death and
    // respawn if recovery is enabled.
    TerminateProcess(GetCurrentProcess(), 1);
}
#else
inline
static void s_signal_handler(int sig, siginfo_t* info, void* ctx)
{
    auto& slot_table = signal_slots();

    auto* mproc = s_mproc;
    // Process-wide depth guard: conservative to avoid TLS access in signal handler.
    const bool can_wait = can_wait_for_signal_dispatch();
    const bool should_wait_for_dispatch = mproc && mproc->m_out_req_c && sig != SIGCHLD;
    uint32_t dispatched_before = 0;
    if (should_wait_for_dispatch && can_wait) {
        dispatched_before = dispatched_signal_counter().load(std::memory_order_acquire);
    }

    auto& pipefd = signal_pipe();
    if (pipefd[1] != -1) {
        int sig_number = sig;
        int last_errno = 0;
        bool delivered = false;
        constexpr int max_attempts = 4;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            ssize_t result = ::write(pipefd[1], &sig_number, sizeof(sig_number));
            if (result == sizeof(sig_number)) {
                delivered = true;
                break;
            }

            if (result == -1) {
                last_errno = errno;
                if (last_errno == EINTR || last_errno == EAGAIN) {
                    continue;
                }
            }

            break;
        }

        if (!delivered && (last_errno == EAGAIN || last_errno == EINTR)) {
            auto index = signal_index(sig);
            if (index < slot_table.size()) {
                pending_signal_mask().fetch_or(1U << index);
            }
        }
    }

    if (should_wait_for_dispatch && can_wait) {
        wait_for_signal_dispatch(dispatched_before);
    }

    if (auto* slot = find_slot(slot_table, sig); slot && slot->has_previous) {
        if (slot->previous.sa_handler == SIG_IGN) {
            return;
        }

        if ((slot->previous.sa_flags & SA_SIGINFO) && slot->previous.sa_sigaction) {
            slot->previous.sa_sigaction(sig, info, ctx);
            return;
        }

        if (slot->previous.sa_handler && slot->previous.sa_handler != SIG_DFL) {
            slot->previous.sa_handler(sig);
            return;
        }
    }

    // SIGCHLD is informational for us (we reap children in the dispatch loop).
    // Do NOT restore default/raise here, or the handler will uninstall itself
    // after the first child exit and subsequent SIGCHLDs will be ignored.
    if (sig == SIGCHLD) {
        return;
    }

    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}
#endif

inline
void Managed_process::enable_recovery()
{
    /*
     * Recovery overview
     * -----------------
     *  - enable_recovery() RPCs the coordinator so the caller's process slot is
     *    added to Coordinator::m_requested_recovery. When a crash is observed the
     *    coordinator routes the cached Spawn_swarm_process_args back through
     *    Coordinator::recover_if_required(), which in turn calls
     *    Managed_process::spawn_swarm_process().
     *  - spawn_swarm_process() persists the executable + argument vector in
     *    m_cached_spawns and bumps the occurrence counter so every respawn uses a
     *    fresh ``req``/``rep`` ring name (see Message_ring_{R,W}::get_base_filename
     *    adding the ``_occN`` suffix). Before a replacement child is launched we
     *    tear down any previous Process_message_reader for that slot (see the
     *    erase() call just above) so the old occurrence's shared memory is
     *    unmapped before a fresh reader is created.
     *  - The coordinator pre-attaches new Process_message_reader instances before
     *    the child is launched, ensuring the new process sees ready request/reply
     *    channels the moment it starts. There is no pre-allocation for future
     *    occurrences; only the rings for the active occurrence stay mapped.
     *  - Each reader/writer ring is implemented via Ring_data::attach(), which
     *    reserves a 2× span and double maps the 2 MiB data file so wrap-around is
     *    linear for zero-copy reads. Those double-mapped spans are what appear as
     *    "guard"/reserved regions inside Mach-O cores-the mapping design is
     *    required for the ring abstraction rather than recovery itself, but the
     *    recovery harness crashes the process often enough that the platform
     *    keeps dumping them.
     */
    // Mark this process as recoverable in the coordinator
    // so that abnormal termination triggers a respawn.
    Coordinator::rpc_enable_recovery(s_coord_id, process_of(m_instance_id));
    m_recoverable = true;
}

inline
void install_signal_handler()
{
    std::call_once(signal_handler_once_flag(), []() {
        auto& slot_table = signal_slots();
        (void)pending_signal_mask();
        (void)dispatched_signal_counter();
#ifdef _WIN32
        (void)signal_event();
#else
        (void)signal_pipe();
#endif

#ifdef _WIN32
        ensure_signal_dispatcher_win();
        AddVectoredExceptionHandler(0, s_vectored_exception_handler);

        for (auto& slot : slot_table) {
            auto previous = std::signal(slot.sig, s_signal_handler);
            if (previous != SIG_ERR) {
                slot.has_previous = previous != s_signal_handler;
                slot.previous = slot.has_previous ? previous : SIG_DFL;
            }
            else {
                slot.has_previous = false;
            }
        }
#else
        auto& storage = alt_stack_storage();
        auto& installed = alt_stack_installed();
        if (!installed) {
            stack_t ss {};
            ss.ss_sp = storage.data();
            ss.ss_size = storage.size();
            ss.ss_flags = 0;
            if (sigaltstack(&ss, nullptr) == 0) {
                installed = true;
            }
        }

        ensure_signal_dispatcher();

        for (auto& slot : slot_table) {
            struct sigaction sa {};
            sigemptyset(&sa.sa_mask);
            sa.sa_sigaction = s_signal_handler;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
#ifdef SA_RESTART
            sa.sa_flags |= SA_RESTART;
#endif
            if (slot.sig == SIGCHLD) {
#ifdef SA_NOCLDSTOP
                sa.sa_flags |= SA_NOCLDSTOP;
#endif
            }

            if (sigaction(slot.sig, &sa, &slot.previous) == 0) {
                slot.has_previous = slot.previous.sa_sigaction != s_signal_handler;
                if (!slot.has_previous) {
                    slot.previous.sa_handler = SIG_DFL;
                }
            }
            else {
                slot.has_previous = false;
            }
        }
#endif
    });
}

namespace detail {

template <typename T, typename = void>
struct has_sintra_type_id : std::false_type {};

template <typename T>
struct has_sintra_type_id<T, std::void_t<decltype(T::sintra_type_id())>>
    : std::true_type {};

template <typename T>
type_id_type explicit_type_id()
{
    if constexpr (has_sintra_type_id<T>::value) {
        return T::sintra_type_id();
    }
    else
    {
        return invalid_type_id;
    }
}

template <typename MapT, typename KeyT, typename RpcFn, typename InvalidT>
auto cached_resolve(MapT& cache, const KeyT& key, RpcFn&& rpc, InvalidT invalid)
    -> decltype(rpc(key))
{
    // Hold spinlock while accessing the iterator to prevent use-after-invalidation.
    {
        auto scoped_map = cache.scoped();
        auto it = scoped_map.get().find(key);
        if (it != scoped_map.get().end()) {
            return it->second;
        }
        // Spinlock released here automatically when scoped_map goes out of scope
    }

    // Caution the Coordinator call will refer to the map that is being assigned,
    // if the Coordinator is local. Do not be tempted to simplify the temporary,
    // because depending on the order of evaluation, it may or it may not work.
    auto resolved = rpc(key);

    // If it is not invalid, cache it.
    if (resolved != invalid) {
        auto scoped_map = cache.scoped();
        scoped_map.get()[key] = resolved;
    }

    return resolved;
}

} // namespace detail

template <typename T>
sintra::type_id_type get_type_id()
{
    const type_id_type explicit_id = detail::explicit_type_id<T>();
    if (explicit_id != invalid_type_id) {
        const std::string type_name = detail::type_name<T>();
        {
            auto scoped_map = s_mproc->m_type_name_of_explicit_id.scoped();
            auto it = scoped_map.get().find(explicit_id);
            if (it != scoped_map.get().end()) {
                if (it->second != type_name) {
                    throw std::runtime_error(
                        "Explicit type id collision: id=" +
                        std::to_string(
                            static_cast<unsigned long long>(explicit_id)) +
                        " existing='" + it->second +
                        "' new='" + type_name + "'");
                }
            }
            else
            {
                auto scoped_map = s_mproc->m_type_name_of_explicit_id.scoped();
                scoped_map.get()[explicit_id] = type_name;
            }
        }
        return make_type_id(explicit_id);
    }

    const std::string type_name = detail::type_name<T>();
    return detail::cached_resolve(
        s_mproc->m_type_id_of_type_name,
        type_name,
        [](const std::string& name) {
            return Coordinator::rpc_resolve_type(s_coord_id, name);
        },
        invalid_type_id);
}

// helper
template <typename T>
sintra::type_id_type get_type_id(const T&) {return get_type_id<T>();}

template <typename>
sintra::instance_id_type get_instance_id(std::string&& assigned_name)
{
    return detail::cached_resolve(
        s_mproc->m_instance_id_of_assigned_name,
        assigned_name,
        [](const std::string& name) {
            return Coordinator::rpc_resolve_instance(s_coord_id, name);
        },
        invalid_instance_id);
}

inline
Managed_process::Managed_process():
    Derived_transceiver<Managed_process>(nullptr),
    m_communication_state(COMMUNICATION_STOPPED),
    m_must_stop(false),
    m_swarm_id(0),
    m_last_message_sequence(0),
    m_check_sequence(0),
    m_message_stats_reference_time(0.),
    m_messages_accepted_since_reference_time(0),
    m_messages_rejected_since_reference_time(0),
    m_total_sequences_missed(0)
{
    assert(s_mproc == nullptr);
    s_mproc = this;

    m_pid = detail::get_current_process_id();
    if (auto start_stamp = current_process_start_stamp()) {
        m_process_start_stamp = *start_stamp;
    }

    install_signal_handler();

    // NOTE: Do not use external library helpers for process creation time,
    // it is only implemented for Windows.
    m_time_instantiated = std::chrono::steady_clock::now();
}

inline
Managed_process::~Managed_process()
{
    // The coordinating process will be removing its readers whenever
    // they are unpublished - when they are all done, the process may exit.
    //
    // Previously we waited for readers to report completion before asking
    // them to stop.  On Linux this could deadlock during shutdown because
    // the reader threads were still blocked on their rings, so the
    // m_num_active_readers counter never reached the expected value and the
    // destructor blocked forever.  Stop the readers first so they can wake
    // up, decrement the counter and unblock the wait.  When invoked from a
    // request reader thread, make sure the deferred reply-ring shutdown runs
    // before waiting and allow for the current thread in the count so we do
    // not deadlock on ourselves.
    if (s_coord) {
        stop();

        const bool called_from_request_reader = tl_is_req_thread;
        if (called_from_request_reader && tl_post_handler_function_ready()) {
            auto post_handler = std::move(*tl_post_handler_function);
            tl_post_handler_function_clear();
            if (post_handler) {
                Post_handler_guard post_guard;
                post_handler();
            }
        }

        wait_until_all_external_readers_are_done(called_from_request_reader ? 1 : 0);
    }

    release_all_lifelines();

    assert(m_communication_state <= COMMUNICATION_PAUSED); // i.e. paused or stopped

    // this is called explicitly, in order to inform the coordinator of the destruction early.
    // it would not be possible to communicate it after the channels were closed.
    this->Derived_transceiver<Managed_process>::destroy();

    // no more reading
    {
        Dispatch_unique_lock readers_lock(m_readers_mutex);
        m_readers.clear();
    }

    // no more writing (prevent signal dispatch from using freed rings)
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
        if (m_out_req_c) {
            delete m_out_req_c;
            m_out_req_c = nullptr;
        }

        if (m_out_rep_c) {
            delete m_out_rep_c;
            m_out_rep_c = nullptr;
        }
    }

    if (s_coord) {

        // now it's safe to delete the Coordinator.
        delete s_coord;
        s_coord = 0;

        mark_run_directory_for_cleanup(std::filesystem::path(m_directory));

        // removes the swarm directory
        remove_directory(m_directory);
    }

#ifndef _WIN32
    std::vector<pid_t> reap_targets;
    {
        std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
        reap_targets.swap(m_spawned_child_pids);
    }

    for (pid_t pid : reap_targets) {
        if (pid <= 0) {
            continue;
        }

        const auto poll_delay = std::chrono::milliseconds(10);
        auto graceful_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        auto forceful_deadline = graceful_deadline + std::chrono::seconds(1);
        bool sent_sigterm = false;
        bool sent_sigkill = false;

        while (true) {
            int status = 0;
            pid_t result = ::waitpid(pid, &status, sent_sigkill ? 0 : WNOHANG);

            if (result == pid) {
                break;
            }

            if (result == 0) {
                auto now = std::chrono::steady_clock::now();
                if (!sent_sigterm && now >= graceful_deadline) {
                    if (::kill(pid, SIGTERM) == -1 && errno == ESRCH) {
                        break;
                    }
                    sent_sigterm = true;
                    forceful_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    continue;
                }

                if (sent_sigterm && !sent_sigkill && now >= forceful_deadline) {
                    if (::kill(pid, SIGKILL) == -1 && errno == ESRCH) {
                        break;
                    }
                    sent_sigkill = true;
                    continue;
                }

                std::this_thread::sleep_for(poll_delay);
                continue;
            }

            if (result == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECHILD || errno == ESRCH) {
                    break;
                }

                // Unexpected error: escalate to SIGKILL once before aborting the loop.
                if (!sent_sigkill) {
                    if (::kill(pid, SIGKILL) == -1 && errno == ESRCH) {
                        break;
                    }
                    sent_sigkill = true;
                    continue;
                }

                break;
            }
        }
    }

    // Close the signal dispatch pipe to allow the dispatch thread to exit cleanly
    auto& pipefd = signal_pipe();
    if (pipefd[1] != -1) {
        ::close(pipefd[1]);
        pipefd[1] = -1;
    }
    if (pipefd[0] != -1) {
        ::close(pipefd[0]);
        pipefd[0] = -1;
    }

    // Wait for any in-flight signal dispatches to complete before clearing s_mproc.
    // On POSIX: The dispatch thread may still be executing dispatch_signal_number()
    //           after we closed the pipes above.
    // On Windows: The CRT signal handler thread may be executing s_signal_handler().
    // Taking an exclusive lock here ensures all shared locks are released before we
    // clear s_mproc and destroy the object, preventing use-after-free.
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
        s_mproc = nullptr;
        s_mproc_id = 0;
    }
#else
    // Close the signal dispatch event to allow the dispatch thread to exit cleanly
    if (signal_event() != NULL) {
        CloseHandle(signal_event());
        signal_event() = NULL;
    }

    // Wait for any in-flight signal dispatches to complete before clearing s_mproc.
    // The Windows dispatcher thread may still be executing dispatch_signal_number()
    // after we closed the event above.
    // Taking an exclusive lock ensures all shared locks are released before we
    // clear s_mproc and destroy the object, preventing use-after-free.
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
        s_mproc = nullptr;
        s_mproc_id = 0;
    }
#endif
}

#ifndef _WIN32
inline void Managed_process::reap_finished_children()
{
    std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
    if (m_spawned_child_pids.empty()) {
        return;
    }

    std::vector<pid_t> remaining;
    remaining.reserve(m_spawned_child_pids.size());

    for (pid_t pid : m_spawned_child_pids) {
        if (pid <= 0) {
            continue;
        }

        int status = 0;
        pid_t result = 0;
        do {
            result = ::waitpid(pid, &status, WNOHANG);
        }
        while (result == -1 && errno == EINTR);

        if (result == 0) {
            remaining.push_back(pid);
            continue;
        }

        if (result == -1 && errno != ECHILD) {
            remaining.push_back(pid);
        }
    }

    m_spawned_child_pids.swap(remaining);
}
#endif

// returns the argc/argv as a vector of strings
inline
std::vector<std::string> argc_argv_to_vector(int argc, const char* const* argv)
{
    std::vector<std::string> ret;
    for (int i = 0; i < argc; i++) {
        ret.push_back(argv[i]);
    }
    return ret;
}

struct Filtered_args
{
    vector<string> remained;
    vector<string> extracted;
};

inline std::string join_strings(const std::vector<std::string>& parts, const std::string& delimiter)
{
    if (parts.empty()) {
        return std::string();
    }

    std::string result;
    size_t total_size = delimiter.size() * (parts.size() - 1);
    for (const auto& part : parts) {
        total_size += part.size();
    }
    result.reserve(total_size);

    auto it = parts.begin();
    result += *it++;
    for (; it != parts.end(); ++it) {
        result += delimiter;
        result += *it;
    }

    return result;
}

inline
Filtered_args filter_option(
    std::vector<std::string> in_args, std::string in_option, unsigned int num_args
)
{
    Filtered_args ret;
    for (size_t i = 0; i < in_args.size(); ++i) {
        const auto& e = in_args[i];
        if (e == in_option) {
            ret.extracted.clear();
            ret.extracted.push_back(e);
            for (unsigned int picked = 0; picked < num_args && i + 1 < in_args.size(); ++picked) {
                ret.extracted.push_back(in_args[++i]);
            }
            continue;
        }
        ret.remained.push_back(e);
    }
    return ret;
}

inline
void Managed_process::init(int argc, const char* const* argv)
{
    m_binary_name = argv[0];

    // Reset lifeline state to prevent stale values from a previous init() call
    s_lifeline_handle_value.clear();
    s_lifeline_exit_code = Lifetime_policy{}.hard_exit_code;
    s_lifeline_timeout_ms = Lifetime_policy{}.hard_exit_timeout_ms;
    s_lifeline_disabled = false;

    std::string branch_index_arg;
    std::string swarm_id_arg;
    std::string instance_id_arg;
    std::string coordinator_id_arg;
    std::string recovery_arg;

    size_t recovery_occurrence_value = 0;
    auto fa = filter_option(argc_argv_to_vector(argc, argv), "--recovery_occurrence", 1);

    if (fa.extracted.size() == 2) {
        try {
            recovery_occurrence_value =  std::stoul(fa.extracted[1]);
        }
        catch (...) {
            assert(!"not implemented");
        }
    }

    // Filter out lifeline arguments from remained - they contain stale handle values
    // and will be freshly added by spawn_swarm_process during recovery
    auto fa2 = filter_option(std::move(fa.remained), k_lifeline_handle_arg, 1);
    auto fa3 = filter_option(std::move(fa2.remained), k_lifeline_exit_code_arg, 1);
    auto fa4 = filter_option(std::move(fa3.remained), k_lifeline_timeout_arg, 1);
    auto fa5 = filter_option(std::move(fa4.remained), k_lifeline_disable_arg, 0);

    m_recovery_cmd = join_strings(fa5.remained, " ") + " --recovery_occurrence " +
        std::to_string(recovery_occurrence_value+1);

    auto option_value = [&](const std::string& arg, const char* long_name, char short_name, bool requires_value, int& index) -> std::optional<std::string> {
        const std::string long_prefix = std::string(long_name) + "=";

        if (arg == long_name) {
            if (!requires_value) {
                return std::string();
            }
            if (index + 1 >= argc) {
                throw 1;
            }
            return std::string(argv[++index]);
        }

        if (requires_value && arg.rfind(long_prefix, 0) == 0) {
            return arg.substr(long_prefix.size());
        }

        if (short_name != '\0' && arg.size() >= 2 && arg[0] == '-' && arg[1] == short_name) {
            if (!requires_value) {
                return std::string();
            }

            if (arg.size() > 2) {
                if (arg[2] == '=') {
                    return arg.substr(3);
                }
                return arg.substr(2);
            }

            if (index + 1 >= argc) {
                throw 1;
            }
            return std::string(argv[++index]);
        }

        return std::nullopt;
    };

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--help" || arg == "-h") {
                throw 1;
            }

            if (auto value = option_value(arg, "--branch_index", 'a', true, i)) {
                try {
                    branch_index_arg = *value;
                    s_branch_index = static_cast<int32_t>(std::stol(*value));
                }
                catch (...) {
                    throw 1;
                }

                if (s_branch_index < 1) {
                    throw 1;
                }

                continue;
            }

            if (auto value = option_value(arg, "--swarm_id", 'b', true, i)) {
                try {
                    swarm_id_arg = *value;
                    m_swarm_id = static_cast<decltype(m_swarm_id)>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--instance_id", 'c', true, i)) {
                try {
                    instance_id_arg = *value;
                    m_instance_id = static_cast<decltype(m_instance_id)>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--coordinator_id", 'd', true, i)) {
                try {
                    coordinator_id_arg = *value;
                    s_coord_id = static_cast<instance_id_type>(std::stoull(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            if (auto value = option_value(arg, "--recovery_occurrence", 'e', true, i)) {
                try {
                    recovery_arg = *value;
                    s_recovery_occurrence = static_cast<uint32_t>(std::stoul(*value));
                }
                catch (...) {
                    throw 1;
                }
                continue;
            }

            // Lifeline arguments - these are internal and not shown in help
            // Long-option only to avoid collisions with user args
            if (auto value = option_value(arg, k_lifeline_handle_arg, '\0', true, i)) {
                s_lifeline_handle_value = *value;
                continue;
            }

            if (auto value = option_value(arg, k_lifeline_exit_code_arg, '\0', true, i)) {
                try {
                    s_lifeline_exit_code = std::stoi(*value);
                }
                catch (...) {
                    // Invalid value - use default
                }
                continue;
            }

            if (auto value = option_value(arg, k_lifeline_timeout_arg, '\0', true, i)) {
                try {
                    s_lifeline_timeout_ms = std::stoi(*value);
                    if (s_lifeline_timeout_ms < 0) {
                        s_lifeline_timeout_ms = 0;
                    }
                }
                catch (...) {
                    // Invalid value - use default
                }
                continue;
            }

            // Note: --lifeline_disable is a flag (no value required)
            if (auto value = option_value(arg, k_lifeline_disable_arg, '\0', false, i)) {
                s_lifeline_disabled = true;
                continue;
            }

            if (!arg.empty() && arg[0] == '-') {
                // Ignore unknown options so the examples can run under environments
                // that inject additional debugger flags (e.g. Visual Studio).
                continue;
            }
        }
    }
    catch(...) {
        Log_stream(log_level::error)
            << "\nManaged process options:\n"
            << "  --help                   (optional) produce help message and exit\n"
            << "  --branch_index arg       used by the coordinator process, when it invokes\n"
            << "                           itself\n"
            << "                           with a different entry index. It must be 1 or\n"
            << "                           greater.\n"
            << "  --swarm_id arg           unique identifier of the swarm that is being joined\n"
            << "  --instance_id arg        the instance id assigned to the new process\n"
            << "  --coordinator_id arg     the instance id of the coordinator that this\n"
            << "                           process should refer to\n"
            << "  --recovery_occurrence arg (optional) number of times the process recovered an\n"
            << "                           abnormal termination.\n";
        exit(1);
    }

    bool coordinator_is_local = false;
    if (swarm_id_arg.empty()) {
        s_mproc_id = m_instance_id = make_process_instance_id();

        m_swarm_id = 
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_time_instantiated.time_since_epoch()
            ).count();
        coordinator_is_local = true;

        // NOTE: s_branch_index remains uninitialized here for the coordinator.
        // This is safe because:
        // 1. The coordinator does not have a branch entry (no entry function index)
        // 2. s_branch_index is only used in the non-coordinator path of init() at
        //    lines 1149-1159, which requires branch_index_arg to be provided
        // 3. For the coordinator, s_branch_index is explicitly set to 0 in branch()
        //    (line 1570) before any subsequent use
        // 4. Non-coordinator processes always receive --branch_index, which sets
        //    s_branch_index at line 1047 before it's used
    }
    else {
        if (coordinator_id_arg.empty() || (branch_index_arg.empty() && instance_id_arg.empty()) ) {

            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }

        if (!branch_index_arg.empty()) {
            assert(s_branch_index < max_process_index - 1);
        }

        assert(m_instance_id != 0);

        if (instance_id_arg.empty()) {
            // If branch_index is specified, the explicit instance_id may not, thus
            // we need to make a process instance id based on the branch_index.
            // Transceiver IDs start from 2 (0 is invalid, 1 is the first process)
            m_instance_id = make_process_instance_id(s_branch_index + 2);
        }

        s_mproc_id = m_instance_id;
    }

    if (!coordinator_is_local) {
        start_lifeline_watcher(Lifetime_policy{}, true);
    }
    m_directory = obtain_swarm_directory();

    const auto abi_path = std::filesystem::path(m_directory) / detail::abi_marker_filename();
    const std::string current_abi = detail::abi_token();

    if (coordinator_is_local) {
        run_marker_record run_marker{};
        run_marker.pid = static_cast<uint32_t>(m_pid);
        run_marker.start_stamp = m_process_start_stamp;
        run_marker.created_monotonic_ns = monotonic_now_ns();
        run_marker.recovery_occurrence = static_cast<uint32_t>(s_recovery_occurrence);

        if (!write_run_marker(std::filesystem::path(m_directory), run_marker)) {
            throw std::runtime_error(
                "Sintra failed to persist the coordinator run marker at " + m_directory);
        }

        std::ofstream marker(abi_path, std::ios::out | std::ios::trunc);
        if (!marker) {
            throw std::runtime_error(
                "Sintra failed to write the ABI fingerprint file at " + abi_path.string());
        }
        marker << current_abi;
        marker.close();
        if (!marker) {
            throw std::runtime_error(
                "Sintra failed to persist the ABI fingerprint file at " + abi_path.string());
        }
    }
    else {
        std::ifstream marker(abi_path);
        if (!marker) {
            throw std::runtime_error(
                "Sintra could not read the coordinator ABI fingerprint at " + abi_path.string() +
                ". Ensure all processes start from the same swarm directory.");
        }
        std::string coordinator_abi;
        std::getline(marker, coordinator_abi);
        if (!marker.good() && !marker.eof()) {
            throw std::runtime_error(
                "Sintra encountered an error while reading the ABI fingerprint at " + abi_path.string());
        }

        if (coordinator_abi != current_abi) {
            throw std::runtime_error(
                std::string("Sintra ABI mismatch detected. The coordinator reports ") +
                detail::describe_abi_token(coordinator_abi) +
                ", while this process was built with " + detail::abi_description() +
                ". Mixing toolchains (for example MSVC and MinGW) is not supported.");
        }
    }

    m_out_req_c = new Message_ring_W(m_directory, "req", m_instance_id, s_recovery_occurrence);
    m_out_rep_c = new Message_ring_W(m_directory, "rep", m_instance_id, s_recovery_occurrence);

    if (coordinator_is_local) {
        s_coord = new Coordinator;
        s_coord_id = s_coord->m_instance_id;

        {
            lock_guard<mutex> lock(s_coord->m_publish_mutex);
            s_coord->m_transceiver_registry[s_mproc_id];
        }
    }

    {
        Dispatch_unique_lock readers_lock(m_readers_mutex);
        assert(!m_readers.count(process_of(s_coord_id)));
        auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
        auto reader = std::make_shared<Process_message_reader>(
            process_of(s_coord_id), progress, 0u);
        auto [reader_it, inserted] = m_readers.emplace(process_of(s_coord_id), reader);
        assert(inserted == true);
        reader->wait_until_ready();
    }

    // Up to this point, there was no infrastructure for a proper construction
    // of Transceiver base.

    this->Derived_transceiver<Managed_process>::construct("", m_instance_id);

    auto published_handler = [this](const Coordinator::instance_published& msg)
    {
        tn_type tn = {msg.type_id, msg.assigned_name};

        while (true) {
            function<void()> next_call;
            {
                lock_guard<mutex> lock(m_availability_mutex);
                const bool has_call = m_queued_availability_calls.with_lock(
                    [&](auto& queued_calls) {
                        auto it = queued_calls.find(tn);
                        if (it == queued_calls.end()) {
                            return false;
                        }

                        if (it->second.empty()) {
                            queued_calls.erase(it);
                            return false;
                        }

                        next_call = it->second.front();
                        return true;
                    });
                if (!has_call) {
                    return;
                }
            }

            // each function call, which is a lambda defined inside
            // call_on_availability(), clears itself from the list as well.
            next_call();
        }
    };

    auto unpublished_handler = [this](const Coordinator::instance_unpublished& msg)
    {
        auto iid = msg.instance_id;
        auto process_iid = process_of(iid);
        if (iid == process_iid) {

            // the unpublished transceiver was a process, thus we should
            // remove all transceiver records who are known to live in it.

            auto name_map = m_instance_id_of_assigned_name.scoped();
            for (auto it = name_map.begin(); it != name_map.end();) {
                if (process_of(it->second) == iid) {
                    it = name_map.erase(it);
                }
                else {
                    ++it;
                }
            }

            s_mproc->unblock_rpc(iid);
        }

        // if the unpublished transceiver is the coordinator process, we have to stop.
        if (process_of(s_coord_id) == msg.instance_id) {
            // Coordinator process has unpublished - pause communication outside the handler
            // to avoid reentrancy into barrier machinery.
            s_mproc->run_after_current_handler([]{
                s_mproc->stop(); // idempotent
            });
        }
    };

    if (!s_coord) {
        activate(published_handler,   Typed_instance_id<Coordinator>(s_coord_id));
        activate(unpublished_handler, Typed_instance_id<Coordinator>(s_coord_id));
    }

    if (coordinator_is_local) {
        auto unpublish_notify_handler = [](const Managed_process::unpublish_transceiver_notify& msg)
        {
            if (s_coord) {
                s_coord->unpublish_transceiver_notify(msg.transceiver_instance_id);
            }
        };

        // Deduplication for terminated_abnormally: the coordinator can see the same
        // crash notification from both the crashed process ring and its own relay.
        // A per-process atomic flag suppresses concurrent duplicates without locks.
        static std::array<std::atomic<uint8_t>, max_process_index + 1> s_crash_inflight{};

        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            struct Crash_dedup_guard {
                std::atomic<uint8_t>* flag;
                ~Crash_dedup_guard()
                {
                    flag->store(0, std::memory_order_release);
                }
            };

            const auto crash_index = get_process_index(msg.sender_instance_id);
            if (crash_index == 0 || crash_index > static_cast<uint64_t>(max_process_index)) {
                Crash_info info;
                info.process_iid = msg.sender_instance_id;
                info.process_slot = static_cast<uint32_t>(crash_index);
                info.status = msg.status;
                s_coord->note_process_crash(info);
                s_coord->unpublish_transceiver(msg.sender_instance_id);
                s_coord->recover_if_required(info);
                return;
            }

            auto& inflight = s_crash_inflight[static_cast<size_t>(crash_index)];
            uint8_t expected = 0;
            if (!inflight.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                // Duplicate crash notification - already being handled
                return;
            }

            Crash_dedup_guard guard{&inflight};
            Crash_info info;
            info.process_iid = msg.sender_instance_id;
            info.process_slot = static_cast<uint32_t>(crash_index);
            info.status = msg.status;
            s_coord->note_process_crash(info);
            s_coord->unpublish_transceiver(msg.sender_instance_id);
            s_coord->recover_if_required(info);
        };
        activate<Managed_process>(unpublish_notify_handler, any_remote);
        activate<Managed_process>(cr_handler, any_remote);
    }
    else {
        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            // Remote coordinator crashed: fail outstanding RPCs and pause communication
            if (process_of(s_coord_id) == msg.sender_instance_id) {
                // 1) Wake any RPCs waiting on the coordinator so they fail fast.
                s_mproc->unblock_rpc(process_of(s_coord_id));
                // 2) Pause communication *after* this handler completes to avoid reentrancy.
                s_mproc->run_after_current_handler([]{
                    s_mproc->stop(); // idempotent
                });
            }
        };
        activate<Managed_process>(cr_handler, any_remote);
    }

    m_start_stop_mutex.lock();

    m_communication_state = COMMUNICATION_RUNNING;
    m_start_stop_mutex.unlock();
}

inline
Managed_process::Spawn_result Managed_process::spawn_swarm_process(
    const Spawn_swarm_process_args& s )
{
    assert(s_coord);
    Spawn_result result;
    result.binary_name = s.binary_name;
    result.instance_id = s.piid;
    result.errno_value = 0;

    auto args = s.args;
    args.insert(args.end(), {"--recovery_occurrence", std::to_string(s.occurrence)} );

    {
        Dispatch_unique_lock readers_lock(m_readers_mutex);

        // If a reader for this process id exists (from a previous crashed instance),
        // stop it and remove it before creating a fresh one for recovery.
        if (auto existing = m_readers.find(s.piid); existing != m_readers.end()) {
            if (existing->second) {
                existing->second->stop_and_wait(1.0);
            }
            m_readers.erase(existing);
        }

        auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
        auto reader = std::make_shared<Process_message_reader>(
            s.piid, progress, s.occurrence);
        auto [eit, inserted] = m_readers.emplace(s.piid, reader);
        assert(inserted == true);

        // Before spawning the new process, we have to assure that the
        // corresponding reading threads are up and running.
        reader->wait_until_ready();
    }

    bool spawn_ready = true;
    int spawn_error = 0;
    std::string spawn_error_message;
    Managed_process::lifeline_handle_type lifeline_write_handle =
        static_cast<Managed_process::lifeline_handle_type>(-1);
#ifdef _WIN32
    HANDLE lifeline_read_handle = nullptr;
    HANDLE lifeline_write_handle_win = nullptr;
#else
    int lifeline_read_fd = -1;
    int lifeline_write_fd = -1;
#endif

    int spawned_pid = -1;
    Spawn_detached_options spawn_options;
    spawn_options.prog = s.binary_name.c_str();
    spawn_options.child_pid_out = &spawned_pid;

    if (s.lifetime.enable_lifeline) {
#ifdef _WIN32
        if (!create_lifeline_pipe(lifeline_read_handle, lifeline_write_handle_win, &spawn_error)) {
            spawn_ready = false;
            spawn_error_message = "Failed to create lifeline pipe";
        }
        else {
            lifeline_write_handle =
                static_cast<Managed_process::lifeline_handle_type>(
                    reinterpret_cast<uintptr_t>(lifeline_write_handle_win));
            spawn_options.inherit_handles.push_back(lifeline_read_handle);
            const std::string handle_value =
                std::to_string(reinterpret_cast<uintptr_t>(lifeline_read_handle));

            // Add lifeline arguments (inserted into args, not env)
            args.push_back(k_lifeline_handle_arg);
            args.push_back(handle_value);
            args.push_back(k_lifeline_exit_code_arg);
            args.push_back(std::to_string(s.lifetime.hard_exit_code));
            args.push_back(k_lifeline_timeout_arg);
            args.push_back(std::to_string(s.lifetime.hard_exit_timeout_ms));
        }
#else
        if (!create_lifeline_pipe(lifeline_read_fd, lifeline_write_fd, &spawn_error)) {
            spawn_ready = false;
            spawn_error_message = "Failed to create lifeline pipe";
        }
        else {
            lifeline_write_handle =
                static_cast<Managed_process::lifeline_handle_type>(lifeline_write_fd);
            const std::string handle_value = std::to_string(lifeline_read_fd);

            // Add lifeline arguments (inserted into args, not env)
            args.push_back(k_lifeline_handle_arg);
            args.push_back(handle_value);
            args.push_back(k_lifeline_exit_code_arg);
            args.push_back(std::to_string(s.lifetime.hard_exit_code));
            args.push_back(k_lifeline_timeout_arg);
            args.push_back(std::to_string(s.lifetime.hard_exit_timeout_ms));
        }
#endif
    }
    else {
        // Lifeline disabled - add disable flag
        args.push_back(k_lifeline_disable_arg);
    }

    // Build argv after all internal arguments are appended
    cstring_vector cargs(std::move(args));
    spawn_options.argv = cargs.v();

    if (s_coord) {
        std::lock_guard<mutex> init_lock(s_coord->m_init_tracking_mutex);
        s_coord->m_processes_in_initialization.insert(s.piid);
    }

    if (spawn_ready) {
        result.success = spawn_detached(spawn_options);
    }
    else {
        result.success = false;
        result.errno_value = spawn_error;
        std::ostringstream error_msg;
        error_msg << spawn_error_message;
        if (spawn_error != 0) {
            std::error_code ec(spawn_error, std::system_category());
            error_msg << " (errno " << spawn_error << ": " << ec.message() << ')';
        }
        result.error_message = error_msg.str();
    }

#ifdef _WIN32
    if (lifeline_read_handle) {
        CloseHandle(lifeline_read_handle);
        lifeline_read_handle = nullptr;
    }
#else
    if (lifeline_read_fd >= 0) {
        ::close(lifeline_read_fd);
        lifeline_read_fd = -1;
    }
#endif

    if (result.success) {
#ifndef _WIN32
        if (spawned_pid > 0) {
            std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
            m_spawned_child_pids.push_back(static_cast<pid_t>(spawned_pid));
        }
#endif
        if (lifeline_handle_valid(lifeline_write_handle)) {
            std::lock_guard<std::mutex> guard(m_lifeline_mutex);
            auto it = m_lifeline_writes.find(s.piid);
            if (it != m_lifeline_writes.end()) {
                close_lifeline_handle(it->second);
                it->second = lifeline_write_handle;
            }
            else {
                m_lifeline_writes.emplace(s.piid, lifeline_write_handle);
            }
        }
        // Create an entry in the coordinator's transceiver registry.
        // This is essential for the implementation of publish_transceiver()
        {
            lock_guard<mutex> lock(s_coord->m_publish_mutex);
            s_coord->m_transceiver_registry[s.piid];
        }

        // create the readers. The next line will start the reader threads,
        // which might take some time. At this stage, we do not have to wait
        // until they are ready for messages.

        m_cached_spawns[s.piid] = s;
        m_cached_spawns[s.piid].occurrence++;
    }
    else {
        if (lifeline_handle_valid(lifeline_write_handle)) {
            close_lifeline_handle(lifeline_write_handle);
        }
        int saved_errno = 0;
#ifdef _WIN32
        if (result.errno_value != 0 || !result.error_message.empty()) {
            saved_errno = result.errno_value;
        }
        else {
            _get_errno(&saved_errno);
        }
#else
        saved_errno = result.errno_value != 0 ? result.errno_value : errno;
#endif
        if (result.error_message.empty()) {
            result.errno_value = saved_errno;
            std::ostringstream error_msg;
            error_msg << "Failed to spawn process";
            if (saved_errno != 0) {
                std::error_code ec(saved_errno, std::system_category());
                error_msg << " (errno " << saved_errno << ": " << ec.message() << ')';
            }
            result.error_message = error_msg.str();
        }

        // Log spawn failures to stderr
        if (!spawn_ready && !result.error_message.empty()) {
            Log_stream(log_level::error) << result.error_message << "\n";
        }
        else if (result.errno_value != 0) {
            std::error_code ec(result.errno_value, std::system_category());
            Log_stream(log_level::error)
                << "failed to launch " << s.binary_name
                << " (errno " << result.errno_value << ": " << ec.message() << ")\n";
        }
        else {
            Log_stream(log_level::error)
                << "failed to launch " << s.binary_name << "\n";
        }

        // Avoid destroying readers from within the request handler thread.
        run_after_current_handler([piid = s.piid]() {
            if (!s_mproc) {
                return;
            }
            Dispatch_unique_lock readers_lock(s_mproc->m_readers_mutex);
            s_mproc->m_readers.erase(piid);
        });

        if (s_coord) {
            s_coord->mark_initialization_complete(s.piid);
        }
    }

    return result;
}

inline
bool Managed_process::branch(vector<Process_descriptor>& branch_vector)
{
    // this function may only be called when a group of processes start.
    assert(!branch_vector.empty());

    using namespace sintra;

    // Variables for error tracking (used by coordinator)
    std::unordered_set<instance_id_type> successfully_spawned;
    std::vector<init_error::failed_process> spawn_failures;

    bool init_completion_notified = false;
    auto notify_init_complete = [&]() {
        if (init_completion_notified) {
            return;
        }
        init_completion_notified = true;

        if (s_coord) {
            s_coord->mark_initialization_complete(m_instance_id);
        }
        else if (s_coord_id != invalid_instance_id) {
            Coordinator::rpc_mark_initialization_complete(s_coord_id, m_instance_id);
        }
    };

    struct Init_completion_guard
    {
        std::function<void()> fn;
        ~Init_completion_guard()
        {
            if (fn) {
                fn();
            }
        }
    } init_guard{notify_init_complete};

    if (s_coord) {

        {
            std::lock_guard<mutex> init_lock(s_coord->m_init_tracking_mutex);
            s_coord->m_processes_in_initialization.insert(m_instance_id);
        }

        // 1. prepare the command line for each invocation
        auto it = branch_vector.begin();
        for (int i = 1; it != branch_vector.end(); it++, i++) {

            auto& options = it->sintra_options;
            if (it->entry.m_binary_name.empty()) {
                it->entry.m_binary_name = m_binary_name;
                options.insert(options.end(), { "--branch_index", std::to_string(i) });
            }
            options.insert(options.end(), {
                "--swarm_id",       std::to_string(m_swarm_id),
                "--instance_id",    std::to_string(it->assigned_instance_id = make_process_instance_id()),
                "--coordinator_id", std::to_string(s_coord_id)
            });
        }

        // 2. spawn
        it = branch_vector.begin();
        for (int i = 0; it != branch_vector.end(); it++, i++) {

            std::vector<std::string> all_args = {it->entry.m_binary_name.c_str()};
            all_args.insert(all_args.end(), it->sintra_options.begin(), it->sintra_options.end());
            all_args.insert(all_args.end(), it->user_options.begin(), it->user_options.end());

            auto result = spawn_swarm_process({it->entry.m_binary_name, all_args, it->assigned_instance_id});
            if (result.success) {
                successfully_spawned.insert(it->assigned_instance_id);
            }
            else {
                spawn_failures.emplace_back(
                    result.binary_name,
                    result.instance_id,
                    init_error::cause::spawn_failed,
                    result.errno_value,
                    result.error_message
                );
            }
        }

        auto all_processes = successfully_spawned;
        all_processes.insert(m_instance_id);

        m_group_all      = s_coord->make_process_group("_sintra_all_processes", all_processes);
        m_group_external = s_coord->make_process_group("_sintra_external_processes", successfully_spawned);
        notify_init_complete();

        s_branch_index = 0;
    }
    else {
        assert(s_branch_index != -1);
        assert( (ptrdiff_t)branch_vector.size() > s_branch_index-1);
        Process_descriptor& own_pd = branch_vector[s_branch_index-1];
        if (own_pd.entry.m_entry_function != nullptr) {
            m_entry_function = own_pd.entry.m_entry_function;
        }
        else {
            assert(!"if the binary was not invoked manually, this is definitely a bug.");
            exit(1);
        }

        notify_init_complete();
        m_group_all      = Coordinator::rpc_wait_for_instance(s_coord_id, "_sintra_all_processes");
        m_group_external = Coordinator::rpc_wait_for_instance(s_coord_id, "_sintra_external_processes");
    }

    // assign_name requires that all group processes are instantiated, in order
    // to receive the instance_published event
    if (s_recovery_occurrence == 0) {
        bool all_started = Process_group::rpc_barrier(m_group_all, UIBS);

        // If we're the coordinator and have failures to report, throw init_error
        if (s_coord && (!spawn_failures.empty() || !all_started)) {
            // If barrier failed, some successfully spawned processes didn't reach it
            if (!all_started && spawn_failures.empty()) {
                // All processes that we attempted to spawn but didn't reach the barrier
                for (const auto& iid : successfully_spawned) {
                    spawn_failures.emplace_back(
                        "", // We don't have binary name readily available here
                        iid,
                        init_error::cause::barrier_timeout,
                        0,
                        "Process spawned successfully but did not reach initialization barrier (may have crashed during startup)"
                    );
                }
            }

            // Collect successfully spawned instance IDs to include in exception
            std::vector<instance_id_type> successful_list(
                successfully_spawned.begin(),
                successfully_spawned.end()
            );
            successful_list.push_back(m_instance_id); // Include coordinator

            notify_init_complete();
            throw init_error(std::move(spawn_failures), std::move(successful_list));
        }

        if (!all_started) {
            notify_init_complete();
            return false;
        }
    }

    return true;
}

inline
void Managed_process::release_lifeline(instance_id_type process_instance_id)
{
    std::lock_guard<mutex> lock(m_lifeline_mutex);
    auto it = m_lifeline_writes.find(process_instance_id);
    if (it == m_lifeline_writes.end()) {
        return;
    }
    close_lifeline_handle(it->second);
    m_lifeline_writes.erase(it);
}

inline
void Managed_process::release_all_lifelines()
{
    std::lock_guard<mutex> lock(m_lifeline_mutex);
    for (auto& entry : m_lifeline_writes) {
        close_lifeline_handle(entry.second);
    }
    m_lifeline_writes.clear();
}

inline
void Managed_process::go()
{
    assign_name(std::string("sintra_process_") + std::to_string(m_pid));

    m_entry_function();
}

 //////////////////////////////////////////////////////////////////////////
///// BEGIN START/STOP /////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//

inline
void Managed_process::pause()
{
    std::lock_guard<mutex> start_stop_lock(m_start_stop_mutex);

    // pause() might be called when the entry function finishes execution,
    // explicitly from one of the handlers, or from the entry function itself.
    // If called when the process is already paused, this should not have any
    // side effects.
    if (m_communication_state <= COMMUNICATION_PAUSED)
        return;

    {
        Dispatch_shared_lock readers_lock(m_readers_mutex);
        for (auto& entry : m_readers) {
            if (auto& reader = entry.second) {
                reader->pause();
            }
        }
    }

    m_communication_state = COMMUNICATION_PAUSED;
    m_start_stop_condition.notify_all();
    m_delivery_condition.notify_all();
}

inline
void Managed_process::stop()
{
    std::lock_guard<mutex> start_stop_lock(m_start_stop_mutex);

    // stop() might be called explicitly from one of the handlers, or from the
    // entry function. If called when the process is already stopped, this
    // should not have any side effects.
    if (m_communication_state == COMMUNICATION_STOPPED)
        return;

    {
        Dispatch_shared_lock readers_lock(m_readers_mutex);
        for (auto& entry : m_readers) {
            if (auto& reader = entry.second) {
                reader->stop_nowait();
            }
        }
    }

    m_communication_state = COMMUNICATION_STOPPED;
    m_start_stop_condition.notify_all();
    m_delivery_condition.notify_all();
}

inline
void Managed_process::wait_for_stop()
{
    std::unique_lock<mutex> start_stop_lock(m_start_stop_mutex);
    while (m_communication_state == COMMUNICATION_RUNNING) {
        m_start_stop_condition.wait(start_stop_lock);
    }
}

  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END START/STOP ///////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////

inline
std::string Managed_process::obtain_swarm_directory()
{
    const std::filesystem::path sintra_directory = std::filesystem::temp_directory_path() / "sintra";
    if (!check_or_create_directory(sintra_directory.string())) {
        throw std::runtime_error("access to a working directory failed");
    }

    cleanup_stale_swarm_directories(
        sintra_directory,
        static_cast<uint32_t>(m_pid),
        m_process_start_stamp);

    std::stringstream stream;
    stream << std::hex << m_swarm_id;
    const std::filesystem::path swarm_directory = sintra_directory / stream.str();
    if (!check_or_create_directory(swarm_directory.string())) {
        throw std::runtime_error("access to a working directory failed");
    }

    return swarm_directory.string();
}

// Calls f when the specified transceiver becomes available.
// if the transceiver is available, f is invoked immediately.
template <typename T>
function<void()> Managed_process::call_on_availability(Named_instance<T> transceiver, function<void()> f)
{
    lock_guard<mutex> lock(m_availability_mutex);

    std::string transceiver_name = transceiver;
    auto iid = Typed_instance_id<T>(get_instance_id(std::string(transceiver_name)));

    //if the transceiver is available, call f and skip the queue
    if (iid.id != invalid_instance_id) {
        f();

        // it's done - there is nothing to disable, thus returning an empty function.
        return []() {};
    }

    tn_type tn = { get_type_id<T>(), std::move(transceiver_name) };

    // insert an empty function, in order to be able to capture the iterator within it
    using Call_list_iterator = list<function<void()>>::iterator;
    Call_list_iterator f_it = m_queued_availability_calls.with_lock(
        [&](auto& queued_calls) {
            auto& call_list = queued_calls[tn];
            call_list.emplace_back();
            return std::prev(call_list.end());
        });

    struct availability_call_state {
        bool active = true;
        Call_list_iterator iterator;
    };

    auto state = std::make_shared<availability_call_state>();
    state->iterator = f_it;

    auto mark_completed = [this, tn, state](bool erase_empty_entry) -> bool {
        if (!state->active) {
            state->iterator = decltype(state->iterator){};
            return false;
        }

        bool completed = false;
        bool found_queue = false;
        m_queued_availability_calls.with_lock([&](auto& queued_calls) {
            auto queue_it = queued_calls.find(tn);
            if (queue_it == queued_calls.end()) {
                return;
            }

            found_queue = true;
            auto call_it = state->iterator;
            queue_it->second.erase(call_it);
            if (erase_empty_entry && queue_it->second.empty()) {
                queued_calls.erase(queue_it);
            }
            completed = true;
        });

        if (!found_queue) {
            state->active = false;
            state->iterator = decltype(state->iterator){};
            return false;
        }
        state->active = false;
        state->iterator = decltype(state->iterator){};
        return completed;
    };

    // this is the abort call
    auto ret = Adaptive_function([this, state, mark_completed]() {
        std::lock_guard<std::mutex> lock(m_availability_mutex);
        mark_completed(true);
    });

    // and this is the actual call, which besides calling f, also neutralizes the
    // returned abort calls and marks this entry as completed.
    *f_it = [this, f, ret, mark_completed]() mutable {
        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(m_availability_mutex);
            completed = mark_completed(false);
        }
        // Release the mutex before invoking the callback so user code can
        // safely register additional availability handlers without
        // encountering recursive locking.
        if (!completed) {
            return;
        }

        ret.set([]() {});
        f();
    };

    return ret;
}

inline
void Managed_process::wait_until_all_external_readers_are_done(int extra_allowed_readers)
{
    unique_lock<mutex> lock(m_num_active_readers_mutex);
    while (m_num_active_readers > 2 + extra_allowed_readers) {
        m_num_active_readers_condition.wait(lock);
    }
}

inline void Managed_process::unpublish_all_transceivers()
{
    std::vector<Transceiver*> to_destroy;

    // Hold the spinlock during iteration to prevent concurrent modification.
    // Using scoped() ensures the map isn't modified while we build the list.
    {
        auto scoped_map = m_local_pointer_of_instance_id.scoped();
        to_destroy.reserve(scoped_map.get().size());

        for (auto& entry : scoped_map.get()) {
            auto iid = entry.first;
            auto* transceiver = entry.second;
            if (!transceiver || iid == m_instance_id) {
                continue;
            }

            to_destroy.push_back(transceiver);
        }
    }

    for (auto* transceiver : to_destroy) {
        if (transceiver) {
            transceiver->destroy();
        }
    }
}

inline void Managed_process::flush(instance_id_type process_id, sequence_counter_type flush_sequence)
{
    assert(is_process(process_id));

    std::shared_ptr<Process_message_reader> reader;
    sequence_counter_type rs = invalid_sequence;
    {
        Dispatch_shared_lock readers_lock(m_readers_mutex);
        auto it = m_readers.find(process_id);
        if (it == m_readers.end()) {
            throw std::logic_error(
                "attempted to flush the channel of a process which is not being read"
            );
        }

        // Barrier completion messages are RPC responses sent on the reply ring.
        // Check the reply reading sequence, not the request reading sequence.
        reader = it->second;
        if (!reader) {
            throw std::logic_error(
                "attempted to flush the channel of a process without an active reader"
            );
        }
        rs = reader->get_reply_reading_sequence();
    }

    if (rs >= flush_sequence) {
        return;
    }

    std::unique_lock<mutex> flush_lock(m_flush_sequence_mutex);
    m_flush_sequence.push_back(flush_sequence);

    while (reader->get_reply_reading_sequence() < flush_sequence &&
           m_communication_state == COMMUNICATION_RUNNING)
    {
        m_flush_sequence_condition.wait_for(
            flush_lock,
            std::chrono::milliseconds(500));
    }

    while (!m_flush_sequence.empty() &&
           m_flush_sequence.front() <= flush_sequence)
    {
        m_flush_sequence.pop_front();
    }
}

inline void Managed_process::run_after_current_handler(function<void()> task)
{
    if (!task) {
        return;
    }

    if (!tl_is_req_thread) {
        task();
        return;
    }

    if (!tl_post_handler_function_ready()) {
        tl_post_handler_function_ref() = std::move(task);
        return;
    }

    auto previous = std::move(*tl_post_handler_function);
    tl_post_handler_function_ref() = [prev = std::move(previous), task = std::move(task)]() mutable {
        if (prev) {
            prev();
        }
        task();
    };
}

inline
void Managed_process::wait_for_delivery_fence()
{
    std::vector<Process_message_reader::Delivery_target> targets;

    {
        Dispatch_shared_lock readers_lock(m_readers_mutex);
        targets.reserve(m_readers.size() * 2);

        for (auto& [process_id, reader_ptr] : m_readers) {
            (void)process_id;
            if (!reader_ptr) {
                continue;
            }

            auto& reader = *reader_ptr;
            if (reader.state() != Process_message_reader::READER_NORMAL) {
                continue;
            }

            const auto req_target = reader.get_request_leading_sequence();
            auto req_target_info = reader.prepare_delivery_target(
                Process_message_reader::Delivery_stream::Request,
                req_target);
            if (req_target_info.wait_needed) {
                targets.emplace_back(std::move(req_target_info));
            }

            const auto rep_target = reader.get_reply_leading_sequence();
            auto rep_target_info = reader.prepare_delivery_target(
                Process_message_reader::Delivery_stream::Reply,
                rep_target);
            if (rep_target_info.wait_needed) {
                targets.emplace_back(std::move(rep_target_info));
            }
        }
    }

    if (targets.empty()) {
        return;
    }

    auto all_targets_satisfied = [&]() {
        if (m_communication_state != COMMUNICATION_RUNNING) {
            return true;
        }

        for (const auto& target : targets) {
            auto progress = target.progress.lock();
            if (!progress) {
                // Reader was replaced or destroyed; treat as satisfied because
                // no further progress is possible on the captured stream.
                continue;
            }

            const auto observed = (target.stream == Process_message_reader::Delivery_stream::Request)
                ? progress->request_sequence.load()
                : progress->reply_sequence.load();

            if (observed >= target.target) {
                continue;
            }

            const auto stopped = (target.stream == Process_message_reader::Delivery_stream::Request)
                ? progress->request_stopped.load()
                : progress->reply_stopped.load();

            if (stopped) {
                continue;
            }

            return false;
        }

        return true;
    };

    if (all_targets_satisfied()) {
        return;
    }

    std::unique_lock<std::mutex> lk(m_delivery_mutex);

    if (!tl_is_req_thread) {
        m_delivery_condition.wait(lk, all_targets_satisfied);
        return;
    }

    while (!all_targets_satisfied()) {
        if (tl_post_handler_function_ready()) {
            auto post_handler = std::move(*tl_post_handler_function);
            tl_post_handler_function_clear();

            lk.unlock();
            Post_handler_guard post_guard;
            try {
                post_handler();
            }
            catch (...) {
                lk.lock();
                throw;
            }
            lk.lock();

            continue;
        }

        // Spurious wake-ups are possible here, so re-evaluate the delivery
        // targets on each iteration rather than relying on the condition's
        // predicate form. This keeps the post-handler draining path symmetric
        // with the non-request thread case.
        m_delivery_condition.wait(lk);
    }
}

inline void Managed_process::notify_delivery_progress()
{
    // Acquire the mutex before notifying to prevent lost notifications.
    // Even though the shared state (progress sequences) is atomic, we must
    // hold the mutex when notifying to ensure the notification doesn't arrive
    // before a waiter transitions from predicate-check to wait-state.
    // See: wait_for_delivery_fence() which waits on this condition variable.
    std::lock_guard<std::mutex> lk(m_delivery_mutex);
    m_delivery_condition.notify_all();
}

inline
size_t Managed_process::unblock_rpc(instance_id_type process_instance_id)
{
    assert(!process_instance_id || is_process(process_instance_id));
    size_t ret = 0;
    unique_lock<mutex> ol(s_outstanding_rpcs_mutex());
    if (!s_outstanding_rpcs().empty()) {

        for (auto& c : s_outstanding_rpcs()) {
            unique_lock<mutex> il(c->keep_waiting_mutex);

            if (process_instance_id != invalid_instance_id &&
                process_of(c->remote_instance) == process_instance_id)
            {
                c->success = false;
                c->keep_waiting = false;
                c->cancelled = true;
                c->keep_waiting_condition.notify_one();
                ret++;
            }
        }
    }
    return ret;
}

} // sintra


