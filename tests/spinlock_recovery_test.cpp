#include <sintra/detail/ipc/spinlock.h>
#include <sintra/detail/ipc/platform_utils.h>
#include <sintra/detail/debug_pause.h>
#include <sintra/detail/time_utils.h>
#include <sintra/detail/utility.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

[[noreturn]] void fail(const std::string& message)
{
    std::fprintf(stderr, "spinlock_recovery_test failure: %s\n", message.c_str());
    std::fflush(stderr);
    std::exit(1);
}

void require_true(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

struct spinlock_layout {
    std::atomic_flag m_locked;
    std::atomic<uint32_t> m_owner_pid;
    std::atomic<uint64_t> m_last_progress_ns;
};

spinlock_layout& access_layout(sintra::spinlock& lock)
{
    static_assert(std::is_standard_layout_v<sintra::spinlock>, "spinlock must be standard layout");
    static_assert(sizeof(spinlock_layout) == sizeof(sintra::spinlock), "spinlock layout mismatch");
    return *reinterpret_cast<spinlock_layout*>(&lock);
}

uint32_t find_dead_pid(uint32_t self_pid)
{
    for (uint32_t candidate = 500000; candidate < 510000; ++candidate) {
        if (candidate == self_pid) {
            continue;
        }
        if (!sintra::is_process_alive(candidate)) {
            return candidate;
        }
    }

    for (uint32_t candidate = self_pid + 1; candidate < self_pid + 10000; ++candidate) {
        if (!sintra::is_process_alive(candidate)) {
            return candidate;
        }
    }

    return 0;
}

void disable_abort_dialog()
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    // Disable core dumps on POSIX to speed up abort() termination.
    // On macOS especially, core dump generation can take many seconds,
    // causing waitpid to report the process as still running.
    struct rlimit core_limit = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &core_limit);
#endif
}

int spawn_sleep_child(const char* program, int sleep_ms)
{
    std::string sleep_arg = std::to_string(sleep_ms);
    std::vector<const char*> args = {
        program,
        "--spinlock-sleeper",
        sleep_arg.c_str(),
        nullptr
    };

    int child_pid = -1;
    sintra::Spawn_detached_options options;
    options.prog = program;
    options.argv = args.data();
    options.child_pid_out = &child_pid;

    if (!sintra::spawn_detached(options)) {
        return -1;
    }

    return child_pid;
}

int spawn_stall_child(const char* program, uint32_t owner_pid)
{
    std::string owner_arg = std::to_string(owner_pid);
    std::vector<const char*> args = {
        program,
        "--spinlock-stall-child",
        owner_arg.c_str(),
        nullptr
    };

    int child_pid = -1;
    sintra::Spawn_detached_options options;
    options.prog = program;
    options.argv = args.data();
    options.child_pid_out = &child_pid;

    if (!sintra::spawn_detached(options)) {
        return -1;
    }

    return child_pid;
}

bool wait_for_process_exit(int pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE,
                                static_cast<DWORD>(pid));
    if (!handle) {
        return true;
    }
    DWORD wait_ms = static_cast<DWORD>(timeout.count());
    DWORD result = WaitForSingleObject(handle, wait_ms);
    CloseHandle(handle);
    return result != WAIT_TIMEOUT;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid)) {
            return true;
        }
        if (result == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (errno != EINTR) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
#endif
}

void terminate_child(int pid)
{
    if (pid <= 0) {
        return;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE,
                                static_cast<DWORD>(pid));
    if (handle) {
        TerminateProcess(handle, 0);
        WaitForSingleObject(handle, 2000);
        CloseHandle(handle);
    }
#else
    ::kill(static_cast<pid_t>(pid), SIGKILL);
    // Wait for the child to exit to prevent zombies and ensure clean termination.
    // Use a timeout to avoid blocking indefinitely in case of unexpected behavior.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid) || (result == -1 && errno != EINTR)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "--spinlock-sleeper") {
        int sleep_ms = 4000;
        if (argc >= 3) {
            sleep_ms = std::atoi(argv[2]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return 0;
    }

    if (argc >= 3 && std::string_view(argv[1]) == "--spinlock-stall-child") {
        sintra::detail::set_debug_pause_active(false);
        disable_abort_dialog();
        const uint32_t owner_pid = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
        sintra::spinlock stall_lock;
        auto& stall_layout = access_layout(stall_lock);
        stall_layout.m_locked.clear(std::memory_order_release);
        stall_layout.m_locked.test_and_set(std::memory_order_acquire);
        stall_layout.m_owner_pid.store(owner_pid, std::memory_order_release);
        stall_layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
        stall_lock.lock();
        return 1;
    }

    sintra::spinlock lock;
    auto& layout = access_layout(lock);

    const uint32_t self_pid = static_cast<uint32_t>(sintra::detail::get_current_process_id());

    // Case 1: recover from a dead owner.
    const uint32_t dead_pid = find_dead_pid(self_pid);
    require_true(dead_pid != 0 && dead_pid != self_pid, "failed to locate a dead pid");
    require_true(!sintra::is_process_alive(dead_pid), "dead pid should not be alive");

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(dead_pid, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    lock.lock();
    lock.unlock();

    // Case 2: live owner with debug pause active should force unlock.
    int child_pid = spawn_sleep_child(argv[0], 5000);
    require_true(child_pid > 0, "failed to spawn live-owner child");

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(static_cast<uint32_t>(child_pid), std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    sintra::detail::set_debug_pause_active(true);
    lock.lock();
    lock.unlock();
    sintra::detail::set_debug_pause_active(false);

    terminate_child(child_pid);

    // Case 3: live owner with debug pause inactive should abort (report_live_owner_stall).
    const int stall_pid = spawn_stall_child(argv[0], self_pid);
    require_true(stall_pid > 0, "failed to spawn stall child");
#ifdef __APPLE__
    const int stall_timeout_default_ms = 12000;
#else
    const int stall_timeout_default_ms = 6000;
#endif
    const auto stall_timeout_ms =
        sintra::test::read_env_int("SINTRA_SPINLOCK_STALL_TIMEOUT_MS", stall_timeout_default_ms);
    if (!wait_for_process_exit(stall_pid, std::chrono::milliseconds(stall_timeout_ms))) {
        terminate_child(stall_pid);
        fail("stall child did not terminate as expected");
    }

    return 0;
}
