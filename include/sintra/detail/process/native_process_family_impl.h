// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "native_process_family.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace sintra {

namespace detail {

class Native_family_change_publication
{
public:
    Native_family_change_publication(
        const Native_family_status& status, Managed_child_change_condition& changes)
    :
        m_status(status), m_previous(status), m_changes(changes)
    {}

    ~Native_family_change_publication()
    {
        if (!(m_status == m_previous)) {
            m_changes.notify_all();
        }
    }

private:
    const Native_family_status&     m_status;
    Native_family_status            m_previous;
    Managed_child_change_condition& m_changes;
};

} // namespace detail

inline void Managed_process::observe_native_family_changes(const Managed_child_native_change_signal& signal)
{
    std::lock_guard<std::mutex> records_lock(m_child_custody_mutex);
    if (std::any_of(m_native_family_observers.begin(), m_native_family_observers.end(),
            [&](const Managed_child_native_change_signal& existing) { return existing.m_state == signal.m_state; }))
    {
        return;
    }
    m_native_family_observers.push_back(signal);
    m_native_family_changed.observe(signal);
    for (const auto& original : m_native_family_custodies) {
        if (const auto custody = original.lock()) {
            custody->changed.observe(signal);
        }
    }
}

#if defined(__linux__)
inline void Managed_process::reap_native_family_children()
{
    // This is the family mode of the existing sole reaper. Native spawn's
    // pre-reserved handoff keeps its exec-failure waiter exclusive until the
    // original occurrence can receive the authoritative root exit.
    std::lock_guard<std::mutex> roster_lock(m_spawned_child_pids_mutex);
    auto& family = detail::native_process_family();
    std::lock_guard<std::mutex> family_lock(family.m_mutex);
    auto& status = family.m_status;
    detail::Native_family_change_publication publication(status, m_native_family_changed);
    status.native_empty = false;
    const bool external_birth_pending = std::any_of(
        family.m_external_children.begin(), family.m_external_children.end(),
        [](const auto& child) { return child->phase == detail::External_native_child::Phase::PENDING; });
    if (external_birth_pending || std::any_of(m_spawned_child_pids.begin(), m_spawned_child_pids.end(),
            [](const Spawned_child_reap_slot& slot) { return slot.pid <= 0; }))
    {
        // A different launch may remain unresolved. Its unpublished PID must
        // stay with the exec handshake, but already committed root slots keep
        // their independent exact exit observation throughout that wait.
        for (auto slot = m_spawned_child_pids.begin(); slot != m_spawned_child_pids.end();) {
            if (slot->pid <= 0) {
                ++slot;
                continue;
            }
            int wait_status = 0;
            pid_t reaped;
            do {
                reaped = waitpid(slot->pid, &wait_status, WNOHANG | __WALL);
            }
            while (reaped < 0 && errno == EINTR);
            if (reaped == slot->pid) {
                note_child_os_exit(slot->occurrence, wait_status);
                const auto retained = m_native_family_pidfds.find(reaped);
                if (retained != m_native_family_pidfds.end()) {
                    close(retained->second);
                    m_native_family_pidfds.erase(retained);
                }
                slot = m_spawned_child_pids.erase(slot);
                detail::child_reaped_for_test(reaped, wait_status);
            }
            else {
                if (reaped < 0) {
                    family.fail(errno, "waitpid(committed root during native handoff)");
                }
                ++slot;
            }
        }
        return;
    }

    // P_ALL is unavailable while a maintained caller owns an exact direct
    // waiter: even observing and then skipping its zombie would starve the
    // other children. Enumerate below and consume only individually owned
    // results until every external wait authority has settled.
    while (family.m_external_children.empty()) {
        siginfo_t info{};
        int result;
        do {
            result = waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT | __WALL);
        }
        while (result < 0 && errno == EINTR);
        if (result < 0) {
            if (errno == ECHILD) {
                status.native_empty = true;
                status.observed_process_ids.clear();
            }
            else {
                family.fail(errno, "waitid(P_ALL)");
            }
            break;
        }
        if (info.si_pid == 0) {
            break;
        }
        int wait_status = 0;
        pid_t reaped;
        do {
            reaped = waitpid(info.si_pid, &wait_status, WNOHANG | __WALL);
        }
        while (reaped < 0 && errno == EINTR);
        if (reaped != info.si_pid) {
            family.fail(reaped < 0 ? errno : EAGAIN, "waitpid(family exact child)");
            return;
        }
        const auto slot = std::find_if(m_spawned_child_pids.begin(), m_spawned_child_pids.end(),
            [&](const Spawned_child_reap_slot& candidate) { return candidate.pid == reaped; });
        if (slot != m_spawned_child_pids.end()) {
            note_child_os_exit(slot->occurrence, wait_status);
            m_spawned_child_pids.erase(slot);
        }
        const auto adopted = m_native_family_pidfds.find(reaped);
        if (adopted != m_native_family_pidfds.end()) {
            close(adopted->second);
            m_native_family_pidfds.erase(adopted);
        }
        detail::child_reaped_for_test(reaped, wait_status);
    }

    if (status.native_empty) {
        return;
    }

    // /proc's children list is per thread. The owner uses multiple spawning
    // threads; looking only at thread-self misses their adopted descendants.
    std::set<pid_t> children;
    std::error_code directory_error;
    std::filesystem::directory_iterator task(detail::k_owned_thread_root, directory_error);
    const std::filesystem::directory_iterator end;
    if (directory_error) {
        family.fail(directory_error.value(), "enumerate /proc/self/task");
        return;
    }
    for (; task != end; task.increment(directory_error)) {
        if (directory_error) {
            break;
        }
        std::ifstream child_file = detail::open_owned_thread_children(task->path());
        if (!child_file) {
            std::error_code exists_error;
            if (!std::filesystem::exists(task->path(), exists_error) && !exists_error) {
                continue;
            }
            family.fail(errno ? errno : EIO, "read owned thread children");
            return;
        }
        if (!detail::parse_owned_thread_children(child_file, [&children](pid_t child) {
                children.insert(child);
            }))
        {
            family.fail(EIO, "parse owned thread children");
            return;
        }
    }
    if (directory_error) {
        family.fail(directory_error.value(), "enumerate /proc/self/task");
        return;
    }
    status.observed_process_ids.assign(children.begin(), children.end());
    for (const pid_t child : children) {
        int identity_error = 0;
        const auto external = std::find_if(
            family.m_external_children.begin(), family.m_external_children.end(),
            [child, &identity_error](const auto& entry) {
                if (entry->phase != detail::External_native_child::Phase::CREATED ||
                    entry->process_id != static_cast<std::uint32_t>(child)) return false;
                siginfo_t exact{};
                // A retained pidfd whose original child has already been
                // reaped cannot exclude a successor that reused its PID.
                int inspected;
                do {
                    inspected = waitid(P_PIDFD, static_cast<id_t>(entry->native), &exact,
                        WEXITED | WNOHANG | WNOWAIT | __WALL);
                } while (inspected < 0 && errno == EINTR);
                if (inspected < 0 && errno != ECHILD) identity_error = errno;
                return inspected == 0;
            });
        if (identity_error != 0) {
            family.fail(identity_error, "waitid(external child identity)");
            return;
        }
        if (external != family.m_external_children.end()) {
            const auto entry = *external;
            if (!entry->direct_waiter) {
                siginfo_t exact{};
                int reaped;
                do {
                    reaped = waitid(P_PIDFD, static_cast<id_t>(entry->native), &exact,
                        WEXITED | WNOHANG | __WALL);
                } while (reaped < 0 && errno == EINTR);
                if (reaped == 0 && exact.si_pid != 0) {
                    entry->phase = detail::External_native_child::Phase::EXITED;
                    family.m_external_children.erase(external);
                    // This pass enumerated before consuming the child. Publish
                    // its removal and schedule the now-unblocked P_ALL pass;
                    // no later SIGCHLD is guaranteed for the last handoff.
                    status.observed_process_ids.erase(std::remove(
                        status.observed_process_ids.begin(), status.observed_process_ids.end(), child),
                        status.observed_process_ids.end());
                    family.wake_observer();
                    continue;
                }
                if (reaped < 0) {
                    family.fail(errno, "waitid(external child handoff)");
                    return;
                }
            }
            if (family.m_force_enabled && family.m_status.action_active &&
                std::chrono::steady_clock::now() < family.m_force_until &&
                syscall(SYS_pidfd_send_signal, entry->native, SIGKILL, nullptr, 0) != 0 && errno != ESRCH)
            {
                family.fail(errno, "pidfd_send_signal(external child)");
            }
            continue;
        }
        if (!family.m_external_children.empty()) {
            siginfo_t ready{};
            int observed;
            do {
                observed = waitid(P_PID, static_cast<id_t>(child), &ready,
                    WEXITED | WNOHANG | WNOWAIT | __WALL);
            } while (observed < 0 && errno == EINTR);
            if (observed < 0) {
                family.fail(errno, "waitid(individually owned family child)");
                return;
            }
            if (ready.si_pid != 0) {
                int wait_status = 0;
                pid_t reaped;
                do { reaped = waitpid(child, &wait_status, WNOHANG | __WALL); }
                while (reaped < 0 && errno == EINTR);
                if (reaped != child) {
                    family.fail(reaped < 0 ? errno : EAGAIN, "waitpid(individually owned family child)");
                    return;
                }
                const auto slot = std::find_if(m_spawned_child_pids.begin(), m_spawned_child_pids.end(),
                    [child](const Spawned_child_reap_slot& candidate) { return candidate.pid == child; });
                if (slot != m_spawned_child_pids.end()) {
                    note_child_os_exit(slot->occurrence, wait_status);
                    m_spawned_child_pids.erase(slot);
                }
                const auto retained = m_native_family_pidfds.find(child);
                if (retained != m_native_family_pidfds.end()) {
                    close(retained->second);
                    m_native_family_pidfds.erase(retained);
                }
                detail::child_reaped_for_test(child, wait_status);
                continue;
            }
        }
        auto retained = m_native_family_pidfds.find(child);
        if (retained == m_native_family_pidfds.end()) {
            // With the sole reaper locked, an own child cannot be reaped and
            // its PID reused between this ownership check and pidfd_open.
            siginfo_t own_child{};
            int result;
            do {
                result = waitid(P_PID, child, &own_child,
                    WEXITED | WNOHANG | WNOWAIT | __WALL);
            }
            while (result < 0 && errno == EINTR);
            if (result < 0) {
                family.fail(errno, "waitid(owned child before pidfd_open)");
                return;
            }
            const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, child, 0));
            if (pidfd < 0) {
                family.fail(errno, "pidfd_open(owned child)");
                return;
            }
            try {
                retained = m_native_family_pidfds.emplace(child, pidfd).first;
            }
            catch (...) {
                close(pidfd);
                throw;
            }
        }
        if (family.m_force_enabled && family.m_status.action_active &&
            std::chrono::steady_clock::now() < family.m_force_until)
        {
            if (syscall(SYS_pidfd_send_signal, retained->second, SIGKILL, nullptr, 0) != 0 &&
                errno != ESRCH)
            {
                family.fail(errno, "pidfd_send_signal(SIGKILL)");
            }
        }
    }
}
#endif

