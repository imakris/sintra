#include "sintra/detail/utility.h"

#include "test_utils.h"
#include "exact_child_test_support.h"

#include <iostream>

#ifndef _WIN32

#include <chrono>
#include <atomic>
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

struct Override_guard
{
    enum class Kind{ Pipe2, Write, Read, Waitpid, SpawnDebug };

    Override_guard(Kind k, void* fn) : kind(k)
    {
        switch (kind) {
            case Kind::Pipe2: previous.pipe2 = sintra::testing::set_pipe2_override(
                reinterpret_cast<sintra::detail::pipe2_fn>(fn));
                break;
            case Kind::Write: previous.write = sintra::testing::set_write_override(
                reinterpret_cast<sintra::detail::write_fn>(fn));
                break;
            case Kind::Read: previous.read = sintra::testing::set_read_override(
                reinterpret_cast<sintra::detail::read_fn>(fn));
                break;
            case Kind::Waitpid: previous.waitpid = sintra::testing::set_waitpid_override(
                reinterpret_cast<sintra::detail::waitpid_fn>(fn));
                break;
            case Kind::SpawnDebug: previous.spawn_debug = sintra::testing::set_spawn_detached_debug(
                reinterpret_cast<sintra::detail::spawn_detached_debug_fn>(fn));
                break;
        }
    }

    ~Override_guard()
    {
        switch (kind) {
            case Kind::Pipe2:      sintra::testing::set_pipe2_override(previous.pipe2);             break;
            case Kind::Write:      sintra::testing::set_write_override(previous.write);             break;
            case Kind::Read:       sintra::testing::set_read_override(previous.read);               break;
            case Kind::Waitpid:    sintra::testing::set_waitpid_override(previous.waitpid);         break;
            case Kind::SpawnDebug: sintra::testing::set_spawn_detached_debug(previous.spawn_debug); break;
        }
    }

    Kind kind;
    union
    {
        sintra::detail::pipe2_fn pipe2;
        sintra::detail::write_fn write;
        sintra::detail::read_fn read;
        sintra::detail::waitpid_fn waitpid;
        sintra::detail::spawn_detached_debug_fn spawn_debug;
    } previous{};
};

constexpr std::string_view k_failure_prefix = "spawn_detached_test: ";

