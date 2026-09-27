// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/detail/ipc/process_utils.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

using sintra::Process_identity_status;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

uint64_t own_stamp()
{
    const auto stamp = sintra::current_process_start_stamp();
    require(stamp && *stamp != 0, "current process must have a nonzero creation stamp");
    return *stamp;
}

void live_identity()
{
    const auto result = sintra::probe_process_identity(sintra::get_current_pid(), own_stamp());
    require(result.status == Process_identity_status::LIVE && !result.error,
        "matching running incarnation must be LIVE without an error");
}

void mismatched_identity()
{
    const auto result = sintra::probe_process_identity(sintra::get_current_pid(), own_stamp() + 1);
    require(result.status == Process_identity_status::DEAD && !result.error,
        "a different incarnation must be DEAD even while that PID is live");
}

void invalid_identity()
{
    const auto missing_pid = sintra::probe_process_identity(0, own_stamp());
    const auto missing_stamp = sintra::probe_process_identity(sintra::get_current_pid(), 0);
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
    require(sintra::probe_process_identity(child.pid(), child.stamp()).status == Process_identity_status::DEAD,
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
    require(sintra::probe_process_identity(child.pid(), child.stamp()).status == Process_identity_status::DEAD,
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
    const auto pid = sintra::get_current_pid();
    const auto stamp = own_stamp();
    detail::process_identity_open_process = fail_open;
    for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_INVALID_HANDLE}) {
        injected_error = error;
        const auto result = sintra::probe_process_identity(pid, stamp);
        require(result.status == Process_identity_status::UNKNOWN &&
            result.error == std::error_code(error, std::system_category()),
            "open failures must preserve the native UNKNOWN error");
    }
    injected_error = ERROR_INVALID_PARAMETER;
    require(sintra::probe_process_identity(pid, stamp).status == Process_identity_status::DEAD,
        "the valid-PID native absence error must classify DEAD");
    detail::process_identity_open_process = ::OpenProcess;

    injected_error = ERROR_ACCESS_DENIED;
    detail::process_identity_get_process_times = fail_query;
    const auto query = sintra::probe_process_identity(pid, stamp);
    detail::process_identity_get_process_times = ::GetProcessTimes;
    DWORD flags = 0;
    require(!::GetHandleInformation(observed_handle, &flags) && ::GetLastError() == ERROR_INVALID_HANDLE,
        "query failure must close its held process handle");
    require(query.status == Process_identity_status::UNKNOWN && query.error.value() == ERROR_ACCESS_DENIED,
        "query failure must preserve UNKNOWN and native error");

    injected_error = ERROR_INVALID_HANDLE;
    detail::process_identity_wait_for_single_object = fail_wait;
    const auto wait = sintra::probe_process_identity(pid, stamp);
    detail::process_identity_wait_for_single_object = ::WaitForSingleObject;
    require(!::GetHandleInformation(observed_handle, &flags) && ::GetLastError() == ERROR_INVALID_HANDLE,
        "wait failure must close its held process handle");
    require(wait.status == Process_identity_status::UNKNOWN && wait.error.value() == ERROR_INVALID_HANDLE,
        "wait failure must preserve UNKNOWN and native error");
    require(sintra::probe_process_identity(pid, stamp).status == Process_identity_status::LIVE,
        "UNKNOWN must not be cached as a durable liveness result");
}
#else
#if defined(__linux__)
int hidden_stat_open(const char*, int, ...)
{
    errno = ENOENT;
    return -1;
}

int unavailable_pidfd(pid_t)
{
    errno = ENOSYS;
    return -1;
}