inline Native_family_status Managed_process::observe_native_family()
{
    auto& family = detail::native_process_family();
    if (!family.m_active.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(family.m_mutex);
        return family.m_status;
    }
    bool pending_launches = false;
    {
        std::lock_guard<std::mutex> records_lock(m_child_custody_mutex);
        for (const auto& [identity, custody] : m_child_custodies) {
            std::lock_guard<std::mutex> custody_lock(custody->mutex);
            pending_launches |= std::any_of(custody->occurrences.begin(), custody->occurrences.end(),
                [](const detail::Managed_child_occurrence_record& occurrence) {
                    return occurrence.setup == detail::Managed_child_occurrence_record::setup_state::pending;
                });
        }
    }
#if defined(__linux__)
    reap_finished_children();
#endif
    std::lock_guard<std::mutex> lock(family.m_mutex);
    auto& status = family.m_status;
    detail::Native_family_change_publication publication(status, m_native_family_changed);
    status.admission_closed = family.m_closed.load(std::memory_order_acquire);
    pending_launches |= std::any_of(family.m_external_children.begin(), family.m_external_children.end(),
        [](const auto& child) { return child->phase == detail::External_native_child::Phase::PENDING; });
    status.pending_launches = pending_launches;
#ifdef _WIN32
    std::erase_if(family.m_external_children, [](const auto& child) {
        return child->phase == detail::External_native_child::Phase::CREATED &&
            !child->direct_waiter && WaitForSingleObject(child->native, 0) == WAIT_OBJECT_0;
    });
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (QueryInformationJobObject(family.m_job, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr))
    {
        status.native_empty = accounting.ActiveProcesses == 0;
        if (status.native_empty) {
            status.observed_process_ids.clear();
        }
        else {
            DWORD capacity = accounting.ActiveProcesses + 16;
            for (unsigned attempt = 0; attempt != 3; ++attempt) {
                std::vector<ULONG_PTR> storage(static_cast<size_t>(capacity) + 2);
                auto* processes = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
                if (QueryInformationJobObject(family.m_job, JobObjectBasicProcessIdList,
                        processes, static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR)), nullptr))
                {
                    status.observed_process_ids.clear();
                    for (DWORD index = 0; index < processes->NumberOfProcessIdsInList; ++index) {
                        status.observed_process_ids.push_back(
                            static_cast<std::uint32_t>(processes->ProcessIdList[index]));
                    }
                    break;
                }
                const DWORD error = GetLastError();
                if (error != ERROR_MORE_DATA || attempt == 2) {
                    family.fail(error, "QueryInformationJobObject(ProcessIdList)");
                    break;
                }
                capacity = processes->NumberOfAssignedProcesses + 16;
            }
        }
    }
    else {
        family.fail(GetLastError(), "QueryInformationJobObject(ActiveProcesses)");
    }
