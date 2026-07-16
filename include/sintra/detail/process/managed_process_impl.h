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
#include <limits>
#include <vector>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
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

using std::unique_lock;

extern thread_local bool tl_is_req_thread;

#ifndef _WIN32
namespace detail {
namespace test_hooks {

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Child_reaped_callback = void (*)(pid_t, int) noexcept;
inline std::atomic<Child_reaped_callback> s_child_reaped{nullptr};
#endif

} // namespace test_hooks

inline void child_reaped_for_test(pid_t pid, int status) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_child_reaped.load(std::memory_order_acquire)) {
        callback(pid, status);
    }
#else
    (void)pid;
    (void)status;
#endif
}

} // namespace detail
#endif

namespace detail {

inline Managed_child_exit_subscription_state::
Managed_child_exit_subscription_state(
    std::weak_ptr<Managed_child_custody_record> custody,
    Managed_child_occurrence_identity          occurrence,
    Managed_child_exit_callback                callback)
:
    m_custody(std::move(custody)),
    m_occurrence(occurrence),
    m_callback(std::move(callback))
{}

inline void Managed_child_exit_subscription_state::deliver(
    const Managed_child_exit& event) noexcept
{
    Managed_child_exit_callback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_active || !m_callback) {
            return;
        }
        m_active = false;
        m_callback_running = true;
        m_callback_thread = std::this_thread::get_id();
        callback = std::move(m_callback);
    }

    const bool previous_callback_context =
        tl_in_managed_child_exit_callback;
    tl_in_managed_child_exit_callback = true;
    try {
        callback(event);
    }
    catch (const std::exception& error) {
        Log_stream(log_level::error)
            << "Managed-child exit callback threw for process_instance_id="
            << static_cast<unsigned long long>(
                event.occurrence.process_instance_id)
            << " occurrence="
            << event.occurrence.occurrence
            << " custody_identity="
            << event.occurrence.custody_identity
            << " message='"
            << error.what()
            << "'\n";
    }
    catch (...) {
        Log_stream(log_level::error)
            << "Managed-child exit callback threw for process_instance_id="
            << static_cast<unsigned long long>(
                event.occurrence.process_instance_id)
            << " occurrence="
            << event.occurrence.occurrence
            << " custody_identity="
            << event.occurrence.custody_identity
            << "\n";
    }
    tl_in_managed_child_exit_callback = previous_callback_context;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback_running = false;
        m_callback_thread = {};
    }
    m_quiesced.notify_all();
}

inline void Managed_child_exit_subscription_state::unsubscribe() noexcept
{
    bool called_from_callback = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_callback = {};
        called_from_callback =
            m_callback_running && m_callback_thread == std::this_thread::get_id();
    }

    remove_from_occurrence();
    if (called_from_callback) {
        return;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_quiesced.wait(lock, [this]() { return !m_callback_running; });
}

inline void Managed_child_exit_subscription_state::
remove_from_occurrence() noexcept
{
    const auto custody = m_custody.lock();
    if (!custody) {
        return;
    }

    std::lock_guard<std::mutex> lock(custody->mutex);
    auto* occurrence = custody->find_occurrence_locked(
        m_occurrence.process_instance_id,
        m_occurrence.occurrence);
    if (!occurrence) {
        return;
    }
    auto& subscriptions = occurrence->exit_subscriptions;
    subscriptions.erase(
        std::remove_if(
            subscriptions.begin(),
            subscriptions.end(),
            [this](const auto& candidate) {
                return candidate.get() == this;
            }),
        subscriptions.end());
}

inline Managed_child_exit make_managed_child_exit(
    Managed_child_occurrence_identity occurrence,
    std::uint32_t                     wait_status,
    bool                              wait_status_available) noexcept
{
    Managed_child_exit event;
    event.occurrence              = occurrence;
    event.native_status           = wait_status;
    event.native_status_available = wait_status_available;
    if (!wait_status_available) {
        return event;
    }

#ifdef _WIN32
    event.status_kind = Managed_child_exit_status_kind::exited;
    event.status = wait_status;
#else
    const auto native_status = static_cast<int>(wait_status);
    if (WIFEXITED(native_status)) {
        event.status_kind = Managed_child_exit_status_kind::exited;
        event.status = WEXITSTATUS(native_status);
    }
    else
    if (WIFSIGNALED(native_status)) {
        event.status_kind = Managed_child_exit_status_kind::signaled;
        event.status = WTERMSIG(native_status);
    }
#endif
    return event;
}

inline Managed_child_occurrence_identity make_managed_child_occurrence_identity(
    uint64_t                               custody_identity,
    const Managed_child_occurrence_record& occurrence) noexcept
{
    Managed_child_occurrence_identity identity;
    identity.process_instance_id = occurrence.process_instance_id;
    identity.occurrence          = occurrence.occurrence;
    identity.custody_identity    = custody_identity;
    return identity;
}

inline Managed_child_exit_publication record_managed_child_exit_locked(
    uint64_t                         custody_identity,
    Managed_child_occurrence_record& occurrence,
    std::uint32_t                    wait_status,
    bool                             wait_status_available)
{
    Managed_child_exit_publication publication;
    const bool was_exited = occurrence.native.exited();
    publication.transition_valid = occurrence.native.note_exit(
        wait_status,
        wait_status_available);
    if (!publication.transition_valid || was_exited) {
        return publication;
    }

    publication.event = make_managed_child_exit(
        make_managed_child_occurrence_identity(custody_identity, occurrence),
        wait_status,
        wait_status_available);
    publication.subscriptions.swap(occurrence.exit_subscriptions);
    return publication;
}

namespace test_hooks {

enum class Managed_child_invariant
{
    exact_occurrence_lookup,
    posix_reap_reservation_lookup
};

inline constexpr const char* managed_child_invariant_name(
    Managed_child_invariant invariant) noexcept
{
    switch (invariant) {
    case Managed_child_invariant::exact_occurrence_lookup:
        return "managed_child_exact_occurrence_lookup";
    case Managed_child_invariant::posix_reap_reservation_lookup:
        return "managed_child_posix_reap_reservation_lookup";
    }
    return "managed_child_unknown_invariant";
}

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Managed_child_setup_callback = void (*)(instance_id_type, uint32_t);
inline std::atomic<Managed_child_setup_callback> s_managed_child_reader_setup{nullptr};

using Managed_child_failure_callback = bool (*)(const char*, instance_id_type, uint32_t) noexcept;
inline std::atomic<Managed_child_failure_callback> s_managed_child_failure{nullptr};

using Managed_child_roster_reserved_callback =
    void (*)(instance_id_type, uint32_t, uint64_t);
inline std::atomic<Managed_child_roster_reserved_callback>
    s_managed_child_roster_reserved{nullptr};

using Managed_child_invariant_callback =
    bool (*)(Managed_child_invariant, instance_id_type, uint32_t, uint64_t)
        noexcept;
inline std::atomic<Managed_child_invariant_callback>
    s_managed_child_invariant{nullptr};

using Managed_child_prepublication_cleanup_callback =
    void (*)(const char*, instance_id_type, uint32_t);
inline std::atomic<Managed_child_prepublication_cleanup_callback>
    s_managed_child_prepublication_cleanup{nullptr};

using Managed_child_cleanup_callback =
    void (*)(const char*, instance_id_type, uint32_t);
inline std::atomic<Managed_child_cleanup_callback>
    s_managed_child_cleanup{nullptr};

using Managed_child_transport_retirement_callback =
    void (*)(const char*, instance_id_type, uint32_t);
inline std::atomic<Managed_child_transport_retirement_callback>
    s_managed_child_transport_retirement{nullptr};

using Managed_child_custody_retirement_callback =
    void (*)(const char*, uint64_t) noexcept;
inline std::atomic<Managed_child_custody_retirement_callback>
    s_managed_child_custody_retirement{nullptr};
#endif

inline constexpr const char* k_managed_child_fail_pre_create_setup =
    "managed_child_pre_create_setup";
inline constexpr const char* k_managed_child_fail_post_native_setup =
    "managed_child_post_native_setup";
inline constexpr const char* k_managed_child_fail_native_observer_start =
    "managed_child_native_observer_start";
inline constexpr const char* k_managed_child_native_observer_before_wait =
    "managed_child_native_observer_before_wait";
inline constexpr const char*
    k_managed_child_native_observer_after_cancel =
        "managed_child_native_observer_after_cancel";
inline constexpr const char*
    k_managed_child_native_observer_before_registration =
        "managed_child_native_observer_before_registration";
inline constexpr const char*
    k_managed_child_native_observer_registered =
        "managed_child_native_observer_registered";
inline constexpr const char*
    k_managed_child_native_observer_registration_complete =
        "managed_child_native_observer_registration_complete";
inline constexpr const char*
    k_managed_child_fail_native_observer_after_registration =
        "managed_child_native_observer_after_registration";
inline constexpr const char* k_managed_child_fail_native_observer_before_wait =
    "managed_child_native_observer_before_wait";
inline constexpr const char* k_managed_child_fail_native_observer_wait =
    "managed_child_native_observer_wait";
inline constexpr const char* k_managed_child_native_observer_fallback_available =
    "managed_child_native_observer_fallback_available";
inline constexpr const char* k_managed_child_native_exit_before_publication =
    "managed_child_native_exit_before_publication";
inline constexpr const char* k_managed_child_windows_fallback_handle_closed =
    "managed_child_windows_fallback_handle_closed";
inline constexpr const char* k_managed_child_fail_admission_mapping =
    "managed_child_admission_mapping";
inline constexpr const char* k_managed_child_fail_release_worker =
    "managed_child_release_worker";
inline constexpr const char* k_managed_child_fail_release_worker_start =
    "managed_child_release_worker_start";
inline constexpr const char* k_managed_child_fail_cleanup_actions =
    "managed_child_cleanup_actions";
inline constexpr const char* k_managed_child_fail_communication_worker_start =
    "managed_child_communication_worker_start";
inline constexpr const char* k_managed_child_fail_exit_dispatcher_start =
    "managed_child_exit_dispatcher_start";
inline constexpr const char* k_managed_child_force_exit_status_unavailable =
    "managed_child_exit_status_unavailable";
inline constexpr const char* k_managed_child_prepublication_first_miss =
    "managed_child_prepublication_first_miss";
inline constexpr const char* k_managed_child_prepublication_reader_terminal =
    "managed_child_prepublication_reader_terminal";
inline constexpr const char* k_managed_child_cleanup_before_actions =
    "managed_child_cleanup_before_actions";
inline constexpr const char* k_managed_child_cleanup_lifeline_released =
    "managed_child_cleanup_lifeline_released";
inline constexpr const char* k_managed_child_cleanup_retirement_confirmed =
    "managed_child_cleanup_retirement_confirmed";
inline constexpr const char* k_managed_child_cleanup_before_native_convergence =
    "managed_child_cleanup_before_native_convergence";
inline constexpr const char* k_managed_child_cleanup_soft_termination =
    "managed_child_cleanup_soft_termination";
inline constexpr const char* k_managed_child_fail_native_hard_termination =
    "managed_child_native_hard_termination";
inline constexpr const char* k_managed_child_cleanup_hard_termination =
    "managed_child_cleanup_hard_termination";
inline constexpr const char* k_managed_child_cleanup_native_exit_confirmed =
    "managed_child_cleanup_native_exit_confirmed";
inline constexpr const char* k_managed_child_release_waiting_passive =
    "managed_child_release_waiting_passive";
inline constexpr const char* k_managed_child_communication_before_join =
    "managed_child_communication_before_join";
inline constexpr const char* k_managed_child_communication_after_join =
    "managed_child_communication_after_join";
inline constexpr const char*
    k_managed_child_communication_terminal_before_reader_erase =
        "managed_child_communication_terminal_before_reader_erase";
inline constexpr const char* k_managed_child_communication_join_incomplete =
    "managed_child_communication_join_incomplete";
inline constexpr const char* k_managed_child_custody_retirement_before_cache_erase =
    "managed_child_custody_retirement/before_cache_erase";
inline constexpr const char* k_managed_child_custody_retirement_complete =
    "managed_child_custody_retirement/complete";

} // namespace test_hooks

inline void managed_child_custody_retirement_for_test(
    const char* stage,
    uint64_t    custody_identity) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_custody_retirement.load(
            std::memory_order_acquire))
    {
        callback(stage, custody_identity);
    }
#else
    (void)stage;
    (void)custody_identity;
#endif
}

inline void managed_child_reader_setup_for_test(
    instance_id_type process_instance_id,
    uint32_t occurrence) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_reader_setup.load(
            std::memory_order_acquire))
    {
        callback(process_instance_id, occurrence);
    }
#else
    (void)process_instance_id;
    (void)occurrence;
#endif
}

inline bool managed_child_failure_selected_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_failure.load(
            std::memory_order_acquire))
    {
        return callback(stage, process_instance_id, occurrence);
    }
#else
    (void)stage;
    (void)process_instance_id;
    (void)occurrence;
#endif
    return false;
}

inline void managed_child_failure_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
    if (managed_child_failure_selected_for_test(
            stage, process_instance_id, occurrence))
    {
        throw std::runtime_error(
            std::string("Injected managed-child failure at ") + stage);
    }
}

inline void managed_child_roster_reserved_for_test(
    instance_id_type process_instance_id,
    uint32_t occurrence,
    uint64_t reservation_id)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS) && !defined(_WIN32)
    if (auto callback = test_hooks::s_managed_child_roster_reserved.load(
            std::memory_order_acquire))
    {
        callback(process_instance_id, occurrence, reservation_id);
    }
#else
    (void)process_instance_id;
    (void)occurrence;
    (void)reservation_id;
#endif
}

inline bool managed_child_invariant_for_test(
    test_hooks::Managed_child_invariant invariant,
    instance_id_type process_instance_id,
    uint32_t occurrence,
    uint64_t reservation_id = 0) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_invariant.load(
            std::memory_order_acquire))
    {
        return callback(
            invariant, process_instance_id, occurrence, reservation_id);
    }
#else
    (void)invariant;
    (void)process_instance_id;
    (void)occurrence;
    (void)reservation_id;
#endif
    return false;
}