void native_visibility_errors()
{
    const auto pid = sintra::get_current_pid();
    const auto stamp = own_stamp();
    sintra::detail::process_identity_open_stat = hidden_stat_open;
    const auto hidden = sintra::probe_process_identity(pid, stamp);
    sintra::detail::process_identity_pidfd_open = unavailable_pidfd;
    const auto unavailable = sintra::probe_process_identity(pid, stamp);
    sintra::detail::process_identity_pidfd_open = sintra::detail::open_process_pidfd;
    sintra::detail::process_identity_open_stat = ::open;
    require(hidden.status == Process_identity_status::UNKNOWN &&
        (hidden.error.value() == ENOENT || hidden.error.value() == ENOSYS),
        "a hidden proc record cannot establish process death with or without pidfd support");
    require(unavailable.status == Process_identity_status::UNKNOWN && unavailable.error.value() == ENOSYS,
        "an unavailable pidfd syscall cannot establish process death");
}
#elif defined(__APPLE__)
int hidden_proc_pidinfo(int, int, uint64_t, void*, int)
{
    errno = ESRCH;
    return 0;
}

void native_visibility_errors()
{
    sintra::detail::process_identity_proc_pidinfo = hidden_proc_pidinfo;
    const auto result = sintra::probe_process_identity(sintra::get_current_pid(), own_stamp());
    sintra::detail::process_identity_proc_pidinfo = ::proc_pidinfo;
    require(result.status == Process_identity_status::UNKNOWN && result.error.value() == ESRCH,
        "proc_pidinfo ESRCH cannot establish death while kill still sees the PID");
}
#elif defined(__FreeBSD__)
bool empty_process_record = false;

int hidden_process_sysctl(const int*, u_int, void*, size_t* size, const void*, size_t)
{
    if (empty_process_record) {
        *size = 0;
        return 0;
    }
    errno = ESRCH;
    return -1;
}

void native_visibility_errors()
{
    sintra::detail::process_identity_sysctl = hidden_process_sysctl;
    const auto pid = sintra::get_current_pid();
    const auto stamp = own_stamp();
    const auto hidden = sintra::probe_process_identity(pid, stamp);
    empty_process_record = true;
    const auto empty = sintra::probe_process_identity(pid, stamp);
    empty_process_record = false;
    sintra::detail::process_identity_sysctl = ::sysctl;
    require(hidden.status == Process_identity_status::UNKNOWN && hidden.error.value() == ESRCH,
        "FreeBSD visibility ESRCH cannot establish process death");
    require(empty.status == Process_identity_status::UNKNOWN && empty.error.value() == EIO,
        "FreeBSD empty process record cannot establish process death");
}
#endif

void require_reaped_identity(pid_t child, uint64_t stamp)
{
    const auto observed = sintra::probe_process_identity(child, stamp);
#if defined(__FreeBSD__)
    require(observed.status == Process_identity_status::UNKNOWN && observed.error,
        "FreeBSD cannot distinguish a reaped process from policy concealment");
#elif defined(__linux__)
    const int pidfd = sintra::detail::open_process_pidfd(::getpid());
    const int pidfd_error = errno;
    if (pidfd >= 0) {
        ::close(pidfd);
        require(observed.status == Process_identity_status::DEAD,
            "pidfd-confirmed absence of a reaped process must be DEAD");
    }
    else {
        require(observed.status == Process_identity_status::UNKNOWN &&
            observed.error.value() == pidfd_error,
            "unavailable pidfd cannot establish the absence of a reaped process");
    }
#else
    require(observed.status == Process_identity_status::DEAD,
        "a reaped process must be DEAD when native absence can be confirmed");
#endif
}

void exited_process()
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
    const auto stamp = sintra::query_process_start_stamp(child);
    ::close(release_pipe[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    require(stamp && *stamp != 0, "child incarnation missing");
    require_reaped_identity(child, *stamp);
}

#if defined(__linux__)
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
    const auto live = sintra::probe_process_identity(child, stamp.value_or(0));
    ::close(release_pipe[1]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    require(record.state == 'Z' && record.num_threads > 1,
        "test must observe a zombie leader with another running thread");
    require(live.status == Process_identity_status::LIVE,
        "exact-identity probe must retain a zombie leader's live worker threads");
    require_reaped_identity(child, *stamp);
}
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
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        {"native_visibility_errors", native_visibility_errors},
#endif
        {"exited_process", exited_process},
#if defined(__linux__)
        {"live_thread_after_leader_exit", live_thread_after_leader_exit},
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