#endif
    auto result = status;
    // An empty OS observation is final only after native launch outcomes have
    // settled under closed admission. Communication release remains separate.
    result.native_empty &= result.admission_closed && !pending_launches && family.m_external_children.empty();
    return result;
}

#ifdef _WIN32
inline void Managed_process::observe_native_family_job()
{
    auto& family = detail::native_process_family();
    while (true) {
        DWORD message = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* process = nullptr;
        // Job messages provide normal wakeups. Windows does not guarantee
        // every job notification; a closed-family observation deadline bounds
        // the fallback without polling while normal admission remains open.
        const DWORD wait = family.m_closed.load() ? 50 : INFINITE;
        const BOOL received = GetQueuedCompletionStatus(family.m_completion_port,
            &message, &key, &process, wait);
        if (!received && GetLastError() != WAIT_TIMEOUT) {
            std::lock_guard<std::mutex> lock(family.m_mutex);
            family.fail(GetLastError(), "GetQueuedCompletionStatus(native family)");
            m_native_family_changed.notify_all();
            return;
        }
        if (observe_native_family().native_empty) {
            return;
        }
    }
}
#endif

inline bool Managed_process::request_native_family_termination(
    std::chrono::steady_clock::time_point deadline)
{
    auto& family = detail::native_process_family();
    {
        std::lock_guard<std::mutex> lock(family.m_mutex);
        if (!family.m_active.load() || !family.m_closed.load() ||
            family.m_status.action_active || deadline <= std::chrono::steady_clock::now())
        {
            return false;
        }
        family.m_status.action_active = true;
        family.m_force_enabled = false;
        family.m_force_until = deadline;
        family.m_status.termination_requested = true;
        ++family.m_status.action_generation;
        family.m_status.native_error = 0;
        family.m_status.failed_operation.clear();
    }
    m_native_family_changed.notify_all();
    try {
        start_owned_lifecycle_worker([this, deadline]() {
            execute_native_family_termination(deadline);
        });
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(family.m_mutex);
        family.m_status.action_active = false;
        family.fail(EAGAIN, "start native family termination worker");
        return false;
    }
    return true;
}