inline void managed_child_prepublication_cleanup_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_prepublication_cleanup.load(
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

inline void managed_child_cleanup_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_cleanup.load(
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

inline void managed_child_transport_retirement_for_test(
    const char* stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_managed_child_transport_retirement.load(
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
    struct signal_slot_t
    {
        int sig;

        void (__cdecl* previous)(int) = SIG_DFL;
        bool has_previous = false;
    };

#ifdef SIGTRAP
    constexpr std::size_t k_signal_slot_count = 7;
#else
    constexpr std::size_t k_signal_slot_count = 6;
#endif

    inline std::array<signal_slot_t, k_signal_slot_count>& signal_slots()
    {
        static std::array<signal_slot_t, k_signal_slot_count> slot_table {{
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
        auto*      mproc                    = s_mproc;
        const bool should_wait_for_dispatch = mproc && mproc->m_out_req_c;
        const bool can_wait                 = can_wait_for_signal_dispatch();
        uint32_t   dispatched_before        = 0;
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
            std::thread(detail::Exception_boundary{"signal_dispatch_win"}.wrap(signal_dispatch_loop_win)).detach();
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
    struct signal_slot_t
    {
        int                sig;
        struct sigaction   previous{};
        bool               has_previous = false;
    };

#ifdef SIGTRAP
    constexpr std::size_t k_signal_slot_count = 8;
#else
    constexpr std::size_t k_signal_slot_count = 7;
#endif

    inline std::array<signal_slot_t, k_signal_slot_count>& signal_slots()
    {
        static std::array<signal_slot_t, k_signal_slot_count> slot_table {{
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
        return
            static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
            static_cast<uint64_t>(ts.tv_nsec);
    }

    inline void wait_for_signal_dispatch(uint32_t expected_count)
    {
        constexpr uint64_t k_wait_timeout_ns = 200'000'000ULL;
        uint64_t           start_ns          = signal_dispatch_now_ns();
        constexpr int      k_fallback_rounds = 5000;
        int                fallback_rounds   = (start_ns == 0) ? k_fallback_rounds : -1;

        // Exponential backoff: start responsive (32 pauses), grow to max (1024).
        // This reduces CPU usage during longer waits while staying responsive
        // for the common case where dispatch completes quickly.
        constexpr int k_initial_pause_count = 32;
        constexpr int k_max_pause_count     = 1024;
        int           pause_count           = k_initial_pause_count;

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
            else
            if (fallback_rounds > 0) {
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
            int     sig_number = 0;
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
            if (detail::call_pipe2(pipefd_local, O_CLOEXEC) != 0) {
                return;
            }

            const int flags = detail::fcntl_retry(pipefd_local[1], F_GETFL);
            if (flags == -1 ||
                detail::fcntl_retry(
                    pipefd_local[1], F_SETFL, flags | O_NONBLOCK) == -1)
            {
                const int saved_errno = errno;
                ::close(pipefd_local[0]);
                ::close(pipefd_local[1]);
                errno = saved_errno;
                return;
            }

            auto& pipefd = signal_pipe();
            pipefd[0] = pipefd_local[0];
            pipefd[1] = pipefd_local[1];

            std::thread(detail::Exception_boundary{"signal_dispatch"}.wrap(signal_dispatch_loop)).detach();
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
    inline signal_slot_t* find_slot(std::array<signal_slot_t, N>& slot_table, int sig)
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
    constexpr const char* k_lifeline_handle_arg    = "--lifeline_handle";
    constexpr const char* k_lifeline_exit_code_arg = "--lifeline_exit_code";
    constexpr const char* k_lifeline_timeout_arg   = "--lifeline_timeout_ms";
    constexpr const char* k_lifeline_disable_arg   = "--lifeline_disable";

    // Storage for parsed lifeline values (set during init, read by start_lifeline_watcher)
    static inline std::string s_lifeline_handle_value;
    static inline int  s_lifeline_exit_code  = 99;
    static inline int  s_lifeline_timeout_ms = 100;
    static inline bool s_lifeline_disabled   = false;

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

        // Arm the hard-exit watchdog before local teardown; stop() can block
        // behind lifecycle locks.
        schedule_lifeline_hard_exit(timeout_ms, exit_code);

        if (s_mproc) {
            s_mproc->m_must_stop.store(true, std::memory_order_release);
            s_mproc->stop();
            s_mproc->unblock_rpc();
        }
    }

#ifdef _WIN32
    inline void lifeline_watch_loop(HANDLE handle, int timeout_ms, int exit_code)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE) {
            signal_lifeline_shutdown(timeout_ms, exit_code);
            return;
        }

        char  buffer     = 0;
        DWORD bytes_read = 0;
        while (true) {
            const BOOL ok = ReadFile(handle, &buffer, 1, &bytes_read, nullptr);
            if (!ok)             { break; }
            if (bytes_read == 0) { break; }
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
            if (bytes_read > 0)  { continue; }
            if (bytes_read == 0) { break;    }
            if (errno == EINTR)  { continue; }
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
        char*                    end    = nullptr;
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
        const int exit_code  = s_lifeline_exit_code;
        int       timeout_ms = s_lifeline_timeout_ms;
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }
#ifdef _WIN32
        const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(parsed));
        std::thread(detail::Exception_boundary{"lifeline_watch"}.wrap(
            [=]{ lifeline_watch_loop(handle, timeout_ms, exit_code); })).detach();
#else
        const int fd = static_cast<int>(parsed);
        std::thread(detail::Exception_boundary{"lifeline_watch"}.wrap(
            [=]{ lifeline_watch_loop(fd, timeout_ms, exit_code); })).detach();
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

inline detail::Managed_child_launch_attempt::Managed_child_launch_attempt(
    Managed_process* owner,
    std::shared_ptr<Managed_child_custody_record> custody,
    instance_id_type process_instance_id,
    uint32_t occurrence) noexcept
    : m_setup_settlement(custody, process_instance_id, occurrence)
    , m_owner(owner)
    , m_custody(std::move(custody))
    , m_process_instance_id(process_instance_id)
    , m_occurrence(occurrence)
{}

inline detail::Managed_child_launch_attempt::Managed_child_launch_attempt(
    Managed_child_launch_attempt&& other) noexcept
    : m_setup_settlement(std::move(other.m_setup_settlement))
    , m_owner(other.m_owner)
    , m_custody(std::move(other.m_custody))
    , m_process_instance_id(other.m_process_instance_id)
    , m_occurrence(other.m_occurrence)
    , m_reader(other.m_reader)
    , m_initialization(other.m_initialization)
    , m_initialization_coordinator(other.m_initialization_coordinator)
#ifndef _WIN32
    , m_posix_reap(other.m_posix_reap)
    , m_posix_reap_reservation_id(other.m_posix_reap_reservation_id)
#else
    , m_windows_native(other.m_windows_native)
    , m_windows_process_handle(other.m_windows_process_handle)
#endif
    , m_lifeline_read_endpoint(other.m_lifeline_read_endpoint)
    , m_lifeline_write_endpoint(other.m_lifeline_write_endpoint)
{
    other.m_owner = nullptr;
    other.m_process_instance_id = invalid_instance_id;
    other.m_occurrence = 0;
    other.m_reader = Reader_ownership::absent;
    other.m_initialization = Initialization_ownership::absent;
    other.m_initialization_coordinator = nullptr;
#ifndef _WIN32
    other.m_posix_reap = Posix_reap_ownership::absent;
    other.m_posix_reap_reservation_id = 0;
#else
    other.m_windows_native = Windows_native_ownership::absent;
    other.m_windows_process_handle = 0;
#endif
    other.m_lifeline_read_endpoint = invalid_lifeline_endpoint;
    other.m_lifeline_write_endpoint = invalid_lifeline_endpoint;
}

inline detail::Managed_child_launch_attempt&
detail::Managed_child_launch_attempt::operator=(
    Managed_child_launch_attempt&& other) noexcept
{
    if (this != &other) {
        rollback();
        m_setup_settlement = std::move(other.m_setup_settlement);
        m_owner = other.m_owner;
        m_custody = std::move(other.m_custody);
        m_process_instance_id = other.m_process_instance_id;
        m_occurrence = other.m_occurrence;
        m_reader = other.m_reader;
        m_initialization = other.m_initialization;
        m_initialization_coordinator = other.m_initialization_coordinator;
#ifndef _WIN32
        m_posix_reap = other.m_posix_reap;
        m_posix_reap_reservation_id =
            other.m_posix_reap_reservation_id;
#else
        m_windows_native = other.m_windows_native;
        m_windows_process_handle = other.m_windows_process_handle;
#endif
        m_lifeline_read_endpoint = other.m_lifeline_read_endpoint;
        m_lifeline_write_endpoint = other.m_lifeline_write_endpoint;

        other.m_owner = nullptr;
        other.m_process_instance_id = invalid_instance_id;
        other.m_occurrence = 0;
        other.m_reader = Reader_ownership::absent;
        other.m_initialization = Initialization_ownership::absent;
        other.m_initialization_coordinator = nullptr;
#ifndef _WIN32
        other.m_posix_reap = Posix_reap_ownership::absent;
        other.m_posix_reap_reservation_id = 0;
#else
        other.m_windows_native = Windows_native_ownership::absent;
        other.m_windows_process_handle = 0;
#endif
        other.m_lifeline_read_endpoint = invalid_lifeline_endpoint;
        other.m_lifeline_write_endpoint = invalid_lifeline_endpoint;
    }
    return *this;
}

inline detail::Managed_child_launch_attempt::~Managed_child_launch_attempt() noexcept
{
    rollback();
}

inline void detail::Managed_child_launch_attempt::mark_reader_prepared() noexcept
{
    if (m_reader == Reader_ownership::absent) {
        m_reader = Reader_ownership::rollback_required;
    }
}

inline void detail::Managed_child_launch_attempt::transfer_reader() noexcept
{
    if (m_reader == Reader_ownership::rollback_required) {
        m_reader = Reader_ownership::transferred;
    }
}

inline bool detail::Managed_child_launch_attempt::lifeline_endpoint_valid(
    uintptr_t endpoint) const noexcept
{
#ifdef _WIN32
    return endpoint != 0 && endpoint != invalid_lifeline_endpoint;
#else
    return endpoint != invalid_lifeline_endpoint;
#endif
}

inline bool detail::Managed_child_launch_attempt::create_lifeline(int* error_out)
{
    if (lifeline_endpoint_valid(m_lifeline_read_endpoint) ||
        lifeline_endpoint_valid(m_lifeline_write_endpoint))
    {
        return false;
    }
#ifdef _WIN32
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!create_lifeline_pipe(read_handle, write_handle, error_out)) {
        return false;
    }
    m_lifeline_read_endpoint = reinterpret_cast<uintptr_t>(read_handle);
    m_lifeline_write_endpoint = reinterpret_cast<uintptr_t>(write_handle);
#else
    int read_fd = -1;
    int write_fd = -1;
    if (!create_lifeline_pipe(read_fd, write_fd, error_out)) {
        return false;
    }
    m_lifeline_read_endpoint = static_cast<uintptr_t>(read_fd);
    m_lifeline_write_endpoint = static_cast<uintptr_t>(write_fd);
#endif
    return true;
}

inline void
detail::Managed_child_launch_attempt::close_lifeline_read_endpoint() noexcept
{
    if (!lifeline_endpoint_valid(m_lifeline_read_endpoint)) {
        return;
    }
    close_lifeline_handle(m_lifeline_read_endpoint);
    m_lifeline_read_endpoint = invalid_lifeline_endpoint;
}

inline void
detail::Managed_child_launch_attempt::close_lifeline_write_endpoint() noexcept
{
    if (!lifeline_endpoint_valid(m_lifeline_write_endpoint)) {
        return;
    }
    close_lifeline_handle(m_lifeline_write_endpoint);
    m_lifeline_write_endpoint = invalid_lifeline_endpoint;
}

inline bool detail::Managed_child_launch_attempt::transfer_lifeline_write()
{
    if (!m_owner || !lifeline_endpoint_valid(m_lifeline_write_endpoint)) {
        return false;
    }
    std::lock_guard<std::mutex> guard(m_owner->m_lifeline_mutex);
    auto it = m_owner->m_lifeline_writes.find(m_process_instance_id);
    if (it != m_owner->m_lifeline_writes.end()) {
        close_lifeline_handle(it->second);
        it->second = m_lifeline_write_endpoint;
    }
    else {
        m_owner->m_lifeline_writes.emplace(
            m_process_instance_id, m_lifeline_write_endpoint);
    }
    m_lifeline_write_endpoint = invalid_lifeline_endpoint;
    return true;
}

inline bool
detail::Managed_child_launch_attempt::acquire_initialization_reservation(
    Coordinator* coordinator)
{
    if (!m_owner || !m_custody ||
        m_initialization != Initialization_ownership::absent)
    {
        return false;
    }

    const bool inject_exact_occurrence_lookup_failure =
        managed_child_invariant_for_test(
            test_hooks::Managed_child_invariant::exact_occurrence_lookup,
            m_process_instance_id,
            m_occurrence);

    bool coordinator_reserved = false;
    if (coordinator) {
        std::lock_guard init_lock(coordinator->m_init_tracking_mutex);
        coordinator->m_processes_in_initialization.insert(
            m_process_instance_id);
        coordinator_reserved = true;
    }

    bool occurrence_reserved = false;
    try {
        std::lock_guard<std::mutex> lock(m_custody->mutex);
        auto* exact = m_custody->find_occurrence_locked(
            m_process_instance_id, m_occurrence);
        if (!inject_exact_occurrence_lookup_failure &&
            exact)
        {
            exact->initialization_reservation_active = true;
            m_initialization = Initialization_ownership::rollback_required;
            m_initialization_coordinator = coordinator;
            occurrence_reserved = true;
        }
    }
    catch (...) {
        if (coordinator_reserved) {
            m_owner->mark_child_coordinator_initialization_complete(
                coordinator, m_process_instance_id);
        }
        throw;
    }
    if (!occurrence_reserved) {
        if (coordinator_reserved) {
            m_owner->mark_child_coordinator_initialization_complete(
                coordinator, m_process_instance_id);
        }
        return false;
    }
    m_custody->changed.notify_all();
    return true;
}

inline void
detail::Managed_child_launch_attempt::transfer_initialization_reservation() noexcept
{
    if (m_initialization == Initialization_ownership::rollback_required) {
        m_initialization = Initialization_ownership::transferred;
    }
}

#ifndef _WIN32
inline void detail::Managed_child_launch_attempt::reserve_posix_reap_slot()
{
    if (!m_owner || !m_custody ||
        m_posix_reap != Posix_reap_ownership::absent)
    {
        throw std::runtime_error(
            "Managed-child POSIX reap reservation is not available");
    }

    uint64_t reservation_id = 0;
    {
        std::lock_guard<std::mutex> roster_lock(
            m_owner->m_spawned_child_pids_mutex);
        reservation_id = m_owner->m_next_spawned_child_reservation++;
        m_owner->m_spawned_child_pids.push_back(
            {reservation_id,
             0,
             0,
             false,
             {m_custody, m_process_instance_id, m_occurrence}});
        m_posix_reap = Posix_reap_ownership::reserved;
        m_posix_reap_reservation_id = reservation_id;
    }

    managed_child_roster_reserved_for_test(
        m_process_instance_id, m_occurrence, reservation_id);
}

inline void
detail::Managed_child_launch_attempt::cancel_posix_native_handoff()
{
    if (!m_owner || !m_custody ||
        m_posix_reap != Posix_reap_ownership::reserved ||
        m_posix_reap_reservation_id == 0)
    {
        throw std::runtime_error(
            "Managed-child POSIX reap reservation is not owned");
    }

    const auto reservation_id = m_posix_reap_reservation_id;
    {
        std::lock_guard<std::mutex> roster_lock(
            m_owner->m_spawned_child_pids_mutex);
        std::lock_guard<std::mutex> custody_lock(m_custody->mutex);

        auto* occurrence = m_custody->find_occurrence_locked(
            m_process_instance_id, m_occurrence);
        if (!occurrence) {
            throw std::runtime_error(
                "Managed-child exact occurrence lookup failed");
        }
        if (!occurrence->native.confirm_absent()) {
            throw std::runtime_error(
                "Managed-child native authority was not absent");
        }

        const auto slot = std::find_if(
            m_owner->m_spawned_child_pids.begin(),
            m_owner->m_spawned_child_pids.end(),
            [&](const Managed_process::Spawned_child_reap_slot& candidate) {
                return candidate.reservation_id == reservation_id;
            });
        if (slot != m_owner->m_spawned_child_pids.end()) {
            m_owner->m_spawned_child_pids.erase(slot);
        }
        m_posix_reap = Posix_reap_ownership::absent;
        m_posix_reap_reservation_id = 0;
    }
    m_custody->changed.notify_all();
}

inline detail::Managed_child_launch_attempt::Posix_native_handoff_result
detail::Managed_child_launch_attempt::commit_posix_native_handoff(
    int pid,
    bool already_reaped,
    bool wait_status_available,
    int wait_status)
{
    if (!m_owner || !m_custody ||
        m_posix_reap != Posix_reap_ownership::reserved ||
        m_posix_reap_reservation_id == 0)
    {
        throw std::runtime_error(
            "Managed-child POSIX reap reservation is not owned");
    }
    if (pid <= 0) {
        throw std::runtime_error(
            "Managed-child POSIX native identity is invalid");
    }

    const bool inject_reservation_lookup_failure =
        managed_child_invariant_for_test(
            test_hooks::Managed_child_invariant::
                posix_reap_reservation_lookup,
            m_process_instance_id,
            m_occurrence,
            m_posix_reap_reservation_id);

    bool start_stamp_available = false;
    uint64_t start_stamp = 0;
    if (!already_reaped) {
        try {
            if (auto stamp = query_process_start_stamp(
                    static_cast<uint32_t>(pid)))
            {
                start_stamp = *stamp;
                start_stamp_available = true;
            }
        }
        catch (...) {
            // Native ownership is more important than an optional start stamp.
        }
    }

    Posix_native_handoff_result result;
    const auto reservation_id = m_posix_reap_reservation_id;

    {
        // The roster is always acquired before custody so the central reaper
        // cannot observe a filled slot until exact native state is committed.
        std::lock_guard<std::mutex> roster_lock(
            m_owner->m_spawned_child_pids_mutex);
        std::lock_guard<std::mutex> custody_lock(m_custody->mutex);

        auto* occurrence = m_custody->find_occurrence_locked(
            m_process_instance_id, m_occurrence);
        if (!occurrence) {
            throw std::runtime_error(
                "Managed-child exact occurrence lookup failed");
        }

        auto slot = inject_reservation_lookup_failure
            ? m_owner->m_spawned_child_pids.end()
            : std::find_if(
                m_owner->m_spawned_child_pids.begin(),
                m_owner->m_spawned_child_pids.end(),
                [&](const Managed_process::Spawned_child_reap_slot& candidate) {
                    return candidate.reservation_id == reservation_id;
                });
        result.reservation_lookup_failed =
            slot == m_owner->m_spawned_child_pids.end();

        if (result.reservation_lookup_failed) {
            slot = std::find_if(
                m_owner->m_spawned_child_pids.begin(),
                m_owner->m_spawned_child_pids.end(),
                [&](const Managed_process::Spawned_child_reap_slot& candidate) {
                    return candidate.occurrence.custody.lock() == m_custody &&
                        candidate.occurrence.process_instance_id ==
                            m_process_instance_id &&
                        candidate.occurrence.occurrence == m_occurrence;
                });
            if (slot == m_owner->m_spawned_child_pids.end()) {
                throw std::runtime_error(
                    "Managed-child POSIX reap reservation reconstruction failed");
            }
        }

        if (!occurrence->native.confirm_absent()) {
            throw std::runtime_error(
                "Managed-child native authority was not absent");
        }

        if (already_reaped) {
            if (!occurrence->native.commit_created(
                    pid,
                    false,
                    0,
                    true,
                    wait_status_available,
                    static_cast<std::uint32_t>(wait_status)))
            {
                throw std::runtime_error(
                    "Managed-child native authority commit failed");
            }
            if (slot != m_owner->m_spawned_child_pids.end()) {
                m_owner->m_spawned_child_pids.erase(slot);
            }
            m_posix_reap = Posix_reap_ownership::absent;
            m_posix_reap_reservation_id = 0;
            result.child_reaped = wait_status_available;
        }
        else {
            if (slot == m_owner->m_spawned_child_pids.end()) {
                throw std::runtime_error(
                    "Managed-child POSIX reap reservation reconstruction failed");
            }
            slot->reservation_id = reservation_id;
            slot->pid = static_cast<pid_t>(pid);
            slot->start_stamp = start_stamp;
            slot->start_stamp_available = start_stamp_available;
            if (!occurrence->native.commit_created(
                    pid,
                    start_stamp_available,
                    start_stamp,
                    false,
                    false,
                    0))
            {
                throw std::runtime_error(
                    "Managed-child native authority commit failed");
            }
            m_posix_reap = Posix_reap_ownership::transferred;
        }
    }

    m_custody->changed.notify_all();
    return result;
}
#else
inline void detail::Managed_child_launch_attempt::adopt_windows_process_handle(
    uintptr_t process_handle) noexcept
{
    if (process_handle == 0) {
        return;
    }
    assert(m_windows_native == Windows_native_ownership::absent);
    assert(m_windows_process_handle == 0);
    m_windows_process_handle = process_handle;
    m_windows_native = Windows_native_ownership::raw_owned;
}

inline void
detail::Managed_child_launch_attempt::commit_windows_native_authority(
    int pid)
{
    if (!m_custody ||
        m_windows_native != Windows_native_ownership::raw_owned ||
        m_windows_process_handle == 0)
    {
        throw std::runtime_error(
            "Managed-child Windows native handoff lacks an owned raw handle");
    }

    bool start_stamp_available = false;
    uint64_t start_stamp = 0;
    if (pid > 0) {
        if (auto stamp = query_process_start_stamp(static_cast<uint32_t>(pid))) {
            start_stamp = *stamp;
            start_stamp_available = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_custody->mutex);
        auto* exact = m_custody->find_occurrence_locked(
            m_process_instance_id, m_occurrence);
        if (!exact) {
            throw std::runtime_error(
                "Managed-child exact occurrence lookup failed during Windows "
                "native handoff");
        }
        if (!exact->native.commit_created(
                pid,
                start_stamp_available,
                start_stamp,
                false,
                false,
                0,
                m_windows_process_handle,
                true))
        {
            throw std::runtime_error(
                "Managed-child Windows native authority commit failed");
        }

        // The exact native record now owns the original process handle.  Until
        // an observer is registered, it is also the authoritative fallback.
        m_windows_native =
            Windows_native_ownership::native_observer_pending;
    }
    m_custody->changed.notify_all();
}

inline void
detail::Managed_child_launch_attempt::confirm_windows_native_absent()
{
    if (!m_custody) {
        throw std::runtime_error(
            "Managed-child Windows native absence check lacks custody");
    }
    std::lock_guard<std::mutex> lock(m_custody->mutex);
    auto* exact = m_custody->find_occurrence_locked(
        m_process_instance_id, m_occurrence);
    if (!exact || !exact->native.confirm_absent())
    {
        throw std::runtime_error(
            "Managed-child Windows native authority was not absent");
    }
}

inline void
detail::Managed_child_launch_attempt::start_windows_native_observer()
{
    if (!m_owner || !m_custody ||
        m_windows_native !=
            Windows_native_ownership::native_observer_pending ||
        m_windows_process_handle == 0)
    {
        throw std::runtime_error(
            "Managed-child Windows observer lacks pending native authority");
    }

    auto custody = m_custody;
    const auto process_instance_id = m_process_instance_id;
    const auto occurrence_number = m_occurrence;
    const auto process_handle_value = m_windows_process_handle;
    const auto process_handle =
        reinterpret_cast<HANDLE>(process_handle_value);
    auto observer_complete = std::make_shared<std::atomic<bool>>(false);

    m_owner->start_child_custody_worker(
        [owner = m_owner, custody, process_instance_id, occurrence_number,
         process_handle, process_handle_value, observer_complete]() {
            struct Windows_native_observer_guard
            {
                std::shared_ptr<Managed_child_custody_record> custody;
                std::shared_ptr<std::atomic<bool>> complete;
                instance_id_type process_instance_id = invalid_instance_id;
                uint32_t occurrence_number = 0;
                uintptr_t process_handle_value = 0;
                const char* message =
                    "Managed-child native observer failed unexpectedly";
                int native_error = 0;
                bool committed = false;

                ~Windows_native_observer_guard() noexcept
                {
                    bool fallback_available = false;
                    if (!committed) {
                        try {
                            std::lock_guard<std::mutex> lock(custody->mutex);
                            complete->store(true, std::memory_order_release);
                            auto* exact = custody->find_occurrence_locked(
                                process_instance_id, occurrence_number);
                            if (exact &&
                                exact->native.cancel_exit_observer(
                                    process_handle_value) &&
                                exact->native.fallback_wait_available())
                            {
                                fallback_available = true;
                                if (custody->last_failure.kind ==
                                        Managed_child_failure_kind::none ||
                                    occurrence_number >=
                                        custody->last_failure.occurrence)
                                {
                                    custody->last_failure.kind =
                                        Managed_child_failure_kind::native_observer;
                                    custody->last_failure.occurrence =
                                        occurrence_number;
                                    custody->last_failure.native_error =
                                        native_error;
                                    custody->last_failure.message = message;
                                }
                            }
                        }
                        catch (...) {
                        }
                    }
                    if (!committed) {
                        try {
                            managed_child_cleanup_for_test(
                                test_hooks::
                                    k_managed_child_native_observer_after_cancel,
                                process_instance_id,
                                occurrence_number);
                        }
                        catch (...) {
                        }
                    }
                    complete->store(true, std::memory_order_release);
                    custody->changed.notify_all();
                    if (fallback_available) {
                        try {
                            managed_child_cleanup_for_test(
                                test_hooks::
                                    k_managed_child_native_observer_fallback_available,
                                process_instance_id,
                                occurrence_number);
                        }
                        catch (...) {
                        }
                    }
                }

                void fail(const char* failure_message, int error) noexcept
                {
                    message = failure_message;
                    native_error = error;
                }
            } observer_guard{
                custody,
                observer_complete,
                process_instance_id,
                occurrence_number,
                process_handle_value};

            managed_child_cleanup_for_test(
                test_hooks::k_managed_child_native_observer_before_wait,
                process_instance_id,
                occurrence_number);
            managed_child_failure_for_test(
                test_hooks::
                    k_managed_child_fail_native_observer_after_registration,
                process_instance_id,
                occurrence_number);
            managed_child_failure_for_test(
                test_hooks::k_managed_child_fail_native_observer_before_wait,
                process_instance_id,
                occurrence_number);
            const auto wait_result = managed_child_failure_selected_for_test(
                    test_hooks::k_managed_child_fail_native_observer_wait,
                    process_instance_id,
                    occurrence_number)
                ? WAIT_FAILED
                : WaitForSingleObject(process_handle, INFINITE);
            if (wait_result != WAIT_OBJECT_0) {
                observer_guard.fail(
                    "Managed-child native observer wait failed",
                    wait_result == WAIT_FAILED
                        ? static_cast<int>(GetLastError())
                        : static_cast<int>(ERROR_INVALID_DATA));
                return;
            }
            uintptr_t released_handle = 0;
            bool transition_valid = false;
            Managed_child_exit_publication exit_publication;
            managed_child_cleanup_for_test(
                test_hooks::k_managed_child_native_exit_before_publication,
                process_instance_id,
                occurrence_number);
            const bool exit_status_forced_unavailable =
                managed_child_failure_selected_for_test(
                    test_hooks::k_managed_child_force_exit_status_unavailable,
                    process_instance_id,
                    occurrence_number);
            {
                std::lock_guard<std::mutex> lock(custody->mutex);
                auto* occurrence = custody->find_occurrence_locked(
                    process_instance_id, occurrence_number);
                if (occurrence) {
                    DWORD exit_code = 0;
                    const bool exit_code_available =
                        GetExitCodeProcess(process_handle, &exit_code) != 0;
                    exit_publication = record_managed_child_exit_locked(
                        custody->identity,
                        *occurrence,
                        exit_code,
                        exit_code_available &&
                            !exit_status_forced_unavailable);
                    transition_valid = exit_publication.transition_valid;
                    if (transition_valid &&
                        occurrence->native.process_handle_owned())
                    {
                        transition_valid =
                            occurrence->native.take_owned_process_handle(
                                process_handle_value, released_handle);
                    }
                    if (transition_valid) {
                        observer_guard.committed = true;
                    }
                }
                custody->changed.notify_all();
            }
            owner->dispatch_child_exit_publication(
                std::move(exit_publication));
            if (released_handle != 0) {
                CloseHandle(reinterpret_cast<HANDLE>(released_handle));
            }
            if (!transition_valid) {
                Log_stream(log_level::error)
                    << "Managed-child native observer transition failed.\n";
            }
        },
        test_hooks::k_managed_child_fail_native_observer_start,
        process_instance_id,
        occurrence_number);

    managed_child_cleanup_for_test(
        test_hooks::k_managed_child_native_observer_before_registration,
        process_instance_id,
        occurrence_number);

    bool observer_registered = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* exact = custody->find_occurrence_locked(
            process_instance_id, occurrence_number);
        if (!exact) {
            throw std::runtime_error(
                "Managed-child exact occurrence lookup failed during Windows "
                "observer registration");
        }

        const bool worker_complete =
            observer_complete->load(std::memory_order_acquire);
        if (exact->native.running()) {
            if (worker_complete) {
                if (!exact->native.fallback_wait_available() ||
                    exact->native.process_handle() != process_handle_value)
                {
                    throw std::runtime_error(
                        "Managed-child Windows observer fallback handoff failed");
                }
            }
            else if (!exact->native.register_exit_observer(
                          process_handle_value))
            {
                throw std::runtime_error(
                    "Managed-child native observer registration failed");
            }
            else {
                observer_registered = true;
            }
        }
        else if (!exact->native.exited()) {
            throw std::runtime_error(
                "Managed-child Windows observer found absent native authority");
        }

        m_windows_native = Windows_native_ownership::transferred;
        m_windows_process_handle = 0;
    }
    if (observer_registered) {
        managed_child_cleanup_for_test(
            test_hooks::k_managed_child_native_observer_registered,
            process_instance_id,
            occurrence_number);
    }
    managed_child_cleanup_for_test(
        test_hooks::k_managed_child_native_observer_registration_complete,
        process_instance_id,
        occurrence_number);
}
#endif

inline void
detail::Managed_child_launch_attempt::rollback_initialization() noexcept
{
    if (m_initialization != Initialization_ownership::rollback_required) {
        return;
    }
    m_initialization = Initialization_ownership::absent;
    try {
        if (m_initialization_coordinator) {
            m_owner->mark_child_coordinator_initialization_complete(
                m_initialization_coordinator,
                m_process_instance_id);
        }
    }
    catch (...) {
    }
    try {
        if (m_owner) {
            m_owner->note_child_initialization_complete(
                {m_custody, m_process_instance_id, m_occurrence});
        }
    }
    catch (...) {
    }
    m_initialization_coordinator = nullptr;
}

inline void detail::Managed_child_launch_attempt::rollback_reader() noexcept
{
    if (m_reader != Reader_ownership::rollback_required || !m_owner) {
        return;
    }
    m_reader = Reader_ownership::absent;
    auto* owner = m_owner;
    const auto process_instance_id = m_process_instance_id;
    std::shared_ptr<Managed_child_setup_settlement> deferred_settlement;

    if (tl_is_req_thread) {
        try {
            deferred_settlement = std::make_shared<
                Managed_child_setup_settlement>(
                    std::move(m_setup_settlement));
            owner->run_after_current_handler(
                [owner, process_instance_id,
                 deferred_settlement]() mutable {
                    owner->remove_process_reader(process_instance_id, 1.0);
                    deferred_settlement.reset();
                });
            return;
        }
        catch (...) {
        }
    }

    try {
        owner->remove_process_reader(process_instance_id, 1.0);
    }
    catch (...) {
    }
    deferred_settlement.reset();
}

#ifndef _WIN32
inline void
detail::Managed_child_launch_attempt::rollback_posix_reap_reservation() noexcept
{
    if (m_posix_reap != Posix_reap_ownership::reserved || !m_owner ||
        m_posix_reap_reservation_id == 0)
    {
        return;
    }

    const auto reservation_id = m_posix_reap_reservation_id;
    m_posix_reap = Posix_reap_ownership::absent;
    m_posix_reap_reservation_id = 0;
    try {
        std::lock_guard<std::mutex> roster_lock(
            m_owner->m_spawned_child_pids_mutex);
        m_owner->m_spawned_child_pids.erase(
            std::remove_if(
                m_owner->m_spawned_child_pids.begin(),
                m_owner->m_spawned_child_pids.end(),
                [&](const Managed_process::Spawned_child_reap_slot& slot) {
                    return slot.reservation_id == reservation_id;
                }),
            m_owner->m_spawned_child_pids.end());
    }
    catch (...) {
    }
}
#else
inline void
detail::Managed_child_launch_attempt::rollback_windows_native_authority() noexcept
{
    uintptr_t raw_handle = 0;
    bool notify_custody = false;

    if (m_windows_native == Windows_native_ownership::raw_owned) {
        raw_handle = m_windows_process_handle;
        m_windows_process_handle = 0;
        m_windows_native = Windows_native_ownership::absent;
    }
    else if (m_windows_native ==
                 Windows_native_ownership::native_observer_pending)
    {
        const auto expected_handle = m_windows_process_handle;
        try {
            if (m_custody) {
                std::lock_guard<std::mutex> lock(m_custody->mutex);
                auto* exact = m_custody->find_occurrence_locked(
                    m_process_instance_id, m_occurrence);
                if (exact) {
                    exact->native.cancel_exit_observer(expected_handle);
                    notify_custody = true;
                }
            }
        }
        catch (...) {
        }
        // Native authority owns the original.  Disarming the launch attempt
        // exposes that exact handle to the retained fallback without closing it.
        m_windows_process_handle = 0;
        m_windows_native = Windows_native_ownership::transferred;
    }

    if (notify_custody) {
        m_custody->changed.notify_all();
    }
    if (raw_handle != 0) {
        // No custody lock is held while closing launch-local raw authority.
        CloseHandle(reinterpret_cast<HANDLE>(raw_handle));
    }
}
#endif

inline void detail::Managed_child_launch_attempt::rollback() noexcept
{
#ifndef _WIN32
    rollback_posix_reap_reservation();
#else
    rollback_windows_native_authority();
#endif
    close_lifeline_read_endpoint();
    close_lifeline_write_endpoint();
    rollback_initialization();
    rollback_reader();
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
    const bool can_wait                 = can_wait_for_signal_dispatch();
    const bool should_wait_for_dispatch = mproc && mproc->m_out_req_c && sig != SIGCHLD;
    uint32_t   dispatched_before        = 0;
    if (should_wait_for_dispatch && can_wait) {
        dispatched_before = dispatched_signal_counter().load(std::memory_order_acquire);
    }

    auto& pipefd = signal_pipe();
    if (pipefd[1] != -1) {
        int           sig_number   = sig;
        int           last_errno   = 0;
        bool          delivered    = false;
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
     *  - enable_recovery() RPCs the coordinator, which authenticates the
     *    caller's exact occurrence and records the opt-in on its custody. When
     *    a crash is observed the coordinator captures that custody's cached
     *    Spawn_swarm_process_args and routes it through
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
    Coordinator::rpc_enable_recovery(s_coord_id, process_of(m_instance_id));
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
        auto& storage   = alt_stack_storage();
        auto& installed = alt_stack_installed();
        if (!installed) {
            stack_t ss {};
            ss.ss_sp    = storage.data();
            ss.ss_size  = storage.size();
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
    else {
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
        auto it         = scoped_map.get().find(key);
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
            auto it         = scoped_map.get().find(explicit_id);
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
            else {
                s_mproc->m_type_name_of_explicit_id.set_value(explicit_id, type_name);
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
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
        assert(s_mproc == nullptr);
        s_mproc = this;
    }

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
#ifndef _WIN32
    // SIGCHLD publication runs under the shared dispatch lock. Wait for any
    // active producer to enqueue its owned callbacks before draining.
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
    }
#endif
    // finalize_impl() only reaches destruction after every retained custody
    // record is terminal, so the owned observers/cleanup workers are bounded
    // joins here and cannot outlive the runtime they report into.
    join_child_custody_workers();
    drain_child_exit_dispatcher();

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
    std::vector<Spawned_child_reap_slot> reap_targets;
    {
        std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
        reap_targets.swap(m_spawned_child_pids);
    }

    for (const auto& slot : reap_targets) {
        const pid_t pid = slot.pid;
        if (pid <= 0) {
            continue;
        }

        const auto poll_delay        = std::chrono::milliseconds(10);
        auto       graceful_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        auto       forceful_deadline = graceful_deadline + std::chrono::seconds(1);
        bool       sent_sigterm      = false;
        bool       sent_sigkill      = false;
        auto exact_process_alive = [&]() {
            if (!slot.start_stamp_available) {
                return false;
            }
            const auto observed = query_process_start_stamp(
                static_cast<uint32_t>(pid));
            return observed && *observed == slot.start_stamp;
        };

        while (true) {
            int   status = 0;
            pid_t result = ::waitpid(pid, &status, sent_sigkill ? 0 : WNOHANG);

            if (result == pid) {
                note_child_os_exit(slot.occurrence, status);
                detail::child_reaped_for_test(pid, status);
                break;
            }

            if (result == 0) {
                auto now = std::chrono::steady_clock::now();
                if (!sent_sigterm && now >= graceful_deadline) {
                    if (!exact_process_alive()) {
                        break;
                    }
                    if (::kill(pid, SIGTERM) == -1 && errno == ESRCH) {
                        break;
                    }
                    sent_sigterm = true;
                    forceful_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    continue;
                }

                if (sent_sigterm && !sent_sigkill && now >= forceful_deadline) {
                    if (!exact_process_alive()) {
                        break;
                    }
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
                if (errno == EINTR)                    { continue; }
                if (errno == ECHILD || errno == ESRCH) { break;    }

                // Unexpected error: escalate to SIGKILL once before aborting the loop.
                if (!sent_sigkill) {
                    if (!exact_process_alive()) {
                        break;
                    }
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

#endif

    // The once-created signal pipe/event and its detached dispatcher are
    // process-lifetime infrastructure. Keep the transport live across runtime
    // teardown so a later init can bind to the same dispatcher. The OS closes
    // the process-owned descriptors/handle at process exit. Taking the unique
    // lock here waits out any callback bound to this runtime and prevents a new
    // callback from observing the pointer while it is cleared.
    {
        Dispatch_unique_lock dispatch_lock(dispatch_shutdown_mutex_instance);
        s_mproc = nullptr;
        s_mproc_id = 0;
    }
    stop_child_exit_dispatcher();
}

#ifndef _WIN32
inline void Managed_process::reap_finished_children()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    std::vector<std::pair<pid_t, int>> reaped_children;
#endif
    {
        std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
        if (m_spawned_child_pids.empty()) {
            return;
        }

        for (auto slot = m_spawned_child_pids.begin();
             slot != m_spawned_child_pids.end();)
        {
            const pid_t pid = slot->pid;
            if (pid <= 0) {
                ++slot;
                continue;
            }

            int   status = 0;
            pid_t result = 0;
            do {
                result = ::waitpid(pid, &status, WNOHANG);
            }
            while (result == -1 && errno == EINTR);

            if (result == 0) {
                ++slot;
                continue;
            }

            if (result == pid) {
                note_child_os_exit(slot->occurrence, status);
#if defined(SINTRA_ENABLE_TEST_HOOKS)
                reaped_children.emplace_back(pid, status);
#endif
                slot = m_spawned_child_pids.erase(slot);
                continue;
            }

            if (result == -1 && errno != ECHILD) {
                ++slot;
                continue;
            }
            if (result == -1 && errno == ECHILD) {
                note_child_os_exit(slot->occurrence, 0, false);
            }
            slot = m_spawned_child_pids.erase(slot);
        }
    }

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    for (const auto& [pid, status] : reaped_children) {
        detail::child_reaped_for_test(pid, status);
    }
#endif
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

struct filtered_args_t
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
filtered_args_t filter_option(
    std::vector<std::string>   in_args,
    std::string                in_option,
    unsigned int               num_args)
{
    filtered_args_t ret;
    const std::string option_prefix = in_option + "=";
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
        if (num_args == 1 && e.rfind(option_prefix, 0) == 0) {
            ret.extracted.clear();
            ret.extracted.push_back(in_option);
            ret.extracted.push_back(e.substr(option_prefix.size()));
            continue;
        }
        ret.remained.push_back(e);
    }
    return ret;
}

/// Strips a sequence of options from args, discarding their extracted values
/// and returning only the leftover argv tokens. Each entry is {option_name, num_args}.
inline std::vector<std::string> strip_options(
    std::vector<std::string>                                          args,
    std::initializer_list<std::pair<std::string_view, unsigned int>>  options)
{
    for (const auto& [name, n] : options) {
        args = filter_option(std::move(args), std::string(name), n).remained;
    }
    return args;
}

inline
void Managed_process::init(int argc, const char* const* argv)
{
    m_binary_name = argv[0];
    m_skip_startup_barrier = false;

    // Reset lifeline state to prevent stale values from a previous init() call
    s_lifeline_handle_value.clear();
    s_lifeline_exit_code  = Lifetime_policy{}.hard_exit_code;
    s_lifeline_timeout_ms = Lifetime_policy{}.hard_exit_timeout_ms;
    s_lifeline_disabled   = false;

    std::string branch_index_arg;
    std::string swarm_id_arg;
    std::string instance_id_arg;
    std::string coordinator_id_arg;
    std::string external_attach_token_arg;
    uint32_t    external_attach_occurrence = 0;
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

    // Filter out non-reusable runtime arguments from remained - lifeline values
    // contain stale handles, and external attach tokens are single-use.
    auto reusable_args = strip_options(std::move(fa.remained), {
        {detail::k_external_attach_token_arg,      1},
        {detail::k_external_attach_occurrence_arg, 1},
        {k_lifeline_handle_arg,                    1},
        {k_lifeline_exit_code_arg,                 1},
        {k_lifeline_timeout_arg,                   1},
        {k_lifeline_disable_arg,                   0},
    });

    m_recovery_cmd = join_strings(reusable_args, " ") + " --recovery_occurrence " +
        std::to_string(recovery_occurrence_value+1);

    auto option_value = [&](
        const std::string& arg,
        const char*        long_name,
        char               short_name,
        bool               requires_value,
        int&               index) -> std::optional<std::string>
    {
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

            if (auto value = option_value(
                    arg, detail::k_external_attach_token_arg, '\0', true, i))
            {
                external_attach_token_arg = *value;
                continue;
            }

            if (auto value = option_value(
                    arg, detail::k_external_attach_occurrence_arg, '\0', true, i))
            {
                try {
                    external_attach_occurrence = static_cast<uint32_t>(std::stoul(*value));
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

            if (option_value(
                    arg, detail::k_skip_startup_barrier_arg, '\0', false, i))
            {
                m_skip_startup_barrier = true;
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
            << "  --external_attach_token arg token produced by an external-process\n"
            << "                           invitation\n"
            << "  --recovery_occurrence arg (optional) number of times the process recovered an\n"
            << "                           abnormal termination.\n";
        exit(1);
    }

    bool coordinator_is_local = false;
    if (swarm_id_arg.empty()) {
        s_mproc_id = m_instance_id = make_process_instance_id();

        m_swarm_id = std::chrono::duration_cast<std::chrono::nanoseconds>(
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

    if (!external_attach_token_arg.empty() && s_recovery_occurrence != 0) {
        throw std::runtime_error("Sintra external process invitation was rejected.");
    }

    if (!coordinator_is_local) {
        start_lifeline_watcher(Lifetime_policy{}, external_attach_token_arg.empty());
    }
    m_directory = obtain_swarm_directory();

    const auto        abi_path    = std::filesystem::path(m_directory) / detail::abi_marker_filename();
    const std::string current_abi = detail::abi_token();

    if (coordinator_is_local) {
        run_marker_record_t run_marker{};
        run_marker.pid                  = static_cast<uint32_t>(m_pid);
        run_marker.start_stamp          = m_process_start_stamp;
        run_marker.created_monotonic_ns = monotonic_now_ns();
        run_marker.recovery_occurrence  = static_cast<uint32_t>(s_recovery_occurrence);

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

    const auto ring_occurrence = external_attach_token_arg.empty()
        ? s_recovery_occurrence
        : external_attach_occurrence;
    m_out_req_c = new Message_ring_W(m_directory, "req", m_instance_id, ring_occurrence);
    m_out_rep_c = new Message_ring_W(m_directory, "rep", m_instance_id, ring_occurrence);

    if (coordinator_is_local) {
        s_coord = new Coordinator;
        s_coord_id = s_coord->m_instance_id;

        {
            std::lock_guard lock(s_coord->m_publish_mutex);
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
        Tn_type tn = {msg.type_id, msg.assigned_name};

        while (true) {
            function<void()> next_call;
            {
                lock_guard<mutex> lock(m_availability_mutex);
                auto it = m_queued_availability_calls.find(tn);
                if (it == m_queued_availability_calls.end()) {
                    return;
                }

                if (it->second.empty()) {
                    m_queued_availability_calls.erase(it);
                    return;
                }

                next_call = it->second.front();
            }

            // each function call, which is a lambda defined inside
            // call_on_availability(), clears itself from the list as well.
            next_call();
        }
    };

    auto unpublished_handler = [this](const Coordinator::instance_unpublished& msg)
    {
        auto iid         = msg.instance_id;
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
            s_mproc->m_must_stop.store(true, std::memory_order_release);
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

        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            if (!s_coord || msg.managed_child_custody_identity == 0) {
                return;
            }

            Coordinator::Managed_child_publication_identity expected_identity;
            expected_identity.custody_identity =
                msg.managed_child_custody_identity;
            expected_identity.process_iid =
                process_of(msg.sender_instance_id);
            expected_identity.occurrence = msg.managed_child_occurrence;

            Crash_info info;
            info.process_iid  = msg.sender_instance_id;
            info.process_slot = static_cast<uint32_t>(
                get_process_index(msg.sender_instance_id));
            info.status       = msg.status;
            s_coord->unpublish_transceiver_exact(
                msg.sender_instance_id,
                expected_identity,
                info);
        };
        activate<Managed_process>(unpublish_notify_handler, any_remote);
        activate<Managed_process>(cr_handler, any_remote);
    }
    else {
        auto cr_handler = [](const Managed_process::terminated_abnormally& msg)
        {
            // Remote coordinator crashed: fail outstanding RPCs and pause communication
            if (process_of(s_coord_id) == msg.sender_instance_id) {
                s_mproc->m_must_stop.store(true, std::memory_order_release);
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

    if (!external_attach_token_arg.empty()) {
        auto handle = Coordinator::rpc_async_claim_external_process_invitation(
            s_coord_id,
            m_instance_id,
            external_attach_token_arg);
        const auto deadline = std::chrono::steady_clock::now() +
            detail::k_external_attach_claim_timeout;

        bool claim_accepted = false;
        try {
            claim_accepted = handle.get_until(deadline);
        }
        catch (const rpc_timeout&) {
            unblock_rpc(process_of(s_coord_id));
        }

        if (!claim_accepted) {
            throw std::runtime_error("Sintra external process invitation was rejected.");
        }
    }
}

inline std::shared_ptr<detail::Managed_child_custody_record>
Managed_process::accept_child_custody()
{
    auto custody = std::make_shared<detail::Managed_child_custody_record>();
    custody->runtime_lifetime = m_runtime_lifetime;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        custody->identity = m_next_child_custody_identity++;
        m_child_custodies.emplace(custody->identity, custody);
    }
    return custody;
}

inline bool Managed_process::can_accept_child_custody(
    instance_id_type process_instance_id) const
{
    std::shared_ptr<detail::Managed_child_custody_record> existing;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        auto it = m_child_custody_by_process.find(process_instance_id);
        if (it == m_child_custody_by_process.end()) {
            return true;
        }
        existing = it->second.custody.lock();
    }
    if (!existing) {
        return true;
    }
    std::lock_guard<std::mutex> lock(existing->mutex);
    return existing->release_state.released();
}

inline detail::Managed_child_launch_attempt
Managed_process::admit_child_custody_occurrence(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
    if (!custody) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (!custody->release_state.open()) {
            return {};
        }
        if (custody->find_occurrence_locked(process_instance_id, occurrence)) {
            return {};
        }
        detail::Managed_child_occurrence_record admitted;
        admitted.occurrence = occurrence;
        admitted.process_instance_id = process_instance_id;
        custody->occurrences.push_back(admitted);
    }
    try {
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_admission_mapping,
            process_instance_id,
            occurrence);
        {
            std::lock_guard<std::mutex> lock(m_child_custody_mutex);
            m_child_custody_by_process[process_instance_id] = {custody, occurrence};
        }
    }
    catch (...) {
        {
            std::lock_guard<std::mutex> lock(custody->mutex);
            const auto admitted = std::find_if(
                custody->occurrences.begin(),
                custody->occurrences.end(),
                [&](const detail::Managed_child_occurrence_record& candidate) {
                    return candidate.process_instance_id == process_instance_id &&
                        candidate.occurrence == occurrence &&
                        candidate.setup == detail::Managed_child_occurrence_record::
                            setup_state::pending &&
                        candidate.native.confirm_absent();
                });
            if (admitted != custody->occurrences.end()) {
                custody->occurrences.erase(admitted);
            }
        }
        custody->changed.notify_all();
        throw;
    }
    custody->changed.notify_all();
    return detail::Managed_child_launch_attempt(
        this, custody, process_instance_id, occurrence);
}

inline void Managed_process::note_child_custody_failure(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    Managed_child_failure failure)
{
    if (!custody || failure.kind == Managed_child_failure_kind::none) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (custody->last_failure.kind == Managed_child_failure_kind::none ||
            failure.occurrence >= custody->last_failure.occurrence)
        {
            custody->last_failure = std::move(failure);
        }
    }
    custody->changed.notify_all();
}

inline void Managed_process::fail_release_attempt(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    uint64_t release_attempt_generation,
    Managed_child_failure failure,
    instance_id_type process_instance_id)
{
    if (!custody || failure.kind == Managed_child_failure_kind::none) {
        return;
    }

    uint64_t custody_identity = 0;
    uint64_t rerun_generation = 0;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (custody->release_state.mark_retryable(
                release_attempt_generation))
        {
            custody_identity = custody->identity;
            // This is the failure of the current release-attempt generation.
            // Do not apply setup failure's occurrence-number precedence: an
            // exact predecessor can fail after a newer occurrence was admitted.
            custody->last_failure = failure;
            if (custody->release_state.running()) {
                rerun_generation = custody->release_state.generation();
            }
            failed = true;
        }
    }
    if (!failed) {
        return;
    }

    custody->changed.notify_all();
    if (rerun_generation != 0) {
        start_child_custody_release_worker(custody, rerun_generation);
    }
    Log_stream(log_level::warning)
        << "Managed-child release attempt failed: custody=" << custody_identity
        << " generation=" << release_attempt_generation
        << " process_instance_id=" << process_instance_id
        << " occurrence=" << failure.occurrence
        << " kind=" << static_cast<int>(failure.kind)
        << " native_error=" << failure.native_error
        << " message='" << failure.message << "'\n";
}

inline void Managed_process::record_release_attempt_blocker(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    uint64_t release_attempt_generation,
    instance_id_type process_instance_id,
    uint32_t occurrence,
    const char* message)
{
    if (!custody) {
        return;
    }

    Managed_child_failure failure{
        Managed_child_failure_kind::release_worker_execution,
        occurrence,
        0,
        message};
    uint64_t custody_identity = 0;
    bool recorded = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (custody->release_state.mark_failing(
                release_attempt_generation))
        {
            custody_identity = custody->identity;
            custody->last_failure = std::move(failure);
            recorded = true;
        }
    }
    if (!recorded) {
        return;
    }

    custody->changed.notify_all();
    Log_stream(log_level::warning)
        << "Managed-child release attempt blocked: custody="
        << custody_identity
        << " generation=" << release_attempt_generation
        << " process_instance_id=" << process_instance_id
        << " occurrence=" << occurrence
        << " message='" << message << "'\n";
}

inline void Managed_process::start_child_custody_worker(
    std::function<void()> worker,
    const char* failure_stage,
    instance_id_type failure_process_instance_id,
    uint32_t failure_occurrence)
{
    std::lock_guard<std::mutex> lock(m_child_custody_workers_mutex);
    for (auto it = m_child_custody_workers.begin(); it != m_child_custody_workers.end();) {
        if (it->complete->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = m_child_custody_workers.erase(it);
        }
        else {
            ++it;
        }
    }
    auto complete = std::make_shared<std::atomic<bool>>(false);
    auto guarded = detail::Exception_boundary{"managed_child_custody"}.wrap(
        std::move(worker));
    // Register a nonjoinable owned slot first. If injection or std::thread
    // construction fails, erasing this slot is safe; no joinable local thread
    // can escape registration and trigger std::terminate during unwinding.
    m_child_custody_workers.emplace_back();
    auto& owned = m_child_custody_workers.back();
    owned.complete = complete;
    try {
        if (failure_stage) {
            detail::managed_child_failure_for_test(
                failure_stage,
                failure_process_instance_id,
                failure_occurrence);
        }
        owned.thread = std::thread(
            [guarded = std::move(guarded), complete]() mutable {
                Instantiator completion_guard(std::function<void()>([complete]() {
                    complete->store(true, std::memory_order_release);
                }));
                guarded();
            });
    }
    catch (...) {
        m_child_custody_workers.pop_back();
        throw;
    }
}

inline bool Managed_process::ensure_child_exit_dispatcher() noexcept
{
    std::lock_guard<std::mutex> lock(m_child_exit_dispatch_mutex);
    if (m_child_exit_dispatch_thread.joinable()) {
        return true;
    }
    try {
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_exit_dispatcher_start,
            invalid_instance_id,
            0);
        m_child_exit_dispatch_stopping = false;
        m_child_exit_dispatch_thread = std::thread(
            [this]() { run_child_exit_dispatcher(); });
        return true;
    }
    catch (...) {
        return false;
    }
}

inline void Managed_process::run_child_exit_dispatcher() noexcept
{
    while (true) {
        std::shared_ptr<detail::Managed_child_exit_subscription_state>
            subscription;
        Managed_child_exit event;
        {
            std::unique_lock<std::mutex> lock(m_child_exit_dispatch_mutex);
            m_child_exit_dispatch_changed.wait(lock, [this]() {
                return m_child_exit_dispatch_stopping ||
                    m_child_exit_dispatch_head != nullptr;
            });
            if (!m_child_exit_dispatch_head) {
                return;
            }
            auto* queued = m_child_exit_dispatch_head;
            m_child_exit_dispatch_head = queued->m_dispatch_next;
            if (!m_child_exit_dispatch_head) {
                m_child_exit_dispatch_tail = nullptr;
            }
            queued->m_dispatch_next = nullptr;
            event = queued->m_dispatch_event;
            subscription = std::move(queued->m_dispatch_owner);
            m_child_exit_dispatch_active = true;
        }

        subscription->deliver(event);

        {
            std::lock_guard<std::mutex> lock(m_child_exit_dispatch_mutex);
            m_child_exit_dispatch_active = false;
        }
        m_child_exit_dispatch_changed.notify_all();
    }
}

inline void Managed_process::drain_child_exit_dispatcher() noexcept
{
    std::unique_lock<std::mutex> lock(m_child_exit_dispatch_mutex);
    m_child_exit_dispatch_changed.wait(lock, [this]() {
        return !m_child_exit_dispatch_head && !m_child_exit_dispatch_active;
    });
}

inline void Managed_process::stop_child_exit_dispatcher() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_child_exit_dispatch_mutex);
        if (!m_child_exit_dispatch_thread.joinable()) {
            return;
        }
        m_child_exit_dispatch_stopping = true;
    }
    m_child_exit_dispatch_changed.notify_all();
    m_child_exit_dispatch_thread.join();
}

inline void Managed_process::enqueue_child_exit_subscription_locked(
    std::shared_ptr<detail::Managed_child_exit_subscription_state> subscription,
    const Managed_child_exit& event) noexcept
{
    if (!subscription) {
        return;
    }
    auto* queued = subscription.get();
    if (m_child_exit_dispatch_stopping || queued->m_dispatch_owner ||
        queued->m_dispatch_next)
    {
        std::terminate();
    }
    queued->m_dispatch_event = event;
    queued->m_dispatch_owner = std::move(subscription);
    if (m_child_exit_dispatch_tail) {
        m_child_exit_dispatch_tail->m_dispatch_next = queued;
    }
    else {
        m_child_exit_dispatch_head = queued;
    }
    m_child_exit_dispatch_tail = queued;
}

inline void Managed_process::dispatch_child_exit_publication(
    detail::Managed_child_exit_publication publication) noexcept
{
    if (publication.subscriptions.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_child_exit_dispatch_mutex);
        for (auto& subscription : publication.subscriptions) {
            enqueue_child_exit_subscription_locked(
                std::move(subscription), publication.event);
        }
    }
    m_child_exit_dispatch_changed.notify_one();
}

inline void Managed_process::dispatch_child_exit_subscription(
    std::shared_ptr<detail::Managed_child_exit_subscription_state> subscription,
    Managed_child_exit event) noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_child_exit_dispatch_mutex);
        enqueue_child_exit_subscription_locked(
            std::move(subscription), event);
    }
    m_child_exit_dispatch_changed.notify_one();
}

inline void Managed_process::join_child_custody_workers()
{
    std::vector<Child_custody_worker> workers;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_workers_mutex);
        workers.swap(m_child_custody_workers);
    }
    for (auto& worker : workers) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
}

inline void Managed_process::retire_child_custody_if_complete(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody)
{
    if (!custody) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (!custody->release_state.released() ||
            custody->readiness == detail::Readiness_phase::pending)
        {
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        auto record_it = m_child_custodies.find(custody->identity);
        if (record_it == m_child_custodies.end() ||
            record_it->second != custody)
        {
            return;
        }
    }
    detail::managed_child_custody_retirement_for_test(
        detail::test_hooks::k_managed_child_custody_retirement_before_cache_erase,
        custody->identity);
    {
        std::scoped_lock lock(m_child_custody_mutex, m_cached_spawns_mutex);
        auto record_it = m_child_custodies.find(custody->identity);
        if (record_it == m_child_custodies.end() ||
            record_it->second != custody)
        {
            return;
        }
        std::erase_if(m_cached_spawns, [&](const auto& entry) {
            return entry.second.custody == custody;
        });
        for (auto it = m_child_custody_by_process.begin();
             it != m_child_custody_by_process.end();)
        {
            if (it->second.custody.lock() == custody) {
                it = m_child_custody_by_process.erase(it);
            }
            else {
                ++it;
            }
        }
        m_child_custodies.erase(record_it);
    }
    m_child_custody_changed.notify_all();
    detail::managed_child_custody_retirement_for_test(
        detail::test_hooks::k_managed_child_custody_retirement_complete,
        custody->identity);
}

namespace detail {
namespace managed_child_release_action {

struct stop_attempt {};
struct complete_release {};

struct apply_cleanup
{
    Managed_child_release_roster targets;
};

struct retire_current_transport
{
    Managed_child_release_roster targets;
};

struct retire_historical_communication
{
    Managed_child_release_roster targets;
};

struct poll_windows_fallback
{
    instance_id_type process_instance_id = invalid_instance_id;
    uint32_t         occurrence = 0;
    uintptr_t        process_handle = 0;
};

struct wait_for_change {};

} // namespace managed_child_release_action

using Managed_child_release_action_plan = std::variant<
    managed_child_release_action::stop_attempt,
    managed_child_release_action::complete_release,
    managed_child_release_action::apply_cleanup,
    managed_child_release_action::retire_current_transport,
    managed_child_release_action::retire_historical_communication,
    managed_child_release_action::poll_windows_fallback,
    managed_child_release_action::wait_for_change>;

// Precondition: custody.mutex is held continuously from this selection until
// the caller either claims the selected action or begins the condition wait.
inline Managed_child_release_action_plan plan_managed_child_release_action_locked(
    const Managed_child_custody_record& custody,
    const Managed_child_release_roster& release_roster,
    uint64_t release_attempt_generation,
    bool cleanup_applied)
{
    using namespace managed_child_release_action;

    if (!custody.release_state.running(release_attempt_generation))
    {
        return stop_attempt{};
    }

    const bool all_terminal = std::all_of(
        custody.occurrences.begin(), custody.occurrences.end(),
        [](const Managed_child_occurrence_record& occurrence) {
            return !occurrence.initialization_reservation_active &&
                (occurrence.setup ==
                    Managed_child_occurrence_record::setup_state::no_child ||
                 (occurrence.setup == Managed_child_occurrence_record::
                        setup_state::ownership_ready &&
                  occurrence.transport.fully_retired() &&
                  occurrence.native.exited()));
        });
    if (all_terminal) {
        return complete_release{};
    }

    if (custody.release_state.cleanup_requested() && !cleanup_applied) {
        return apply_cleanup{release_roster};
    }

    Managed_child_release_roster current_targets;
    Managed_child_release_roster historical_targets;
#ifdef _WIN32
    std::optional<poll_windows_fallback> windows_fallback;
#endif
    for (const auto& target : release_roster) {
        const auto* occurrence = custody.find_occurrence_locked(
            target.process_instance_id, target.occurrence);
        if (!occurrence) {
            continue;
        }
        if (target.slot_current &&
            (cleanup_applied || occurrence->native.exited()) &&
            !occurrence->transport.retirement_started() &&
            (occurrence->initialization_reservation_active ||
             !occurrence->transport.publication_retired() ||
             !occurrence->transport.retirement_terminal()))
        {
            current_targets.push_back(target);
        }
        if (occurrence->transport.ready_to_retire()) {
            historical_targets.push_back(target);
        }
#ifdef _WIN32
        if (!windows_fallback &&
            occurrence->native.fallback_wait_available())
        {
            windows_fallback = poll_windows_fallback{
                target.process_instance_id,
                target.occurrence,
                occurrence->native.process_handle()};
        }
#endif
    }

    if (!current_targets.empty()) {
        return retire_current_transport{std::move(current_targets)};
    }
    if (!historical_targets.empty()) {
        return retire_historical_communication{
            std::move(historical_targets)};
    }
#ifdef _WIN32
    if (windows_fallback) {
        return *windows_fallback;
    }
#endif

    return wait_for_change{};
}

} // namespace detail

#ifdef _WIN32
namespace detail {
namespace managed_child_windows_fallback_result {

struct timed_out {};

struct signaled
{
    std::uint32_t exit_status = 0;
};

enum class failed_operation
{
    wait,
    exit_query
};

struct wait_failed
{
    failed_operation operation = failed_operation::wait;
    DWORD            native_error = 0;
};

using execution_result = std::variant<timed_out, signaled, wait_failed>;

struct retry {};

struct applied
{
    uintptr_t released_handle = 0;
};

struct transition_failed {};

using application_result = std::variant<
    retry,
    applied,
    wait_failed,
    transition_failed>;

} // namespace managed_child_windows_fallback_result

inline managed_child_windows_fallback_result::execution_result
execute_managed_child_windows_fallback(
    const managed_child_release_action::poll_windows_fallback& target)
{
    using namespace managed_child_windows_fallback_result;

    // Borrowed authority: selection retains ownership in the exact occurrence.
    const auto process_handle = reinterpret_cast<HANDLE>(target.process_handle);
    const auto wait_result = WaitForSingleObject(process_handle, 20);
    if (wait_result == WAIT_TIMEOUT) {
        return timed_out{};
    }
    if (wait_result == WAIT_FAILED) {
        return wait_failed{failed_operation::wait, GetLastError()};
    }
    if (wait_result != WAIT_OBJECT_0) {
        return wait_failed{failed_operation::wait, ERROR_INVALID_DATA};
    }

    DWORD exit_status = 0;
    if (GetExitCodeProcess(process_handle, &exit_status) == 0) {
        return wait_failed{failed_operation::exit_query, GetLastError()};
    }
    return signaled{exit_status};
}

inline managed_child_windows_fallback_result::application_result
apply_managed_child_windows_fallback_locked(
    Managed_child_custody_record& custody,
    const managed_child_release_action::poll_windows_fallback& target,
    const managed_child_windows_fallback_result::execution_result& result,
    Managed_child_exit_publication& exit_publication)
{
    using namespace managed_child_windows_fallback_result;

    if (std::holds_alternative<timed_out>(result)) {
        return retry{};
    }
    if (const auto failure = std::get_if<wait_failed>(&result)) {
        return *failure;
    }

    auto* exact = custody.find_occurrence_locked(
        target.process_instance_id, target.occurrence);
    if (!exact ||
        !exact->native.fallback_wait_available() ||
        exact->native.process_handle() != target.process_handle)
    {
        return transition_failed{};
    }

    const auto& exit = std::get<signaled>(result);
    uintptr_t released_handle = 0;
    exit_publication = record_managed_child_exit_locked(
        custody.identity,
        *exact,
        exit.exit_status,
        true);
    if (!exit_publication.transition_valid ||
        !exact->native.take_owned_process_handle(
            target.process_handle, released_handle))
    {
        return transition_failed{};
    }
    return applied{released_handle};
}

} // namespace detail
#endif

inline void Managed_process::request_child_custody_release(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    detail::Release_mode release_mode)
{
    if (!custody) {
        return;
    }

    uint64_t release_attempt_generation = 0;
    bool readiness_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        release_attempt_generation =
            custody->release_state.request(release_mode);
        readiness_cancelled = !custody->readiness_cancelled.exchange(
            true, std::memory_order_release);
        if (release_attempt_generation == 0) {
            custody->changed.notify_all();
        }
    }
    if (readiness_cancelled && s_coord) {
        s_coord->notify_managed_child_readiness_cancelled();
    }
    if (release_attempt_generation == 0) {
        return;
    }

    start_child_custody_release_worker(
        custody, release_attempt_generation);
}

inline void Managed_process::start_child_custody_release_worker(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    uint64_t release_attempt_generation)
{
    instance_id_type failure_iid = invalid_instance_id;
    uint32_t failure_occurrence = 0;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (!custody->occurrences.empty()) {
            failure_iid = custody->occurrences.front().process_instance_id;
            failure_occurrence = custody->occurrences.front().occurrence;
        }
    }

    try {
        start_child_custody_worker(
            std::bind(
                &Managed_process::execute_child_custody_release_attempt,
                this,
                custody,
                release_attempt_generation,
                failure_iid,
                failure_occurrence),
            detail::test_hooks::k_managed_child_fail_release_worker_start,
            failure_iid,
            failure_occurrence);
    }
    catch (const std::exception& exception) {
        fail_release_attempt(
            custody,
            release_attempt_generation,
            {Managed_child_failure_kind::release_worker_start,
             failure_occurrence,
             0,
             exception.what()},
            failure_iid);
    }
    catch (...) {
        fail_release_attempt(
            custody,
            release_attempt_generation,
            {Managed_child_failure_kind::release_worker_start,
             failure_occurrence,
             0,
             "Unknown exception while starting managed-child release worker"},
            failure_iid);
    }
}

inline void Managed_process::execute_child_custody_release_attempt(
    std::shared_ptr<detail::Managed_child_custody_record> custody,
    uint64_t release_attempt_generation,
    instance_id_type failure_iid,
    uint32_t failure_occurrence)
{
    try {
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_release_worker,
            failure_iid,
            failure_occurrence);

    detail::Managed_child_release_roster release_roster;
    {
        std::unique_lock<std::mutex> lock(custody->mutex);
        custody->changed.wait(lock, [&]() {
            return std::none_of(
                custody->occurrences.begin(), custody->occurrences.end(),
                [](const detail::Managed_child_occurrence_record& occurrence) {
                    return occurrence.setup ==
                        detail::Managed_child_occurrence_record::setup_state::pending;
                });
        });
        for (const auto& occurrence : custody->occurrences) {
            if (occurrence.setup ==
                detail::Managed_child_occurrence_record::setup_state::ownership_ready)
            {
                release_roster.push_back({
                    occurrence.process_instance_id,
                    occurrence.occurrence,
                    false});
            }
        }
    }

    for (auto& exact : release_roster) {
        {
            std::lock_guard<std::mutex> lock(m_child_custody_mutex);
            auto it = m_child_custody_by_process.find(
                exact.process_instance_id);
            if (it != m_child_custody_by_process.end()) {
                exact.slot_current =
                    it->second.custody.lock() == custody &&
                    it->second.occurrence == exact.occurrence;
            }
        }
    }

    auto release_attempt_active = [&]() {
        std::lock_guard<std::mutex> lock(custody->mutex);
        return custody->release_state.running(release_attempt_generation);
    };

    bool cleanup_applied = false;
    bool passive_wait_reported = false;
    using namespace detail::managed_child_release_action;
    while (true) {
        std::unique_lock<std::mutex> lock(custody->mutex);
        auto action = detail::plan_managed_child_release_action_locked(
            *custody,
            release_roster,
            release_attempt_generation,
            cleanup_applied);

        if (std::holds_alternative<stop_attempt>(action)) {
            uint64_t rerun_generation = 0;
            if (custody->release_state.mark_retryable(
                    release_attempt_generation) &&
                custody->release_state.running())
            {
                rerun_generation = custody->release_state.generation();
            }
            lock.unlock();
            custody->changed.notify_all();
            if (rerun_generation != 0) {
                start_child_custody_release_worker(
                    custody, rerun_generation);
            }
            return;
        }

        if (std::holds_alternative<complete_release>(action)) {
            if (!custody->release_state.mark_released(
                    release_attempt_generation))
            {
                lock.unlock();
                custody->changed.notify_all();
                return;
            }
            break;
        }

        if (!custody->release_state.cleanup_requested() &&
            !passive_wait_reported)
        {
            // Observation-only test seam: at this point the single
            // retained worker has evaluated the monotone cleanup flag
            // as false and is about to wait (or poll a fallback native
            // handle). The callback must not re-enter custody APIs.
            const auto first_current = std::find_if(
                release_roster.begin(), release_roster.end(),
                [](const detail::Managed_child_release_occurrence& exact) {
                    return exact.slot_current;
                });
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::k_managed_child_release_waiting_passive,
                first_current == release_roster.end()
                    ? invalid_instance_id
                    : first_current->process_instance_id,
                first_current == release_roster.end()
                    ? 0
                    : first_current->occurrence);
            passive_wait_reported = true;
        }

        if (std::holds_alternative<wait_for_change>(action)) {
            // Selection and the unconditional wait share this lock. Any
            // notification after planning is therefore observed before the
            // next plan, without a second availability predicate to drift.
            custody->changed.wait(lock);
            continue;
        }

        lock.unlock();

        if (auto cleanup = std::get_if<apply_cleanup>(&action)) {
            if (!execute_current_slot_child_retirement(
                    custody,
                    cleanup->targets,
                    release_attempt_generation,
                    detail::Release_mode::cleanup))
            {
                continue;
            }
            bool historical_cleanup_complete = true;
            for (const auto& target : cleanup->targets) {
                if (!release_attempt_active()) {
                    historical_cleanup_complete = false;
                    break;
                }
                const detail::Managed_child_occurrence_token token{
                    custody,
                    target.process_instance_id,
                    target.occurrence};
                bool native_terminal = false;
                bool communication_eligible = false;
                {
                    std::lock_guard<std::mutex> custody_lock(custody->mutex);
                    const auto* occurrence = custody->find_occurrence_locked(
                        target.process_instance_id, target.occurrence);
                    if (!occurrence) {
                        historical_cleanup_complete = false;
                        break;
                    }
                    native_terminal = occurrence->native.exited();
                    communication_eligible =
                        occurrence->transport.ready_to_retire();
                }
                if (!native_terminal && !cleanup_child_native(token)) {
                    record_release_attempt_blocker(
                        custody,
                        release_attempt_generation,
                        target.process_instance_id,
                        target.occurrence,
                        "Managed-child native cleanup remained incomplete");
                    historical_cleanup_complete = false;
                    break;
                }
                if (communication_eligible) {
                    const detail::Managed_child_release_roster exact_target{
                        target};
                    if (!execute_exact_historical_child_communication(
                            custody,
                            exact_target,
                            release_attempt_generation))
                    {
                        historical_cleanup_complete = false;
                        break;
                    }
                }
            }
            if (!historical_cleanup_complete) {
                continue;
            }
            cleanup_applied = true;
            continue;
        }

        if (auto current = std::get_if<retire_current_transport>(&action)) {
            if (!execute_current_slot_child_retirement(
                    custody,
                    current->targets,
                    release_attempt_generation,
                    detail::Release_mode::passive))
            {
                continue;
            }
            continue;
        }

        if (auto historical =
                std::get_if<retire_historical_communication>(&action))
        {
            if (!execute_exact_historical_child_communication(
                    custody,
                    historical->targets,
                    release_attempt_generation))
            {
                continue;
            }
            continue;
        }

#ifdef _WIN32
        auto fallback_target =
            std::get_if<poll_windows_fallback>(&action);
        // A retained fallback handle must not hide a later cleanup upgrade.
        // Poll briefly in this owned worker, then re-check the release mode;
        // public deadline callers continue to wait only on custody->changed.
        const auto fallback_execution =
            detail::execute_managed_child_windows_fallback(
                *fallback_target);
        auto fallback_application = detail::
            managed_child_windows_fallback_result::application_result{
                detail::managed_child_windows_fallback_result::retry{}};
        detail::Managed_child_exit_publication exit_publication;
        {
            std::lock_guard<std::mutex> lock(custody->mutex);
            fallback_application =
                detail::apply_managed_child_windows_fallback_locked(
                    *custody,
                    *fallback_target,
                    fallback_execution,
                    exit_publication);
            custody->changed.notify_all();
        }
        dispatch_child_exit_publication(std::move(exit_publication));
        if (const auto applied = std::get_if<detail::
                managed_child_windows_fallback_result::applied>(
                    &fallback_application))
        {
            CloseHandle(reinterpret_cast<HANDLE>(
                applied->released_handle));
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::
                    k_managed_child_windows_fallback_handle_closed,
                fallback_target->process_instance_id,
                fallback_target->occurrence);
        }
        if (const auto failure = std::get_if<detail::
                managed_child_windows_fallback_result::wait_failed>(
                    &fallback_application))
        {
            if (failure->operation == detail::
                    managed_child_windows_fallback_result::
                        failed_operation::exit_query)
            {
                throw std::runtime_error(
                    "Managed-child fallback native-exit query failed");
            }
            throw std::runtime_error(
                "Managed-child fallback native-exit wait failed");
        }
        if (std::holds_alternative<detail::
                managed_child_windows_fallback_result::transition_failed>(
                    fallback_application))
        {
            throw std::runtime_error(
                "Managed-child fallback native-exit transition failed");
        }
#endif
    }
    custody->changed.notify_all();
    retire_child_custody_if_complete(custody);
    }
    catch (const std::exception& exception) {
        fail_release_attempt(
            custody,
            release_attempt_generation,
            {Managed_child_failure_kind::release_worker_execution,
             failure_occurrence,
             0,
             exception.what()},
            failure_iid);
    }
    catch (...) {
        fail_release_attempt(
            custody,
            release_attempt_generation,
            {Managed_child_failure_kind::release_worker_execution,
             failure_occurrence,
             0,
             "Unknown exception in managed-child release worker"},
            failure_iid);
    }
}

