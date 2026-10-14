#pragma once

#include <sintra/detail/utility.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sintra::test {

enum class Exact_child_state
{
    running,
    exited,
    error
};

#ifdef _WIN32
namespace detail {

inline std::string describe_windows_exit_code(DWORD exit_code)
{
    std::ostringstream message;
    message << "0x" << std::uppercase << std::hex << std::setw(8)
            << std::setfill('0') << static_cast<std::uint32_t>(exit_code)
            << " (unsigned " << std::dec << static_cast<std::uint32_t>(exit_code) << ')';
    return message.str();
}

} // namespace detail
#endif

class Exact_child
{
public:
    explicit Exact_child(std::chrono::milliseconds cleanup_timeout) noexcept
        : m_cleanup_timeout(cleanup_timeout)
    {}

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
        Spawn_detached_options options;
        options.prog          = program;
        options.argv          = argv;
        options.child_pid_out = &m_pid;
#ifdef _WIN32
        options.child_process_handle_out = &m_handle;
        if (!spawn_detached(options)) {
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
            m_reaped           = true;
            m_status_available = result.wait_status_available;
            m_wait_status      = result.wait_status;
            if (!m_status_available) {
                m_error = "spawn observed an already-reaped child without exact wait status";
                return false;
            }
        }
#endif
        return true;
    }

    int pid() const noexcept { return m_pid; }

    Exact_child_state poll()
    {
#ifdef _WIN32
        if (!m_handle || m_handle == INVALID_HANDLE_VALUE) {
            m_error = "exact Windows process handle is unavailable";
            return Exact_child_state::error;
        }
        if (m_status_available) {
            return Exact_child_state::exited;
        }

        const DWORD wait_result = WaitForSingleObject(m_handle, 0);
        if (wait_result == WAIT_TIMEOUT) {
            return Exact_child_state::running;
        }
        if (wait_result != WAIT_OBJECT_0) {
            std::ostringstream message;
            message << "WaitForSingleObject failed while polling exact child (result "
                    << wait_result << ", error " << GetLastError() << ')';
            m_error = message.str();
            return Exact_child_state::error;
        }

        DWORD exit_code = STILL_ACTIVE;
        if (!GetExitCodeProcess(m_handle, &exit_code)) {
            std::ostringstream message;
            message << "GetExitCodeProcess failed for signaled exact child (error "
                    << GetLastError() << ')';
            m_error = message.str();
            return Exact_child_state::error;
        }
        if (exit_code == STILL_ACTIVE) {
            m_error = "signaled exact child still reported STILL_ACTIVE";
            return Exact_child_state::error;
        }
        m_exit_code        = exit_code;
        m_status_available = true;
        return Exact_child_state::exited;
#else
        if (m_reaped) {
            if (!m_status_available) {
                if (m_error.empty()) {
                    m_error = "exact POSIX child was reaped without wait status";
                }
                return Exact_child_state::error;
            }
            return Exact_child_state::exited;
        }
        if (m_pid <= 0) {
            m_error = "exact POSIX child pid is unavailable";
            return Exact_child_state::error;
        }

        int status = 0;
        pid_t wait_result = -1;
        do {
            wait_result = ::waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
        }
        while (wait_result == -1 && errno == EINTR);

        if (wait_result == 0) {
            return Exact_child_state::running;
        }
        if (wait_result == static_cast<pid_t>(m_pid)) {
            m_wait_status      = status;
            m_status_available = true;
            m_reaped           = true;
            return Exact_child_state::exited;
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
        return Exact_child_state::error;
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
        if (before != Exact_child_state::exited) {
            if (!TerminateProcess(m_handle, k_cleanup_exit_code)) {
                const DWORD terminate_error = GetLastError();
                const auto  raced_state     = poll();
                if (raced_state != Exact_child_state::exited) {
                    std::ostringstream message;
                    message << "TerminateProcess failed for exact child (error "
                            << terminate_error << "); " << m_error;
                    diagnostic = message.str();
                    return false;
                }
            }
            else {
                terminated_by_cleanup = true;
                const DWORD wait_result = WaitForSingleObject(m_handle, cleanup_timeout_ms());
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
            message << "cleanup exit code mismatch: expected "
                    << detail::describe_windows_exit_code(k_cleanup_exit_code)
                    << ", observed "
                    << detail::describe_windows_exit_code(m_exit_code);
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
        if (before == Exact_child_state::exited) {
            return true;
        }
        if (before == Exact_child_state::error) {
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
        if (state != Exact_child_state::exited) {
            diagnostic = state == Exact_child_state::running
                ? "exact child is still running"
                : m_error;
            return false;
        }
#ifdef _WIN32
        close_handle();
#endif
        return true;
    }

    bool exited_with_code(std::uint32_t expected_code) const noexcept
    {
        if (!m_status_available) {
            return false;
        }
#ifdef _WIN32
        return static_cast<std::uint32_t>(m_exit_code) == expected_code;
#else
        return WIFEXITED(m_wait_status) &&
            static_cast<std::uint32_t>(WEXITSTATUS(m_wait_status)) == expected_code;
#endif
    }

    bool exited_from_signal(int expected_signal) const noexcept
    {
#ifdef _WIN32
        (void)expected_signal;
        return false;
#else
        return m_status_available && WIFSIGNALED(m_wait_status) &&
            WTERMSIG(m_wait_status) == expected_signal;
#endif
    }

    std::string describe_status() const
    {
        if (!m_status_available) {
            return m_error.empty() ? "status unavailable" : m_error;
        }
        std::ostringstream message;
#ifdef _WIN32
        message << "Windows exit code " << detail::describe_windows_exit_code(m_exit_code);
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
    static constexpr DWORD k_cleanup_exit_code = 0x53434c4b;

    DWORD cleanup_timeout_ms() const noexcept
    {
        using rep = std::chrono::milliseconds::rep;
        const auto count = m_cleanup_timeout.count();
        if (count <= 0) {
            return 0;
        }
        constexpr auto max_wait = static_cast<rep>((std::numeric_limits<DWORD>::max)() - 1);
        return static_cast<DWORD>((std::min)(count, max_wait));
    }

    void close_handle() noexcept
    {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }
#endif

    std::chrono::milliseconds m_cleanup_timeout;
    int                       m_pid = -1;
    std::string               m_error;
    bool                      m_status_available = false;
#ifdef _WIN32
    HANDLE m_handle    = nullptr;
    DWORD  m_exit_code = STILL_ACTIVE;
#else
    bool m_reaped      = false;
    int  m_wait_status = 0;
#endif
};

} // namespace sintra::test
