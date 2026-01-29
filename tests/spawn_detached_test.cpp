#include "sintra/detail/utility.h"

#include "test_environment.h"

#include <iostream>

#ifndef _WIN32

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Override_guard {
    enum class Kind { Pipe2, Write, Read, Waitpid, SpawnDebug };

    Override_guard(Kind k, void* fn) : kind(k)
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
            case Kind::Waitpid:
                previous.waitpid = sintra::testing::set_waitpid_override(
                    reinterpret_cast<sintra::detail::waitpid_fn>(fn));
                break;
            case Kind::SpawnDebug:
                previous.spawn_debug = sintra::testing::set_spawn_detached_debug(reinterpret_cast<sintra::detail::spawn_detached_debug_fn>(fn));
                break;
        }
    }

    ~Override_guard()
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
            case Kind::Waitpid:
                sintra::testing::set_waitpid_override(previous.waitpid);
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
        sintra::detail::waitpid_fn waitpid;
        sintra::detail::spawn_detached_debug_fn spawn_debug;
    } previous{};
};

bool assert_true(bool condition, const std::string& message)
{
    if (!condition) {
        int saved_errno = errno;
        std::cerr << "spawn_detached_test: " << message;
        if (saved_errno != 0) {
            std::cerr << " (errno=" << saved_errno << " " << std::strerror(saved_errno) << ")";
        }
        std::cerr << std::endl;
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

const char* locate_true_binary()
{
    static const char* const cached = []() -> const char* {
        static const char* const candidates[] = {
            "/bin/true",
            "/usr/bin/true",
            nullptr,
        };

        for (const char* candidate : candidates) {
            if (candidate == nullptr) {
                break;
            }
            if (::access(candidate, X_OK) == 0) {
                return candidate;
            }
        }
        return nullptr;
    }();
    return cached;
}

const char* locate_shell_binary()
{
    static const char* const cached = []() -> const char* {
        static const char* const candidates[] = {
            "/bin/sh",
            "/usr/bin/sh",
            nullptr,
        };

        for (const char* candidate : candidates) {
            if (candidate == nullptr) {
                break;
            }
            if (::access(candidate, X_OK) == 0) {
                return candidate;
            }
        }
        return nullptr;
    }();
    return cached;
}

} // namespace

bool spawn_detached_with_args(const char* prog, const char* const* args)
{
    sintra::Spawn_detached_options options;
    options.prog = prog;
    options.argv = args;
    return sintra::spawn_detached(options);
}

bool spawn_should_fail_due_to_fd_exhaustion()
{
    const char* true_prog = locate_true_binary();
    if (!assert_true(true_prog != nullptr, "failed to locate executable for 'true'")) {
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

    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);

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
    const char* true_prog = locate_true_binary();
    if (!assert_true(true_prog != nullptr, "failed to locate executable for 'true'")) {
        return false;
    }

    Override_guard guard(Override_guard::Kind::Pipe2, reinterpret_cast<void*>(&failing_pipe2));
    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
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
    const char* true_prog = locate_true_binary();
    if (!assert_true(true_prog != nullptr, "failed to locate executable for 'true'")) {
        return false;
    }

    Override_guard write_guard(Override_guard::Kind::Write, reinterpret_cast<void*>(&flaky_write));
    Override_guard read_guard(Override_guard::Kind::Read, reinterpret_cast<void*>(&flaky_read));
    reset_spawn_debug_capture();
    Override_guard debug_guard(Override_guard::Kind::SpawnDebug, reinterpret_cast<void*>(&capture_spawn_debug));

    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
    if (!result && debug_captured) {
        std::cerr << "spawn_detached_test: debug stage=" << stage_to_string(last_debug_info.stage)
                  << ", errno=" << last_debug_info.errno_value
                  << ", exec_errno=" << last_debug_info.exec_errno
                  << std::endl;
    }
    return assert_true(result, "spawn_detached must retry on EINTR and eventually succeed");
}

pid_t waitpid_returns_echild(pid_t, int*, int)
{
    errno = ECHILD;
    return -1;
}

bool spawn_succeeds_when_waitpid_reports_echild()
{
    const char* true_prog = locate_true_binary();
    if (!assert_true(true_prog != nullptr, "failed to locate executable for 'true'")) {
        return false;
    }

    Override_guard guard(Override_guard::Kind::Waitpid, reinterpret_cast<void*>(&waitpid_returns_echild));
    const char* const args[] = {true_prog, nullptr};
    errno = 0;
    bool result = spawn_detached_with_args(true_prog, args);
    int saved_errno = errno;
    return assert_true(result,
                       "spawn_detached must tolerate waitpid reporting ECHILD after a successful exec") &&
           assert_true(saved_errno == 0,
                       "spawn_detached must clear errno when waitpid reports ECHILD after success");
}

ssize_t broken_write(int, const void*, size_t)
{
    errno = EPIPE;
    return -1;
}

bool spawn_fails_when_grandchild_cannot_report_readiness()
{
    const char* true_prog = locate_true_binary();
    if (!assert_true(true_prog != nullptr, "failed to locate executable for 'true'")) {
        return false;
    }

    Override_guard guard(Override_guard::Kind::Write, reinterpret_cast<void*>(&broken_write));
    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
    return assert_true(!result, "write failures must be reported as spawn failures");
}

bool spawn_reports_exec_failure()
{
    const char* const args[] = {"/definitely/not/a/program", nullptr};
    errno = 0;
    bool result = spawn_detached_with_args("/definitely/not/a/program", args);
    int saved_errno = errno;
    return assert_true(!result, "spawn_detached must fail when execv cannot launch the target") &&
           assert_true(saved_errno == ENOENT, "spawn_detached must surface the exec errno");
}

bool spawn_detached_sets_env_overrides()
{
    const char* shell = locate_shell_binary();
    if (!assert_true(shell != nullptr, "failed to locate /bin/sh for env override test")) {
        return false;
    }

    auto dir = sintra::test::unique_scratch_directory("spawn_detached_env");
    auto output_path = dir / "env_override_output.txt";
    std::string command = "printf \"%s\" \"$SINTRA_ENV_OVERRIDE_TEST\" > \"" + output_path.string() + "\"";
    const char* const args[] = {shell, "-c", command.c_str(), nullptr};

    sintra::Spawn_detached_options options;
    options.prog = shell;
    options.argv = args;
    options.env_overrides.push_back("SINTRA_ENV_OVERRIDE_TEST=spawn_detached_env_value");

    bool result = sintra::spawn_detached(options);
    if (!assert_true(result, "spawn_detached failed to launch shell with env override")) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(output_path)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!assert_true(std::filesystem::exists(output_path), "env override output file not created")) {
        return false;
    }

    std::ifstream in(output_path, std::ios::binary);
    if (!assert_true(in.good(), "failed to read env override output file")) {
        return false;
    }

    std::string content;
    std::getline(in, content);
    return assert_true(content == "spawn_detached_env_value", "env override value mismatch");
}

} // namespace

int main()
{
    bool ok = true;
    ok &= spawn_should_fail_due_to_fd_exhaustion();
    ok &= spawn_should_fail_when_pipe2_injected_failure();
    ok &= spawn_succeeds_under_eintr_pressure();
    ok &= spawn_succeeds_when_waitpid_reports_echild();
    ok &= spawn_fails_when_grandchild_cannot_report_readiness();
    ok &= spawn_reports_exec_failure();
    ok &= spawn_detached_sets_env_overrides();
    return ok ? 0 : 1;
}

#else

int main()
{
    std::cout << "spawn_detached_test is a POSIX-only test" << std::endl;
    return 0;
}

#endif
