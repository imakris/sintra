/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef SINTRA_UTILITY_H
#define SINTRA_UTILITY_H

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
#else
    #include <atomic>
    #include <cerrno>
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
        initialize_view();
    }

    explicit cstring_vector(std::vector<std::string>&& v_in)
        : m_storage(std::move(v_in))
    {
        initialize_view();
    }

    cstring_vector(const cstring_vector& other)
        : m_storage(other.m_storage)
    {
        initialize_view();
    }

    cstring_vector(cstring_vector&& other) noexcept
        : m_storage(std::move(other.m_storage))
    {
        initialize_view();
        other.initialize_view();
    }

    cstring_vector& operator=(const cstring_vector& other)
    {
        if (this != &other) {
            m_storage = other.m_storage;
            initialize_view();
        }
        return *this;
    }

    cstring_vector& operator=(cstring_vector&& other) noexcept
    {
        if (this != &other) {
            m_storage = std::move(other.m_storage);
            initialize_view();
            other.initialize_view();
        }
        return *this;
    }

    const char* const* v() const { return m_view.data(); }
    size_t size() const { return m_storage.size(); }

private:
    void initialize_view()
    {
        m_view.resize(m_storage.size() + 1, nullptr);
        for (size_t i = 0; i < m_storage.size(); ++i) {
            m_view[i] = m_storage[i].c_str();
        }
        m_view.back() = nullptr;
    }

    std::vector<std::string> m_storage;
    std::vector<const char*> m_view;
};



namespace detail {

#ifndef _WIN32

using pipe2_fn = int(*)(int[2], int);
using write_fn = ssize_t(*)(int, const void*, size_t);
using read_fn = ssize_t(*)(int, void*, size_t);

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

inline int system_pipe2(int pipefd[2], int flags)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return ::pipe2(pipefd, flags);
#else
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }

    if (::pipe(pipefd) == -1) {
        return -1;
    }

    const auto set_flag = [&](int fd, int cmd, int value) {
        int current = ::fcntl(fd, cmd == F_SETFD ? F_GETFD : F_GETFL);
        if (current == -1) {
            return -1;
        }
        return ::fcntl(fd, cmd, current | value);
    };

    if (flags & O_CLOEXEC) {
        if (set_flag(pipefd[0], F_SETFD, FD_CLOEXEC) == -1 ||
            set_flag(pipefd[1], F_SETFD, FD_CLOEXEC) == -1) {
            int saved_errno = errno;
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            errno = saved_errno;
            return -1;
        }
    }

    if (flags & O_NONBLOCK) {
        if (set_flag(pipefd[0], F_SETFL, O_NONBLOCK) == -1 ||
            set_flag(pipefd[1], F_SETFL, O_NONBLOCK) == -1) {
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

} // namespace testing

#endif // !_WIN32

inline
bool spawn_detached(const char* prog, const char * const*argv)
{

#ifdef _WIN32
    if (prog==nullptr || argv==nullptr) {
        return false;
    }

    char full_path[_MAX_PATH];
    if( _fullpath(full_path, prog, _MAX_PATH ) != NULL ) {

        size_t argv_size=0;
        for (size_t i=0; argv[i] != nullptr; i++) {
            argv_size++;
        }

        const char** argv_with_prog = new const char*[argv_size+2];
        argv_with_prog[0] = full_path;

        for (size_t i=0; i!=argv_size; i++) {
            argv_with_prog[i+1] = argv[i];
        }
        argv_with_prog[argv_size+1] = nullptr;
        auto ret = _spawnv(P_DETACH, full_path, argv_with_prog) != -1;
        delete [] argv_with_prog;
        return ret;
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

    int rv = -1;
    pid_t child_pid = fork();
    if (child_pid == 0) {

        IGNORE_SIGPIPE
        ::setsid();

        int ready_pipe[2] = {-1, -1};
        if (detail::call_pipe2(ready_pipe, O_CLOEXEC) == -1) {
            ::_exit(EXIT_FAILURE);
        }

        pid_t grandchild_pid = fork();

        if (grandchild_pid == 0) {
            IGNORE_SIGPIPE
            if (ready_pipe[0] >= 0) {
                close(ready_pipe[0]);
            }

            // copy argv, to be no longer dependent on pages that have not been copied yet
            auto prog_copy = strdup(prog);
            int argc = 0;
            while (argv[argc]) { argc++; }
            auto argv_copy = new char* [argc+1]; // +1 for null terminator
            argv_copy[argc] = nullptr;
            for (int i = 0; i < argc; i++) {
                argv_copy[i] = strdup(argv[i]);
            }

            // allow the parent (child) process to exit. The write is retried on EINTR
            // so the parent only proceeds once the grandchild is ready to exec.
            int status = 0;
            if (!detail::write_fully(ready_pipe[1], &status, sizeof(int))) {
                if (ready_pipe[1] >= 0) {
                    close(ready_pipe[1]);
                }
                ::_exit(EXIT_FAILURE);
            }

            // proceed with the new program
            ::execv(prog_copy, (char* const*)argv_copy);

            free(prog_copy);
            for (int i = 0; i < argc; i++) {
                free(argv_copy[i]);
            }
            delete[] argv_copy;

            // execv failed; propagate the failure back to the parent process via the pipe.
            status = -1;
            detail::write_fully(ready_pipe[1], &status, sizeof(int));   // best effort
            if (ready_pipe[1] >= 0) {
                close(ready_pipe[1]);
            }
            ::_exit(1);
        }
        else
        if (grandchild_pid == -1) {
            // second fork failed
            IGNORE_SIGPIPE
            rv = -1;
        }
        if (ready_pipe[1] >= 0) {
            close(ready_pipe[1]);
        }
        if (ready_pipe[0] >= 0) {
            if (!detail::read_fully(ready_pipe[0], &rv, sizeof(int))) {
                rv = -1;
            }
            close(ready_pipe[0]);
        }
        ::_exit(rv == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    if (child_pid == -1) {
        // first fork failed
        return false;
    }

    int status = 0;
    if (::waitpid(child_pid, &status, 0) == -1) {
        return false;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0;
    }

    return false;

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



#endif
