// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#ifdef _WIN32
#include "../sintra_windows.h"
#elif defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

namespace sintra {

/// Local optional capability. No shared swarm schema or per-session ownership
/// is implied by this process-wide observation.
struct Native_family_status
{
    std::uint32_t             version = 1;
    bool                      active = false;
    bool                      admission_closed = false;
    bool                      pending_launches = false;
    bool                      native_empty = false;
    bool                      termination_requested = false;
    bool                      action_active = false;
    std::uint64_t             action_generation = 0;
    std::uint32_t             native_error = 0;
    std::string               failed_operation;
    std::vector<std::uint32_t> observed_process_ids;

    bool operator==(const Native_family_status& other) const
    {
        return version == other.version && active == other.active &&
            admission_closed == other.admission_closed && pending_launches == other.pending_launches &&
            native_empty == other.native_empty && termination_requested == other.termination_requested &&
            action_active == other.action_active && action_generation == other.action_generation &&
            native_error == other.native_error &&
            failed_operation == other.failed_operation && observed_process_ids == other.observed_process_ids;
    }
};

namespace detail {

struct External_native_child
{
    enum class Phase { PENDING, CREATED, EXITED };
    Phase phase = Phase::PENDING;
    std::uint32_t process_id = 0;
    bool direct_waiter = true;
#ifdef _WIN32
    HANDLE native = nullptr;
    ~External_native_child() { if (native) CloseHandle(native); }
#elif defined(__linux__)
    int native = -1;
    ~External_native_child() { if (native >= 0) close(native); }
#endif
};

class Native_process_family
{
public:
    ~Native_process_family()
    {
#ifdef _WIN32
        if (m_job) {
            CloseHandle(m_job);
        }
        if (m_completion_port) {
            CloseHandle(m_completion_port);
        }
#endif
    }

    bool activate()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active.load()) {
            return true;
        }
#ifdef _WIN32
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) {
            fail(GetLastError(), "CreateJobObject");
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)))
        {
            fail(GetLastError(), "SetInformationJobObject");
            CloseHandle(m_job);
            m_job = nullptr;
            return false;
        }
        m_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT completion{};
        completion.CompletionKey = this;
        completion.CompletionPort = m_completion_port;
        if (!m_completion_port || !SetInformationJobObject(m_job,
                JobObjectAssociateCompletionPortInformation, &completion, sizeof(completion)))
        {
            fail(GetLastError(), "associate native family job completion port");
            if (m_completion_port) {
                CloseHandle(m_completion_port);
                m_completion_port = nullptr;
            }
            CloseHandle(m_job);
            m_job = nullptr;
            return false;
        }
#elif defined(__linux__)
        // Activation promises exclusive child wait ownership. A pre-existing
        // child could belong to another library's native waiter.
        siginfo_t info{};
        int result;
        do {
            result = waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT | __WALL);
        }
        while (result < 0 && errno == EINTR);
        if (!(result < 0 && errno == ECHILD)) {
            fail(result < 0 ? errno : EBUSY, "native family activation requires no existing children");
            return false;
        }
        const int probe = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
        if (probe < 0) {
            fail(errno, "pidfd_open");
            return false;
        }
        close(probe);
        if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
            fail(errno, "prctl(PR_SET_CHILD_SUBREAPER)");
            return false;
        }
#else
        fail(ENOTSUP, "native process family unavailable on this platform");
        return false;
#endif
        m_status = {};
        m_status.active = true;
        m_active.store(true);
        return true;
    }

    void fail(std::uint32_t error, const char* operation)
    {
        m_status.native_error = error;
        m_status.failed_operation = operation;
        m_status.native_empty = false;
    }

    void wake_observer()
    {
#ifdef _WIN32
        if (m_completion_port) {
            PostQueuedCompletionStatus(m_completion_port, 0, reinterpret_cast<ULONG_PTR>(this), nullptr);
        }
#endif
#if defined(__linux__)
        // Membership can settle after the original SIGCHLD was observed.
        // Wake the existing sole reaper; do not introduce another waiter.
        if (m_active.load()) {
            (void)::kill(::getpid(), SIGCHLD);
        }
#endif
    }

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_closed{false};
    std::mutex       m_mutex;
    Native_family_status m_status;
    std::chrono::steady_clock::time_point m_force_until{};
    bool m_force_enabled = false;
    std::vector<std::shared_ptr<External_native_child>> m_external_children;
#ifdef _WIN32
    HANDLE m_job = nullptr;
    HANDLE m_completion_port = nullptr;
#endif
};

inline Native_process_family& native_process_family()
{
    static Native_process_family family;
    return family;
}

} // namespace detail

