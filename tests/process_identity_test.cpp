// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/detail/ipc/process_utils.h>

#include "test_process_identity_fakes.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

using sintra::Process_identity_status;
namespace fakes = sintra::test::identity_fakes;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// An UNKNOWN result carries the given native error; other results carry none.
void require_result(
    const sintra::process_identity_result_t&   result,
    Process_identity_status                    status,
    int                                        error,
    const char*                                message)
{
    require(result.status == status && (error == 0 ? !result.error : result.error.value() == error), message);
}

uint64_t own_stamp()
{
    const auto stamp = sintra::current_process_start_stamp();
    require(stamp && *stamp != 0, "current process must have a nonzero creation stamp");
    return *stamp;
}

sintra::process_incarnation_t own_incarnation()
{
    const auto incarnation = sintra::current_process_incarnation();
    require(incarnation && incarnation->pid == sintra::get_current_pid() && incarnation->start_stamp != 0,
        "current process must capture its incarnation");
    return *incarnation;
}

void live_identity()
{
    const auto result = sintra::probe_process_identity(own_incarnation());
    require(result.status == Process_identity_status::LIVE && !result.error,
        "matching running incarnation must be LIVE without an error");
}

void mismatched_identity()
{
    auto owner = own_incarnation();
    owner.start_stamp += 1;
    const auto result = sintra::probe_process_identity(owner);
#if defined(__FreeBSD__)
    // A missed boot-time change can make the stamps of one live process differ.
    require_result(result, Process_identity_status::UNKNOWN, sintra::detail::k_start_stamp_mismatch_error,
        "a different start stamp in a live FreeBSD record must be UNKNOWN, never DEAD");
#else
    require(result.status == Process_identity_status::DEAD && !result.error,
        "a different incarnation must be DEAD even while that PID is live");
#endif
}

void invalid_identity()
{
    const auto missing_pid = sintra::probe_process_identity({0, own_stamp(), {}});
    const auto missing_stamp = sintra::probe_process_identity({sintra::get_current_pid(), 0, {}});
    require(missing_pid.status == Process_identity_status::UNKNOWN && missing_pid.error,
        "PID zero must be UNKNOWN, not proof of absence");
    require(missing_stamp.status == Process_identity_status::UNKNOWN && missing_stamp.error,
        "a missing incarnation must be UNKNOWN");
}

#ifdef _WIN32
class Child_process
{
public:
    Child_process()
    {
        wchar_t executable[32768];
        const DWORD length = ::GetModuleFileNameW(nullptr, executable, 32768);
        require(length != 0 && length < 32768, "GetModuleFileNameW failed");
        std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --identity-child";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &m_process), "CreateProcessW failed");
        ::CloseHandle(m_process.hThread);
        m_process.hThread = nullptr;
        const auto stamp = sintra::query_process_start_stamp(m_process.dwProcessId);
        require(stamp && *stamp != 0, "child creation stamp missing");
        m_stamp = *stamp;
    }

    ~Child_process()
    {
        if (m_process.hProcess) {
            ::TerminateProcess(m_process.hProcess, 1);
            ::WaitForSingleObject(m_process.hProcess, 5000);
            ::CloseHandle(m_process.hProcess);
        }
    }

    void terminate(DWORD exit_code)
    {
        require(::TerminateProcess(m_process.hProcess, exit_code), "TerminateProcess failed");
        require(::WaitForSingleObject(m_process.hProcess, 5000) == WAIT_OBJECT_0,
            "terminated child must become signaled");
    }

    void close()
    {
        require(::CloseHandle(m_process.hProcess), "closing the last owned process handle failed");
        m_process.hProcess = nullptr;
    }

    uint32_t pid() const { return m_process.dwProcessId; }
    uint64_t stamp() const { return m_stamp; }

private:
    PROCESS_INFORMATION m_process{};
    uint64_t m_stamp = 0;
};

void retained_exit_259()
{
    Child_process child;
    child.terminate(259);
    require(sintra::probe_process_identity({child.pid(), child.stamp()}).status == Process_identity_status::DEAD,
        "a retained signaled process with exit code 259 must be DEAD");
}

