// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "../../shared_mutex.h"
#include "../config.h"
#include "../globals.h"
#include "../ipc/rings.h"
#include "../messaging/message.h"
#include "../process/process_id.h"
#include "../messaging/process_message_reader.h"
#include "lifecycle_types.h"
#include "../resolve_type.h"
#include "../ipc/spinlocked_containers.h"
#include "../transceiver.h"
#include "../messaging/call_function_with_message_args.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include <deque>
#include <float.h>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>



namespace sintra {

using std::atomic;
using std::condition_variable;
using std::deque;
using std::function;
using std::list;
using std::map;
using std::mutex;
using std::recursive_mutex;
using std::string;
using std::vector;


inline
string uibs_impl(const char *file, int line)
{
    return std::string(file) + ":" + std::to_string(line);
};

#define UIBS uibs_impl(__FILE__, __LINE__)



struct Entry_descriptor
{
    Entry_descriptor(const string& binary_name)
    {
        m_binary_name = binary_name;
        assert(m_entry_function == nullptr);
    }
    Entry_descriptor(int(*entry_function)())
    {
        m_entry_function = entry_function;
        assert(m_binary_name.empty());
    }
private:
    int (*m_entry_function)() = nullptr;
    string             m_binary_name;

    friend struct Managed_process;
};


struct Process_descriptor
{
    Entry_descriptor   entry;
    vector<string>     sintra_options;
    vector<string>     user_options;
    instance_id_type   assigned_instance_id = invalid_instance_id;

    Process_descriptor(
        const Entry_descriptor&    aentry,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(aentry),
        user_options(auser_options)
    {
    }
    Process_descriptor(
        const string&              binary_path,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(binary_path),
        user_options(auser_options)
    {
    }
    Process_descriptor(
        const char*                binary_path,
        const vector<string>&      auser_options = vector<string>())
    :
        entry(binary_path),
        user_options(auser_options)
    {
    }

    Process_descriptor(int(*entry_function)())
    :
        entry(entry_function)
    {
    }

    friend struct Managed_process;
};

struct Lifetime_policy
{
    bool               enable_lifeline      = true;
    int                hard_exit_code       = 99;
    int                hard_exit_timeout_ms = 100;
};

enum class Managed_child_failure_kind
{
    none,
    custody_not_accepted,
    custody_closed,
    occurrence_admission,
    reader_setup,
    lifeline_setup,
    native_spawn,
    native_identity,
    setup_exception,
    setup_worker_start,
    readiness_observation,
    release_worker_start,
    release_worker_execution,
    native_observer
};

struct Managed_child_failure
{
    Managed_child_failure_kind kind = Managed_child_failure_kind::none;
    uint32_t                   occurrence = 0;
    int                        native_error = 0;
    std::string                message;
};

namespace detail {
class Managed_child_exit_subscription_state;
}

struct Managed_child_occurrence_identity
{
    instance_id_type process_instance_id = invalid_instance_id;
    // Custody-relative: 0 is the original launch and 1 is its first recovery.
    uint32_t         occurrence = 0;
    // Opaque and unique only among custodies owned by one active runtime.
    uint64_t         custody_identity = 0;

    bool operator==(const Managed_child_occurrence_identity&) const = default;
};

enum class Managed_child_exit_status_kind
{
    unavailable,
    // On Windows this is the native process exit code; the OS does not
    // distinguish an ordinary return from TerminateProcess.
    exited,
    // Reported only when the platform supplies signal termination status.
    signaled
};

struct Managed_child_exit
{
    Managed_child_occurrence_identity occurrence;
    Managed_child_exit_status_kind    status_kind =
        Managed_child_exit_status_kind::unavailable;
    // Portable exit code or signal number interpreted by status_kind.
    std::uint32_t                     status = 0;
    // Exact Windows DWORD or the POSIX wait-status bit pattern.
    std::uint32_t                     native_status = 0;
    bool                              native_status_available = false;
};

using Managed_child_exit_callback =
    std::function<void(const Managed_child_exit&)>;

class Managed_child_exit_subscription
{
public:
    Managed_child_exit_subscription() = default;
    ~Managed_child_exit_subscription();

    Managed_child_exit_subscription(
        Managed_child_exit_subscription&& other) noexcept = default;
    Managed_child_exit_subscription& operator=(
        Managed_child_exit_subscription&& other) noexcept;

    Managed_child_exit_subscription(
        const Managed_child_exit_subscription&) = delete;
    Managed_child_exit_subscription& operator=(
        const Managed_child_exit_subscription&) = delete;

    explicit operator bool() const noexcept;
    void unsubscribe() noexcept;

private:
    explicit Managed_child_exit_subscription(
        std::shared_ptr<detail::Managed_child_exit_subscription_state> state);

    std::shared_ptr<detail::Managed_child_exit_subscription_state> m_state;

    friend class Managed_child_custody;
};

struct Managed_child_exit_observation
{
    Managed_child_occurrence_identity occurrence;
    Managed_child_exit_subscription   subscription;

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(subscription);
    }
};


// Branch indices have the following meaning:
// -1: No branching has taken place - this variable is not relevant
//  0: The starter process, with the coordinator.
// >0: A spawned process.
inline int32_t s_branch_index = -1;


inline uint32_t s_recovery_occurrence = 0;


namespace detail {

inline constexpr const char* k_external_attach_token_arg      = "--external_attach_token";
inline constexpr const char* k_external_attach_occurrence_arg = "--external_attach_occurrence";
inline constexpr const char* k_skip_startup_barrier_arg       = "--sintra_skip_startup_barrier";
inline constexpr auto        k_external_attach_claim_timeout  = std::chrono::seconds(5);

enum class Managed_child_post_spawn_stage
{
    public_sync_no_child,
    join_no_child
};

namespace test_hooks {
#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Managed_child_post_spawn_callback = void (*)(
    Managed_child_post_spawn_stage,
    instance_id_type,
    uint32_t);
inline std::atomic<Managed_child_post_spawn_callback>
    s_managed_child_post_spawn{nullptr};

using Coordinator_departure_pre_rpc_unblock_callback = void (*)();
inline std::atomic<Coordinator_departure_pre_rpc_unblock_callback>
    s_coordinator_departure_pre_rpc_unblock{nullptr};
#endif
} // namespace test_hooks

inline void coordinator_departure_pre_rpc_unblock_for_test()
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_coordinator_departure_pre_rpc_unblock.load(
            std::memory_order_acquire))
    {
        callback();
    }
#endif
}