inline void Managed_process::request_all_child_custody_releases()
{
    std::vector<std::shared_ptr<detail::Managed_child_custody_record>> custodies;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        for (const auto& entry : m_child_custodies) {
            custodies.push_back(entry.second);
        }
    }
    for (const auto& custody : custodies) {
        request_child_custody_release(custody);
    }
}

inline bool Managed_process::all_child_custodies_released() const
{
    std::lock_guard<std::mutex> lock(m_child_custody_mutex);
    return m_child_custodies.empty();
}

inline bool Managed_process::wait_for_all_child_custodies(
    std::chrono::steady_clock::time_point deadline)
{
    // Finalization closes teardown admission before waiting, so registry size
    // can only move toward zero here.
    std::unique_lock<std::mutex> lock(m_child_custody_mutex);
    return m_child_custody_changed.wait_until(
        lock, deadline, [this] { return m_child_custodies.empty(); });
}

inline detail::Managed_child_occurrence_token
Managed_process::child_custody_occurrence_token(
    instance_id_type process_instance_id) const
{
    detail::Managed_child_occurrence_token token;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        auto it = m_child_custody_by_process.find(process_instance_id);
        if (it != m_child_custody_by_process.end()) {
            token.custody = it->second.custody;
            token.process_instance_id = process_instance_id;
            token.occurrence = it->second.occurrence;
        }
    }
    return token;
}