void absent_process()
{
    Child_process child;
    child.terminate(0);
    child.close();
    // This mode runs without the test harness: its process-tree monitor retains
    // descendant handles. Verify the absence precondition before probing Sintra.
    bool absent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        HANDLE remaining = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child.pid());
        const DWORD error = ::GetLastError();
        if (remaining) {
            ::CloseHandle(remaining);
        }
        absent = !remaining && error == ERROR_INVALID_PARAMETER;
        std::this_thread::yield();
    }
    while (!absent && std::chrono::steady_clock::now() < deadline);
    require(absent,
        "absence case requires every parent, duplicate and harness handle closed");
    require(sintra::probe_process_identity({child.pid(), child.stamp()}).status == Process_identity_status::DEAD,
        "confirmed absent valid process identity must be DEAD");
}

DWORD injected_error = ERROR_ACCESS_DENIED;
HANDLE observed_handle = nullptr;

HANDLE WINAPI fail_open(DWORD access, BOOL inherit, DWORD)
{
    require(access == (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION) && !inherit,
        "identity open must request wait and creation-time rights without inheritance");
    ::SetLastError(injected_error);
    return nullptr;
}

BOOL WINAPI fail_query(HANDLE process, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME)
{
    observed_handle = process;
    ::SetLastError(injected_error);
    return FALSE;
}

DWORD WINAPI fail_wait(HANDLE process, DWORD timeout)
{
    require(timeout == 0, "identity observation must never block in the kernel");
    observed_handle = process;
    ::SetLastError(injected_error);
    return WAIT_FAILED;
}

void native_errors()
{
    namespace detail = sintra::detail;
    const sintra::process_incarnation_t owner{sintra::get_current_pid(), own_stamp()};
    detail::process_identity_open_process = fail_open;
    for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_INVALID_HANDLE}) {
        injected_error = error;
        const auto result = sintra::probe_process_identity(owner);
        require(result.status == Process_identity_status::UNKNOWN &&
            result.error == std::error_code(error, std::system_category()),
            "open failures must preserve the native UNKNOWN error");
    }
    injected_error = ERROR_INVALID_PARAMETER;
    require(sintra::probe_process_identity(owner).status == Process_identity_status::DEAD,
        "the valid-PID native absence error must classify DEAD");
    detail::process_identity_open_process = ::OpenProcess;

    injected_error = ERROR_ACCESS_DENIED;
    detail::process_identity_get_process_times = fail_query;
    const auto query = sintra::probe_process_identity(owner);
    detail::process_identity_get_process_times = ::GetProcessTimes;
    DWORD flags = 0;
    require(!::GetHandleInformation(observed_handle, &flags) && ::GetLastError() == ERROR_INVALID_HANDLE,
        "query failure must close its held process handle");
    require(query.status == Process_identity_status::UNKNOWN && query.error.value() == ERROR_ACCESS_DENIED,
        "query failure must preserve UNKNOWN and native error");

    injected_error = ERROR_INVALID_HANDLE;
    detail::process_identity_wait_for_single_object = fail_wait;
    const auto wait = sintra::probe_process_identity(owner);
    detail::process_identity_wait_for_single_object = ::WaitForSingleObject;
    require(!::GetHandleInformation(observed_handle, &flags) && ::GetLastError() == ERROR_INVALID_HANDLE,
        "wait failure must close its held process handle");
    require(wait.status == Process_identity_status::UNKNOWN && wait.error.value() == ERROR_INVALID_HANDLE,
        "wait failure must preserve UNKNOWN and native error");
    require(sintra::probe_process_identity(owner).status == Process_identity_status::LIVE,
        "UNKNOWN must not be cached as a durable liveness result");
}
#else
// Forks a child that exits at once; it stays unreaped until reap() is called.
struct exited_child_t
{
    pid_t    pid   = 0;
    uint64_t stamp = 0;
};

