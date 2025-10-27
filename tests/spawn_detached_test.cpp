#include "sintra/detail/utility.h"

#include <iostream>

#ifndef _WIN32

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

template <typename Fn, Fn (*Setter)(Fn)>
class OverrideGuard {
public:
    explicit OverrideGuard(Fn fn) : previous_(Setter(fn)) {}

    OverrideGuard(const OverrideGuard&) = delete;
    OverrideGuard& operator=(const OverrideGuard&) = delete;

    ~OverrideGuard()
    {
        Setter(previous_);
    }

private:
    Fn previous_{};
};

using Pipe2OverrideGuard = OverrideGuard<sintra::detail::pipe2_fn, sintra::testing::set_pipe2_override>;
using WriteOverrideGuard = OverrideGuard<sintra::detail::write_fn, sintra::testing::set_write_override>;
using ReadOverrideGuard = OverrideGuard<sintra::detail::read_fn, sintra::testing::set_read_override>;

bool assert_true(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "spawn_detached_test: " << message << std::endl;
    }
    return condition;
}

bool spawn_should_fail_due_to_fd_exhaustion()
{
    int sentinel = ::open("/dev/null", O_RDONLY);
    if (sentinel == -1) {
        std::perror("open");
        return false;
    }

    std::vector<int> handles;
    handles.reserve(256);
    bool exhausted = false;
    for (;;) {
        int fd = ::open("/dev/null", O_RDONLY);
        if (fd == -1) {
            exhausted = (errno == EMFILE);
            break;
        }
        handles.push_back(fd);
    }

    const char* const args[] = {"/bin/true", nullptr};
    bool result = sintra::spawn_detached("/bin/true", args);

    bool sentinel_ok = (::fcntl(sentinel, F_GETFD) != -1);

    for (int fd : handles) {
        ::close(fd);
    }
    ::close(sentinel);

    return assert_true(exhausted, "failed to exhaust file descriptors for test") &&
           assert_true(!result, "spawn_detached should fail when the pipe cannot be created") &&
           assert_true(sentinel_ok, "existing descriptors must remain untouched");
}

int failing_pipe2(int[2], int)
{
    errno = EIO;
    return -1;
}

bool spawn_should_fail_when_pipe2_injected_failure()
{
    Pipe2OverrideGuard guard(&failing_pipe2);
    const char* const args[] = {"/bin/true", nullptr};
    bool result = sintra::spawn_detached("/bin/true", args);
    return assert_true(!result, "spawn_detached must report failure when pipe2 fails");
}

ssize_t flaky_write(int fd, const void* buf, size_t count)
{
    static int attempts = 0;
    if (attempts++ == 0) {
        errno = EINTR;
        return -1;
    }
    return ::write(fd, buf, count);
}

ssize_t flaky_read(int fd, void* buf, size_t count)
{
    static int attempts = 0;
    if (attempts++ == 0) {
        errno = EINTR;
        return -1;
    }
    return ::read(fd, buf, count);
}

bool spawn_succeeds_under_eintr_pressure()
{
    WriteOverrideGuard write_guard(&flaky_write);
    ReadOverrideGuard read_guard(&flaky_read);

    const char* const args[] = {"/bin/true", nullptr};
    bool result = sintra::spawn_detached("/bin/true", args);
    return assert_true(result, "spawn_detached must retry on EINTR and eventually succeed");
}

ssize_t broken_write(int, const void*, size_t)
{
    errno = EPIPE;
    return -1;
}

bool spawn_fails_when_grandchild_cannot_report_readiness()
{
    WriteOverrideGuard guard(&broken_write);
    const char* const args[] = {"/bin/true", nullptr};
    bool result = sintra::spawn_detached("/bin/true", args);
    return assert_true(!result, "write failures must be reported as spawn failures");
}

bool spawn_reports_exec_failure()
{
    const char* const args[] = {"/definitely/not/a/program", nullptr};
    errno = 0;
    bool result = sintra::spawn_detached("/definitely/not/a/program", args);
    int saved_errno = errno;
    return assert_true(!result, "spawn_detached must fail when execv cannot launch the target") &&
           assert_true(saved_errno == ENOENT, "spawn_detached must surface the exec errno");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= spawn_should_fail_due_to_fd_exhaustion();
    ok &= spawn_should_fail_when_pipe2_injected_failure();
    ok &= spawn_succeeds_under_eintr_pressure();
    ok &= spawn_fails_when_grandchild_cannot_report_readiness();
    ok &= spawn_reports_exec_failure();
    return ok ? 0 : 1;
}

#else

int main()
{
    std::cout << "spawn_detached_test is a POSIX-only test" << std::endl;
    return 0;
}

#endif