namespace {

bool debug_captured = false;
sintra::detail::spawn_detached_debug_info_t last_debug_info{};

void capture_spawn_debug(const sintra::detail::spawn_detached_debug_info_t& info)
{
    debug_captured = true;
    last_debug_info = info;
}

void reset_spawn_debug_capture()
{
    debug_captured = false;
    last_debug_info = {};
}

const char* stage_to_string(sintra::detail::spawn_detached_debug_info_t::Stage stage)
{
    using Stage = sintra::detail::spawn_detached_debug_info_t::Stage;
    switch (stage) {
        case Stage::PIPE_CREATION:            return "PIPE_CREATION";
        case Stage::FORK:                     return "FORK";
        case Stage::CHILD_READY_PIPE_WRITE:   return "CHILD_READY_PIPE_WRITE";
        case Stage::PARENT_READ_READY_STATUS: return "PARENT_READ_READY_STATUS";
        case Stage::PARENT_READ_EXEC_STATUS:  return "PARENT_READ_EXEC_STATUS";
        case Stage::PARENT_WAITPID:           return "PARENT_WAITPID";
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
            if (candidate == nullptr)           { break;            }
            if (::access(candidate, X_OK) == 0) { return candidate; }
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
            if (candidate == nullptr)           { break;            }
            if (::access(candidate, X_OK) == 0) { return candidate; }
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
    if (!sintra::test::assert_true_errno(true_prog != nullptr,
        k_failure_prefix, "failed to locate executable for 'true'"))
    {
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

    return
        sintra::test::assert_true_errno(exhausted,   k_failure_prefix,
            "failed to exhaust file descriptors for test")
        &&
        sintra::test::assert_true_errno(!result,     k_failure_prefix,
            "spawn_detached should fail when the pipe cannot be created")
        &&
        sintra::test::assert_true_errno(sentinel_ok, k_failure_prefix,
            "existing descriptors must remain untouched");
}

int failing_pipe2(int[2], int)
{
    errno = EIO;
    return -1;
}

bool spawn_should_fail_when_pipe2_injected_failure()
{
    const char* true_prog = locate_true_binary();
    if (!sintra::test::assert_true_errno(true_prog != nullptr,
        k_failure_prefix, "failed to locate executable for 'true'"))
    {
        return false;
    }

    Override_guard guard(Override_guard::Kind::Pipe2, reinterpret_cast<void*>(&failing_pipe2));
    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
    return
        sintra::test::assert_true_errno(
            !result,
            k_failure_prefix,
            "spawn_detached must report failure when pipe2 fails"
        );
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
    if (!sintra::test::assert_true_errno(true_prog != nullptr,
        k_failure_prefix, "failed to locate executable for 'true'"))
    {
        return false;
    }

    Override_guard write_guard(Override_guard::Kind::Write, reinterpret_cast<void*>(&flaky_write));
    Override_guard read_guard(Override_guard::Kind::Read, reinterpret_cast<void*>(&flaky_read));
    reset_spawn_debug_capture();
    Override_guard debug_guard(
        Override_guard::Kind::SpawnDebug,
        reinterpret_cast<void*>(&capture_spawn_debug));

    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
    if (!result && debug_captured) {
        std::cerr << "spawn_detached_test: debug stage=" << stage_to_string(last_debug_info.stage)
            << ", errno=" << last_debug_info.errno_value
            << ", exec_errno=" << last_debug_info.exec_errno
            << std::endl;
    }
    return
        sintra::test::assert_true_errno(
            result,
            k_failure_prefix,
            "spawn_detached must retry on EINTR and eventually succeed"
        );
}

pid_t waitpid_returns_echild(pid_t, int*, int)
{
    errno = ECHILD;
    return -1;
}

bool spawn_succeeds_when_waitpid_reports_echild()
{
    const char* true_prog = locate_true_binary();
    if (!sintra::test::assert_true_errno(true_prog != nullptr,
        k_failure_prefix, "failed to locate executable for 'true'"))
    {
        return false;
    }

    Override_guard guard(
        Override_guard::Kind::Waitpid,
        reinterpret_cast<void*>(&waitpid_returns_echild));
    const char* const args[] = {true_prog, nullptr};
    errno = 0;
    bool result      = spawn_detached_with_args(true_prog, args);
    int  saved_errno = errno;
    return
        sintra::test::assert_true_errno(result,           k_failure_prefix,
            "spawn_detached must tolerate waitpid reporting ECHILD after a successful exec")
        &&
        sintra::test::assert_true_errno(saved_errno == 0, k_failure_prefix,
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
    if (!sintra::test::assert_true_errno(true_prog != nullptr,
        k_failure_prefix, "failed to locate executable for 'true'"))
    {
        return false;
    }

    Override_guard guard(Override_guard::Kind::Write, reinterpret_cast<void*>(&broken_write));
    const char* const args[] = {true_prog, nullptr};
    bool result = spawn_detached_with_args(true_prog, args);
    return
        sintra::test::assert_true_errno(
            !result,
            k_failure_prefix,
            "write failures must be reported as spawn failures"
        );
}

bool spawn_reports_exec_failure()
{
    const char* const args[] = {"/definitely/not/a/program", nullptr};
    errno = 0;
    bool result      = spawn_detached_with_args("/definitely/not/a/program", args);
    int  saved_errno = errno;
    return
        sintra::test::assert_true_errno(!result,               k_failure_prefix,
            "spawn_detached must fail when execv cannot launch the target")
        &&
        sintra::test::assert_true_errno(saved_errno == ENOENT, k_failure_prefix,
            "spawn_detached must surface the exec errno");
}

bool spawn_detached_sets_env_overrides()
{
    const char* shell = locate_shell_binary();
    if (!sintra::test::assert_true_errno(shell != nullptr,
        k_failure_prefix, "failed to locate /bin/sh for env override test"))
    {
        return false;
    }

    auto        dir         = sintra::test::unique_scratch_directory("spawn_detached_env");
    auto        output_path = dir / "env_override_output.txt";
    std::string command     = "printf \"%s\" \"$SINTRA_ENV_OVERRIDE_TEST\" > \"" + output_path.string() + "\"";
    const char* const args[] = {shell, "-c", command.c_str(), nullptr};

    sintra::Spawn_detached_options options;
    options.prog = shell;
    options.argv = args;
    options.env_overrides.push_back("SINTRA_ENV_OVERRIDE_TEST=spawn_detached_env_value");

    bool result = sintra::spawn_detached(options);
    if (!sintra::test::assert_true_errno(result, k_failure_prefix,
        "spawn_detached failed to launch shell with env override"))
    {
        return false;
    }

    const auto        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const std::string expected = "spawn_detached_env_value";
    std::string content;
    bool matched = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!std::filesystem::exists(output_path)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::ifstream in(output_path, std::ios::binary);
        if (in.good()) {
            std::getline(in, content);
            if (content == expected) {
                matched = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!sintra::test::assert_true_errno(std::filesystem::exists(output_path),
        k_failure_prefix, "env override output file not created"))
    {
        return false;
    }

    return sintra::test::assert_true_errno(matched, k_failure_prefix, "env override value mismatch");
}

std::atomic_bool pipe_created{false};
std::atomic_bool finish_pipe_creation{false};
std::atomic_uint pipe_calls{0};

int paused_pipe_creation(int pipefd[2], int flags)
{
    if (pipe_calls.fetch_add(1) != 0) {
        return sintra::detail::system_pipe2(pipefd, flags);
    }
    // Force the macOS pipe()+fcntl() window on every POSIX test host.
    if (::pipe(pipefd) != 0) {
        return -1;
    }
    pipe_created.store(true, std::memory_order_release);
    while (!finish_pipe_creation.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    for (int i = 0; i != 2; ++i) {
        if (::fcntl(pipefd[i], F_SETFD, FD_CLOEXEC) == -1) {
            const int saved_errno = errno;
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            pipefd[0] = pipefd[1] = -1;
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

bool wait_for_flag(const std::atomic_bool& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
}

bool concurrent_spawn_does_not_inherit_status_pipe(const char* binary)
{
    using namespace std::chrono_literals;
    namespace test = sintra::test;
    const auto directory = test::unique_scratch_directory("spawn_pipe_race");
    const auto release = directory / "release";
    const auto first_ready = directory / "first_ready";
    const auto second_ready = directory / "second_ready";
    const auto release_string = release.string();
    const auto first_ready_string = first_ready.string();
    const auto second_ready_string = second_ready.string();
    const char* const first_args[] = {
        binary, "--pipe-race-child", first_ready_string.c_str(),
        release_string.c_str(), nullptr};
    const char* const second_args[] = {
        binary, "--pipe-race-child", second_ready_string.c_str(),
        release_string.c_str(), nullptr};
    test::Exact_child first(2s);
    test::Exact_child second(2s);
    std::atomic_bool first_done{false};
    std::atomic_bool second_done{false};
    bool first_spawned = false;
    bool second_spawned = false;
    pipe_created.store(false);
    finish_pipe_creation.store(false);
    pipe_calls.store(0);
    Override_guard override(Override_guard::Kind::Pipe2,
        reinterpret_cast<void*>(paused_pipe_creation));
    std::thread first_launcher([&] {
        first_spawned = first.spawn(binary, first_args);
        first_done.store(true, std::memory_order_release);
    });
    const bool entered = wait_for_flag(pipe_created, 2s);
    std::thread second_launcher([&] {
        second_spawned = second.spawn(binary, second_args);
        second_done.store(true, std::memory_order_release);
    });
    // The competing spawn may serialize behind the unfinished pipe creation.
    // Give an unguarded fork the opportunity to exec, then release either path.
    const bool competitor_finished_during_pause = wait_for_flag(second_done, 250ms);
    finish_pipe_creation.store(true, std::memory_order_release);
    const bool handshakes_finished = wait_for_flag(first_done, 2s) &&
        wait_for_flag(second_done, 2s);
    const bool children_alive = handshakes_finished && first_spawned &&
        second_spawned && test::wait_for_file(first_ready, 2s) &&
        test::wait_for_file(second_ready, 2s) &&
        first.poll() == test::Exact_child_state::running &&
        second.poll() == test::Exact_child_state::running;

    // Record the oracle before allowing either child to close inherited FDs.
    bool ok = test::assert_true(entered && children_alive, k_failure_prefix,
        "concurrent spawn handshakes must finish while both children remain alive");
    if (!ok) {
        std::cerr << k_failure_prefix << "competing exec handshake during pipe pause: "
            << competitor_finished_during_pause << '\n';
    }
    test::write_lines(release, {"release"});
    first_launcher.join();
    second_launcher.join();
    for (auto* child : {&first, &second}) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (child->poll() == test::Exact_child_state::running &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        std::string diagnostic;
        if (!child->exited_with_code(0) || !child->settle_observed_exit(diagnostic)) {
            ok = false;
            (void)child->terminate_and_settle(diagnostic);
        }
    }
    std::filesystem::remove_all(directory);
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 4 && std::string_view(argv[1]) == "--pipe-race-child") {
        sintra::test::write_lines(argv[2], {"ready"});
        return sintra::test::wait_for_file(argv[3], std::chrono::seconds(10)) ? 0 : 1;
    }
    bool ok = true;
    ok &= spawn_should_fail_due_to_fd_exhaustion();
    ok &= spawn_should_fail_when_pipe2_injected_failure();
    ok &= spawn_succeeds_under_eintr_pressure();
    ok &= spawn_succeeds_when_waitpid_reports_echild();
    ok &= spawn_fails_when_grandchild_cannot_report_readiness();
    ok &= spawn_reports_exec_failure();
    ok &= spawn_detached_sets_env_overrides();
    ok &= concurrent_spawn_does_not_inherit_status_pipe(argv[0]);
    return ok ? 0 : 1;
}

#else

#include <windows.h>

#include <cerrno>
#include <iomanip>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> command_line_arguments()
{
    std::vector<std::string> args = {
        R"(C:\program files\sintra.exe)", "--argv-child", "", "plain",
        "two words", "\t", "left\tright", " leading", "trailing ",
        "\"", "\"\"", "before\"after"
    };
    for (size_t count = 0; count != 5; ++count) {
        const std::string slashes(count, '\\');
        args.push_back(slashes);
        args.push_back("prefix" + slashes + "\"suffix");
        args.push_back("prefix " + slashes + "\"suffix");
        args.push_back("prefix " + slashes);
        args.push_back("prefix\t" + slashes);
        args.push_back(slashes + "ordinary");
    }
    return args;
}

bool check_child_arguments(int argc, char* argv[])
{
    const auto expected = command_line_arguments();
    bool ok = (size_t)argc == expected.size();
    if (!ok) {
        std::cerr << "spawn_detached_test: expected " << expected.size()
            << " arguments, received " << argc << '\n';
    }
    for (size_t i = 0; i < expected.size() && i < (size_t)argc; ++i) {
        if (expected[i] != argv[i]) {
            std::cerr << "spawn_detached_test: argv[" << i << "] expected "
                << std::quoted(expected[i]) << ", received "
                << std::quoted(argv[i]) << '\n';
            ok = false;
        }
    }
    return ok;
}

bool spawn_preserves_arguments(const char* executable, bool use_handle_list)
{
    const auto args = command_line_arguments();
    std::vector<const char*> argv;
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    HANDLE event = nullptr;
    if (use_handle_list) {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            std::cerr << "spawn_detached_test: CreateEventW failed: "
                << GetLastError() << '\n';
            return false;
        }
    }

    HANDLE process = nullptr;
    sintra::Spawn_detached_options options;
    options.prog                     = executable;
    options.argv                     = argv.data();
    options.child_process_handle_out = &process;
    if (event) {
        options.inherit_handles.push_back(event);
    }
    const bool spawned = sintra::spawn_detached(options);
    if (event) {
        CloseHandle(event);
    }
    if (!spawned) {
        std::cerr << "spawn_detached_test: failed to spawn argv child: "
            << errno << '\n';
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process, 10000);
    DWORD exit_code = 1;
    const bool exited = wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process, &exit_code);
    if (!exited) {
        std::cerr << "spawn_detached_test: argv child did not exit: "
            << wait_result << '\n';
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 10000);
    }
    CloseHandle(process);
    return exited && exit_code == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string_view(argv[1]) == "--argv-child") {
        return check_child_arguments(argc, argv) ? 0 : 1;
    }

    // The child CRT parses the actual CreateProcessW command line. The oracle
    // is Microsoft's documented C command-line parsing, not another encoder.
    bool ok = spawn_preserves_arguments(argv[0], false);
    ok &= spawn_preserves_arguments(argv[0], true);
    return ok ? 0 : 1;
}

#endif
