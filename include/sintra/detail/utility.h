// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "config.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>


#ifdef _WIN32
    #include "sintra_windows.h"
    #include <process.h>
    #include <errno.h>
    #include <cerrno>
#else
    #include <atomic>
    #include <cerrno>
    #include <cstdio>
    #include <cstring>
    #include <signal.h>
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif


namespace sintra {


using std::function;
using std::shared_ptr;
using std::mutex;
using std::lock_guard;

struct Adaptive_function
{
    // Single shared state struct instead of two separate shared_ptrs.
    // Preserves the same semantics: copies share state, all operations are
    // mutex-protected, and set() replaces the function for all copies.
    struct State {
        function<void()> func;
        mutex m;
    };

    Adaptive_function(function<void()> f) :
        state(std::make_shared<State>())
    {
        state->func = std::move(f);
    }

    Adaptive_function(const Adaptive_function& rhs)
    {
        lock_guard<mutex> lock(rhs.state->m);
        state = rhs.state;
    }

    Adaptive_function& operator=(const Adaptive_function& rhs)
    {
        if (this == &rhs || state == rhs.state) {
            return *this;
        }

        auto old_state = state;
        auto new_state = rhs.state;
        std::scoped_lock lock(old_state->m, new_state->m);
        state = std::move(new_state);
        return *this;
    }

    void operator()()
    {
        lock_guard<mutex> lock(state->m);
        if (state->func) {
            (state->func)();
        }
    }

    void set(function<void()> f)
    {
        lock_guard<mutex> lock(state->m);
        state->func = std::move(f);
    }

    shared_ptr<State> state;
};



inline
size_t get_cache_line_size()
{
#ifdef _WIN32

    size_t line_size = 0;
    DWORD buffer_size = 0;
    DWORD i = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION * buffer = 0;

    GetLogicalProcessorInformation(0, &buffer_size);
    buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(buffer_size);
    GetLogicalProcessorInformation(&buffer[0], &buffer_size);

    for (i = 0; i != buffer_size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
        // assuming all cache levels have the same line size...
        if (buffer[i].Relationship == RelationCache && buffer[i].Cache.Level == 1) {
            line_size = buffer[i].Cache.LineSize;
            break;
        }
    }

    free(buffer);
    return line_size;

#else

    // Return the assumed value. Portable runtime detection of cache line size
    // is complex and not worth the effort for a debug-only validation check.
    return assumed_cache_line_size;

#endif
}



// C++ vector of strings to C style null terminated array of pointers
// conversion utility
struct cstring_vector
{
    explicit cstring_vector(const std::vector<std::string>& v_in)
        : m_storage(v_in)
    {
        initialize();
    }

    explicit cstring_vector(std::vector<std::string>&& v_in)
        : m_storage(std::move(v_in))
    {
        initialize();
    }

    ~cstring_vector()
    {
        delete [] m_v;
    }

    cstring_vector(const cstring_vector&) = delete;
    cstring_vector& operator=(const cstring_vector&) = delete;
    cstring_vector(cstring_vector&&) = delete;
    cstring_vector& operator=(cstring_vector&&) = delete;

    const char* const* v() const { return m_v; }
    size_t size() const { return m_storage.size(); }

private:
    void initialize()
    {
        const auto count = m_storage.size();
        m_v = new const char*[count + 1];
        for (size_t i = 0; i < count; ++i) {
            m_v[i] = m_storage[i].c_str();
        }
        m_v[count] = nullptr;
    }

    std::vector<std::string> m_storage;
    const char** m_v = nullptr;
};



namespace detail {

#ifndef _WIN32

using pipe2_fn = int(*)(int[2], int);
using write_fn = ssize_t(*)(int, const void*, size_t);
using read_fn = ssize_t(*)(int, void*, size_t);
using waitpid_fn = pid_t(*)(pid_t, int*, int);
using spawn_detached_debug_fn = void(*)(const struct spawn_detached_debug_info&);

struct spawn_detached_debug_info
{
    enum class Stage {
        PipeCreation,
        Fork,
        ChildReadyPipeWrite,
        ParentReadReadyStatus,
        ParentReadExecStatus,
        ParentWaitpid,
    };