inline detail::Managed_child_occurrence_token
Managed_process::child_custody_occurrence_token_exact(
    uint64_t          custody_identity,
    instance_id_type process_instance_id,
    uint32_t         occurrence) const
{
    std::shared_ptr<detail::Managed_child_custody_record> custody;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        const auto it = m_child_custodies.find(custody_identity);
        if (it == m_child_custodies.end()) {
            return {};
        }
        custody = it->second;
    }
    if (!custody) {
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (!custody->find_occurrence_locked(process_instance_id, occurrence)) {
            return {};
        }
    }
    return {custody, process_instance_id, occurrence};
}

inline void Managed_process::note_child_initialization_complete(
    const detail::Managed_child_occurrence_token& token)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return;
    }
    std::lock_guard<std::mutex> lock(custody->mutex);
    auto* occurrence = custody->find_occurrence_locked(
        token.process_instance_id, token.occurrence);
    if (occurrence) {
        occurrence->initialization_reservation_active = false;
    }
    custody->changed.notify_all();
}

inline void Managed_process::mark_child_coordinator_initialization_complete(
    Coordinator* coordinator,
    instance_id_type process_instance_id)
{
    if (coordinator) {
        coordinator->mark_initialization_complete(process_instance_id);
    }
}

inline void Managed_process::note_child_publication_retired(
    const detail::Managed_child_occurrence_token& token)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return;
    }
    std::lock_guard<std::mutex> lock(custody->mutex);
    auto* occurrence = custody->find_occurrence_locked(
        token.process_instance_id, token.occurrence);
    if (occurrence) {
        occurrence->transport.note_publication_retired();
    }
    custody->changed.notify_all();
}

