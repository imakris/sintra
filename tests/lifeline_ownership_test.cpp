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
#include <string_view>
#include <thread>

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
        !(read_flags & FD_CLOEXEC) && (write_flags & FD_CLOEXEC);
#endif
    if (!valid) {
        std::fprintf(stderr, "Lifeline writer must remain noninheritable from creation\n");
    }
    return valid;
}

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
    const bool shutdown_only = argc > 1 && std::string_view(argv[1]) == "--shutdown-only";
    if (!shutdown_only && !check_pipe_inheritance()) {
        return 1;
    }
    return check_shutdown_lifetime(argc, argv) ? 0 : 1;
}