exited_child_t exited_unreaped_child()
{
    int release_pipe[2];
    require(::pipe(release_pipe) == 0, "pipe failed");
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::close(release_pipe[1]);
        char release = 0;
        while (::read(release_pipe[0], &release, 1) < 0 && errno == EINTR) {}
        ::_exit(0);
    }
    ::close(release_pipe[0]);
    const auto stamp = sintra::query_process_start_stamp(static_cast<uint32_t>(child));
    ::close(release_pipe[1]);
    siginfo_t info{};
    int waited;
    do {
        waited = ::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT);
    }
    while (waited < 0 && errno == EINTR);
    require(stamp && *stamp != 0, "child incarnation missing");
    require(waited == 0 && info.si_pid == child, "exited child must remain a zombie");
    return {child, *stamp};
}

void reap(pid_t child)
{
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

// The child's published incarnation: this process's namespaces, which it shares.
sintra::process_incarnation_t incarnation_of(const exited_child_t& child)
{
    auto owner = own_incarnation();
    owner.pid = static_cast<uint32_t>(child.pid);
    owner.start_stamp = child.stamp;
    return owner;
}

#if defined(__linux__)
sintra::process_namespaces_t unavailable_namespaces(sintra::Process_metadata_state state)
{
    return {{state, 0, 0}, {state, 0, 0}};
}

// Makes this process's namespace metadata and procfs view absent, as with
// CONFIG_PID_NS=n, CONFIG_TIME_NS=n and no NStgid, or unreadable.
void make_observer_metadata(sintra::Process_metadata_state state)
{
    fakes::s_namespaces = unavailable_namespaces(state);
    if (state == sintra::Process_metadata_state::ABSENT) {
        fakes::use_status_nstgid("");
    }
    else {
        fakes::s_status_error = EACCES;
    }
}

void self_resolving_stamp()
{
    const auto native = sintra::current_process_start_stamp();
    fakes::Scoped_fakes injected;
    // A procfs view in which this PID names another process, or no process.
    fakes::s_hide_process_records = true;
    const auto self = sintra::current_process_start_stamp();
    require(native && self && *self == *native,
        "the reader's stamp must come from its self-resolving process record");
}

void hidden_record_observations()
{
    const auto owner = own_incarnation();
    fakes::Scoped_fakes injected;
    fakes::s_hide_process_records = true;
    fakes::s_pidfd_result = fakes::k_succeed;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ENOENT,
        "a hidden record of a process that pidfd finds must be UNKNOWN with the record's error");
    fakes::s_pidfd_result = EPERM;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EPERM,
        "a failed pidfd lookup must report its own error");
    fakes::s_pidfd_result = ENOSYS;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ENOENT,
        "a hidden record of a process that a signal lookup finds must be UNKNOWN with the record's error");
    fakes::s_kill_result = EPERM;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EPERM,
        "a denied signal lookup must report its own error, not the record's");

    // Absence comes from native lookup alone: namespace metadata, even when
    // it contradicts, never vetoes it.
    auto foreign = owner;
    foreign.namespaces = {fakes::valid_namespace(0x11), fakes::valid_namespace(0x21)};
    fakes::s_namespaces = sintra::process_namespaces_t{fakes::valid_namespace(0x12), fakes::valid_namespace(0x22)};
    fakes::use_status_nstgid("4321\t17");
    fakes::s_kill_result = ESRCH;
    require_result(sintra::probe_process_identity(foreign), Process_identity_status::DEAD, 0,
        "signal ESRCH after pidfd ENOSYS must confirm absence");
    fakes::s_pidfd_result = ESRCH;
    fakes::s_kill_result = fakes::k_native;
    require_result(sintra::probe_process_identity(foreign), Process_identity_status::DEAD, 0,
        "pidfd ESRCH must confirm absence");
}