inline detail::Managed_child_communication_authority_capture
Managed_process::capture_child_communication_retirement_authority(
    const detail::Managed_child_occurrence_token& token,
    const std::shared_ptr<Process_message_reader>& reader)
{
    using detail::Managed_child_communication_authority_capture;
    auto custody = token.custody.lock();
    if (!custody) {
        return Managed_child_communication_authority_capture::conflict;
    }
    if (reader &&
        (reader->get_process_instance_id() != token.process_instance_id ||
         reader->get_occurrence() != token.occurrence ||
         reader->get_managed_child_custody_identity() != custody->identity))
    {
        Log_stream(log_level::error)
            << "Managed-child communication authority identity mismatch.\n";
        return Managed_child_communication_authority_capture::conflict;
    }

    std::lock_guard<std::mutex> lock(custody->mutex);
    auto* occurrence = custody->find_occurrence_locked(
        token.process_instance_id, token.occurrence);
    if (!occurrence) {
        return Managed_child_communication_authority_capture::conflict;
    }
    // The release action was planned outside this lock. Coordinator-side
    // retirement may have completed meanwhile, before its exact reader is
    // erased from m_readers. That terminal transition satisfies the stale
    // action; it is not a loss or replacement of authority.
    if (occurrence->transport.fully_retired()) {
        return Managed_child_communication_authority_capture::already_terminal;
    }
    if (!occurrence->transport.capture_authority(reader)) {
        Log_stream(log_level::error)
            << "Managed-child communication authority changed for an exact occurrence.\n";
        return Managed_child_communication_authority_capture::conflict;
    }
    custody->changed.notify_all();
    return Managed_child_communication_authority_capture::captured;
}

