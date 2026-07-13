#include <sintra/detail/ipc/spinlock.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/debug_pause.h>
#include <sintra/detail/time_utils.h>
#include <sintra/detail/utility.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr std::string_view k_failure_prefix = "spinlock_recovery_test failure: ";
constexpr auto k_child_poll_interval = std::chrono::milliseconds(10);
constexpr auto k_child_cleanup_timeout = std::chrono::seconds(10);
#ifdef _WIN32
constexpr DWORD k_cleanup_exit_code = 0x53434c4b;
#endif

struct spinlock_layout_t
{
    std::atomic_flag       m_locked;
    std::atomic<uint32_t>  m_owner_pid;
    std::atomic<uint64_t>  m_last_progress_ns;
};

spinlock_layout_t& access_layout(sintra::spinlock& lock)
{
    static_assert(std::is_standard_layout_v<sintra::spinlock>, "spinlock must be standard layout");
    static_assert(sizeof(spinlock_layout_t) == sizeof(sintra::spinlock), "spinlock layout mismatch");
    return *reinterpret_cast<spinlock_layout_t*>(&lock);
}

uint32_t find_dead_pid(uint32_t self_pid)
{
    for (uint32_t candidate = 500000; candidate < 510000; ++candidate) {
        if (candidate == self_pid)                { continue;         }
        if (!sintra::is_process_alive(candidate)) { return candidate; }
    }

    for (uint32_t candidate = self_pid + 1; candidate < self_pid + 10000; ++candidate) {
        if (!sintra::is_process_alive(candidate)) {
            return candidate;
        }
    }

    return 0;
}

enum class Child_state
{
    running,
    exited,
    error
};

class Exact_child
{
public:
    Exact_child() = default;
    Exact_child(const Exact_child&) = delete;
    Exact_child& operator=(const Exact_child&) = delete;

    ~Exact_child()
    {
        if (!is_settled()) {
            std::string ignored;
            (void)terminate_and_settle(ignored);
        }
    }

    bool spawn(const char* program, const char* const* argv)
    {
        sintra::Spawn_detached_options options;
        options.prog          = program;
        options.argv          = argv;
        options.child_pid_out = &m_pid;
#ifdef _WIN32
        options.child_process_handle_out = &m_handle;
        if (!sintra::spawn_detached(options)) {
            m_error = "CreateProcessW failed";
            return false;
        }
        if (m_pid <= 0 || !m_handle || m_handle == INVALID_HANDLE_VALUE) {
            m_error = "spawn returned without exact Windows process authority";
            return false;
        }
#else
        const auto result = sintra::detail::spawn_detached_with_result(options);
        m_pid = result.pid;
        if (!result.created() || m_pid <= 0) {
            m_error = "fork/exec failed";
            return false;
        }
        if (result.state == sintra::detail::Spawn_detached_result::State::created_reaped) {
            m_reaped          = true;
            m_status_available = result.wait_status_available;
            m_wait_status     = result.wait_status;
            if (!m_status_available) {
                m_error = "spawn observed an already-reaped child without exact wait status";
                return false;
            }
        }
#endif
        return true;
    }

    int pid() const noexcept { return m_pid; }

    Child_state poll()
    {
#ifdef _WIN32
        if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
            m_error = "exact Windows process handle is unavailable";
            return Child_state::error;
        }
        if (m_status_available) {
            return Child_state::exited;
        }

        const DWORD wait_result = WaitForSingleObject(m_handle, 0);
        if (wait_result == WAIT_TIMEOUT) {
            return Child_state::running;
        }
        if (wait_result != WAIT_OBJECT_0) {
            std::ostringstream message;
            message << "WaitForSingleObject failed while polling exact child (result "
                    << wait_result << ", error " << GetLastError() << ')';
            m_error = message.str();
            return Child_state::error;
        }