void record_coordinates()
{
    // This PID's record shows another incarnation than the published one.
    auto owner = own_incarnation();
    owner.start_stamp += 1;
    const sintra::process_namespaces_t published{fakes::valid_namespace(0x11), fakes::valid_namespace(0x21)};
    owner.namespaces = published;
    fakes::Scoped_fakes injected;
    fakes::s_namespaces = published;
    fakes::use_status_nstgid("4321");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a record read in the published coordinates must prove another incarnation");

    const int foreign = sintra::detail::k_foreign_process_record_error;
    fakes::s_namespaces = sintra::process_namespaces_t{fakes::valid_namespace(0x12), published.time};
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, foreign,
        "a record read from another PID namespace must be UNKNOWN");
    fakes::s_namespaces = sintra::process_namespaces_t{published.pid, fakes::valid_namespace(0x22)};
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, foreign,
        "a stamp read with another time-namespace offset must be UNKNOWN");
    fakes::s_namespaces = published;
    fakes::use_status_nstgid("4321\t17");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, foreign,
        "a record read through an ancestor namespace's procfs must be UNKNOWN");

    fakes::use_status_nstgid("");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a kernel without NStgid must not veto record evidence");
    fakes::s_status_error = EACCES;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "an unreadable status must not veto record evidence");
    fakes::s_status_error = 0;
    fakes::use_status_nstgid("4321");
    for (const auto state : {sintra::Process_metadata_state::ABSENT, sintra::Process_metadata_state::UNKNOWN}) {
        fakes::s_namespaces = unavailable_namespaces(state);
        require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
            "absent or unknown observer namespaces must not veto record evidence");
        fakes::s_namespaces = published;
        owner.namespaces = unavailable_namespaces(state);
        require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
            "absent or unknown published namespaces must not veto record evidence");
        owner.namespaces = published;
    }
}

// A process can join another time namespace between reader lifetimes. Both
// capture and classification must use the current one, never a cached one.
void current_observer_context()
{
    fakes::Scoped_fakes injected;
    fakes::use_status_nstgid("4321");
    auto namespaces = sintra::detail::current_linux_namespaces();
    namespaces.time = fakes::valid_namespace(0xa);
    fakes::s_namespaces = namespaces;
    const auto in_a = own_incarnation();
    auto owner = in_a;
    owner.start_stamp += 1;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "an observer in time namespace A must classify a record published in A");

    namespaces.time = fakes::valid_namespace(0xb);
    fakes::s_namespaces = namespaces;
    owner = own_incarnation();
    require(in_a.namespaces.time.inode == 0xa && owner.namespaces.time.inode == 0xb,
        "each capture must publish the current time namespace");
    owner.start_stamp += 1;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "an observer that joined time namespace B must classify a record published in B");

    namespaces.time = fakes::valid_namespace(0xa);
    fakes::s_namespaces = namespaces;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN,
        sintra::detail::k_foreign_process_record_error,
        "an observer back in time namespace A must not use a record published in B");
}

void exited_process()
{
    const auto child = exited_unreaped_child();
    auto owner = incarnation_of(child);
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "an unreaped exited process must be DEAD from its terminal record");
    {
        fakes::Scoped_fakes injected;
        for (const auto state : {sintra::Process_metadata_state::ABSENT, sintra::Process_metadata_state::UNKNOWN}) {
            auto unavailable = owner;
            unavailable.namespaces = unavailable_namespaces(state);
            make_observer_metadata(state);
            require_result(sintra::probe_process_identity(unavailable), Process_identity_status::DEAD, 0,
                "a terminal record must prove death without namespace metadata");
            fakes::s_status_error = 0;
        }
        fakes::use_status_nstgid("4321");
        auto foreign = owner;
        foreign.namespaces.time = fakes::valid_namespace(0x21);
        fakes::s_namespaces = sintra::process_namespaces_t{owner.namespaces.pid, fakes::valid_namespace(0x22)};
        require_result(sintra::probe_process_identity(foreign), Process_identity_status::UNKNOWN,
            sintra::detail::k_foreign_process_record_error,
            "a terminal record read with another time-namespace offset is not death evidence");
    }

    reap(child.pid);
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a reaped process must be DEAD");
    fakes::Scoped_fakes injected;
    fakes::s_pidfd_result = ENOSYS;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "without pidfd, signal lookup must confirm a reaped process's absence");
    for (const auto state : {sintra::Process_metadata_state::ABSENT, sintra::Process_metadata_state::UNKNOWN}) {
        auto unavailable = owner;
        unavailable.namespaces = unavailable_namespaces(state);
        make_observer_metadata(state);
        require_result(sintra::probe_process_identity(unavailable), Process_identity_status::DEAD, 0,
            "absence without pidfd must not depend on namespace metadata");
        fakes::s_status_error = 0;
    }
}