inline bool Managed_process::capture_replaced_child_communication_authority(
    const std::shared_ptr<Process_message_reader>& reader)
{
    if (!reader || reader->get_managed_child_custody_identity() == 0) {
        return true;
    }
    std::shared_ptr<detail::Managed_child_custody_record> custody;
    {
        std::lock_guard<std::mutex> lock(m_child_custody_mutex);
        const auto found = m_child_custodies.find(
            reader->get_managed_child_custody_identity());
        if (found != m_child_custodies.end()) {
            custody = found->second;
        }
    }
    if (!custody) {
        return false;
    }
    bool communication_terminal = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        const auto* exact = custody->find_occurrence_locked(
            reader->get_process_instance_id(), reader->get_occurrence());
        if (!exact ||
            !exact->transport.publication_retired())
        {
            return false;
        }
        communication_terminal = exact->transport.retirement_terminal();
    }
    if (communication_terminal) {
        return true;
    }
    return capture_child_communication_retirement_authority(
            {custody, reader->get_process_instance_id(), reader->get_occurrence()},
            reader) !=
        detail::Managed_child_communication_authority_capture::conflict;
}

inline bool Managed_process::begin_child_communication_retirement(
    const detail::Managed_child_occurrence_token& token,
    uint64_t expected_release_attempt_generation,
    uint64_t& claimed_release_attempt_generation,
    std::shared_ptr<Process_message_reader>& reader)
{
    claimed_release_attempt_generation = 0;
    auto custody = token.custody.lock();
    if (!custody) {
        return false;
    }
    std::lock_guard<std::mutex> lock(custody->mutex);
    if (expected_release_attempt_generation != 0 &&
        !custody->release_state.running(
            expected_release_attempt_generation))
    {
        return false;
    }
    // A failed release pass is reopened only by the next explicit custody
    // release/cleanup/finalization call, which clears this attempt latch.
    if (custody->release_state.failing() ||
        custody->release_state.retryable())
    {
        return false;
    }
    auto* occurrence = custody->find_occurrence_locked(
        token.process_instance_id, token.occurrence);
    if (!occurrence || !occurrence->transport.begin_retirement(reader)) {
        return false;
    }
    if (custody->release_state.running())
    {
        claimed_release_attempt_generation = custody->release_state.generation();
    }
    return true;
}

inline bool Managed_process::complete_child_communication_retirement(
    const detail::Managed_child_occurrence_token& token,
    const std::shared_ptr<Process_message_reader>& reader)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return false;
    }
    std::shared_ptr<Process_message_reader> retired_authority;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* exact = custody->find_occurrence_locked(
            token.process_instance_id, token.occurrence);
        if (!exact ||
            !exact->transport.complete_retirement(reader, retired_authority))
        {
            return false;
        }
    }
    custody->changed.notify_all();

    detail::managed_child_transport_retirement_for_test(
        detail::test_hooks::
            k_managed_child_communication_terminal_before_reader_erase,
        token.process_instance_id,
        token.occurrence);

    if (reader) {
        Dispatch_unique_lock readers_lock(m_readers_mutex);
        auto it = m_readers.find(token.process_instance_id);
        if (it != m_readers.end() && it->second == reader) {
            m_readers.erase(it);
        }
    }
    // `retired_authority` and the caller's copy keep destruction outside both
    // lifecycle locks. The exact reader has already stopped successfully.
    return true;
}

inline void Managed_process::reset_child_communication_retirement(
    const detail::Managed_child_occurrence_token& token,
    uint64_t release_attempt_generation)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return;
    }
    bool retirement_reset = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* occurrence = custody->find_occurrence_locked(
            token.process_instance_id, token.occurrence);
        retirement_reset = occurrence && occurrence->transport.reset_retirement();
    }
    if (retirement_reset) {
        record_release_attempt_blocker(
            custody,
            release_attempt_generation,
            token.process_instance_id,
            token.occurrence,
            "Managed-child communication retirement remained incomplete");
    }
}

inline bool Managed_process::join_child_communication(
    const detail::Managed_child_occurrence_token& token,
    const std::shared_ptr<Process_message_reader>& reader)
{
    detail::managed_child_transport_retirement_for_test(
        detail::test_hooks::k_managed_child_communication_before_join,
        token.process_instance_id,
        token.occurrence);
    if (reader && !reader->stop_and_wait(1.0)) {
        detail::managed_child_transport_retirement_for_test(
            detail::test_hooks::k_managed_child_communication_join_incomplete,
            token.process_instance_id,
            token.occurrence);
        return false;
    }
    if (!complete_child_communication_retirement(token, reader)) {
        return false;
    }
    detail::managed_child_transport_retirement_for_test(
        detail::test_hooks::k_managed_child_communication_after_join,
        token.process_instance_id,
        token.occurrence);
    return true;
}

inline bool Managed_process::execute_current_slot_child_retirement(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    const detail::Managed_child_release_roster& targets,
    uint64_t release_attempt_generation,
    detail::Release_mode release_mode)
{
    auto release_attempt_active = [&]() {
        std::lock_guard<std::mutex> lock(custody->mutex);
        return custody->release_state.running(release_attempt_generation);
    };
    const auto first_current = std::find_if(
        targets.begin(), targets.end(),
        [](const detail::Managed_child_release_occurrence& target) {
            return target.slot_current;
        });
    if (release_mode == detail::Release_mode::cleanup &&
        first_current != targets.end())
    {
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_cleanup_actions,
            first_current->process_instance_id,
            first_current->occurrence);
    }
    for (const auto& target : targets) {
        if (!target.slot_current) {
            continue;
        }
        const auto occurrence = target.occurrence;
        const auto process_iid = target.process_instance_id;
        if (!release_attempt_active()) {
            return false;
        }
        const detail::Managed_child_occurrence_token token{
            custody,
            process_iid,
            occurrence};
        auto converge_native = [&]() -> bool {
            if (release_mode == detail::Release_mode::passive ||
                cleanup_child_native(token))
            {
                return true;
            }
            record_release_attempt_blocker(
                custody,
                release_attempt_generation,
                process_iid,
                occurrence,
                "Managed-child native cleanup remained incomplete");
            return false;
        };
        if (release_mode == detail::Release_mode::cleanup) {
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::k_managed_child_cleanup_before_actions,
                process_iid,
                occurrence);
        }
        if (s_coord && s_coord->set_draining_state(process_iid, 1)) {
            s_coord->note_draining_state_change();
        }
        if (release_mode == detail::Release_mode::cleanup) {
            release_lifeline(process_iid);
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::k_managed_child_cleanup_lifeline_released,
                process_iid,
                occurrence);
        }
        if (s_coord && !s_coord->unpublish_transceiver(process_iid)) {
            std::shared_ptr<Process_message_reader> retiring_reader;
            {
                Dispatch_shared_lock readers_lock(m_readers_mutex);
                auto reader = m_readers.find(process_iid);
                if (reader != m_readers.end()) {
                    retiring_reader = reader->second;
                }
            }
            const auto authority_capture =
                capture_child_communication_retirement_authority(
                    token, retiring_reader);
            if (authority_capture == detail::
                    Managed_child_communication_authority_capture::conflict)
            {
                record_release_attempt_blocker(
                    custody,
                    release_attempt_generation,
                    process_iid,
                    occurrence,
                    "Managed-child communication retirement authority unavailable");
                return false;
            }
            uint64_t claimed_release_attempt_generation = 0;
            std::shared_ptr<Process_message_reader> claimed_reader;
            const bool communication_claimed =
                authority_capture == detail::
                    Managed_child_communication_authority_capture::captured &&
                begin_child_communication_retirement(
                    token,
                    release_attempt_generation,
                    claimed_release_attempt_generation,
                    claimed_reader);
            // already_terminal falls through: coordinator-side retirement
            // satisfied this stale action without a new retirement claim.
            if (authority_capture == detail::
                    Managed_child_communication_authority_capture::captured &&
                !communication_claimed)
            {
                if (!release_attempt_active()) {
                    converge_native();
                    return false;
                }
            }
            else
            if (communication_claimed) {
            // A first miss is provisional: a publish RPC may already
            // be in flight. Stop and terminally join the exact reader,
            // then re-enter coordinator publication authority. The
            // second canonical unpublish blocks behind any in-flight
            // publisher and either removes it or confirms absence after
            // no reader remains able to deliver another publication.
            detail::managed_child_prepublication_cleanup_for_test(
                detail::test_hooks::k_managed_child_prepublication_first_miss,
                process_iid,
                occurrence);
            try {
                if (!join_child_communication(token, claimed_reader)) {
                    reset_child_communication_retirement(
                        token, claimed_release_attempt_generation);
                    converge_native();
                    return false;
                }
            }
            catch (...) {
                reset_child_communication_retirement(
                    token, claimed_release_attempt_generation);
                converge_native();
                throw;
            }
            detail::managed_child_prepublication_cleanup_for_test(
                detail::test_hooks::k_managed_child_prepublication_reader_terminal,
                process_iid,
                occurrence);
            if (!s_coord->unpublish_transceiver(process_iid)) {
                note_child_publication_retired(token);
            }
            s_coord->mark_initialization_complete(process_iid);
            note_child_initialization_complete(token);
            }
        }
        if (!release_attempt_active()) {
            converge_native();
            return false;
        }
        if (release_mode == detail::Release_mode::cleanup && s_coord) {
            // Observation-only test seam. The coordinator publication
            // transaction has retired the process name and transport
            // retirement has been started for the exact occurrence;
            // native convergence has not started yet. The callback must
            // not re-enter custody APIs.
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::
                    k_managed_child_cleanup_before_native_convergence,
                process_iid,
                occurrence);
        }
        if (release_mode == detail::Release_mode::cleanup &&
            !converge_native())
        {
            return false;
        }
        if (release_mode == detail::Release_mode::cleanup && s_coord) {
            // The coordinator path above either reported the exact
            // occurrence's publication/communication retirement, or
            // the canonical second miss confirmed both after its
            // reader became terminal.
            detail::managed_child_cleanup_for_test(
                detail::test_hooks::k_managed_child_cleanup_retirement_confirmed,
                process_iid,
                occurrence);
        }
    }
    return release_attempt_active();
}

