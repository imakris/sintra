#pragma once

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sintra::test::managed_child {

struct process_identity_t
{
    int            pid         = -1;
    std::uint64_t  start_stamp = 0;

    bool operator==(const process_identity_t&) const = default;
};

struct child_identity_t
{
    sintra::instance_id_type   process_iid = sintra::invalid_instance_id;
    std::uint32_t              occurrence  = 0;
    int                        pid         = -1;
    std::uint64_t              start_stamp = 0;
};

struct exit_capture_snapshot_t
{
    std::optional<sintra::Managed_child_exit> event;
    unsigned                                  deliveries = 0;
};

class Managed_child_exit_capture
{
public:
    void record(const sintra::Managed_child_exit& event)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_event = event;
            ++m_deliveries;
        }
        m_changed.notify_all();
    }

    bool wait_for_one(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, timeout, [&] { return m_deliveries == 1; });
    }

    bool wait_for_one_until(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_until(
            lock, deadline, [&] { return m_deliveries == 1; });
    }

    exit_capture_snapshot_t snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {m_event, m_deliveries};
    }

    bool exact(const sintra::Managed_child_occurrence_identity& identity) const
    {
        const auto state = snapshot();
        return
            state.deliveries == 1 &&
            state.event           &&
            state.event->occurrence == identity;
    }

    bool normal_zero() const
    {
        const auto state = snapshot();
        return
            state.deliveries == 1                                                      &&
            state.event                                                                &&
            state.event->status_kind == sintra::Managed_child_exit_status_kind::exited &&
            state.event->status == 0;
    }

private:
    mutable std::mutex         m_mutex;
    std::condition_variable    m_changed;
    std::optional<sintra::Managed_child_exit>
                               m_event;
    unsigned                   m_deliveries = 0;
};

struct Occurrence_isolation_identity
{
    std::string                    nonce;
    sintra::instance_id_type       process_iid           = sintra::invalid_instance_id;
    sintra::instance_id_type       witness_iid           = sintra::invalid_instance_id;
    std::uint32_t                  occurrence            = 0;
    int                            pid                   = -1;
    bool                           start_stamp_available = false;
    std::uint64_t                  start_stamp           = 0;
    std::string                    witness_name;
};

struct Occurrence_isolation_outcome
{
    bool                           ledger_0_valid                    = false;
    bool                           predecessor_live                  = false;
    bool                           predecessor_request_held          = false;
    bool                           recovery_observed                 = false;
    bool                           exact_crash                       = false;
    bool                           predecessor_name_retired          = false;
    bool                           predecessor_request_still_held    = false;
    bool                           ledger_1_valid                    = false;
    bool                           replacement_live                  = false;
    bool                           replacement_satisfied_predecessor = false;
    bool                           request_completed                 = false;
    bool                           request_threw                     = false;
    std::uint32_t                  request_result                    = 0;

    bool                           custody_record_found              = false;
    bool                           occurrence_count_exact            = false;
    bool                           predecessor_occurrence            = false;
    bool                           predecessor_setup                 = false;
    bool                           predecessor_pid                   = false;
    bool                           predecessor_stamp_available       = false;
    bool                           predecessor_stamp                 = false;
    bool                           predecessor_publication_retired   = false;
    bool                           predecessor_communication_retired = false;
    bool                           predecessor_exit_confirmed        = false;
    bool                           replacement_occurrence            = false;
    bool                           replacement_setup                 = false;
    bool                           replacement_pid                   = false;
    bool                           replacement_stamp_available       = false;
    bool                           replacement_stamp                 = false;
    bool                           replacement_created               = false;
    bool                           replacement_not_exited            = false;

    bool                           release_marker_written                      = false;
    bool                           replacement_finalize_marker                 = false;
    bool                           root_finalized                              = false;
    bool                           predecessor_abnormal_exit                   = false;
    bool                           replacement_normal_exit                     = false;
    bool                           predecessor_native_exit_observer_registered = false;
    bool                           replacement_native_exit_observer_registered = false;
    bool                           forced_cleanup                              = false;
    bool                           survivors_absent                            = false;
    bool                           custody_release_complete                    = false;