void live_thread_after_leader_exit()
{
    int release_pipe[2];
    require(::pipe(release_pipe) == 0, "pipe failed");
    const pid_t child = ::fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        ::close(release_pipe[1]);
        std::thread([fd = release_pipe[0]]() {
            char release = 0;
            while (::read(fd, &release, 1) < 0 && errno == EINTR) {}
            ::_exit(0);
        }).detach();
        ::pthread_exit(nullptr);
    }
    ::close(release_pipe[0]);
    const auto stamp = sintra::query_process_start_stamp(child);
    sintra::detail::linux_process_stat_t record;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        std::ifstream stat_file("/proc/" + std::to_string(child) + "/stat");
        std::string line;
        std::getline(stat_file, line);
        if (sintra::detail::parse_linux_process_stat(line, record) && record.state == 'Z') {
            break;
        }
        std::this_thread::yield();
    }
    while (std::chrono::steady_clock::now() < deadline);
    auto owner = own_incarnation();
    owner.pid = static_cast<uint32_t>(child);
    owner.start_stamp = stamp.value_or(0);
    const auto live = sintra::probe_process_identity(owner);
    ::close(release_pipe[1]);
    reap(child);
    require(record.state == 'Z' && record.num_threads > 1,
        "test must observe a zombie leader with another running thread");
    require(live.status == Process_identity_status::LIVE,
        "exact-identity probe must retain a zombie leader's live worker threads");
    require(sintra::probe_process_identity(owner).status == Process_identity_status::DEAD,
        "process becomes DEAD once its last thread exits and it is reaped");
}
#else
void native_observations()
{
    const auto owner = own_incarnation();
    fakes::Scoped_fakes injected;
    fakes::s_record = fakes::Record::FAILED;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ESRCH,
        "a missing record of a process that a signal lookup finds must be UNKNOWN");
#if defined(__APPLE__)
    require(fakes::s_last_argument == 1, "proc_pidinfo must request zombie-inclusive records");
#endif
    fakes::s_kill_result = EPERM;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EPERM,
        "a denied signal lookup must report its own error, not the record's");
    fakes::s_kill_result = ESRCH;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a missing record confirmed by signal ESRCH must be DEAD");
#if defined(__FreeBSD__)
    fakes::s_kill_result = ECAPMODE;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ECAPMODE,
        "a signal lookup refused in capability mode must be UNKNOWN");
    fakes::s_record = fakes::Record::EMPTY;
    fakes::s_kill_result = fakes::k_native;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ESRCH,
        "an empty record of a process that a signal lookup finds must be UNKNOWN");
    fakes::s_kill_result = ESRCH;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a successful empty result must reach absence confirmation");
    fakes::s_record = fakes::Record::FAILED;
    fakes::s_record_error = ECAPMODE;
    fakes::s_kill_result = fakes::k_native;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, ECAPMODE,
        "a record lookup refused in capability mode must be UNKNOWN");
#else
    fakes::s_record = fakes::Record::EMPTY;
    fakes::s_kill_result = fakes::k_native;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EIO,
        "an empty proc_pidinfo result without an error must be UNKNOWN");
    fakes::s_record = fakes::Record::FAILED;
#endif
    fakes::s_record_error = EPERM;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EPERM,
        "a denied record lookup must be UNKNOWN");
    fakes::s_record = fakes::Record::SHORT;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EIO,
        "a short nonempty record must be UNKNOWN");
}

void exited_process()
{
    const auto child = exited_unreaped_child();
    const auto owner = incarnation_of(child);
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "an unreaped exited process must be DEAD from its terminal record");
#if defined(__FreeBSD__)
    {
        fakes::Scoped_fakes injected;
        fakes::s_moving_boot_time_lookups = fakes::k_every_lookup;
        require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
            "a terminal record must be DEAD without an established start stamp");
    }
    auto reused = owner;
    reused.start_stamp += 1000;
    require_result(sintra::probe_process_identity(reused), Process_identity_status::DEAD, 0,
        "a terminal record must be DEAD whatever start stamp it shows");