inline bool Managed_process::execute_exact_historical_child_communication(
    const std::shared_ptr<detail::Managed_child_custody_record>& custody,
    const detail::Managed_child_release_roster& targets,
    uint64_t release_attempt_generation)
{
    for (const auto& target : targets) {
        bool communication_eligible = false;
        {
            std::lock_guard<std::mutex> lock(custody->mutex);
            if (!custody->release_state.running(
                    release_attempt_generation))
            {
                return false;
            }
            const auto* occurrence = custody->find_occurrence_locked(
                target.process_instance_id, target.occurrence);
            if (!occurrence) {
                return false;
            }
            communication_eligible =
                occurrence->transport.ready_to_retire();
        }
        if (!communication_eligible) {
            continue;
        }

        const detail::Managed_child_occurrence_token token{
            custody, target.process_instance_id, target.occurrence};
        uint64_t claimed_generation = 0;
        std::shared_ptr<Process_message_reader> reader;
        if (!begin_child_communication_retirement(
                token, release_attempt_generation,
                claimed_generation, reader))
        {
            continue;
        }
        try {
            if (!join_child_communication(token, reader)) {
                reset_child_communication_retirement(
                    token, claimed_generation);
                return false;
            }
        }
        catch (...) {
            reset_child_communication_retirement(token, claimed_generation);
            throw;
        }
    }

    std::lock_guard<std::mutex> lock(custody->mutex);
    return custody->release_state.running(release_attempt_generation);
}

inline void Managed_process::retire_child_communication(
    const detail::Managed_child_occurrence_token& token,
    std::shared_ptr<Process_message_reader> reader)
{
    if (!token) {
        return;
    }
    const auto authority_capture =
        capture_child_communication_retirement_authority(token, reader);
    if (authority_capture != detail::
            Managed_child_communication_authority_capture::captured)
    {
        return;
    }
    uint64_t release_attempt_generation = 0;
    std::shared_ptr<Process_message_reader> claimed_reader;
    if (!begin_child_communication_retirement(
            token, 0, release_attempt_generation, claimed_reader))
    {
        return;
    }
    try {
        start_child_custody_worker(
            [this, token, release_attempt_generation,
             reader = std::move(claimed_reader)]() mutable {
            try {
                if (!join_child_communication(token, reader)) {
                    reset_child_communication_retirement(
                        token, release_attempt_generation);
                }
            }
            catch (...) {
                reset_child_communication_retirement(
                    token, release_attempt_generation);
                throw;
            }
        },
        detail::test_hooks::k_managed_child_fail_communication_worker_start,
        token.process_instance_id,
        token.occurrence);
    }
    catch (...) {
        // Publication retirement remains authoritative. Without a terminal
        // reader join, communication stays nonterminal and custody cannot
        // report completion.
        reset_child_communication_retirement(
            token, release_attempt_generation);
    }
}

inline void Managed_process::note_child_os_exit(
    const detail::Managed_child_occurrence_token& token,
    int wait_status,
    bool wait_status_available)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return;
    }
    detail::managed_child_cleanup_for_test(
        detail::test_hooks::k_managed_child_native_exit_before_publication,
        token.process_instance_id,
        token.occurrence);
    if (detail::managed_child_failure_selected_for_test(
            detail::test_hooks::k_managed_child_force_exit_status_unavailable,
            token.process_instance_id,
            token.occurrence))
    {
        wait_status_available = false;
    }
    detail::Managed_child_exit_publication exit_publication;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* occurrence = custody->find_occurrence_locked(
            token.process_instance_id, token.occurrence);
        if (!occurrence) {
            return;
        }
        exit_publication = detail::record_managed_child_exit_locked(
            custody->identity,
            *occurrence,
            static_cast<std::uint32_t>(wait_status),
            wait_status_available);
        if (!exit_publication.transition_valid) {
            Log_stream(log_level::error)
                << "Managed-child exact native-exit fact conflicted with "
                   "retained authority.\n";
        }
        custody->changed.notify_all();
    }
    dispatch_child_exit_publication(std::move(exit_publication));
}

#ifndef _WIN32
inline bool Managed_process::signal_child_native_exact(
    const detail::Managed_child_occurrence_token& token,
    int signal_number)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return false;
    }

    std::lock_guard<std::mutex> guard(m_spawned_child_pids_mutex);
    auto slot = std::find_if(
        m_spawned_child_pids.begin(),
        m_spawned_child_pids.end(),
        [&](const Spawned_child_reap_slot& candidate) {
            if (candidate.pid <= 0 ||
                candidate.occurrence.custody.lock() != custody)
            {
                return false;
            }
            if (candidate.occurrence.process_instance_id !=
                token.process_instance_id)
            {
                return false;
            }
            return candidate.occurrence.occurrence == token.occurrence;
        });
    if (slot == m_spawned_child_pids.end() ||
        !slot->start_stamp_available)
    {
        return false;
    }

    const auto observed = query_process_start_stamp(
        static_cast<uint32_t>(slot->pid));
    if (!observed || *observed != slot->start_stamp) {
        return false;
    }
    return ::kill(slot->pid, signal_number) == 0;
}
#endif

inline bool Managed_process::wait_for_child_native_exit(
    const detail::Managed_child_occurrence_token& token,
    std::chrono::steady_clock::time_point deadline)
{
    auto custody = token.custody.lock();
    if (!custody) {
        return false;
    }
    std::unique_lock<std::mutex> lock(custody->mutex);
    auto exited = [&]() {
        const auto* occurrence = custody->find_occurrence_locked(
            token.process_instance_id, token.occurrence);
        return occurrence && occurrence->native.exited();
    };
    custody->changed.wait_until(lock, deadline, exited);
    return exited();
}

inline bool Managed_process::cleanup_child_native(
    const detail::Managed_child_occurrence_token& token)
{
    constexpr auto grace_period = std::chrono::seconds(6);
    constexpr auto soft_period  = std::chrono::milliseconds(250);
    constexpr auto hard_period  = std::chrono::seconds(5);

#ifndef _WIN32
    if (wait_for_child_native_exit(
            token, std::chrono::steady_clock::now() + grace_period))
    {
        detail::managed_child_cleanup_for_test(
            detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed,
            token.process_instance_id,
            token.occurrence);
        return true;
    }
#endif

    auto custody = token.custody.lock();
    if (!custody) {
        return false;
    }

#ifdef _WIN32
    HANDLE process = nullptr;
    DWORD pid = 0;
    uintptr_t retained_handle_value = 0;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        auto* occurrence = custody->find_occurrence_locked(
            token.process_instance_id, token.occurrence);
        if (!occurrence) {
            return false;
        }
        if (occurrence->native.exited()) {
            return true;
        }
        if (!occurrence->native.process_handle_owned() ||
            occurrence->native.process_handle() == 0)
        {
            return false;
        }
        pid = static_cast<DWORD>(occurrence->native.pid());
        retained_handle_value = occurrence->native.process_handle();
        HANDLE retained = reinterpret_cast<HANDLE>(retained_handle_value);
        if (!DuplicateHandle(
                GetCurrentProcess(),
                retained,
                GetCurrentProcess(),
                &process,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS))
        {
            return false;
        }
    }
    Instantiator process_guard(std::function<void()>([&]() {
        if (process) {
            CloseHandle(process);
        }
    }));

    auto record_exit = [&]() -> bool {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process, &exit_code) == 0) {
            return false;
        }
        uintptr_t released_handle = 0;
        bool transition_valid = false;
        detail::Managed_child_exit_publication exit_publication;
        {
            std::lock_guard<std::mutex> lock(custody->mutex);
            auto* occurrence = custody->find_occurrence_locked(
                token.process_instance_id, token.occurrence);
            if (occurrence) {
                exit_publication = detail::record_managed_child_exit_locked(
                    custody->identity,
                    *occurrence,
                    exit_code,
                    true);
                transition_valid = exit_publication.transition_valid;
                if (transition_valid &&
                    !occurrence->native.exit_observer_registered() &&
                    occurrence->native.process_handle_owned())
                {
                    transition_valid =
                        occurrence->native.take_owned_process_handle(
                            retained_handle_value, released_handle);
                }
                custody->changed.notify_all();
            }
        }
        dispatch_child_exit_publication(std::move(exit_publication));
        if (released_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(released_handle));
        }
        return transition_valid;
    };
    const auto grace_ms = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            grace_period).count());
    const auto grace_wait = WaitForSingleObject(process, grace_ms);
    if (grace_wait == WAIT_OBJECT_0) {
        const bool recorded = record_exit();
        if (!recorded) {
            return false;
        }
        detail::managed_child_cleanup_for_test(
            detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed,
            token.process_instance_id,
            token.occurrence);
        return true;
    }
    if (grace_wait != WAIT_TIMEOUT) {
        return false;
    }

    detail::managed_child_cleanup_for_test(
        detail::test_hooks::k_managed_child_cleanup_soft_termination,
        token.process_instance_id,
        token.occurrence);
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    DWORD wait_result = WaitForSingleObject(
        process, static_cast<DWORD>(soft_period.count()));
    if (wait_result == WAIT_TIMEOUT) {
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_native_hard_termination,
            token.process_instance_id,
            token.occurrence);
        detail::managed_child_cleanup_for_test(
            detail::test_hooks::k_managed_child_cleanup_hard_termination,
            token.process_instance_id,
            token.occurrence);
        if (!TerminateProcess(process, 137)) {
            wait_result = WaitForSingleObject(process, 0);
            if (wait_result != WAIT_OBJECT_0) {
                return false;
            }
        }
        else {
            wait_result = WaitForSingleObject(
                process, static_cast<DWORD>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        hard_period).count()));
        }
    }
    if (wait_result != WAIT_OBJECT_0) {
        return false;
    }
    const bool recorded = record_exit();
    if (!recorded) {
        return false;
    }
    detail::managed_child_cleanup_for_test(
        detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed,
        token.process_instance_id,
        token.occurrence);
    return true;
#else
    detail::managed_child_cleanup_for_test(
        detail::test_hooks::k_managed_child_cleanup_soft_termination,
        token.process_instance_id,
        token.occurrence);
    const bool soft_signaled = signal_child_native_exact(token, SIGTERM);
    if (wait_for_child_native_exit(
            token, std::chrono::steady_clock::now() + soft_period))
    {
        detail::managed_child_cleanup_for_test(
            detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed,
            token.process_instance_id,
            token.occurrence);
        return true;
    }
    if (!soft_signaled) {
        return false;
    }

    detail::managed_child_failure_for_test(
        detail::test_hooks::k_managed_child_fail_native_hard_termination,
        token.process_instance_id,
        token.occurrence);
    detail::managed_child_cleanup_for_test(
        detail::test_hooks::k_managed_child_cleanup_hard_termination,
        token.process_instance_id,
        token.occurrence);
    const bool hard_signaled = signal_child_native_exact(token, SIGKILL);
    if (!hard_signaled &&
        !wait_for_child_native_exit(
            token, std::chrono::steady_clock::now()))
    {
        return false;
    }
#endif

    if (wait_for_child_native_exit(
            token, std::chrono::steady_clock::now() + hard_period))
    {
        detail::managed_child_cleanup_for_test(
            detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed,
            token.process_instance_id,
            token.occurrence);
        return true;
    }
    return false;
}

inline Managed_process::Spawn_result Managed_process::spawn_swarm_process(
    const Spawn_swarm_process_args& s,
    detail::Managed_child_launch_attempt& launch_attempt)
{
    auto admitted_failure_occurrence = [&]() {
        if (!s.custody) {
            return uint32_t{0};
        }
        std::lock_guard<std::mutex> lock(s.custody->mutex);
        return s.custody->find_occurrence_locked(s.piid, s.occurrence)
            ? s.occurrence
            : uint32_t{0};
    };
    Spawn_result result;
    try {
        result = spawn_swarm_process_impl(s, launch_attempt);
    }
    catch (const std::exception& exception) {
        try {
            note_child_custody_failure(
                s.custody,
                {Managed_child_failure_kind::setup_exception,
                 admitted_failure_occurrence(),
                 0,
                 exception.what()});
        }
        catch (...) {
        }
        throw;
    }
    catch (...) {
        try {
            note_child_custody_failure(
                s.custody,
                {Managed_child_failure_kind::setup_exception,
                 admitted_failure_occurrence(),
                 0,
                 "Managed child setup threw an unknown exception"});
        }
        catch (...) {
        }
        throw;
    }
    if (!result.success &&
        result.failure.kind != Managed_child_failure_kind::none)
    {
        note_child_custody_failure(s.custody, result.failure);
    }
    return result;
}

inline Managed_process::Spawn_result Managed_process::spawn_swarm_process_impl(
    const Spawn_swarm_process_args& s,
    detail::Managed_child_launch_attempt& launch_attempt)
{
    assert(s_coord);
    Spawn_result result;
    result.binary_name      = s.binary_name;
    result.instance_id      = s.piid;
    result.lifeline_enabled = s.lifetime.enable_lifeline;

    if (!s.custody) {
        result.failure.kind = Managed_child_failure_kind::custody_not_accepted;
        result.failure.message = "Managed child launch requires accepted custody";
        return result;
    }
    auto custody = s.custody;

    if (!launch_attempt) {
        result.failure.kind = Managed_child_failure_kind::custody_closed;
        result.failure.message = "Managed child custody is closed";
        return result;
    }
    if (!launch_attempt.matches(custody, s.piid, s.occurrence)) {
        result.failure.kind = Managed_child_failure_kind::setup_exception;
        result.failure.message =
            "Managed child setup settlement identity mismatch";
        return result;
    }
    result.failure.occurrence = s.occurrence;

    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        if (!custody->release_state.open()) {
            result.failure.kind = Managed_child_failure_kind::custody_closed;
            result.failure.message = "Managed child custody closed before setup";
            return result;
        }
    }

    {
        std::lock_guard<std::mutex> cache_lock(m_cached_spawns_mutex);
        const auto completed_next =
            s.occurrence == std::numeric_limits<uint32_t>::max()
                ? s.occurrence
                : s.occurrence + 1;
        auto cached = m_cached_spawns.find(s.piid);
        const auto next_occurrence =
            cached != m_cached_spawns.end() &&
                cached->second.custody == custody
                ? std::max(cached->second.occurrence, completed_next)
                : completed_next;
        m_cached_spawns[s.piid] = s;
        m_cached_spawns[s.piid].custody = custody;
        m_cached_spawns[s.piid].occurrence = next_occurrence;
    }

    auto args = s.args;
    if (s.occurrence != 0 &&
        std::find(
            args.begin(), args.end(), detail::k_skip_startup_barrier_arg) ==
            args.end())
    {
        args.push_back(detail::k_skip_startup_barrier_arg);
    }
    args.insert(args.end(), {"--recovery_occurrence", std::to_string(s.occurrence)});

    // Before spawning the new process, we have to assure that the corresponding
    // reading threads are up and running.
    if (!prepare_process_reader(s.piid, s.occurrence, true)) {
        result.success       = false;
        result.failure.kind = Managed_child_failure_kind::reader_setup;
        result.failure.message = "Failed to prepare process reader";
        return result;
    }
    launch_attempt.mark_reader_prepared();

    bool custody_closed_during_reader_setup = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        custody_closed_during_reader_setup =
            !custody->release_state.open();
    }
    if (custody_closed_during_reader_setup) {
        result.failure.kind = Managed_child_failure_kind::custody_closed;
        result.failure.message = "Managed child custody closed during reader setup";
        return result;
    }

    detail::managed_child_failure_for_test(
        detail::test_hooks::k_managed_child_fail_pre_create_setup,
        s.piid,
        s.occurrence);

