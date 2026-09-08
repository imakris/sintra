// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#ifdef _WIN32
#include "../sintra_windows.h"
#elif defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <mutex>
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
    }

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_closed{false};
    std::mutex       m_mutex;
    Native_family_status m_status;
    std::chrono::steady_clock::time_point m_force_until{};
    bool m_force_enabled = false;
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
} // namespace sintra
