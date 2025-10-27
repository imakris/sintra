// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

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
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <process.h>
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
    Adaptive_function(function<void()> f) :
        ppf(new shared_ptr<function<void()>>(new function<void()>(f))),
        m(new mutex)
    {}

    Adaptive_function(const Adaptive_function& rhs)
    {
        lock_guard<mutex> lock(*rhs.m);
        ppf = rhs.ppf;
        m = rhs.m;
    }

    void operator()()
    {
        lock_guard<mutex> lock(*m);
        (**ppf)();
    }

    void set(function<void()> f)
    {
        lock_guard<mutex> lock(*m);
        **ppf = f;
    }

    shared_ptr<shared_ptr<function<void()>>> ppf;
    shared_ptr<mutex> m;
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

    // TODO: implement
    return 0x40;

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

inline std::atomic<spawn_detached_debug_fn>& spawn_detached_debug_override()
{
    static std::atomic<spawn_detached_debug_fn> fn{nullptr};
    return fn;
}

inline void emit_spawn_detached_debug(const spawn_detached_debug_info& info)
{
    if (auto fn = spawn_detached_debug_override().load(std::memory_order_acquire)) {
        fn(info);
    }
}

inline int fcntl_retry(int fd, int cmd)
{
    int rv = -1;
    do {
        rv = ::fcntl(fd, cmd);
    } while (rv == -1 && errno == EINTR);
    return rv;
}

inline int fcntl_retry(int fd, int cmd, int arg)
{
    int rv = -1;
    do {
        rv = ::fcntl(fd, cmd, arg);
    } while (rv == -1 && errno == EINTR);
    return rv;
}

inline int system_pipe2(int pipefd[2], int flags)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
    int rv = -1;
    do {
        rv = ::pipe2(pipefd, flags);
    } while (rv == -1 && errno == EINTR);
    return rv;
#else
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }

    int pipe_result = -1;
    do {
        pipe_result = ::pipe(pipefd);
    } while (pipe_result == -1 && errno == EINTR);
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
    if (auto override = pipe2_override().load(std::memory_order_acquire)) {
        return override(pipefd, flags);
    }
    return system_pipe2(pipefd, flags);
}

inline ssize_t call_write(int fd, const void* buf, size_t count)
{
    if (auto override = write_override().load(std::memory_order_acquire)) {
        return override(fd, buf, count);
    }
    return ::write(fd, buf, count);
}

inline ssize_t call_read(int fd, void* buf, size_t count)
{
    if (auto override = read_override().load(std::memory_order_acquire)) {
        return override(fd, buf, count);
    }
    return ::read(fd, buf, count);
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
    return detail::pipe2_override().exchange(fn, std::memory_order_acq_rel);
}

inline detail::write_fn set_write_override(detail::write_fn fn)
{
    return detail::write_override().exchange(fn, std::memory_order_acq_rel);
}

inline detail::read_fn set_read_override(detail::read_fn fn)
{
    return detail::read_override().exchange(fn, std::memory_order_acq_rel);
}

inline detail::spawn_detached_debug_fn set_spawn_detached_debug(detail::spawn_detached_debug_fn fn)
{
    return detail::spawn_detached_debug_override().exchange(fn, std::memory_order_acq_rel);
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

    char full_path[_MAX_PATH];
    if (_fullpath(full_path, prog, _MAX_PATH) == nullptr) {
        if (child_pid_out) {
            *child_pid_out = -1;
        }
        return false;
    }

    size_t argv_size = 0;
    for (size_t i = 0; argv[i] != nullptr; ++i) {
        ++argv_size;
    }

    std::vector<const char*> argv_with_prog(argv_size + 2, nullptr);
    argv_with_prog[0] = full_path;
    for (size_t i = 0; i != argv_size; ++i) {
        argv_with_prog[i + 1] = argv[i];
    }

    constexpr int kMaxAttempts = 3;
    int last_errno = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto spawned = _spawnv(P_DETACH, full_path, argv_with_prog.data());
        if (spawned != -1) {
            if (child_pid_out) {
                *child_pid_out = static_cast<int>(spawned);
            }
            return true;
        }

        _get_errno(&last_errno);
        if (attempt + 1 < kMaxAttempts &&
            (last_errno == EAGAIN || last_errno == EACCES)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    } while (child_pid == -1 && errno == EINTR);
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

        // Copy argv so the child no longer depends on copy-on-write pages
        auto prog_copy = strdup(prog);
        int argc = 0;
        while (argv[argc]) { ++argc; }
        auto argv_copy = new char*[argc + 1];
        argv_copy[argc] = nullptr;
        for (int i = 0; i < argc; ++i) {
            argv_copy[i] = strdup(argv[i]);
        }

        int ready_status = 0;
        if (!detail::write_fully(ready_pipe[1], &ready_status, sizeof(ready_status))) {
            report_failure(detail::spawn_detached_debug_info::Stage::ChildReadyPipeWrite, errno, 0);
            if (ready_pipe[1] >= 0) {
                close(ready_pipe[1]);
            }
            ::_exit(EXIT_FAILURE);
        }

        ::execv(prog_copy, (char* const*)argv_copy);

        int exec_errno = errno;

        if (prog_copy) {
            free(prog_copy);
        }
        for (int i = 0; i < argc; ++i) {
            if (argv_copy[i]) {
                free(argv_copy[i]);
            }
        }
        delete[] argv_copy;

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
        while (::waitpid(child_pid, &status, 0) == -1) {
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
        wait_result = ::waitpid(child_pid, &wait_status, WNOHANG);
    } while (wait_result == -1 && errno == EINTR);

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