#ifndef _WIN32
    launch_attempt.reserve_posix_reap_slot();
#endif

    bool custody_closed_before_creation = false;
    {
        std::lock_guard<std::mutex> lock(custody->mutex);
        custody_closed_before_creation =
            !custody->release_state.open();
    }
    if (custody_closed_before_creation) {
#ifndef _WIN32
        launch_attempt.cancel_posix_native_handoff();
#endif
        result.failure.kind = Managed_child_failure_kind::custody_closed;
        result.failure.message =
            "Managed child custody closed before OS creation";
        return result;
    }

    bool spawn_ready = true;
    int  spawn_error = 0;
    std::string spawn_error_message;
#ifdef _WIN32
    HANDLE spawned_process_handle    = nullptr;
#endif

    int spawned_pid = -1;
#ifndef _WIN32
    bool spawned_process_already_reaped = false;
    bool spawned_wait_status_available = false;
    int spawned_wait_status = 0;
#endif
    Spawn_detached_options spawn_options;
    spawn_options.prog = s.binary_name.c_str();
    spawn_options.child_pid_out = &spawned_pid;
    spawn_options.env_overrides = s.env_overrides;
#ifdef _WIN32
    spawn_options.child_process_handle_out = &spawned_process_handle;
#endif

    if (s.lifetime.enable_lifeline) {
#ifdef _WIN32
        if (!launch_attempt.create_lifeline(&spawn_error)) {
            spawn_ready = false;
            spawn_error_message = "Failed to create lifeline pipe";
            result.failure.kind = Managed_child_failure_kind::lifeline_setup;
        }
        else {
            const auto lifeline_read_handle = reinterpret_cast<HANDLE>(
                launch_attempt.lifeline_read_endpoint());
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
        if (!launch_attempt.create_lifeline(&spawn_error)) {
            spawn_ready = false;
            spawn_error_message = "Failed to create lifeline pipe";
            result.failure.kind = Managed_child_failure_kind::lifeline_setup;
        }
        else {
            const int lifeline_read_fd = static_cast<int>(
                launch_attempt.lifeline_read_endpoint());
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
    C_string_vector cargs(std::move(args));
    spawn_options.argv = cargs.v();

    if (!launch_attempt.acquire_initialization_reservation(s_coord))
    {
#ifndef _WIN32
        launch_attempt.cancel_posix_native_handoff();
#endif
        result.failure.kind = Managed_child_failure_kind::setup_exception;
        result.failure.message =
            detail::test_hooks::managed_child_invariant_name(
                detail::test_hooks::Managed_child_invariant::
                    exact_occurrence_lookup);
        return result;
    }

    if (spawn_ready) {
        Log_stream(log_level::debug)
            << "spawn_swarm_process: launching OS process"
            << " process_instance_id=" << static_cast<unsigned long long>(s.piid)
            << " occurrence=" << s.occurrence
            << " binary='" << s.binary_name << "'"
            << " lifeline=" << (s.lifetime.enable_lifeline ? "enabled" : "disabled")
            << "\n";

        const auto native_result =
            detail::spawn_detached_with_result(spawn_options);
#ifdef _WIN32
        launch_attempt.adopt_windows_process_handle(
            reinterpret_cast<uintptr_t>(spawned_process_handle));
        spawned_process_handle = nullptr;
#endif
        result.success = native_result.created();
        spawned_pid = native_result.pid;
        result.os_pid = native_result.pid;
#ifndef _WIN32
        spawned_process_already_reaped =
            native_result.state ==
                detail::Spawn_detached_result::State::created_reaped;
        spawned_wait_status_available = native_result.wait_status_available;
        spawned_wait_status = native_result.wait_status;
#endif
        result.os_process_created =
            result.success &&
            (result.os_pid > 0
#ifdef _WIN32
                || launch_attempt.windows_process_handle() != 0
#endif
            );
        if (result.success && !result.os_process_created) {
            result.success = false;
            result.failure.kind = Managed_child_failure_kind::native_identity;
            result.failure.message =
                "OS process creation returned without a bindable native identity";
        }

#ifdef _WIN32
        if (result.os_process_created) {
            launch_attempt.commit_windows_native_authority(result.os_pid);
        }
        else {
            launch_attempt.confirm_windows_native_absent();
        }
#endif
    }
    else {
        result.success = false;
        result.failure.native_error = spawn_error;
        std::ostringstream error_msg;
        error_msg << spawn_error_message;
        if (spawn_error != 0) {
            std::error_code ec(spawn_error, std::system_category());
            error_msg << " (errno " << spawn_error << ": " << ec.message() << ')';
        }
        result.failure.message = error_msg.str();
    }

    if (result.os_process_created) {
        launch_attempt.transfer_reader();
    }
    launch_attempt.close_lifeline_read_endpoint();

    if (result.success) {
#ifndef _WIN32
        const auto posix_handoff =
            launch_attempt.commit_posix_native_handoff(
                spawned_pid,
                spawned_process_already_reaped,
                spawned_wait_status_available,
                spawned_wait_status);
        if (posix_handoff.child_reaped) {
            detail::child_reaped_for_test(
                static_cast<pid_t>(spawned_pid),
                spawned_wait_status);
        }
        else if (!spawned_process_already_reaped && spawned_pid > 0) {
            // A very short-lived child may exit before the SIGCHLD dispatcher
            // observes the newly tracked PID. Reap once after handoff so the
            // authoritative exact-exit fact cannot be lost in that race.
            reap_finished_children();
        }
#endif
        result.lifeline_write_retained =
            launch_attempt.transfer_lifeline_write();

#ifndef _WIN32
        if (posix_handoff.reservation_lookup_failed) {
            throw std::runtime_error(
                detail::test_hooks::managed_child_invariant_name(
                    detail::test_hooks::Managed_child_invariant::
                        posix_reap_reservation_lookup));
        }
#endif

        // Native identity and all platform authority needed to observe exit
        // are now retained. Failures from this point unwind into owned custody.
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_post_native_setup,
            s.piid,
            s.occurrence);

#ifdef _WIN32
        if (result.os_process_created) {
            launch_attempt.start_windows_native_observer();
        }
#else
        // POSIX uses only the central reaper. The PID was inserted into its
        // pre-reserved roster above before this injected registration failure.
        detail::managed_child_failure_for_test(
            detail::test_hooks::k_managed_child_fail_native_observer_start,
            s.piid,
            s.occurrence);
#endif

        Log_stream(log_level::debug)
            << "spawn_swarm_process: OS spawn succeeded"
            << " process_instance_id=" << static_cast<unsigned long long>(s.piid)
            << " pid=" << result.os_pid
            << " binary='" << s.binary_name << "'"
            << "\n";

        // Create an entry in the coordinator's transceiver registry.
        // This is essential for the implementation of publish_transceiver()
        {
            std::lock_guard lock(s_coord->m_publish_mutex);
            s_coord->m_transceiver_registry[s.piid];
        }

        // create the readers. The next line will start the reader threads,
        // which might take some time. At this stage, we do not have to wait
        // until they are ready for messages.

        launch_attempt.transfer_initialization_reservation();
    }
    else {
        int saved_errno = 0;
#ifdef _WIN32
        if (result.failure.native_error != 0 || !result.failure.message.empty()) {
            saved_errno = result.failure.native_error;
        }
        else {
            _get_errno(&saved_errno);
        }
#else
        saved_errno = result.failure.native_error != 0
            ? result.failure.native_error
            : errno;
#endif
        if (result.failure.message.empty()) {
            result.failure.native_error = saved_errno;
            if (result.failure.kind == Managed_child_failure_kind::none) {
                result.failure.kind = Managed_child_failure_kind::native_spawn;
            }
            std::ostringstream error_msg;
            error_msg << "Failed to spawn process";
            if (saved_errno != 0) {
                std::error_code ec(saved_errno, std::system_category());
                error_msg << " (errno " << saved_errno << ": " << ec.message() << ')';
            }
            result.failure.message = error_msg.str();
        }

#ifndef _WIN32
        launch_attempt.cancel_posix_native_handoff();
#else
        launch_attempt.confirm_windows_native_absent();
#endif
        // Log spawn failures to stderr
        if (!spawn_ready && !result.failure.message.empty()) {
            Log_stream(log_level::error) << result.failure.message << "\n";
        }
        else
        if (result.failure.native_error != 0) {
            std::error_code ec(
                result.failure.native_error, std::system_category());
            Log_stream(log_level::error)
                << "failed to launch " << s.binary_name
                << " (errno " << result.failure.native_error << ": "
                << ec.message() << ")\n";
        }
        else {
            Log_stream(log_level::error)
                << "failed to launch " << s.binary_name << "\n";
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
        else
        if (s_coord_id != invalid_instance_id) {
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
            std::lock_guard init_lock(s_coord->m_init_tracking_mutex);
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

            Spawn_swarm_process_args spawn_args;
            spawn_args.binary_name = it->entry.m_binary_name;
            spawn_args.args        = std::move(all_args);
            spawn_args.piid        = it->assigned_instance_id;
            spawn_args.custody     = accept_child_custody();

            Spawn_result result;
            try {
                auto launch_attempt = admit_child_custody_occurrence(
                    spawn_args.custody,
                    spawn_args.piid,
                    spawn_args.occurrence);
                result = spawn_swarm_process(spawn_args, launch_attempt);
            }
            catch (...) {
                request_child_custody_release(
                    spawn_args.custody, detail::Release_mode::cleanup);
                throw;
            }
            if (result.success) {
                successfully_spawned.insert(it->assigned_instance_id);
            }
            else {
                request_child_custody_release(spawn_args.custody);
                spawn_failures.emplace_back(
                    result.binary_name,
                    result.instance_id,
                    init_error::cause::spawn_failed,
                    result.failure.native_error,
                    result.failure.message
                );
            }
        }

        auto all_processes = successfully_spawned;
        all_processes.insert(m_instance_id);

        m_group_all      = s_coord->make_process_group("_sintra_all_processes",      all_processes);
        m_group_external = s_coord->make_process_group("_sintra_external_processes", successfully_spawned);
        notify_init_complete();

        s_branch_index = 0;

        if (!spawn_failures.empty()) {
            std::vector<instance_id_type> successful_list(
                successfully_spawned.begin(),
                successfully_spawned.end()
            );
            successful_list.push_back(m_instance_id); // Include coordinator

            throw init_error(std::move(spawn_failures), std::move(successful_list));
        }
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
    if (!m_skip_startup_barrier) {
        bool all_started = Process_group::rpc_barrier(m_group_all, UIBS, 0);

        // If we're the coordinator and have failures to report, throw init_error
        if (s_coord && !all_started) {
            // If barrier failed, some successfully spawned processes didn't reach it
            // All processes that we attempted to spawn but didn't reach the barrier
            for (const auto& iid : successfully_spawned) {
                spawn_failures.emplace_back(
                    "", // We don't have binary name readily available here
                    iid,
                    init_error::cause::barrier_timeout,
                    0,
                    "Process spawned successfully but did not reach "
                    "initialization barrier (may have crashed during startup)"
                );
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
bool Managed_process::release_lifeline(instance_id_type process_instance_id)
{
    std::lock_guard<mutex> lock(m_lifeline_mutex);
    auto it = m_lifeline_writes.find(process_instance_id);
    if (it == m_lifeline_writes.end()) {
        return false;
    }
    close_lifeline_handle(it->second);
    m_lifeline_writes.erase(it);
    return true;
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
    try {
        assign_name(std::string("sintra_process_") + std::to_string(m_pid));
    }
    catch (const rpc_cancelled&) {
        // The coordinator has shut down (e.g., due to init_error from a spawn
        // failure).  There is nothing left for this worker to do -- return
        // gracefully and let the lifeline mechanism handle termination.
        return;
    }

    try {
        m_entry_function();
    }
    catch (const rpc_cancelled&) {
        // During shutdown cascades, a peer exiting cancels our RPCs and
        // barriers.  Swallow so the worker exits cleanly instead of
        // hitting std::terminate (which would cascade-kill remaining
        // processes and destroy diagnostics).
        // Outside of shutdown, rpc_cancelled indicates a real
        // infrastructure failure — let it propagate.
        if (detail::s_shutdown_state.load(std::memory_order_acquire) ==
            detail::shutdown_protocol_state::idle &&
            !m_must_stop.load(std::memory_order_acquire))
        {
            throw;
        }
        Log_stream(log_level::warning)
            << "Worker entry function exited with rpc_cancelled during shutdown.\n";
    }
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
    if (m_communication_state <= COMMUNICATION_PAUSED) {
        return;
    }

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
    if (m_communication_state == COMMUNICATION_STOPPED) {
        return;
    }

    m_must_stop.store(true, std::memory_order_release);

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

    Tn_type tn = { get_type_id<T>(), std::move(transceiver_name) };

    // insert an empty function, in order to be able to capture the iterator within it
    using Call_list_iterator = list<function<void()>>::iterator;
    Call_list_iterator f_it;
    {
        auto&              call_list = m_queued_availability_calls[tn];

        call_list.emplace_back();
        f_it = std::prev(call_list.end());
    }

    struct availability_call_state
    {
        bool               active = true;
        Call_list_iterator iterator;
    };

    auto state = std::make_shared<availability_call_state>();
    state->iterator = f_it;

    auto mark_completed = [this, tn, state](bool erase_empty_entry) -> bool {
        if (!state->active) {
            state->iterator = decltype(state->iterator){};
            return false;
        }

        auto queue_it = m_queued_availability_calls.find(tn);
        if (queue_it == m_queued_availability_calls.end()) {
            state->active = false;
            state->iterator = decltype(state->iterator){};
            return false;
        }

        auto call_it = state->iterator;
        queue_it->second.erase(call_it);
        if (erase_empty_entry && queue_it->second.empty()) {
            m_queued_availability_calls.erase(queue_it);
        }
        state->active = false;
        state->iterator = decltype(state->iterator){};
        return true;
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

inline bool Managed_process::prepare_process_reader(
    instance_id_type   process_instance_id,
    uint32_t           occurrence,
    bool               replace_existing)
{
    Dispatch_unique_lock readers_lock(m_readers_mutex);

    if (auto existing = m_readers.find(process_instance_id); existing != m_readers.end()) {
        if (!replace_existing) {
            return false;
        }
        if (existing->second) {
            if (!capture_replaced_child_communication_authority(
                    existing->second))
            {
                return false;
            }
            existing->second->stop_and_wait(1.0);
        }
        m_readers.erase(existing);
    }

    auto progress = std::make_shared<Process_message_reader::Delivery_progress>();
    uint64_t managed_child_custody_identity = 0;
    const auto custody_occurrence = child_custody_occurrence_token(process_instance_id);
    if (custody_occurrence.occurrence == occurrence) {
        if (auto custody = custody_occurrence.custody.lock()) {
            managed_child_custody_identity = custody->identity;
        }
    }
    auto reader = std::make_shared<Process_message_reader>(
        process_instance_id,
        progress,
        occurrence,
        managed_child_custody_identity);
    auto [reader_it, inserted] = m_readers.emplace(process_instance_id, reader);
    (void)reader_it;
    assert(inserted == true);
    detail::managed_child_reader_setup_for_test(process_instance_id, occurrence);
    reader->wait_until_ready();
    return true;
}

inline void Managed_process::remove_process_reader(
    instance_id_type   process_instance_id,
    double             waiting_period)
{
    std::shared_ptr<Process_message_reader> reader;
    {
        Dispatch_unique_lock readers_lock(m_readers_mutex);
        auto reader_it = m_readers.find(process_instance_id);
        if (reader_it == m_readers.end()) {
            return;
        }

        reader = std::move(reader_it->second);
        m_readers.erase(reader_it);
    }

    if (reader) {
        reader->stop_and_wait(waiting_period);
    }
}

inline bool Managed_process::has_process_reader(instance_id_type process_instance_id) const
{
    Dispatch_shared_lock readers_lock(m_readers_mutex);
    return m_readers.find(process_instance_id) != m_readers.end();
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
            auto  iid         = entry.first;
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

inline void Managed_process::flush(
    instance_id_type       process_id,
    sequence_counter_type  flush_sequence)
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

    while (reader->get_reply_reading_sequence() <  flush_sequence &&
        m_communication_state                == COMMUNICATION_RUNNING)
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
            if (!c->keep_waiting) {
                continue;
            }

            if (process_instance_id            == invalid_instance_id ||
                process_of(c->remote_instance) == process_instance_id)
            {
                c->keep_waiting = false;
                c->cancelled = true;
                c->keep_waiting_condition.notify_all();
                ret++;
            }
        }
    }
    return ret;
}

} // sintra