inline bool Managed_process::native_family_roots_settled()
{
    std::lock_guard<std::mutex> records_lock(m_child_custody_mutex);
    for (auto entry = m_native_family_custodies.begin(); entry != m_native_family_custodies.end();) {
        const auto custody = entry->lock();
        if (!custody) {
            entry = m_native_family_custodies.erase(entry);
            continue;
        }
        ++entry;
        std::lock_guard<std::mutex> custody_lock(custody->mutex);
        for (const auto& occurrence : custody->occurrences) {
            if (occurrence.setup == detail::Managed_child_occurrence_record::setup_state::pending ||
                occurrence.native.running() || occurrence.native_action.outcome == Managed_child_native_outcome::ACTIVE)
            {
                return false;
            }
        }
    }
    return true;
}

inline void Managed_process::execute_native_family_termination(
    std::chrono::steady_clock::time_point deadline)
{
    auto& family = detail::native_process_family();
    try {
        while (std::chrono::steady_clock::now() < deadline) {
            const bool roots_settled = native_family_roots_settled();
            {
                std::lock_guard<std::mutex> lock(family.m_mutex);
                // Global containment must not bypass the exact original root
                // action/elevation serializer. Only residual descendants are
                // terminated here after every original root is native-terminal.
                family.m_force_enabled = roots_settled;
#ifdef _WIN32
                if (roots_settled && !TerminateJobObject(family.m_job, 1)) {
                    family.fail(GetLastError(), "TerminateJobObject");
                }
#endif
            }
            if (observe_native_family().native_empty) {
                break;
            }
            // Adoption does not reliably produce a SIGCHLD for a live orphan;
            // the bounded tick supplements the existing signal-driven reaper.
            std::this_thread::sleep_until(std::min(deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(25)));
        }
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(family.m_mutex);
        family.fail(EIO, "observe native family termination");
    }
    std::lock_guard<std::mutex> lock(family.m_mutex);
    family.m_status.action_active = false;
    m_native_family_changed.notify_all();
}

} // namespace sintra