#endif
    reap(child.pid);
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "a reaped process must be DEAD once signal lookup confirms its absence");
#if defined(__FreeBSD__)
    require_result(sintra::probe_process_identity(reused), Process_identity_status::DEAD, 0,
        "a missing record confirmed by signal ESRCH must be DEAD whatever stamp was published");
    fakes::Scoped_fakes injected;
    fakes::s_moving_boot_time_lookups = fakes::k_every_lookup;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::DEAD, 0,
        "absence must be DEAD without an established start stamp");
#endif
}

#if defined(__FreeBSD__)
// A wall-clock step moves kern.boottime and the ki_start of every process by
// the same amount; the start stamp, the uptime at fork, must not move.
void clock_step()
{
    const auto owner = own_incarnation();
    const int name[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(owner.pid)};
    struct kinfo_proc native{};
    size_t native_size = sizeof(native);
    require(::sysctl(name, 4, &native, &native_size, nullptr, 0) == 0 && native_size == sizeof(native),
        "native process record missing");

    fakes::Scoped_fakes injected;
    fakes::s_clock_step = 3600;
    struct kinfo_proc stepped{};
    size_t stepped_size = sizeof(stepped);
    require(sintra::detail::process_identity_sysctl(name, 4, &stepped, &stepped_size, nullptr, 0) == 0 &&
        stepped.ki_start.tv_sec == native.ki_start.tv_sec + 3600,
        "the simulated clock step must move the reported ki_start");
    require(sintra::query_process_start_stamp(owner.pid) == owner.start_stamp,
        "a clock step must not change a live process's start stamp");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::LIVE, 0,
        "a live process must stay LIVE across a clock step");
    auto reused = owner;
    reused.start_stamp += 1000;
    require_result(sintra::probe_process_identity(reused), Process_identity_status::UNKNOWN,
        sintra::detail::k_start_stamp_mismatch_error,
        "a live record whose uptime-based stamp differs must be UNKNOWN across a clock step");

    fakes::s_moving_boot_time_lookups = fakes::k_every_lookup;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EAGAIN,
        "a boot time that moves during every attempt must leave a live process UNKNOWN");
    require_result(sintra::probe_process_identity(reused), Process_identity_status::UNKNOWN, EAGAIN,
        "a start stamp that cannot be established must never prove a different incarnation");
    require(!sintra::query_process_start_stamp(owner.pid) && !sintra::current_process_incarnation(),
        "a start stamp that cannot be established must be unavailable for capture");
}

// Records filled under a boot time one hour later than the one reported
// around them: a change reversed between the lookups.
constexpr time_t k_reversed_change_s = 3600;
constexpr uint64_t k_reversed_change_ns = uint64_t(k_reversed_change_s) * 1'000'000'000;

void boot_time_change_during_observation()
{
    const auto owner = own_incarnation();
    fakes::Scoped_fakes injected;
    fakes::s_record_boot_time_shift = k_reversed_change_s;
    require(sintra::query_process_start_stamp(owner.pid) == owner.start_stamp + k_reversed_change_ns,
        "equal boot-time readings must not detect a change reversed between them");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN,
        sintra::detail::k_start_stamp_mismatch_error,
        "a live process observed through a reversed boot-time change must be UNKNOWN, never DEAD");
    fakes::s_record_boot_time_shift = 0;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::LIVE, 0,
        "a later stable observation must find the live process LIVE");
}

void boot_time_change_during_capture()
{
    std::optional<sintra::process_incarnation_t> captured;
    {
        fakes::Scoped_fakes injected;
        fakes::s_record_boot_time_shift = k_reversed_change_s;
        captured = sintra::current_process_incarnation();
    }
    require(captured && captured->start_stamp == own_stamp() + k_reversed_change_ns,
        "a boot-time change reversed during capture must skew the published stamp");
    for (int observation = 0; observation < 3; ++observation) {
        require_result(sintra::probe_process_identity(*captured), Process_identity_status::UNKNOWN,
            sintra::detail::k_start_stamp_mismatch_error,
            "stable observations of a live process with a skewed stamp must stay UNKNOWN, never DEAD");
    }
}