    Occurrence_isolation_identity  predecessor;
    Occurrence_isolation_identity  replacement;
};

using Occurrence_isolation_outcome_callback = void (*)(
    const Occurrence_isolation_outcome&);

inline std::atomic<Occurrence_isolation_outcome_callback>
    s_occurrence_isolation_outcome{nullptr};

inline void emit_occurrence_isolation_outcome(
    const Occurrence_isolation_outcome& outcome)
{
    if (const auto callback = s_occurrence_isolation_outcome.load(
            std::memory_order_acquire))
    {
        callback(outcome);
    }
}

template <typename Callback>
class Scoped_test_hook
{
public:
    Scoped_test_hook(
        std::atomic<Callback>& slot,
        Callback               callback) noexcept
    :
        m_slot(&slot),
        m_previous(slot.exchange(callback, std::memory_order_acq_rel))
    {}

    ~Scoped_test_hook() noexcept
    {
        restore();
    }

    Scoped_test_hook(const Scoped_test_hook&)            = delete;
    Scoped_test_hook& operator=(const Scoped_test_hook&) = delete;
    Scoped_test_hook(Scoped_test_hook&&)                 = delete;
    Scoped_test_hook& operator=(Scoped_test_hook&&)      = delete;

    void restore() noexcept
    {
        if (m_slot) {
            m_slot->store(m_previous, std::memory_order_release);
            m_slot = nullptr;
        }
    }

private:
    std::atomic<Callback>* m_slot;
    Callback               m_previous;
};

inline bool exact_process_is_live(int pid, std::uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<std::uint32_t>(pid))) {
        return false;
    }
    const auto observed =
        sintra::query_process_start_stamp(static_cast<std::uint32_t>(pid));
    return observed && *observed == start_stamp;
}

inline bool exact_process_is_live(
    int            pid,
    bool           start_stamp_available,
    std::uint64_t  start_stamp)
{
    return start_stamp_available && exact_process_is_live(pid, start_stamp);
}

inline bool exact_process_is_live(const process_identity_t& identity)
{
    return exact_process_is_live(identity.pid, identity.start_stamp);
}

inline bool exact_process_is_live(const child_identity_t& identity)
{
    return exact_process_is_live(identity.pid, identity.start_stamp);
}

inline bool exact_process_is_absent(const child_identity_t& identity)
{
    return !exact_process_is_live(identity);
}

inline bool exact_process_is_absent(const process_identity_t& identity)
{
    return !exact_process_is_live(identity);
}

inline bool wait_for_exact_process_absence(
    const child_identity_t&    identity,
    std::chrono::milliseconds  timeout,
    std::chrono::milliseconds  poll = std::chrono::milliseconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (exact_process_is_absent(identity)) {
            return true;
        }
        std::this_thread::sleep_for(poll);
    }
    return exact_process_is_absent(identity);
}

inline bool wait_for_exact_process_absence(
    int                        pid,
    std::uint64_t              start_stamp,
    std::chrono::milliseconds  timeout,
    std::chrono::milliseconds  poll = std::chrono::milliseconds(10))
{
    return wait_for_exact_process_absence(
        child_identity_t{sintra::invalid_instance_id, 0, pid, start_stamp},
        timeout,
        poll);
}

inline bool wait_for_exact_process_absence(
    const process_identity_t&  identity,
    std::chrono::milliseconds  timeout,
    std::chrono::milliseconds  poll = std::chrono::milliseconds(10))
{
    return wait_for_exact_process_absence(
        identity.pid, identity.start_stamp, timeout, poll);
}

inline bool write_complete_file(
    const std::filesystem::path&   path,
    const std::string&             contents)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << contents;
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

inline std::optional<child_identity_t> current_child_identity()
{
    const auto start_stamp = sintra::current_process_start_stamp();
    if (!start_stamp) {
        return std::nullopt;
    }
    return child_identity_t{
        sintra::s_mproc_id,
        sintra::s_recovery_occurrence,
        static_cast<int>(sintra::detail::get_current_process_id()),
        *start_stamp};
}