inline void managed_child_post_spawn_for_test(
    Managed_child_post_spawn_stage stage,
    instance_id_type process_instance_id,
    uint32_t occurrence)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback =
            test_hooks::s_managed_child_post_spawn.load(std::memory_order_acquire))
    {
        callback(stage, process_instance_id, occurrence);
    }
#else
    (void)stage;
    (void)process_instance_id;
    (void)occurrence;
#endif
}

enum class Managed_child_communication_authority_capture
{
    captured,
    already_terminal,
    conflict
};

class Managed_child_transport_retirement
{
public:
    bool publication_retired() const noexcept
    {
        return m_publication_retired;
    }

    bool retirement_started() const noexcept
    {
        return m_state == State::RETIRING;
    }

    bool retirement_terminal() const noexcept
    {
        return m_state == State::RETIRED;
    }

    bool ready_to_retire() const noexcept
    {
        return m_publication_retired && m_state == State::CAPTURED;
    }

    bool fully_retired() const noexcept
    {
        return m_publication_retired && m_state == State::RETIRED;
    }

    void note_publication_retired() noexcept
    {
        m_publication_retired = true;
    }

    bool capture_authority(
        const std::shared_ptr<Process_message_reader>& reader)
    {
        if (m_state == State::RETIRED) {
            return !reader && !m_reader;
        }
        if (m_state != State::UNCAPTURED) {
            return m_reader == reader;
        }
        m_reader = reader;
        m_state = State::CAPTURED;
        return true;
    }

    bool begin_retirement(std::shared_ptr<Process_message_reader>& reader)
    {
        if (m_state != State::CAPTURED) {
            return false;
        }
        m_state = State::RETIRING;
        reader = m_reader;
        return true;
    }

    bool reset_retirement() noexcept
    {
        if (m_state != State::RETIRING && m_state != State::CAPTURED) {
            return false;
        }
        m_state = State::CAPTURED;
        return true;
    }

    bool complete_retirement(
        const std::shared_ptr<Process_message_reader>& reader,
        std::shared_ptr<Process_message_reader>& retired_authority)
    {
        if (m_state == State::RETIRED) {
            return !reader && !m_reader;
        }
        if (m_state != State::RETIRING || m_reader != reader) {
            return false;
        }
        m_state = State::RETIRED;
        retired_authority = std::move(m_reader);
        return true;
    }

private:
    enum class State
    {
        UNCAPTURED,
        CAPTURED,
        RETIRING,
        RETIRED
    };

    bool                                    m_publication_retired = false;
    State                                   m_state = State::UNCAPTURED;
    std::shared_ptr<Process_message_reader> m_reader;
};

class Managed_child_native_authority
{
public:
    bool confirm_absent() const noexcept
    {
        return m_phase == Phase::ABSENT && m_pid < 0 &&
            !m_start_stamp_available && m_start_stamp == 0 &&
            !m_wait_status_available && m_wait_status == 0
#ifdef _WIN32
            && !m_process_handle_owned && m_process_handle == 0 &&
            !m_exit_observer_registered
#endif
            ;
    }

    bool commit_created(
        int pid,
        bool start_stamp_available,
        uint64_t start_stamp,
        bool already_exited,
        bool wait_status_available,
        std::uint32_t wait_status
#ifdef _WIN32
        , uintptr_t process_handle,
        bool process_handle_owned
#endif
        ) noexcept
    {
        if (!confirm_absent() ||
            (pid <= 0
#ifdef _WIN32
             && process_handle == 0
#endif
            ) ||
            (!already_exited && wait_status_available)
#ifdef _WIN32
            || process_handle == 0 || !process_handle_owned
#endif
            )
        {
            return false;
        }
        m_phase = already_exited ? Phase::EXITED : Phase::RUNNING;
        m_pid = pid;
        m_start_stamp_available = start_stamp_available;
        m_start_stamp = start_stamp;
        m_wait_status_available = wait_status_available;
        m_wait_status = wait_status;
#ifdef _WIN32
        m_process_handle = process_handle;
        m_process_handle_owned = process_handle_owned;
#endif
        return true;
    }

    bool note_exit(
        std::uint32_t wait_status,
        bool          wait_status_available) noexcept
    {
        if (m_phase == Phase::ABSENT) {
            return false;
        }
        if (m_phase == Phase::EXITED) {
            return m_wait_status_available == wait_status_available &&
                (!wait_status_available || m_wait_status == wait_status);
        }
        m_phase = Phase::EXITED;
        m_wait_status_available = wait_status_available;
        m_wait_status = wait_status;
        return true;
    }

    bool created() const noexcept { return m_phase != Phase::ABSENT; }
    bool running() const noexcept { return m_phase == Phase::RUNNING; }
    bool exited() const noexcept { return m_phase == Phase::EXITED; }
    int pid() const noexcept { return m_pid; }
    bool start_stamp_available() const noexcept
    {
        return m_start_stamp_available;
    }
    uint64_t start_stamp() const noexcept { return m_start_stamp; }
    bool wait_status_available() const noexcept
    {
        return m_wait_status_available;
    }
    std::uint32_t wait_status() const noexcept { return m_wait_status; }

#ifdef _WIN32
    bool register_exit_observer(uintptr_t expected_handle) noexcept
    {
        if (!running() || !m_process_handle_owned ||
            m_process_handle == 0 || m_process_handle != expected_handle ||
            m_exit_observer_registered)
        {
            return false;
        }
        m_exit_observer_registered = true;
        return true;
    }