void boot_time_settles_on_retry()
{
    const auto owner = own_incarnation();
    const int attempts = sintra::detail::k_freebsd_boot_time_attempts;
    fakes::Scoped_fakes injected;
    // Each attempt reads the boot time twice, so only the last attempt is stable.
    fakes::s_moving_boot_time_lookups = 2 * (attempts - 1);
    require_result(sintra::probe_process_identity(owner), Process_identity_status::LIVE, 0,
        "a boot time that settles by the last attempt must establish the start stamp");
    require(fakes::s_moving_boot_time_lookups == 0 && fakes::s_boot_time_moves == 2 * (attempts - 1),
        "every attempt before the last must have seen the boot time move");

    fakes::s_moving_boot_time_lookups = 2 * attempts;
    require_result(sintra::probe_process_identity(owner), Process_identity_status::UNKNOWN, EAGAIN,
        "a boot time that moves during every attempt must leave the stamp unavailable");
    require(sintra::query_process_start_stamp(owner.pid) == owner.start_stamp,
        "a boot time that settles before a later lookup must yield the original stamp");
    require_result(sintra::probe_process_identity(owner), Process_identity_status::LIVE, 0,
        "a later observation after the boot time settles must be LIVE");
}

// The stamp is ki_start minus the boot time. Boot-time microseconds beyond
// those of ki_start borrow a second.
void fractional_second_borrow()
{
    const uint32_t pid = sintra::get_current_pid();
    constexpr uint64_t k_borrowed_stamp = 499'200'000'000;
    fakes::Scoped_fakes injected;
    fakes::s_boot_time = timeval{1790000000, 900000};
    fakes::s_start_time = timeval{1790000500, 100000};
    require(sintra::query_process_start_stamp(pid) == k_borrowed_stamp,
        "a boot time with more microseconds than ki_start must borrow a second");
    require_result(sintra::probe_process_identity({pid, k_borrowed_stamp, {}}), Process_identity_status::LIVE, 0,
        "a borrowed stamp must match the published one");
    fakes::s_start_time = timeval{1790000500, 950000};
    require(sintra::query_process_start_stamp(pid) == 500'050'000'000,
        "a boot time with fewer microseconds than ki_start must not borrow");
}

// Absence needs no start stamp, whatever stamp was published.
void absence_with_other_stamp()
{
    auto reused = own_incarnation();
    reused.start_stamp += 1000;
    fakes::Scoped_fakes injected;
    fakes::s_kill_result = ESRCH;
    for (const auto record : {fakes::Record::FAILED, fakes::Record::EMPTY}) {
        fakes::s_record = record;
        require_result(sintra::probe_process_identity(reused), Process_identity_status::DEAD, 0,
            "a missing record confirmed by signal ESRCH must be DEAD whatever stamp was published");
    }
}
#endif
#endif
#endif

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    if (argc == 2 && std::strcmp(argv[1], "--identity-child") == 0) {
        ::Sleep(INFINITE);
        return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--absence-only") == 0) {
        try {
            absent_process();
            std::puts("PASS absent_process: all handles closed before identity probe");
            return 0;
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL absent_process: %s\n", error.what());
            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif
    struct test_t { const char* name; void (*run)(); };
    const test_t tests[] = {
        {"live_identity", live_identity},
        {"mismatched_identity", mismatched_identity},
        {"invalid_identity", invalid_identity},
#ifdef _WIN32
        {"retained_exit_259", retained_exit_259},
        {"native_errors", native_errors},
#else
        {"exited_process", exited_process},
#if defined(__linux__)
        {"self_resolving_stamp", self_resolving_stamp},
        {"hidden_record_observations", hidden_record_observations},
        {"record_coordinates", record_coordinates},
        {"current_observer_context", current_observer_context},
        {"live_thread_after_leader_exit", live_thread_after_leader_exit},
#else
        {"native_observations", native_observations},
#if defined(__FreeBSD__)
        {"clock_step", clock_step},
        {"boot_time_change_during_observation", boot_time_change_during_observation},
        {"boot_time_change_during_capture", boot_time_change_during_capture},
        {"boot_time_settles_on_retry", boot_time_settles_on_retry},
        {"fractional_second_borrow", fractional_second_borrow},
        {"absence_with_other_stamp", absence_with_other_stamp},
#endif
#endif
#endif
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::printf("PASS %s\n", test.name);
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
            ++failed;
        }
    }
    return failed ? 1 : 0;
}