    Stage stage{Stage::PipeCreation};
    int errno_value{0};
    int exec_errno{0};
};

inline std::atomic<pipe2_fn>& pipe2_override()
{
    static std::atomic<pipe2_fn> fn{nullptr};
    return fn;
}

inline std::atomic<write_fn>& write_override()
{
    static std::atomic<write_fn> fn{nullptr};
    return fn;
}

inline std::atomic<read_fn>& read_override()
{
    static std::atomic<read_fn> fn{nullptr};
    return fn;
}

inline std::atomic<waitpid_fn>& waitpid_override()
{
    static std::atomic<waitpid_fn> fn{nullptr};
    return fn;
}

inline std::atomic<spawn_detached_debug_fn>& spawn_detached_debug_override()
{
    static std::atomic<spawn_detached_debug_fn> fn{nullptr};
    return fn;
}

inline void emit_spawn_detached_debug(const spawn_detached_debug_info& info)
{
    if (auto fn = spawn_detached_debug_override().load()) {
        fn(info);
    }
}

inline int fcntl_retry(int fd, int cmd)
{
    int rv = -1;
    do {
        rv = ::fcntl(fd, cmd);
    }
    while (rv == -1 && errno == EINTR);
    return rv;
}

inline int fcntl_retry(int fd, int cmd, int arg)
{
    int rv = -1;
    do {
        rv = ::fcntl(fd, cmd, arg);
    }
    while (rv == -1 && errno == EINTR);
    return rv;
}

inline int system_pipe2(int pipefd[2], int flags)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
    int rv = -1;
    do {
        rv = ::pipe2(pipefd, flags);
    }
    while (rv == -1 && errno == EINTR);
    return rv;
#else
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }

    int pipe_result = -1;
    do {
        pipe_result = ::pipe(pipefd);
    }
    while (pipe_result == -1 && errno == EINTR);
    if (pipe_result == -1) {
        return -1;
    }

    const auto set_flag = [&](int fd, int get_cmd, int set_cmd, int value) {
        int current = fcntl_retry(fd, get_cmd);
        if (current == -1) {
            return -1;
        }
        return fcntl_retry(fd, set_cmd, current | value);
    };

    if (flags & O_CLOEXEC) {
        if (set_flag(pipefd[0], F_GETFD, F_SETFD, FD_CLOEXEC) == -1 ||
            set_flag(pipefd[1], F_GETFD, F_SETFD, FD_CLOEXEC) == -1) {
            int saved_errno = errno;
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            errno = saved_errno;
            return -1;
        }
    }

    if (flags & O_NONBLOCK) {
        if (set_flag(pipefd[0], F_GETFL, F_SETFL, O_NONBLOCK) == -1 ||
            set_flag(pipefd[1], F_GETFL, F_SETFL, O_NONBLOCK) == -1) {
            int saved_errno = errno;
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            errno = saved_errno;
            return -1;
        }
    }

    return 0;
#endif
}

inline int call_pipe2(int pipefd[2], int flags)
{
    if (auto override = pipe2_override().load()) {
        return override(pipefd, flags);
    }
    return system_pipe2(pipefd, flags);
}

inline ssize_t call_write(int fd, const void* buf, size_t count)
{
    if (auto override = write_override().load()) {
        return override(fd, buf, count);
    }
    return ::write(fd, buf, count);
}

inline ssize_t call_read(int fd, void* buf, size_t count)
{
    if (auto override = read_override().load()) {
        return override(fd, buf, count);
    }
    return ::read(fd, buf, count);
}

inline pid_t call_waitpid(pid_t pid, int* status, int options)
{
    if (auto override = waitpid_override().load()) {
        return override(pid, status, options);
    }
    return ::waitpid(pid, status, options);
}