    bool cancel_exit_observer(uintptr_t expected_handle) noexcept
    {
        if (!m_process_handle_owned || m_process_handle == 0 ||
            m_process_handle != expected_handle)
        {
            return !m_exit_observer_registered;
        }
        m_exit_observer_registered = false;
        return true;
    }

    bool take_owned_process_handle(
        uintptr_t expected_handle,
        uintptr_t& released_handle) noexcept
    {
        if (!m_process_handle_owned || m_process_handle == 0 ||
            m_process_handle != expected_handle)
        {
            return false;
        }
        released_handle = m_process_handle;
        m_process_handle = 0;
        m_process_handle_owned = false;
        return true;
    }

    bool process_handle_owned() const noexcept
    {
        return m_process_handle_owned;
    }
    uintptr_t process_handle() const noexcept { return m_process_handle; }
    bool exit_observer_registered() const noexcept
    {
        return m_exit_observer_registered;
    }
    bool fallback_wait_available() const noexcept
    {
        return running() && !m_exit_observer_registered &&
            m_process_handle_owned && m_process_handle != 0;
    }
#endif

private:
    enum class Phase
    {
        ABSENT,
        RUNNING,
        EXITED
    };

    Phase    m_phase = Phase::ABSENT;
    int      m_pid = -1;
    uint64_t m_start_stamp = 0;
    bool     m_start_stamp_available = false;
    std::uint32_t m_wait_status = 0;
    bool     m_wait_status_available = false;
#ifdef _WIN32
    uintptr_t m_process_handle = 0;
    bool      m_process_handle_owned = false;
    bool      m_exit_observer_registered = false;
#endif
};

struct Managed_child_occurrence_record
{
    enum class setup_state {
        pending,
        no_child,
        ownership_ready
    };

    uint32_t                   occurrence = 0;
    instance_id_type           process_instance_id = invalid_instance_id;
    setup_state                setup = setup_state::pending;
    bool                       initialization_reservation_active = false;
    Managed_child_transport_retirement
                               transport;
    Managed_child_native_authority
                               native;
    std::vector<std::shared_ptr<Managed_child_exit_subscription_state>>
                               exit_subscriptions;
};

// Immutable exact identity captured by a release attempt after setup settles.
// slot_current is only process-slot authority; exact historical convergence
// and terminal accounting retain every roster entry regardless of its value.
struct Managed_child_release_occurrence
{
    instance_id_type process_instance_id = invalid_instance_id;
    uint32_t         occurrence = 0;
    bool             slot_current = false;
};

using Managed_child_release_roster =
    std::vector<Managed_child_release_occurrence>;

struct Managed_child_custody_record;

struct Managed_child_active_occurrence
{
    std::weak_ptr<Managed_child_custody_record> custody;
    uint32_t                                    occurrence = 0;
};

struct Managed_child_occurrence_token
{
    std::weak_ptr<Managed_child_custody_record> custody;
    instance_id_type                            process_instance_id = invalid_instance_id;
    uint32_t                                    occurrence = 0;

    explicit operator bool() const noexcept { return !custody.expired(); }
};

inline thread_local bool tl_in_managed_child_exit_callback = false;

class Managed_child_exit_subscription_state final
{
public:
    Managed_child_exit_subscription_state(
        std::weak_ptr<Managed_child_custody_record> custody,
        Managed_child_occurrence_identity          occurrence,
        Managed_child_exit_callback                callback);

    void deliver(const Managed_child_exit& event) noexcept;
    void unsubscribe() noexcept;

private:
    void remove_from_occurrence() noexcept;

    std::weak_ptr<Managed_child_custody_record> m_custody;
    Managed_child_occurrence_identity           m_occurrence;
    mutable std::mutex                          m_mutex;
    std::condition_variable                     m_quiesced;
    Managed_child_exit_callback                 m_callback;
    std::thread::id                             m_callback_thread;
    bool                                        m_callback_running = false;

    Managed_child_exit                         m_dispatch_event;
    std::shared_ptr<Managed_child_exit_subscription_state>
                                                m_dispatch_owner;
    Managed_child_exit_subscription_state*     m_dispatch_next = nullptr;

    friend struct ::sintra::Managed_process;
};

struct Managed_child_exit_publication
{
    Managed_child_exit event;
    std::vector<std::shared_ptr<Managed_child_exit_subscription_state>>
                       subscriptions;
    bool               transition_valid = false;
};

Managed_child_exit make_managed_child_exit(
    Managed_child_occurrence_identity occurrence,
    std::uint32_t                     wait_status,
    bool                              wait_status_available) noexcept;

Managed_child_occurrence_identity make_managed_child_occurrence_identity(
    uint64_t                               custody_identity,
    const Managed_child_occurrence_record& occurrence) noexcept;

Managed_child_exit_publication record_managed_child_exit_locked(
    uint64_t                         custody_identity,
    Managed_child_occurrence_record& occurrence,
    std::uint32_t                    wait_status,
    bool                             wait_status_available);

enum class Readiness_phase
{
    not_requested,
    pending,
    reached,
    stopped
};

enum class Release_mode
{
    passive,
    cleanup
};