inline std::optional<process_identity_t> current_process_identity()
{
    const auto start_stamp = sintra::current_process_start_stamp();
    if (!start_stamp) {
        return std::nullopt;
    }
    return process_identity_t{
        static_cast<int>(sintra::detail::get_current_process_id()),
        *start_stamp};
}

inline bool write_process_identity(const std::filesystem::path& path)
{
    const auto identity = current_process_identity();
    if (!identity) {
        return false;
    }
    std::ostringstream contents;
    contents << identity->pid << ' '
        << static_cast<unsigned long long>(identity->start_stamp) << '\n';
    return write_complete_file(path, contents.str());
}

inline std::optional<process_identity_t> read_process_identity(
    const std::filesystem::path& path)
{
    process_identity_t identity;
    std::ifstream input(path, std::ios::binary);
    if (!(input >> identity.pid >> identity.start_stamp) ||
        identity.pid <= 0 || identity.start_stamp == 0)
    {
        return std::nullopt;
    }
    return identity;
}

inline void write_child_identity(
    std::ostream&              output,
    const child_identity_t&    identity)
{
    output
        << static_cast<unsigned long long>(identity.process_iid) << ' '
        << identity.occurrence << ' '
        << identity.pid << ' '
        << static_cast<unsigned long long>(identity.start_stamp);
}

inline bool write_child_identity(const std::filesystem::path& path)
{
    const auto identity = current_child_identity();
    if (!identity) {
        return false;
    }
    std::ostringstream contents;
    write_child_identity(contents, *identity);
    contents << '\n';
    return write_complete_file(path, contents.str());
}

inline std::optional<child_identity_t> read_child_identity(std::istream& input)
{
    child_identity_t identity;
    unsigned long long process_iid = 0;
    unsigned long long start_stamp = 0;
    if (!(input
        >> process_iid
        >> identity.occurrence
        >> identity.pid
        >> start_stamp))
    {
        return std::nullopt;
    }
    identity.process_iid = static_cast<sintra::instance_id_type>(process_iid);
    identity.start_stamp = static_cast<std::uint64_t>(start_stamp);
    if (identity.pid <= 0 || identity.start_stamp == 0) {
        return std::nullopt;
    }
    return identity;
}

inline std::optional<child_identity_t> read_child_identity(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return read_child_identity(input);
}

inline std::optional<child_identity_t> wait_for_child_identity(
    const std::filesystem::path&           path,
    std::chrono::steady_clock::time_point  deadline,
    std::chrono::milliseconds              poll = std::chrono::milliseconds(10))
{
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto identity = read_child_identity(path)) {
            return identity;
        }
        std::this_thread::sleep_for(poll);
    }
    return read_child_identity(path);
}

inline std::optional<child_identity_t> wait_for_child_identity(
    const std::filesystem::path&   path,
    std::chrono::milliseconds      timeout,
    std::chrono::milliseconds      poll = std::chrono::milliseconds(10))
{
    return wait_for_child_identity(
        path, std::chrono::steady_clock::now() + timeout, poll);
}

inline bool terminate_process_by_pid(
    int                            pid,
    std::uint32_t                  windows_exit_code,
    std::chrono::milliseconds      wait_timeout = std::chrono::seconds(5))
{
#ifdef _WIN32
    HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }
    const bool terminated =
        TerminateProcess(process, static_cast<UINT>(windows_exit_code)) != 0;
    if (terminated) {
        (void)WaitForSingleObject(
            process,
            static_cast<DWORD>(wait_timeout.count()));
    }
    CloseHandle(process);
    return terminated;
#else
    (void)windows_exit_code;
    (void)wait_timeout;
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

inline sintra::External_process_invitation make_invitation(
    sintra::instance_id_type       process_iid,
    std::chrono::milliseconds      timeout)
{
    sintra::External_process_invitation_options options;
    options.process_instance_id = process_iid;
    options.timeout             = timeout;
    return sintra::create_external_process_invitation(options);
}

} // namespace sintra::test::managed_child