        DWORD exit_code = STILL_ACTIVE;
        if (!GetExitCodeProcess(m_handle, &exit_code)) {
            std::ostringstream message;
            message << "GetExitCodeProcess failed for signaled exact child (error "
                    << GetLastError() << ')';
            m_error = message.str();
            return Child_state::error;
        }
        if (exit_code == STILL_ACTIVE) {
            m_error = "signaled exact child still reported STILL_ACTIVE";
            return Child_state::error;
        }
        m_exit_code        = exit_code;
        m_status_available = true;
        return Child_state::exited;
#else
        if (m_reaped) {
            if (!m_status_available) {
                if (m_error.empty()) {
                    m_error = "exact POSIX child was reaped without wait status";
                }
                return Child_state::error;
            }
            return Child_state::exited;
        }
        if (m_pid <= 0) {
            m_error = "exact POSIX child pid is unavailable";
            return Child_state::error;
        }

        int status = 0;
        pid_t wait_result = -1;
        do {
            wait_result = ::waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
        }
        while (wait_result == -1 && errno == EINTR);

        if (wait_result == 0) {
            return Child_state::running;
        }
        if (wait_result == static_cast<pid_t>(m_pid)) {
            m_wait_status      = status;
            m_status_available = true;
            m_reaped           = true;
            return Child_state::exited;
        }

        std::ostringstream message;
        if (wait_result == -1 && errno == ECHILD) {
            message << "waitpid lost exact child authority (ECHILD)";
        }
        else if (wait_result == -1) {
            message << "waitpid failed for exact child (errno " << errno << ')';
        }
        else {
            message << "waitpid returned unexpected pid " << wait_result;
        }
        m_error = message.str();
        return Child_state::error;
#endif
    }

    bool terminate_and_settle(std::string& diagnostic)
    {
#ifdef _WIN32
        if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
            if (m_pid <= 0) {
                return true;
            }
            diagnostic = "cannot clean exact Windows child without its retained handle";
            return false;
        }

        const auto before = poll();
        bool terminated_by_cleanup = false;
        if (before != Child_state::exited) {
            if (!TerminateProcess(m_handle, k_cleanup_exit_code)) {
                const DWORD terminate_error = GetLastError();
                const auto  raced_state     = poll();
                if (raced_state != Child_state::exited) {
                    std::ostringstream message;
                    message << "TerminateProcess failed for exact child (error "
                            << terminate_error << "); " << m_error;
                    diagnostic = message.str();
                    return false;
                }
            }
            else {
                terminated_by_cleanup = true;
                const DWORD wait_result = WaitForSingleObject(
                    m_handle,
                    static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        k_child_cleanup_timeout).count()));
                if (wait_result != WAIT_OBJECT_0) {
                    std::ostringstream message;
                    message << "cleanup did not signal exact child (result "
                            << wait_result << ", error " << GetLastError() << ')';
                    diagnostic = message.str();
                    return false;
                }

                DWORD exit_code = STILL_ACTIVE;
                if (!GetExitCodeProcess(m_handle, &exit_code) || exit_code == STILL_ACTIVE) {
                    std::ostringstream message;
                    message << "cleanup could not obtain terminal exact-child status (error "
                            << GetLastError() << ')';
                    diagnostic = message.str();
                    return false;
                }
                m_exit_code        = exit_code;
                m_status_available = true;
            }
        }

        bool exact_cleanup_status = true;
        if (terminated_by_cleanup && m_exit_code != k_cleanup_exit_code) {
            std::ostringstream message;
            message << "cleanup exit code mismatch: expected " << k_cleanup_exit_code
                    << ", observed " << m_exit_code;
            diagnostic = message.str();
            exact_cleanup_status = false;
        }
        close_handle();
        return exact_cleanup_status;