// The sole authority for custody release lifecycle state.  Its private tags
// make invalid cross-products unrepresentable: open and released carry no
// attempt, while releasing always carries a monotone mode, a nonzero
// generation, and exactly one attempt phase.
class Managed_child_release_state
{
public:
    uint64_t request(Release_mode command) noexcept
    {
        if (auto releasing = std::get_if<Releasing>(&m_state)) {
            if (command == Release_mode::cleanup) {
                releasing->cleanup_requested = true;
            }
            if (releasing->attempt != Attempt::retryable) {
                releasing->rerun_requested = true;
                return 0;
            }
            releasing->generation = next_generation(releasing->generation);
            releasing->attempt = Attempt::running;
            return releasing->generation;
        }
        if (!std::holds_alternative<Open>(m_state)) {
            return 0;
        }
        const auto generation = next_generation(0);
        m_state = Releasing{
            command == Release_mode::cleanup,
            false,
            generation,
            Attempt::running};
        return generation;
    }

    bool open() const noexcept
    {
        return std::holds_alternative<Open>(m_state);
    }

    bool releasing() const noexcept
    {
        return std::holds_alternative<Releasing>(m_state);
    }

    bool released() const noexcept
    {
        return std::holds_alternative<Released>(m_state);
    }

    bool cleanup_requested() const noexcept
    {
        const auto releasing = std::get_if<Releasing>(&m_state);
        return releasing && releasing->cleanup_requested;
    }

    bool running() const noexcept
    {
        return has_attempt(Attempt::running);
    }

    bool failing() const noexcept
    {
        return has_attempt(Attempt::failing);
    }

    bool retryable() const noexcept
    {
        return has_attempt(Attempt::retryable);
    }

    bool active() const noexcept
    {
        return running() || failing();
    }

    uint64_t generation() const noexcept
    {
        const auto releasing = std::get_if<Releasing>(&m_state);
        return releasing ? releasing->generation : 0;
    }

    bool running(uint64_t expected_generation) const noexcept
    {
        return expected_generation != 0 && running() &&
            generation() == expected_generation;
    }

    bool mark_failing(uint64_t expected_generation) noexcept
    {
        auto releasing = exact_releasing(expected_generation);
        if (!releasing || releasing->attempt != Attempt::running) {
            return false;
        }
        releasing->attempt = Attempt::failing;
        return true;
    }

    bool mark_retryable(uint64_t expected_generation) noexcept
    {
        auto releasing = exact_releasing(expected_generation);
        if (!releasing ||
            (releasing->attempt != Attempt::running &&
             releasing->attempt != Attempt::failing))
        {
            return false;
        }
        if (releasing->rerun_requested) {
            releasing->rerun_requested = false;
            releasing->generation = next_generation(releasing->generation);
            releasing->attempt = Attempt::running;
            return true;
        }
        releasing->attempt = Attempt::retryable;
        return true;
    }

    bool mark_released(uint64_t expected_generation) noexcept
    {
        if (!running(expected_generation)) {
            return false;
        }
        m_state = Released{};
        return true;
    }

private:
    struct Open {};
    struct Released {};

    enum class Attempt
    {
        running,
        failing,
        retryable
    };

    struct Releasing
    {
        bool        cleanup_requested = false;
        bool        rerun_requested = false;
        uint64_t    generation = 1;
        Attempt     attempt = Attempt::running;
    };

    static uint64_t next_generation(uint64_t generation) noexcept
    {
        ++generation;
        if (generation == 0) {
            ++generation;
        }
        return generation;
    }

    bool has_attempt(Attempt expected) const noexcept
    {
        const auto releasing = std::get_if<Releasing>(&m_state);
        return releasing && releasing->attempt == expected;
    }

    Releasing* exact_releasing(uint64_t expected_generation) noexcept
    {
        auto releasing = std::get_if<Releasing>(&m_state);
        return expected_generation != 0 && releasing &&
                releasing->generation == expected_generation
            ? releasing
            : nullptr;
    }

    std::variant<Open, Releasing, Released> m_state{Open{}};
};

// Fences asynchronous work to one runtime epoch. Closing admission prevents a
// queued callback from becoming active while destruction waits out callbacks
// that already own the epoch.
struct Managed_process_lifetime
{
    bool enter_owner_callback() const
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (!m_callback_admission_open) {
            return false;
        }
        ++m_active_callbacks;
        return true;
    }

    void leave_owner_callback() const
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        assert(m_active_callbacks != 0);
        --m_active_callbacks;
        if (m_active_callbacks == 0) {
            m_callbacks_quiesced.notify_all();
        }
    }

    void close_owner_callback_admission_and_wait() const
    {
        std::unique_lock<std::mutex> lock(m_callback_mutex);
        m_callback_admission_open = false;
        m_callbacks_quiesced.wait(lock, [this]() { return m_active_callbacks == 0; });
    }

    mutable std::atomic<Lifeline_observation>
                                                m_lifeline_observation{
                                                    Lifeline_observation::INACTIVE};

private:
    mutable std::mutex                          m_callback_mutex;
    mutable std::condition_variable             m_callbacks_quiesced;
    mutable bool                                m_callback_admission_open = true;
    mutable std::size_t                         m_active_callbacks = 0;
};

struct Managed_child_custody_record
{
    mutable std::mutex                         mutex;
    std::condition_variable                    changed;
    std::weak_ptr<const Managed_process_lifetime>
                                                runtime_lifetime;
    uint64_t                                   identity = 0;
    bool                                       recovery_requested = false;
    Readiness_phase                            readiness = Readiness_phase::not_requested;
    std::atomic<bool>                          readiness_cancelled{false};
    Managed_child_release_state                release_state;
    Managed_child_failure                      last_failure;
    std::vector<Managed_child_occurrence_record> occurrences;