/// Birth reservation for a maintained non-Sintra child. The caller retains
/// its existing exact direct-child wait authority; the family owns adopted
/// residual descendants. Reserve before any native creation, publish the
/// original native reference before ending that reservation, and observe the
/// exact reap before releasing it. Abandoning a created ticket does not erase
/// custody: hand off only after the caller has stopped every direct wait.
class Native_family_external_child
{
public:
    enum class Admission { NOT_REQUIRED, ADMITTED, CLOSED };
    Native_family_external_child() = default;
    Native_family_external_child(const Native_family_external_child&) = delete;
    Native_family_external_child& operator=(const Native_family_external_child&) = delete;
    Native_family_external_child(Native_family_external_child&&) noexcept = default;
    Native_family_external_child& operator=(Native_family_external_child&&) noexcept = default;

    static Native_family_external_child reserve()
    {
        Native_family_external_child result;
        auto& family = detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        if (!family.m_active.load()) return result;
        result.m_admission = Admission::CLOSED;
        if (family.m_closed.load()) return result;
        result.m_record = std::make_shared<detail::External_native_child>();
        family.m_external_children.push_back(result.m_record);
        result.m_admission = Admission::ADMITTED;
        family.m_status.native_empty = false;
        family.wake_observer();
        return result;
    }

    Admission admission() const noexcept { return m_admission; }

    uintptr_t job_handle() const noexcept
    {
#ifdef _WIN32
        return m_record ? reinterpret_cast<uintptr_t>(detail::native_process_family().m_job) : 0;
#else
        return 0;
#endif
    }

    bool publish_created(std::uint32_t process_id, uintptr_t native)
    {
        if (!m_record) return m_admission == Admission::NOT_REQUIRED;
        auto& family = detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        if (m_record->phase != detail::External_native_child::Phase::PENDING || process_id == 0) return false;
#ifdef _WIN32
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(native),
                GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        BOOL contained = FALSE;
        if (GetProcessId(retained) != process_id ||
            !IsProcessInJob(retained, family.m_job, &contained) || !contained)
        {
            CloseHandle(retained);
            return false;
        }
        m_record->native = retained;
#elif defined(__linux__)
        const int retained = fcntl(static_cast<int>(native), F_DUPFD_CLOEXEC, 0);
        if (retained < 0) return false;
        std::ifstream descriptor_info("/proc/self/fdinfo/" + std::to_string(retained));
        std::string field;
        std::uint32_t original_pid = 0;
        while (descriptor_info >> field) {
            if (field == "Pid:") {
                descriptor_info >> original_pid;
                break;
            }
            std::string remainder;
            std::getline(descriptor_info, remainder);
        }
        if (original_pid != process_id) {
            close(retained);
            return false;
        }
        siginfo_t observed{};
        int inspected;
        do {
            inspected = waitid(P_PIDFD, static_cast<id_t>(retained), &observed,
                WEXITED | WNOHANG | WNOWAIT | __WALL);
        } while (inspected < 0 && errno == EINTR);
        if (inspected != 0)
        {
            close(retained);
            return false;
        }
        m_record->native = retained;
#else
        return false;
#endif
        m_record->process_id = process_id;
        m_record->phase = detail::External_native_child::Phase::CREATED;
        family.wake_observer();
        return true;
    }

    // Only before birth, or after the designated creator has itself reaped
    // an unpublished occurrence. An unresolved created child is not cancelled.
    void cancel_uncreated()
    {
        if (!m_record) return;
        auto& family = detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        if (m_record->phase != detail::External_native_child::Phase::PENDING) return;
        m_record->phase = detail::External_native_child::Phase::EXITED;
        std::erase(family.m_external_children, m_record);
        family.wake_observer();
    }

    bool observe_reaped()
    {
        if (!m_record) return m_admission == Admission::NOT_REQUIRED;
        auto& family = detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        if (m_record->phase == detail::External_native_child::Phase::EXITED) return true;
        if (m_record->phase != detail::External_native_child::Phase::CREATED) return false;
#ifdef _WIN32
        if (WaitForSingleObject(m_record->native, 0) != WAIT_OBJECT_0) return false;
#elif defined(__linux__)
        pollfd observation{m_record->native, POLLIN, 0};
        if (poll(&observation, 1, 0) != 1 || !(observation.revents & POLLIN)) return false;
        siginfo_t observed{};
        int inspected;
        do {
            inspected = waitid(P_PIDFD, static_cast<id_t>(m_record->native), &observed,
                WEXITED | WNOHANG | WNOWAIT | __WALL);
        } while (inspected < 0 && errno == EINTR);
        if (inspected == 0 || errno != ECHILD) return false;
#else
        return false;
#endif
        m_record->phase = detail::External_native_child::Phase::EXITED;
        std::erase(family.m_external_children, m_record);
        family.wake_observer();
        return true;
    }

    void handoff_wait_authority()
    {
        if (!m_record) return;
        auto& family = detail::native_process_family();
        std::lock_guard<std::mutex> lock(family.m_mutex);
        m_record->direct_waiter = false;
        family.wake_observer();
    }

private:
    Admission m_admission = Admission::NOT_REQUIRED;
    std::shared_ptr<detail::External_native_child> m_record;
};

} // namespace sintra