#else
        if (m_pid <= 0) {
            return true;
        }
        if (m_reaped) {
            if (!m_status_available) {
                diagnostic = m_error.empty()
                    ? "exact POSIX child was reaped without wait status"
                    : m_error;
                return false;
            }
            return true;
        }

        const auto before = poll();
        if (before == Child_state::exited) {
            return true;
        }
        if (before == Child_state::error) {
            diagnostic = m_error;
            return false;
        }
        if (::kill(static_cast<pid_t>(m_pid), SIGKILL) == -1 && errno != ESRCH) {
            std::ostringstream message;
            message << "SIGKILL failed for exact child (errno " << errno << ')';
            diagnostic = message.str();
            return false;
        }

        int status = 0;
        pid_t wait_result = -1;
        do {
            wait_result = ::waitpid(static_cast<pid_t>(m_pid), &status, 0);
        }
        while (wait_result == -1 && errno == EINTR);

        if (wait_result != static_cast<pid_t>(m_pid)) {
            std::ostringstream message;
            if (wait_result == -1 && errno == ECHILD) {
                message << "cleanup lost exact child authority (ECHILD)";
            }
            else if (wait_result == -1) {
                message << "cleanup waitpid failed (errno " << errno << ')';
            }
            else {
                message << "cleanup waitpid returned unexpected pid " << wait_result;
            }
            diagnostic = message.str();
            return false;
        }

        m_wait_status      = status;
        m_status_available = true;
        m_reaped           = true;
        return true;
#endif
    }

    bool settle_observed_exit(std::string& diagnostic)
    {
        const auto state = poll();
        if (state != Child_state::exited) {
            diagnostic = state == Child_state::running
                ? "exact child is still running"
                : m_error;
            return false;
        }
#ifdef _WIN32
        close_handle();
#endif
        return true;
    }

    bool exited_as_expected_abort() const noexcept
    {
        if (!m_status_available) {
            return false;
        }
#ifdef _WIN32
        return m_exit_code == 3;
#else
        return WIFSIGNALED(m_wait_status) && WTERMSIG(m_wait_status) == SIGABRT;
#endif
    }

    std::string describe_status() const
    {
        if (!m_status_available) {
            return m_error.empty() ? "status unavailable" : m_error;
        }
        std::ostringstream message;
#ifdef _WIN32
        message << "Windows exit code " << m_exit_code;
#else
        if (WIFEXITED(m_wait_status)) {
            message << "normal POSIX exit code " << WEXITSTATUS(m_wait_status);
        }
        else if (WIFSIGNALED(m_wait_status)) {
            message << "POSIX signal " << WTERMSIG(m_wait_status);
        }
        else {
            message << "POSIX wait status " << m_wait_status;
        }
#endif
        return message.str();
    }

    const std::string& error() const noexcept { return m_error; }

private:
    bool is_settled() const noexcept
    {
#ifdef _WIN32
        return !m_handle || m_handle == INVALID_HANDLE_VALUE;
#else
        return m_pid <= 0 || m_reaped;
#endif
    }

#ifdef _WIN32
    void close_handle() noexcept
    {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }
#endif

    int         m_pid = -1;
    std::string m_error;
    bool        m_status_available = false;
#ifdef _WIN32
    HANDLE m_handle    = nullptr;
    DWORD  m_exit_code = STILL_ACTIVE;
#else
    bool m_reaped      = false;
    int  m_wait_status = 0;
#endif
};

[[noreturn]] void fail_after_settling_child(Exact_child& child, std::string message)
{
    std::string cleanup_diagnostic;
    if (!child.terminate_and_settle(cleanup_diagnostic)) {
        message += "; exact-child cleanup failed: ";
        message += cleanup_diagnostic;
    }
    sintra::test::fail(k_failure_prefix, message);
}

bool publish_ready_marker(
    const std::filesystem::path& marker_path,
    std::string_view             token)
{
    std::filesystem::path temporary_path = marker_path;
    temporary_path += ".tmp." + std::to_string(sintra::test::get_pid());

    FILE* output = std::fopen(temporary_path.string().c_str(), "wb");
    if (!output) {
        return false;
    }

    const bool wrote = std::fwrite(token.data(), 1, token.size(), output) == token.size();
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (!wrote || !flushed || !closed) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, marker_path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return false;
    }
    return true;
}

enum class Marker_state
{
    absent,
    valid,
    invalid,
    error
};