    Managed_child_occurrence_record* find_occurrence_locked(
        instance_id_type process_instance_id,
        uint32_t occurrence) noexcept
    {
        const auto exact = std::find_if(
            occurrences.begin(), occurrences.end(),
            [&](const Managed_child_occurrence_record& candidate) {
                return candidate.process_instance_id == process_instance_id &&
                    candidate.occurrence == occurrence;
            });
        return exact == occurrences.end() ? nullptr : &*exact;
    }

    const Managed_child_occurrence_record* find_occurrence_locked(
        instance_id_type process_instance_id,
        uint32_t occurrence) const noexcept
    {
        const auto exact = std::find_if(
            occurrences.begin(), occurrences.end(),
            [&](const Managed_child_occurrence_record& candidate) {
                return candidate.process_instance_id == process_instance_id &&
                    candidate.occurrence == occurrence;
            });
        return exact == occurrences.end() ? nullptr : &*exact;
    }
};

// Pins one admitted occurrence until setup becomes terminal.  The launch
// attempt below retains this settlement until its locally owned setup
// resources have either transferred or rolled back.
class Managed_child_setup_settlement
{
public:
    Managed_child_setup_settlement() noexcept = default;

    Managed_child_setup_settlement(
        std::shared_ptr<Managed_child_custody_record> custody,
        instance_id_type process_instance_id,
        uint32_t occurrence) noexcept
        : m_custody(std::move(custody))
        , m_process_instance_id(process_instance_id)
        , m_occurrence(occurrence)
    {}

    Managed_child_setup_settlement(
        const Managed_child_setup_settlement&) = delete;
    Managed_child_setup_settlement& operator=(
        const Managed_child_setup_settlement&) = delete;

    Managed_child_setup_settlement(
        Managed_child_setup_settlement&& other) noexcept
        : m_custody(std::move(other.m_custody))
        , m_process_instance_id(other.m_process_instance_id)
        , m_occurrence(other.m_occurrence)
    {
        other.m_process_instance_id = invalid_instance_id;
        other.m_occurrence = 0;
    }

    Managed_child_setup_settlement& operator=(
        Managed_child_setup_settlement&& other) noexcept
    {
        if (this != &other) {
            settle();
            m_custody = std::move(other.m_custody);
            m_process_instance_id = other.m_process_instance_id;
            m_occurrence = other.m_occurrence;
            other.m_process_instance_id = invalid_instance_id;
            other.m_occurrence = 0;
        }
        return *this;
    }

    ~Managed_child_setup_settlement() noexcept { settle(); }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(m_custody);
    }

    bool matches(
        const std::shared_ptr<Managed_child_custody_record>& custody,
        instance_id_type process_instance_id,
        uint32_t occurrence) const noexcept
    {
        return m_custody == custody &&
            m_process_instance_id == process_instance_id &&
            m_occurrence == occurrence;
    }

    bool settle() noexcept
    {
        if (!m_custody) {
            return false;
        }

        bool child_created = false;
        bool terminal = false;
        bool changed = false;
        try {
            std::lock_guard<std::mutex> lock(m_custody->mutex);
            auto* exact = m_custody->find_occurrence_locked(
                m_process_instance_id, m_occurrence);
            if (exact) {
                child_created = exact->native.created();
                if (exact->setup ==
                    Managed_child_occurrence_record::setup_state::pending)
                {
                    exact->setup = child_created
                        ? Managed_child_occurrence_record::setup_state::
                            ownership_ready
                        : Managed_child_occurrence_record::setup_state::no_child;
                    changed = true;
                }
                terminal = exact->setup !=
                    Managed_child_occurrence_record::setup_state::pending;
            }
        }
        catch (...) {
            return false;
        }

        if (changed) {
            m_custody->changed.notify_all();
        }
        if (terminal) {
            m_custody.reset();
        }
        return child_created;
    }

private:
    std::shared_ptr<Managed_child_custody_record> m_custody;
    instance_id_type m_process_instance_id = invalid_instance_id;
    uint32_t m_occurrence = 0;
};

class Managed_child_launch_attempt
{
public:
    Managed_child_launch_attempt() noexcept = default;
    Managed_child_launch_attempt(
        Managed_process* owner,
        std::shared_ptr<Managed_child_custody_record> custody,
        instance_id_type process_instance_id,
        uint32_t occurrence) noexcept;

    Managed_child_launch_attempt(const Managed_child_launch_attempt&) = delete;
    Managed_child_launch_attempt& operator=(
        const Managed_child_launch_attempt&) = delete;
    Managed_child_launch_attempt(Managed_child_launch_attempt&& other) noexcept;
    Managed_child_launch_attempt& operator=(
        Managed_child_launch_attempt&& other) noexcept;
    ~Managed_child_launch_attempt() noexcept;

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(m_setup_settlement);
    }

    bool matches(
        const std::shared_ptr<Managed_child_custody_record>& custody,
        instance_id_type process_instance_id,
        uint32_t occurrence) const noexcept
    {
        return m_setup_settlement.matches(
            custody, process_instance_id, occurrence);
    }

    void mark_reader_prepared() noexcept;
    void transfer_reader() noexcept;

    bool create_lifeline(int* error_out);
    uintptr_t lifeline_read_endpoint() const noexcept
    {
        return m_lifeline_read_endpoint;
    }
    void close_lifeline_read_endpoint() noexcept;
    bool transfer_lifeline_write();

    bool acquire_initialization_reservation(Coordinator* coordinator);
    void transfer_initialization_reservation() noexcept;

#ifndef _WIN32
    struct Posix_native_handoff_result
    {
        bool reservation_lookup_failed = false;
        bool child_reaped = false;
    };

    void reserve_posix_reap_slot();
    void cancel_posix_native_handoff();
    Posix_native_handoff_result commit_posix_native_handoff(
        int pid,
        bool already_reaped,
        bool wait_status_available,
        int wait_status);
