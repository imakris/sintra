#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>

namespace {
bool observe_pipe_creation = false;
bool writer_was_inheritable = false;
bool writer_flags_observed = false;

BOOL WINAPI observed_create_pipe(
    PHANDLE read_handle, PHANDLE write_handle,
    LPSECURITY_ATTRIBUTES attributes, DWORD size)
{
    const BOOL created = ::CreatePipe(read_handle, write_handle, attributes, size);
    if (created && observe_pipe_creation) {
        DWORD flags = 0;
        writer_flags_observed = GetHandleInformation(*write_handle, &flags) != 0;
        writer_was_inheritable = (flags & HANDLE_FLAG_INHERIT) != 0;
    }
    return created;
}
} // namespace

// Observe the native handles before the implementation changes any flags.
// This executable is header-only, so no differently defined library copy exists.
#define CreatePipe observed_create_pipe
#endif
#include <sintra/sintra.h>
#ifdef _WIN32
#undef CreatePipe
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

namespace {

bool check_pipe_inheritance()
{
    int error = 0;
#ifdef _WIN32
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    observe_pipe_creation = true;
    const bool created = sintra::create_lifeline_pipe(read_handle, write_handle, &error);
    observe_pipe_creation = false;
    if (!created) {
        std::fprintf(stderr, "Lifeline pipe creation failed: %d\n", error);
        return false;
    }
    DWORD read_flags = 0;
    DWORD write_flags = 0;
    const bool flags_valid =
        GetHandleInformation(read_handle, &read_flags) &&
        GetHandleInformation(write_handle, &write_flags);
    CloseHandle(read_handle);
    CloseHandle(write_handle);
    const bool valid = flags_valid && writer_flags_observed &&
        !writer_was_inheritable && (read_flags & HANDLE_FLAG_INHERIT) &&
        !(write_flags & HANDLE_FLAG_INHERIT);
#else
    int read_fd = -1;
    int write_fd = -1;
    if (!sintra::create_lifeline_pipe(read_fd, write_fd, &error)) {
        std::fprintf(stderr, "Lifeline pipe creation failed: %d\n", error);
        return false;
    }
    const int read_flags = fcntl(read_fd, F_GETFD);
    const int write_flags = fcntl(write_fd, F_GETFD);
    close(read_fd);
    close(write_fd);
    const bool valid = read_flags >= 0 && write_flags >= 0 &&
        (read_flags & FD_CLOEXEC) && (write_flags & FD_CLOEXEC);
#endif
    if (!valid) {
        std::fprintf(stderr, "Lifeline writer must remain noninheritable from creation\n");
    }
    return valid;
}

#ifndef _WIN32
bool wait_for_spawned_helper(const sintra::detail::Spawn_detached_result& result);

int inspect_inherited_reader(int argc, char* argv[])
{
    if (argc != 6) {
        return 2;
    }
    const int fd = std::stoi(argv[2]);
    struct stat info{};
    const bool same_pipe = ::fstat(fd, &info) == 0 &&
        static_cast<unsigned long long>(info.st_dev) == std::stoull(argv[3]) &&
        static_cast<unsigned long long>(info.st_ino) == std::stoull(argv[4]);
    if (std::string_view(argv[5]) == "absent") {
        return same_pipe ? 3 : 0;
    }
    if (std::string_view(argv[5]) == "adopt") {
        sintra::s_lifeline_handle_value = argv[2];
        sintra::start_lifeline_watcher(sintra::Lifetime_policy{}, true);
        const char* child_args[] = {argv[0], "--inspect-reader", argv[2],
            argv[3], argv[4], "absent", nullptr};
        sintra::Spawn_detached_options options;
        options.prog = argv[0];
        options.argv = child_args;
        const int flags = ::fcntl(fd, F_GETFD);
        const bool descendant_clean = wait_for_spawned_helper(
            sintra::detail::spawn_detached_with_result(options));
        return same_pipe && flags >= 0 && (flags & FD_CLOEXEC) && descendant_clean ? 0 : 5;
    }
    char marker = 0;
    return same_pipe && ::read(fd, &marker, 1) == 1 && marker == 'L' ? 0 : 4;
}

bool wait_for_spawned_helper(const sintra::detail::Spawn_detached_result& result)
{
    if (!result.created()) {
        return false;
    }
    int status = result.wait_status;
    if (result.state == sintra::detail::Spawn_detached_result::State::created_reaped) {
        return result.wait_status_available && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = ::waitpid(result.pid, &status, WNOHANG);
        if (waited == result.pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (waited == -1 && errno != EINTR) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ::kill(result.pid, SIGKILL);
    while (::waitpid(result.pid, &status, 0) == -1 && errno == EINTR) {}
    return false;
}

unsigned invalid_fd_pipe_allocations = 0;

int count_invalid_fd_pipe_allocations(int pipefd[2], int flags)
{
    ++invalid_fd_pipe_allocations;
    return sintra::detail::system_pipe2(pipefd, flags);
}

bool check_exec_inheritance(const char* binary)
{
    int reader = -1;
    int writer = -1;
    int error = 0;
    if (!sintra::create_lifeline_pipe(reader, writer, &error)) {
        return false;
    }
    struct stat info{};
    bool ok = ::fstat(reader, &info) == 0;
    const std::string fd_text = std::to_string(reader);
    const std::string device = std::to_string(static_cast<unsigned long long>(info.st_dev));
    const std::string inode = std::to_string(static_cast<unsigned long long>(info.st_ino));
    const char* args[] = {binary, "--inspect-reader", fd_text.c_str(),
        device.c_str(), inode.c_str(), "absent", nullptr};
    sintra::Spawn_detached_options options;
    options.prog = binary;
    options.argv = args;
    // Keep the lifeline open while an unrelated child execs. Descriptor numbers
    // can be reused by the loader; device/inode identifies the actual pipe.
    ok &= wait_for_spawned_helper(sintra::detail::spawn_detached_with_result(options));
    args[5] = "present";
    options.inherited_fds.push_back(reader);
    ok &= ::write(writer, "L", 1) == 1;
    ok &= wait_for_spawned_helper(sintra::detail::spawn_detached_with_result(options));
    args[5] = "adopt";
    ok &= wait_for_spawned_helper(sintra::detail::spawn_detached_with_result(options));
    const int parent_flags = ::fcntl(reader, F_GETFD);
    ok &= parent_flags >= 0 && (parent_flags & FD_CLOEXEC) != 0;
    ::close(reader);
    ::close(writer);
    // These closed slots are precisely where an internal status pipe would be
    // allocated if validation happened too late.
    invalid_fd_pipe_allocations = 0;
    const auto previous_pipe_hook = sintra::testing::set_pipe2_override(
        count_invalid_fd_pipe_allocations);
    const auto invalid = sintra::detail::spawn_detached_with_result(options);
    sintra::testing::set_pipe2_override(previous_pipe_hook);
    if (invalid.created()) {
        (void)wait_for_spawned_helper(invalid);
    }
    ok &= !invalid.created() && invalid.pid <= 0 && invalid.error.value() == EBADF &&
        invalid_fd_pipe_allocations == 0;
    if (!ok) {
        std::fprintf(stderr, "Lifeline exec inheritance or invalid-fd rejection failed\n");
    }
    return ok;
}
#endif

bool check_shutdown_lifetime(int argc, char* argv[])
{
    sintra::init(argc, argv);
    std::atomic<bool> returned{false};
    std::unique_lock<sintra::shared_mutex> lifetime_lock(
        sintra::dispatch_shutdown_mutex_instance);
    std::thread watcher([&]() {
        sintra::signal_lifeline_shutdown(30000, 97);
        returned.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!sintra::lifeline_shutdown_flag().load() &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    const bool entered = sintra::lifeline_shutdown_flag().load();
    const auto observation_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!sintra::s_mproc->m_must_stop.load() && !returned.load() &&
        std::chrono::steady_clock::now() < observation_deadline)
    {
        std::this_thread::yield();
    }
    const bool excluded =
        !sintra::s_mproc->m_must_stop.load() && !returned.load();
    lifetime_lock.unlock();
    watcher.join();
    const bool stopped = sintra::s_mproc->m_must_stop.load();
    const bool finalized = sintra::shutdown();
    if (!entered || !excluded || !stopped || !finalized) {
        std::fprintf(stderr,
            "Lifeline lifetime guard failed: entered=%d excluded=%d stopped=%d finalized=%d\n",
            entered, excluded, stopped, finalized);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
#ifndef _WIN32
    if (argc > 1 && std::string_view(argv[1]) == "--inspect-reader") {
        return inspect_inherited_reader(argc, argv);
    }
    if (!check_exec_inheritance(argv[0])) {
        return 1;
    }
#endif
    const bool shutdown_only = argc > 1 && std::string_view(argv[1]) == "--shutdown-only";
    if (!shutdown_only && !check_pipe_inheritance()) {
        return 1;
    }
    return check_shutdown_lifetime(argc, argv) ? 0 : 1;
}