inline bool write_fully(int fd, const void* buf, size_t count)
{
    const char* ptr = static_cast<const char*>(buf);
    size_t total_written = 0;
    while (total_written < count) {
        ssize_t rv = call_write(fd, ptr + total_written, count - total_written);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rv == 0) {
            return false;
        }
        total_written += static_cast<size_t>(rv);
    }
    return true;
}

inline bool read_fully(int fd, void* buf, size_t count)
{
    char* ptr = static_cast<char*>(buf);
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t rv = call_read(fd, ptr + total_read, count - total_read);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rv == 0) {
            return false;
        }
        total_read += static_cast<size_t>(rv);
    }
    return true;
}

#endif // !_WIN32

} // namespace detail

#ifndef _WIN32

namespace testing {

inline detail::pipe2_fn set_pipe2_override(detail::pipe2_fn fn)
{
    return detail::pipe2_override().exchange(fn);
}

inline detail::write_fn set_write_override(detail::write_fn fn)
{
    return detail::write_override().exchange(fn);
}

inline detail::read_fn set_read_override(detail::read_fn fn)
{
    return detail::read_override().exchange(fn);
}

inline detail::waitpid_fn set_waitpid_override(detail::waitpid_fn fn)
{
    return detail::waitpid_override().exchange(fn);
}

inline detail::spawn_detached_debug_fn set_spawn_detached_debug(detail::spawn_detached_debug_fn fn)
{
    return detail::spawn_detached_debug_override().exchange(fn);
}

} // namespace testing

#endif // !_WIN32