#else
    void adopt_windows_process_handle(uintptr_t process_handle) noexcept;
    uintptr_t windows_process_handle() const noexcept
    {
        return m_windows_process_handle;
    }
    void commit_windows_native_authority(int pid);
    void confirm_windows_native_absent();
    void start_windows_native_observer();
#endif

private:
    enum class Reader_ownership
    {
        absent,
        rollback_required,
        transferred
    };

    enum class Initialization_ownership
    {
        absent,
        rollback_required,
        transferred
    };

#ifndef _WIN32
    enum class Posix_reap_ownership
    {
        absent,
        reserved,
        transferred
    };
#else
    enum class Windows_native_ownership
    {
        absent,
        raw_owned,
        native_observer_pending,
        transferred
    };
#endif

    static constexpr uintptr_t invalid_lifeline_endpoint =
        static_cast<uintptr_t>(-1);

    void rollback() noexcept;
    void rollback_initialization() noexcept;
    void rollback_reader() noexcept;
#ifndef _WIN32
    void rollback_posix_reap_reservation() noexcept;
#else
    void rollback_windows_native_authority() noexcept;
#endif
    void close_lifeline_write_endpoint() noexcept;
    bool lifeline_endpoint_valid(uintptr_t endpoint) const noexcept;

    // Declared first so exact setup settlement is destroyed last.
    Managed_child_setup_settlement m_setup_settlement;
    Managed_process* m_owner = nullptr;
    std::shared_ptr<Managed_child_custody_record> m_custody;
    instance_id_type m_process_instance_id = invalid_instance_id;
    uint32_t m_occurrence = 0;
    Reader_ownership m_reader = Reader_ownership::absent;
    Initialization_ownership m_initialization =
        Initialization_ownership::absent;
    Coordinator* m_initialization_coordinator = nullptr;
#ifndef _WIN32
    Posix_reap_ownership m_posix_reap = Posix_reap_ownership::absent;
    uint64_t m_posix_reap_reservation_id = 0;
#else
    Windows_native_ownership m_windows_native =
        Windows_native_ownership::absent;
    uintptr_t m_windows_process_handle = 0;
#endif
    uintptr_t m_lifeline_read_endpoint = invalid_lifeline_endpoint;
    uintptr_t m_lifeline_write_endpoint = invalid_lifeline_endpoint;
};

} // namespace detail



template <typename T>
sintra::type_id_type get_type_id();


template <typename = void>
sintra::instance_id_type get_instance_id(string&& assigned_name);


struct Managed_process: Derived_transceiver<Managed_process>
{
    Managed_process();
    ~Managed_process();


    void init(int argc, const char* const* argv);
    bool branch(vector<Process_descriptor>& branch_vector);
    void go();

    // Pauses the process. Once called, reader threads will continue running,
    // but in a mode where only messages originating from the coordinator are
    // processed.
    void pause();

    // Stops the readers and causes their threads to exit.
    void stop();


    void wait_for_stop();

    void enable_recovery();


    Message_ring_W*                     m_out_req_c = nullptr;
    Message_ring_W*                     m_out_rep_c = nullptr;

    spinlocked_umap<
        string,
        type_id_type
    >                                   m_type_id_of_type_name;

    spinlocked_umap<
        type_id_type,
        string
    >                                   m_type_name_of_explicit_id;

    spinlocked_umap<
        string,
        instance_id_type
    >                                   m_instance_id_of_assigned_name;

    spinlocked_umap<
        instance_id_type,
        Transceiver*
    >                                   m_local_pointer_of_instance_id;

    // START/STOP

    enum Communication_state
    {
        COMMUNICATION_STOPPED,  // ring threads are stopped
        COMMUNICATION_PAUSED,   // rings answer to SERVICE_MODE
        COMMUNICATION_RUNNING   // rings in NORMAL_MODE
    };

    Communication_state                 m_communication_state = COMMUNICATION_STOPPED;
    mutex                               m_start_stop_mutex;
    condition_variable                  m_start_stop_condition;


    Managed_process(Managed_process const&) = delete;
    Managed_process& operator=(Managed_process const&) = delete;


    string                              m_binary_name;

    detail::process_id_type             m_pid;
    uint64_t                            m_process_start_stamp = 0;

    atomic<bool>                        m_must_stop;
    std::atomic<detail::Member_lifetime_role>
                                        m_member_lifetime_role{
                                            detail::Member_lifetime_role::
                                                COORDINATOR_BOUND};
    std::atomic<detail::Coordinator_departure_cause>
                                        m_coordinator_departure_cause{
                                            detail::Coordinator_departure_cause::NONE};
    condition_variable                  m_termination_condition;

    uint64_t                            m_swarm_id;
    string                              m_directory;

    inline
    string obtain_swarm_directory();

    function<int()>                     m_entry_function = [] { return 0; };

    sequence_counter_type               m_last_message_sequence;

    // don't be tempted to make this atomic and shared across processes, it's not faster.
    sequence_counter_type               m_check_sequence;

    double                              m_message_stats_reference_time;
    uint64_t                            m_messages_accepted_since_reference_time;
    uint64_t                            m_messages_rejected_since_reference_time;
    sequence_counter_type               m_total_sequences_missed;

    std::chrono::time_point<std::chrono::steady_clock>
                                        m_time_instantiated;

    map<
        instance_id_type,
        std::shared_ptr<Process_message_reader>
    >                                   m_readers;
    mutable shared_mutex                m_readers_mutex;
    mutable mutex                       m_delivery_mutex;
    condition_variable                  m_delivery_condition;

    int                                 m_num_active_readers = 0;
    mutex                               m_num_active_readers_mutex;
    condition_variable                  m_num_active_readers_condition;

    void wait_until_all_external_readers_are_done(
        int                extra_allowed_readers = 0);