Marker_state probe_ready_marker(
    const std::filesystem::path& marker_path,
    std::string_view             expected_token,
    std::string&                 diagnostic)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(marker_path, exists_error);
    if (exists_error) {
        diagnostic = "marker existence check failed: " + exists_error.message();
        return Marker_state::error;
    }
    if (!exists) {
        return Marker_state::absent;
    }

    std::ifstream input(marker_path, std::ios::binary);
    if (!input) {
        diagnostic = "ready marker exists but could not be opened";
        return Marker_state::error;
    }
    const std::string observed{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        diagnostic = "ready marker could not be read completely";
        return Marker_state::error;
    }
    if (observed != expected_token) {
        diagnostic = "ready marker token mismatch";
        return Marker_state::invalid;
    }
    return Marker_state::valid;
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

    if (argc >= 5 && std::string_view(argv[1]) == "--spinlock-stall-child") {
        sintra::detail::set_debug_pause_active(false);
        sintra::test::prepare_for_intentional_crash();
        if (std::signal(SIGABRT, SIG_DFL) == SIG_ERR) {
            return 2;
        }
#ifdef _WIN32
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        if (_set_error_mode(_OUT_TO_STDERR) == -1) {
            return 2;
        }
#endif
        const uint32_t owner_pid = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
        const std::filesystem::path marker_path(argv[3]);
        const std::string_view      marker_token(argv[4]);
        sintra::spinlock stall_lock;
        auto& stall_layout = access_layout(stall_lock);
        stall_layout.m_locked.clear(std::memory_order_release);
        stall_layout.m_locked.test_and_set(std::memory_order_acquire);
        stall_layout.m_owner_pid.store(owner_pid, std::memory_order_release);
        stall_layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
        if (!publish_ready_marker(marker_path, marker_token)) {
            return 2;
        }
        stall_lock.lock();
        return 1;
    }

    sintra::spinlock lock;
    auto& layout = access_layout(lock);

    const uint32_t self_pid = static_cast<uint32_t>(sintra::detail::get_current_process_id());

    // Case 1: recover from a dead owner.
    const uint32_t dead_pid = find_dead_pid(self_pid);
    sintra::test::require_true(dead_pid != 0 && dead_pid != self_pid, k_failure_prefix,
        "failed to locate a dead pid");
    sintra::test::require_true(!sintra::is_process_alive(dead_pid), k_failure_prefix,
        "dead pid should not be alive");

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(dead_pid, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    lock.lock();
    lock.unlock();

    // Case 2: live owner with debug pause active should force unlock.
    const std::string sleep_arg = "5000";
    const std::vector<const char*> sleep_args = {
        argv[0],
        "--spinlock-sleeper",
        sleep_arg.c_str(),
        nullptr
    };
    Exact_child sleep_child;
    if (!sleep_child.spawn(argv[0], sleep_args.data())) {
        fail_after_settling_child(
            sleep_child,
            "case 2 failed to spawn exact live-owner child: " + sleep_child.error());
    }
    const auto sleep_child_state = sleep_child.poll();
    if (sleep_child_state != Child_state::running) {
        fail_after_settling_child(
            sleep_child,
            "case 2 child was not authoritatively live before owner assignment: " +
                (sleep_child_state == Child_state::exited
                    ? sleep_child.describe_status()
                    : sleep_child.error()));
    }
    const int child_pid = sleep_child.pid();

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(static_cast<uint32_t>(child_pid), std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    sintra::detail::set_debug_pause_active(true);
    lock.lock();
    lock.unlock();
    sintra::detail::set_debug_pause_active(false);

    const auto post_recovery_child_state = sleep_child.poll();
    if (post_recovery_child_state != Child_state::running) {
        fail_after_settling_child(
            sleep_child,
            "case 2 did not force-unlock while the exact owner remained live: " +
                (post_recovery_child_state == Child_state::exited
                    ? sleep_child.describe_status()
                    : sleep_child.error()));
    }

    std::string sleep_cleanup_diagnostic;
    if (!sleep_child.terminate_and_settle(sleep_cleanup_diagnostic)) {
        fail_after_settling_child(
            sleep_child,
            "case 2 exact-child cleanup failed: " + sleep_cleanup_diagnostic);
    }

    // Case 3: live owner with debug pause inactive should abort (report_live_owner_stall).
    const auto marker_directory = sintra::test::unique_scratch_directory(
        "spinlock_recovery_stall");
    const auto marker_nonce = sintra::monotonic_now_ns();
    const auto marker_path = marker_directory /
        ("stall-ready-" + std::to_string(self_pid) + '-' +
            std::to_string(marker_nonce) + ".marker");
    const std::string marker_token =
        "spinlock-stall-ready:" + std::to_string(self_pid) + ':' +
        std::to_string(marker_nonce);
    const std::string owner_arg = std::to_string(self_pid);
    const std::string marker_arg = marker_path.string();
    const std::vector<const char*> stall_args = {
        argv[0],
        "--spinlock-stall-child",
        owner_arg.c_str(),
        marker_arg.c_str(),
        marker_token.c_str(),
        nullptr
    };
    Exact_child stall_child;
    if (!stall_child.spawn(argv[0], stall_args.data())) {
        fail_after_settling_child(
            stall_child,
            "case 3 failed to spawn exact stall child: " + stall_child.error());
    }

    const auto marker_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (true) {
        std::string marker_diagnostic;
        auto marker_state = probe_ready_marker(marker_path, marker_token, marker_diagnostic);
        if (marker_state == Marker_state::valid) {
            break;
        }
        if (marker_state == Marker_state::invalid || marker_state == Marker_state::error) {
            fail_after_settling_child(
                stall_child,
                "case 3 readiness-marker failure: " + marker_diagnostic);
        }

        const auto child_state = stall_child.poll();
        if (child_state == Child_state::exited) {
            marker_state = probe_ready_marker(marker_path, marker_token, marker_diagnostic);
            if (marker_state == Marker_state::valid) {
                break;
            }
            fail_after_settling_child(
                stall_child,
                "case 3 child exited before publishing its readiness marker: " +
                    stall_child.describe_status());
        }
        if (child_state == Child_state::error) {
            fail_after_settling_child(
                stall_child,
                "case 3 exact-child observation failed before readiness: " +
                    stall_child.error());
        }
        if (std::chrono::steady_clock::now() >= marker_deadline) {
            fail_after_settling_child(
                stall_child,
                "case 3 child did not publish its readiness marker within 15 seconds");
        }
        std::this_thread::sleep_for(k_child_poll_interval);
    }

    constexpr int stall_timeout_default_ms = 10000;
    int stall_timeout_ms = sintra::test::read_env_int(
        "SINTRA_SPINLOCK_STALL_TIMEOUT_MS",
        stall_timeout_default_ms);
    if (stall_timeout_ms <= 0) {
        stall_timeout_ms = stall_timeout_default_ms;
    }
    const auto stall_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(stall_timeout_ms);
    while (true) {
        const auto child_state = stall_child.poll();
        if (child_state == Child_state::exited) {
            if (!stall_child.exited_as_expected_abort()) {
                const auto observed = stall_child.describe_status();
                fail_after_settling_child(
                    stall_child,
                    "case 3 stall child terminated with unexpected status: " + observed);
            }
            std::string settle_diagnostic;
            if (!stall_child.settle_observed_exit(settle_diagnostic)) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 could not settle the expected exact child exit: " +
                        settle_diagnostic);
            }
            break;
        }
        if (child_state == Child_state::error) {
            fail_after_settling_child(
                stall_child,
                "case 3 exact-child observation failed after readiness: " +
                    stall_child.error());
        }
        if (std::chrono::steady_clock::now() >= stall_deadline) {
            fail_after_settling_child(
                stall_child,
                "case 3 ready stall child did not terminate within " +
                    std::to_string(stall_timeout_ms) + " ms");
        }
        std::this_thread::sleep_for(k_child_poll_interval);
    }

    std::error_code cleanup_error;
    std::filesystem::remove(marker_path, cleanup_error);
    cleanup_error.clear();
    std::filesystem::remove(marker_directory, cleanup_error);

    return 0;
}