inline
bool spawn_detached(const char* prog, const char * const*argv, int* child_pid_out = nullptr)
{

#ifdef _WIN32
    if (prog==nullptr || argv==nullptr) {
        return false;
    }

    // Note: Unlike Unix spawn_detached, Windows _spawnv expects argv[0] to be the program name.
    // The caller already includes the binary name in argv[0], so we use argv as-is.
    // We only need to resolve the full path for the program to execute.

    char full_path[_MAX_PATH];
    if (_fullpath(full_path, prog, _MAX_PATH) == nullptr) {
        if (child_pid_out) {
            *child_pid_out = -1;
        }
        return false;
    }

    // Convert UTF-8 string to wide string
    auto to_wide = [](const char* str) -> std::wstring {
        if (!str || !*str) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
        if (len <= 0) return std::wstring();
        std::wstring result(len - 1, 0); // -1 to exclude null terminator from size
        MultiByteToWideChar(CP_UTF8, 0, str, -1, &result[0], len);
        return result;
    };

    // Build command line from argv with proper escaping
    // Windows requires command line as single string with proper quoting
    // argv should already include program name at [0]
    auto build_command_line = [&to_wide](const char* const* argv) -> std::wstring {
        std::wstring cmdline;
        bool first = true;

        for (const char* const* arg = argv; *arg != nullptr; ++arg) {
            if (!first) cmdline += L' ';
            first = false;

            std::wstring ws = to_wide(*arg);
            // Quote argument if it contains space or is empty
            bool needs_quoting = ws.find(L' ') != std::wstring::npos || ws.empty();

            if (needs_quoting) cmdline += L'"';

            // Escape internal quotes and backslashes before quotes
            for (size_t i = 0; i < ws.length(); ++i) {
                wchar_t c = ws[i];
                if (c == L'"') {
                    cmdline += L"\\\"";
                } else if (c == L'\\') {
                    // Check if backslash is before quote
                    if (i + 1 < ws.length() && ws[i + 1] == L'"') {
                        cmdline += L"\\\\";
                    }
                    // Check if trailing backslash in quoted argument
                    else if (i + 1 == ws.length() && needs_quoting) {
                        cmdline += L"\\\\";
                    } else {
                        cmdline += L'\\';
                    }
                } else {
                    cmdline += c;
                }
            }

            if (needs_quoting) cmdline += L'"';
        }
        return cmdline;
    };

    // Convert full path to wide string for lpApplicationName
    std::wstring full_path_w = to_wide(full_path);

    // Build command line - argv already includes program name at [0] (Windows convention)
    std::wstring cmdline_str = build_command_line(argv);

    // Validate command line length (Windows limit is 32,767 characters)
    constexpr size_t k_max_command_line_length = 32767;
    if (cmdline_str.length() >= k_max_command_line_length) {
        if (child_pid_out) {
            *child_pid_out = -1;
        }
        errno = E2BIG;  // Argument list too long
        return false;
    }

    std::vector<wchar_t> cmdline_buf(cmdline_str.begin(), cmdline_str.end());
    cmdline_buf.push_back(L'\0'); // Null-terminate

    constexpr unsigned k_max_attempts = 5;
    const auto retry_delay = std::chrono::milliseconds(50);

    int last_errno = 0;
    unsigned long last_doserrno = 0;

    for (unsigned attempt = 0; attempt < k_max_attempts; ++attempt) {
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        // CREATE_NEW_PROCESS_GROUP: Like Unix setsid() - new process group for Ctrl-C isolation
        // Note: CREATE_BREAKAWAY_FROM_JOB removed - it requires special permissions and may fail
        DWORD creation_flags = CREATE_NEW_PROCESS_GROUP;

        BOOL success = CreateProcessW(
            full_path_w.c_str(),        // Application name - explicit resolved path (more secure)
            cmdline_buf.data(),         // Command line (mutable, includes program name at [0])
            nullptr,                    // Process security attributes
            nullptr,                    // Thread security attributes
            TRUE,                       // Inherit handles (for stdout/stderr)
            creation_flags,             // Creation flags
            nullptr,                    // Environment (inherit)
            nullptr,                    // Current directory (inherit)
            &si,                        // Startup info
            &pi                         // Process information
        );

        if (success) {
            if (child_pid_out) {
                *child_pid_out = static_cast<int>(pi.dwProcessId);
            }

            // Close handles - we don't need to hold them
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            return true;
        }

        // Handle failure
        last_doserrno = GetLastError();

        // Map Windows error to errno
        if (last_doserrno == ERROR_ACCESS_DENIED) {
            last_errno = EACCES;
        } else if (last_doserrno == ERROR_FILE_NOT_FOUND || last_doserrno == ERROR_PATH_NOT_FOUND) {
            last_errno = ENOENT;
        } else {
            last_errno = EAGAIN; // Generic transient error
        }

        const bool access_denied = (last_errno == EACCES) &&
            (last_doserrno == ERROR_ACCESS_DENIED ||
             last_doserrno == ERROR_SHARING_VIOLATION ||
             last_doserrno == ERROR_LOCK_VIOLATION);
        const bool transient = (last_errno == EAGAIN) || access_denied;

        if (attempt + 1 < k_max_attempts && transient) {
            std::this_thread::sleep_for(retry_delay);
            continue;
        }
        break;
    }

    if (child_pid_out) {
        *child_pid_out = -1;
    }
    if (last_errno != 0) {
        _set_errno(last_errno);
    }
    if (last_doserrno != 0) {
        _set_doserrno(last_doserrno);
    }
    return false;
#else

    // 1. we fork to obtain an inbetween process
    // 2. the inbetween child process gets a new terminal and makes a pipe
    // 3. fork again to get the grandchild process
    // 4. grandchild copies args to force copy, since copy-on-write is not very useful here,
    //    as the pages need to be copied before the execv, in order to be able to signal
    //    the inbetween child process to exit. There are other ways to achieve the same effect.
    // 5. the inbetween child process reads the pipe and exits with pid of the grandchild or -1
    //    in case of error. The grandchild is orphaned (this is to prevent zombification).
    // 6. the parent waits the inbetween process and returns.

    // yes, all that (because, Linux...)

    #define IGNORE_SIGPIPE\
        struct sigaction signal_ignored;\
        memset(&signal_ignored, 0, sizeof(signal_ignored));\
        signal_ignored.sa_handler = SIG_IGN;\
        ::sigaction(SIGPIPE, &signal_ignored, 0);

    if (prog == nullptr || argv == nullptr) {
        return false;
    }

    // Build argv/prog copies in the parent so the child never allocates between
    // fork() and execv(), avoiding malloc mutex deadlocks in multi-threaded parents.
    std::string prog_storage(prog);
    int argc = 0;
    while (argv[argc]) { ++argc; }
    std::vector<std::string> argv_storage;
    argv_storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        argv_storage.emplace_back(argv[i]);
    }
    std::vector<char*> argv_copy(static_cast<size_t>(argc) + 1, nullptr);
    for (int i = 0; i < argc; ++i) {
        argv_copy[static_cast<size_t>(i)] = const_cast<char*>(argv_storage[static_cast<size_t>(i)].c_str());
    }
    char* prog_copy = const_cast<char*>(prog_storage.c_str());

    auto report_failure = [&](detail::spawn_detached_debug_info::Stage stage, int error, int exec_error) {
        detail::spawn_detached_debug_info info;
        info.stage = stage;
        info.errno_value = error;
        info.exec_errno = exec_error;
        detail::emit_spawn_detached_debug(info);
    };

    int ready_pipe[2] = {-1, -1};
    while (true) {
        if (detail::call_pipe2(ready_pipe, O_CLOEXEC) == 0) {
            break;
        }
        int saved_errno = errno;
        if (ready_pipe[0] >= 0) {
            close(ready_pipe[0]);
            ready_pipe[0] = -1;
        }
        if (ready_pipe[1] >= 0) {
            close(ready_pipe[1]);
            ready_pipe[1] = -1;
        }
        if (saved_errno != EINTR) {
            report_failure(detail::spawn_detached_debug_info::Stage::PipeCreation, saved_errno, saved_errno);
            errno = saved_errno;
            return false;
        }
    }

    pid_t child_pid = -1;
    do {
        child_pid = ::fork();
    }
    while (child_pid == -1 && errno == EINTR);
    if (child_pid == -1) {
        if (ready_pipe[0] >= 0) {
            close(ready_pipe[0]);
        }
        if (ready_pipe[1] >= 0) {
            close(ready_pipe[1]);
        }
        report_failure(detail::spawn_detached_debug_info::Stage::Fork, errno, errno);
        return false;
    }

    if (child_pid == 0) {
        IGNORE_SIGPIPE

        if (ready_pipe[0] >= 0) {
            close(ready_pipe[0]);
        }

        // Ensure the status pipe closes on exec so the parent observes EOF
        int flags = detail::fcntl_retry(ready_pipe[1], F_GETFD);
        if (flags != -1) {
            detail::fcntl_retry(ready_pipe[1], F_SETFD, flags | FD_CLOEXEC);
        }

        ::setsid();

        int ready_status = 0;
        if (!detail::write_fully(ready_pipe[1], &ready_status, sizeof(ready_status))) {
            report_failure(detail::spawn_detached_debug_info::Stage::ChildReadyPipeWrite, errno, 0);
            if (ready_pipe[1] >= 0) {
                close(ready_pipe[1]);
            }
            ::_exit(EXIT_FAILURE);
        }

        ::execv(prog_copy, (char* const*)argv_copy.data());

        int exec_errno = errno;

        detail::write_fully(ready_pipe[1], &exec_errno, sizeof(exec_errno));
        if (ready_pipe[1] >= 0) {
            close(ready_pipe[1]);
        }
        ::_exit(EXIT_FAILURE);
    }

    if (ready_pipe[1] >= 0) {
        close(ready_pipe[1]);
    }

    enum class Read_result {
        Value,
        Eof,
        Error,
    };

    auto read_int = [&](int* value, int* error_out) -> Read_result {
        std::array<char, sizeof(int)> buffer{};
        size_t offset = 0;
        while (offset < buffer.size()) {
            ssize_t rv = detail::call_read(ready_pipe[0], buffer.data() + offset, buffer.size() - offset);
            if (rv < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (error_out) {
                    *error_out = errno;
                }
                return Read_result::Error;
            }
            if (rv == 0) {
                if (offset == 0) {
                    return Read_result::Eof;
                }
                if (error_out) {
                    *error_out = EPIPE;
                }
                return Read_result::Error;
            }

            offset += static_cast<size_t>(rv);
        }

        std::memcpy(value, buffer.data(), sizeof(*value));
        return Read_result::Value;
    };

    int exec_errno = 0;
    bool spawn_failed = false;
    int observed_errno = 0;
    auto failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadReadyStatus;

    int ready_status = 0;
    switch (read_int(&ready_status, &exec_errno)) {
        case Read_result::Value:
            break;
        case Read_result::Eof:
            spawn_failed = true;
            exec_errno = exec_errno ? exec_errno : EPIPE;
            observed_errno = exec_errno;
            failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadReadyStatus;
            break;
        case Read_result::Error:
            spawn_failed = true;
            observed_errno = exec_errno;
            failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadReadyStatus;
            break;
    }

    if (!spawn_failed && ready_status != 0) {
        exec_errno = ready_status > 0 ? ready_status : -ready_status;
        spawn_failed = true;
        observed_errno = exec_errno;
        failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadReadyStatus;
    }

    if (!spawn_failed) {
        int exec_status = 0;
        switch (read_int(&exec_status, &exec_errno)) {
            case Read_result::Value:
                exec_errno = exec_status > 0 ? exec_status : -exec_status;
                spawn_failed = true;
                observed_errno = exec_errno;
                failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadExecStatus;
                break;
            case Read_result::Eof:
                break;
            case Read_result::Error:
                spawn_failed = true;
                observed_errno = exec_errno;
                failure_stage = detail::spawn_detached_debug_info::Stage::ParentReadExecStatus;
                break;
        }
    }

    bool read_success = !spawn_failed;

    if (ready_pipe[0] >= 0) {
        close(ready_pipe[0]);
    }

    if (!read_success) {
        int status = 0;
        while (detail::call_waitpid(child_pid, &status, 0) == -1) {
            if (errno != EINTR) {
                break;
            }
        }
        if (!observed_errno) {
            observed_errno = exec_errno ? exec_errno : errno;
        }
        report_failure(failure_stage, observed_errno, exec_errno);
        errno = exec_errno ? exec_errno : errno;
        return false;
    }