    bool prepare_process_reader(
        instance_id_type   process_instance_id,
        uint32_t           occurrence,
        bool               replace_existing);

    void remove_process_reader(
        instance_id_type   process_instance_id,
        double             waiting_period = 1.0);
    void retire_external_process_reader(
        const std::shared_ptr<Process_message_reader>& reader);

    bool has_process_reader(
        instance_id_type   process_instance_id) const;

    void flush(
        instance_id_type       process_id,
        sequence_counter_type  flush_sequence);

    void run_after_current_handler(
        function<void()>   task);

    void coordinator_departed(
        detail::Coordinator_departure_cause cause,
        const std::shared_ptr<const detail::Managed_process_lifetime>&
            runtime_lifetime);

    void start_lifeline_watcher(
        const Lifetime_policy& policy,
        bool                   lifeline_required);
    void stop_lifeline_watcher() noexcept;

    void wait_for_delivery_fence();
    void notify_delivery_progress();


    size_t unblock_rpc(instance_id_type process_instance_id = invalid_instance_id);


    // This signal will be sent BEFORE the coordinator sends instance_unpublished
    // for this process. It is meant to notify crash guards about the reason of the
    // instance_unpublished event, which will follow shortly after.
    SINTRA_MESSAGE_RESERVED(terminated_abnormally, int status);
    SINTRA_MESSAGE_RESERVED(unpublish_transceiver_notify, instance_id_type transceiver_instance_id);

    unordered_map<Tn_type, list<function<void()>>>
                                        m_queued_availability_calls;

    // Guards access to queued availability callbacks.  The corresponding
    // implementation releases this lock before invoking user callbacks to
    // avoid reentrancy issues.
    mutex                               m_availability_mutex;

    // Named-instance activation can re-enter the activation path when
    // call_on_availability() invokes the callback inline for an already
    // published transceiver, so this registry lock must currently be recursive.
    recursive_mutex                     m_handlers_mutex;

    // Incremented before invoking copied local-event handlers outside
    // m_handlers_mutex, decremented after the dispatch window closes.
    // Individual slot state governs deactivation safety.
    std::atomic<uint32_t>               m_handlers_dispatch_depth{0};

    // Calls f when the specified transceiver becomes available.
    // if the transceiver is available, f is invoked immediately.
    template <typename T>
    function<void()> call_on_availability(Named_instance<T> transceiver, function<void()> f);

    void unpublish_all_transceivers();


    deque<sequence_counter_type>        m_flush_sequence;
    mutex                               m_flush_sequence_mutex;
    condition_variable                  m_flush_sequence_condition;

    // Guarded by m_handlers_mutex.
    handler_registry_type               m_active_handlers;

    // standard process groups
    instance_id_type                    m_group_all             = invalid_instance_id;
    instance_id_type                    m_group_external        = invalid_instance_id;

    // recovery
    bool                                m_skip_startup_barrier  = false;


    struct Spawn_swarm_process_args
    {
        std::string                binary_name;
        std::vector<std::string>   args;
        std::vector<std::string>   env_overrides;
        instance_id_type           piid;
        uint32_t                   occurrence = 0;
        Lifetime_policy            lifetime;
        std::shared_ptr<detail::Managed_child_custody_record> custody;
    };

    struct Spawn_result
    {
        bool                       success = false;
        bool                       os_process_created = false;
        bool                       lifeline_enabled = false;
        bool                       lifeline_write_retained = false;
        std::string                binary_name;
        instance_id_type           instance_id;
        int                        os_pid = -1;
        Managed_child_failure      failure;
    };

#ifndef _WIN32
    struct Spawned_child_reap_slot
    {
        uint64_t reservation_id = 0;
        pid_t    pid = 0;
        uint64_t start_stamp = 0;
        bool     start_stamp_available = false;
        detail::Managed_child_occurrence_token occurrence;
    };
    std::vector<Spawned_child_reap_slot> m_spawned_child_pids;
    uint64_t                            m_next_spawned_child_reservation = 1;
    mutable std::mutex                  m_spawned_child_pids_mutex;

    void reap_finished_children();
#endif

    using lifeline_handle_type = uintptr_t;
    map<instance_id_type, lifeline_handle_type>
                                        m_lifeline_writes;
    mutable std::mutex                  m_lifeline_mutex;

    bool breach_lifeline(instance_id_type process_instance_id);
    void breach_all_lifelines();

    std::thread                         m_lifeline_watcher;
    std::atomic<bool>                   m_lifeline_watcher_cancelled{false};
    std::atomic<lifeline_handle_type>   m_lifeline_watch_endpoint{
                                            static_cast<lifeline_handle_type>(-1)};
#ifdef _WIN32
    std::mutex                          m_lifeline_watcher_start_mutex;
    std::condition_variable             m_lifeline_watcher_started;
    bool                                m_lifeline_watcher_start_reported = false;
    lifeline_handle_type                m_lifeline_watcher_thread_handle = 0;
    unsigned long                       m_lifeline_watcher_start_error = 0;
#else
    int                                 m_lifeline_cancel_read = -1;
    int                                 m_lifeline_cancel_write = -1;
#endif

    Spawn_result spawn_swarm_process(
        const Spawn_swarm_process_args& ssp_args,
        detail::Managed_child_launch_attempt& launch_attempt);
    Spawn_result spawn_swarm_process_impl(
        const Spawn_swarm_process_args& ssp_args,
        detail::Managed_child_launch_attempt& launch_attempt);

