#include "sintra/detail/utility.h"

#include <iostream>

#ifndef _WIN32

#include <array>
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

struct OverrideGuard {
    enum class Kind { Pipe2, Write, Read, SpawnDebug };

    OverrideGuard(Kind k, void* fn) : kind(k)
    {
        switch (kind) {
            case Kind::Pipe2:
                previous.pipe2 = sintra::testing::set_pipe2_override(reinterpret_cast<sintra::detail::pipe2_fn>(fn));
                break;
            case Kind::Write:
                previous.write = sintra::testing::set_write_override(reinterpret_cast<sintra::detail::write_fn>(fn));
                break;
            case Kind::Read:
                previous.read = sintra::testing::set_read_override(reinterpret_cast<sintra::detail::read_fn>(fn));
                break;
            case Kind::SpawnDebug:
                previous.spawn_debug = sintra::testing::set_spawn_detached_debug(reinterpret_cast<sintra::detail::spawn_detached_debug_fn>(fn));
                break;
        }
    }

    ~OverrideGuard()
    {
        switch (kind) {
            case Kind::Pipe2:
                sintra::testing::set_pipe2_override(previous.pipe2);
                break;
            case Kind::Write:
                sintra::testing::set_write_override(previous.write);
                break;
            case Kind::Read:
                sintra::testing::set_read_override(previous.read);
                break;
            case Kind::SpawnDebug:
                sintra::testing::set_spawn_detached_debug(previous.spawn_debug);
                break;
        }
    }

    Kind kind;
    union {
        sintra::detail::pipe2_fn pipe2;
        sintra::detail::write_fn write;
        sintra::detail::read_fn read;
        sintra::detail::spawn_detached_debug_fn spawn_debug;
    } previous{};
};

bool assert_true(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "spawn_detached_test: " << message << std::endl;
    }
    return condition;
}

namespace {

bool debug_captured = false;
sintra::detail::spawn_detached_debug_info last_debug_info{};

void capture_spawn_debug(const sintra::detail::spawn_detached_debug_info& info)
{
    debug_captured = true;
    last_debug_info = info;
}

void reset_spawn_debug_capture()
{
    debug_captured = false;
    last_debug_info = {};
}

const char* stage_to_string(sintra::detail::spawn_detached_debug_info::Stage stage)
{
    using Stage = sintra::detail::spawn_detached_debug_info::Stage;
    switch (stage) {
        case Stage::PipeCreation:
            return "PipeCreation";
        case Stage::Fork:
            return "Fork";
        case Stage::ChildReadyPipeWrite:
            return "ChildReadyPipeWrite";
        case Stage::ParentReadReadyStatus:
            return "ParentReadReadyStatus";
        case Stage::ParentReadExecStatus:
            return "ParentReadExecStatus";
        case Stage::ParentWaitpid:
            return "ParentWaitpid";
    }
    return "Unknown";
}

const char* resolve_true_binary()
{
    static const std::string path = []() -> std::string {
        const char* candidates[] = {"/usr/bin/true", "/bin/true"};
        for (const char* candidate : candidates) {
            if (::access(candidate, X_OK) == 0) {
                return std::string(candidate);
            }
        }
        return std::string();
    }();

    return path.empty() ? nullptr : path.c_str();
}

const char* const* make_true_args(const char* true_path, std::array<const char*, 2>& storage)
{
    storage[0] = true_path;
    storage[1] = nullptr;
    return storage.data();
}

} // namespace

bool spawn_should_fail_due_to_fd_exhaustion()
{
    const char* true_path = resolve_true_binary();
    if (!assert_true(true_path != nullptr, "failed to locate 'true' binary")) {
        return false;
    }

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

    std::array<const char*, 2> args_storage{};
    const char* const* args = make_true_args(true_path, args_storage);
    bool result = sintra::spawn_detached(true_path, args);

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
    const char* true_path = resolve_true_binary();
    if (!assert_true(true_path != nullptr, "failed to locate 'true' binary")) {
        return false;
    }

    OverrideGuard guard(OverrideGuard::Kind::Pipe2, reinterpret_cast<void*>(&failing_pipe2));
    std::array<const char*, 2> args_storage{};
    const char* const* args = make_true_args(true_path, args_storage);
    bool result = sintra::spawn_detached(true_path, args);
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
    const char* true_path = resolve_true_binary();
    if (!assert_true(true_path != nullptr, "failed to locate 'true' binary")) {
        return false;
    }

    OverrideGuard write_guard(OverrideGuard::Kind::Write, reinterpret_cast<void*>(&flaky_write));
    OverrideGuard read_guard(OverrideGuard::Kind::Read, reinterpret_cast<void*>(&flaky_read));
    reset_spawn_debug_capture();
    OverrideGuard debug_guard(OverrideGuard::Kind::SpawnDebug, reinterpret_cast<void*>(&capture_spawn_debug));

    std::array<const char*, 2> args_storage{};
    const char* const* args = make_true_args(true_path, args_storage);
    bool result = sintra::spawn_detached(true_path, args);
    if (!result && debug_captured) {
        std::cerr << "spawn_detached_test: debug stage=" << stage_to_string(last_debug_info.stage)
                  << ", errno=" << last_debug_info.errno_value
                  << ", exec_errno=" << last_debug_info.exec_errno
                  << std::endl;
    }
    return assert_true(result, "spawn_detached must retry on EINTR and eventually succeed");
}

ssize_t broken_write(int, const void*, size_t)
{
    errno = EPIPE;
    return -1;
}

bool spawn_fails_when_grandchild_cannot_report_readiness()
{
    const char* true_path = resolve_true_binary();
    if (!assert_true(true_path != nullptr, "failed to locate 'true' binary")) {
        return false;
    }

    OverrideGuard guard(OverrideGuard::Kind::Write, reinterpret_cast<void*>(&broken_write));
    std::array<const char*, 2> args_storage{};
    const char* const* args = make_true_args(true_path, args_storage);
    bool result = sintra::spawn_detached(true_path, args);
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