#ifndef _WIN32
    int wait_status = 0;
    pid_t wait_result = 0;
    do {
        wait_result = detail::call_waitpid(child_pid, &wait_status, WNOHANG);
    }
    while (wait_result == -1 && errno == EINTR);

    if (wait_result == child_pid) {
        if (!(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)) {
            report_failure(detail::spawn_detached_debug_info::Stage::ParentWaitpid,
                           exec_errno ? exec_errno : ECHILD,
                           exec_errno);
            errno = exec_errno ? exec_errno : ECHILD;
            return false;
        }
        if (child_pid_out) {
            *child_pid_out = -1;
        }
        return true;
    }

    if (wait_result == -1) {
        if (errno == ECHILD) {
            if (child_pid_out) {
                *child_pid_out = -1;
            }
            errno = 0;
            return true;
        }
        report_failure(detail::spawn_detached_debug_info::Stage::ParentWaitpid, errno, exec_errno);
        return false;
    }
#endif

    if (child_pid_out) {
        *child_pid_out = static_cast<int>(child_pid);
    }

    return true;

    #undef IGNORE_SIGPIPE

#endif // !defined(_WIN32)

}



struct Instantiator
{
    Instantiator(std::function<void()>&& deinstantiator):
        m_deinstantiator(deinstantiator)
    {}

    ~Instantiator()
    {
        m_deinstantiator();
    }

    std::function<void()> m_deinstantiator;
};


}