    std::shared_ptr<detail::Managed_child_custody_record> accept_child_custody();
    bool can_accept_child_custody(instance_id_type process_instance_id) const;
    detail::Managed_child_launch_attempt admit_child_custody_occurrence(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        instance_id_type process_instance_id,
        uint32_t occurrence);
    void note_child_custody_failure(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        Managed_child_failure failure);
    void fail_release_attempt(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        uint64_t release_attempt_generation,
        Managed_child_failure failure,
        instance_id_type process_instance_id);
    void record_release_attempt_blocker(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        uint64_t release_attempt_generation,
        instance_id_type process_instance_id,
        uint32_t occurrence,
        const char* message);
    void request_child_custody_release(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        detail::Release_mode release_mode = detail::Release_mode::passive);
    void start_child_custody_release_worker(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        uint64_t release_attempt_generation);
    void execute_child_custody_release_attempt(
        std::shared_ptr<detail::Managed_child_custody_record> custody,
        uint64_t release_attempt_generation,
        instance_id_type failure_iid,
        uint32_t failure_occurrence);
    void request_all_child_custody_releases();
    bool wait_for_all_child_custodies(
        std::chrono::steady_clock::time_point deadline);
    bool all_child_custodies_released() const;
    detail::Managed_child_occurrence_token child_custody_occurrence_token(
        instance_id_type process_instance_id) const;
    detail::Managed_child_occurrence_token child_custody_occurrence_token_exact(
        uint64_t          custody_identity,
        instance_id_type process_instance_id,
        uint32_t         occurrence) const;
    void note_child_initialization_complete(
        const detail::Managed_child_occurrence_token& token);
    void mark_child_coordinator_initialization_complete(
        Coordinator* coordinator,
        instance_id_type process_instance_id);
    void note_child_publication_retired(
        const detail::Managed_child_occurrence_token& token);
    detail::Managed_child_communication_authority_capture
        capture_child_communication_retirement_authority(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    bool capture_replaced_child_communication_authority(
        const std::shared_ptr<Process_message_reader>& reader);
    bool begin_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        uint64_t expected_release_attempt_generation,
        uint64_t& claimed_release_attempt_generation,
        std::shared_ptr<Process_message_reader>& reader);
    bool complete_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    void reset_child_communication_retirement(
        const detail::Managed_child_occurrence_token& token,
        uint64_t release_attempt_generation);
    bool join_child_communication(
        const detail::Managed_child_occurrence_token& token,
        const std::shared_ptr<Process_message_reader>& reader);
    bool execute_current_slot_child_retirement(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        const detail::Managed_child_release_roster& targets,
        uint64_t release_attempt_generation,
        detail::Release_mode release_mode);
    bool execute_exact_historical_child_communication(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody,
        const detail::Managed_child_release_roster& targets,
        uint64_t release_attempt_generation);
    void retire_child_communication(
        const detail::Managed_child_occurrence_token& token,
        std::shared_ptr<Process_message_reader> reader);
    void note_child_os_exit(
        const detail::Managed_child_occurrence_token& token,
        int wait_status,
        bool wait_status_available = true);
    bool ensure_child_exit_dispatcher() noexcept;
    void run_child_exit_dispatcher() noexcept;
    void drain_child_exit_dispatcher() noexcept;
    void stop_child_exit_dispatcher() noexcept;
    void enqueue_child_exit_subscription_locked(
        std::shared_ptr<detail::Managed_child_exit_subscription_state> subscription,
        const Managed_child_exit& event) noexcept;
    void dispatch_child_exit_publication(
        detail::Managed_child_exit_publication publication) noexcept;
    void dispatch_child_exit_subscription(
        std::shared_ptr<detail::Managed_child_exit_subscription_state> subscription,
        Managed_child_exit event) noexcept;
#ifndef _WIN32
    bool signal_child_native_exact(
        const detail::Managed_child_occurrence_token& token,
        int signal_number);
#endif
    bool wait_for_child_native_exit(
        const detail::Managed_child_occurrence_token& token,
        std::chrono::steady_clock::time_point deadline);
    bool cleanup_child_native(
        const detail::Managed_child_occurrence_token& token);
    void start_owned_lifecycle_worker(
        std::function<void()> worker,
        const char* failure_stage = nullptr,
        instance_id_type failure_process_instance_id = invalid_instance_id,
        uint32_t failure_occurrence = 0);
    void join_owned_lifecycle_workers();
    void retire_child_custody_if_complete(
        const std::shared_ptr<detail::Managed_child_custody_record>& custody);

    map<instance_id_type, Spawn_swarm_process_args>
                                        m_cached_spawns;
    mutable std::mutex                  m_cached_spawns_mutex;

    mutable std::mutex                  m_child_custody_mutex;
    std::condition_variable             m_child_custody_changed;
    std::shared_ptr<const detail::Managed_process_lifetime>
                                        m_runtime_lifetime = std::make_shared<
                                            const detail::Managed_process_lifetime>();
    uint64_t                            m_next_child_custody_identity = 1;
    std::map<uint64_t, std::shared_ptr<detail::Managed_child_custody_record>>
                                        m_child_custodies;
    std::map<instance_id_type, detail::Managed_child_active_occurrence>
                                        m_child_custody_by_process;
    mutable std::mutex                  m_owned_lifecycle_workers_mutex;
    bool                                m_owned_lifecycle_worker_admission_open = true;
    struct Owned_lifecycle_worker
    {
        std::thread                         thread;
        std::shared_ptr<std::atomic<bool>>  complete;
    };
    std::vector<Owned_lifecycle_worker> m_owned_lifecycle_workers;
    std::mutex                          m_child_exit_dispatch_mutex;
    std::condition_variable             m_child_exit_dispatch_changed;
    std::thread                         m_child_exit_dispatch_thread;
    detail::Managed_child_exit_subscription_state*
                                        m_child_exit_dispatch_head = nullptr;
    detail::Managed_child_exit_subscription_state*
                                        m_child_exit_dispatch_tail = nullptr;
    bool                                m_child_exit_dispatch_active = false;
    bool                                m_child_exit_dispatch_stopping = false;
};


} // namespace sintra
