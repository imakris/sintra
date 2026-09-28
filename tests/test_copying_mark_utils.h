// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <sintra/detail/ipc/process_utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace sintra::test::copying_mark {

inline void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
bool wait_until(Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

inline void signal_file(const std::filesystem::path& path, const std::string& value = "ready")
{
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary);
    file << value;
    file.close();
    require(bool(file), "cannot publish child synchronization file");
    std::filesystem::rename(temporary, path);
}

inline void wait_for_file(const std::filesystem::path& path)
{
    require(wait_until([&]() { return std::filesystem::exists(path); }),
        "child did not reach the required protocol stage");
}

class Test_child
{
public:
    Test_child(const std::string& executable, std::vector<std::string> arguments)
    {
#ifdef _WIN32
        auto quote = [](const std::wstring& value) {
            std::wstring quoted = L"\"";
            size_t slashes = 0;
            for (const wchar_t character : value) {
                if (character == L'\\') {
                    ++slashes;
                    continue;
                }
                quoted.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
                quoted += character;
                slashes = 0;
            }
            quoted.append(2 * slashes, L'\\');
            return quoted + L'"';
        };
        const auto executable_path = std::filesystem::absolute(executable).wstring();
        std::wstring command = quote(executable_path);
        for (const auto& argument : arguments) {
            command += L' ' + quote(std::filesystem::path(argument).wstring());
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(::CreateProcessW(executable_path.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "CreateProcessW failed");
        ::CloseHandle(process.hThread);
        m_process = process.hProcess;
        m_pid = process.dwProcessId;
#else
        arguments.insert(arguments.begin(), std::filesystem::absolute(executable).string());
        std::vector<char*> argv;
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const auto child = ::fork();
        require(child >= 0, "fork failed");
        if (child == 0) {
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        m_pid = static_cast<uint32_t>(child);
#endif
    }

    ~Test_child()
    {
#ifdef _WIN32
        if (m_process) {
            ::TerminateProcess(m_process, 1);
            ::WaitForSingleObject(m_process, 5000);
            ::CloseHandle(m_process);
        }
#else
        if (!m_reaped) {
            ::kill(static_cast<pid_t>(m_pid), SIGKILL);
            int status = 0;
            while (::waitpid(static_cast<pid_t>(m_pid), &status, 0) < 0 && errno == EINTR) {}
        }
#endif
    }

    Test_child(const Test_child&) = delete;
    Test_child& operator=(const Test_child&) = delete;

    uint32_t pid() const { return m_pid; }

    void terminate(unsigned exit_code = 0)
    {
#ifdef _WIN32
        require(::TerminateProcess(m_process, exit_code), "TerminateProcess failed");
        require(::WaitForSingleObject(m_process, 5000) == WAIT_OBJECT_0,
            "terminated child did not become signaled");
#else
        (void)exit_code;
        require(::kill(static_cast<pid_t>(m_pid), SIGKILL) == 0, "SIGKILL failed");
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid(static_cast<pid_t>(m_pid), &status, 0);
        }
        while (waited < 0 && errno == EINTR);
        m_reaped = waited == static_cast<pid_t>(m_pid);
        require(m_reaped && WIFSIGNALED(status), "killed child was not reaped");
#endif
    }

    void close()
    {
#ifdef _WIN32
        require(::CloseHandle(m_process), "closing child process handle failed");
        m_process = nullptr;
#endif
    }

#ifndef _WIN32
    // Kills the child and waits until it has exited, leaving it a zombie.
    void kill_unreaped()
    {
        require(::kill(static_cast<pid_t>(m_pid), SIGKILL) == 0, "SIGKILL failed");
        siginfo_t info{};
        int result;
        do {
            result = ::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOWAIT);
        }
        while (result < 0 && errno == EINTR);
        require(result == 0 && info.si_pid == static_cast<pid_t>(m_pid), "killed child did not become a zombie");
    }

    // Whether the child has exited and is still waiting to be reaped.
    bool unreaped() const
    {
        siginfo_t info{};
        return !m_reaped &&
            ::waitid(P_PID, static_cast<id_t>(m_pid), &info, WEXITED | WNOWAIT | WNOHANG) == 0 &&
            info.si_pid == static_cast<pid_t>(m_pid);
    }
#endif

private:
    uint32_t m_pid = 0;
#ifdef _WIN32
    HANDLE m_process = nullptr;
#else
    bool m_reaped = false;
#endif
};

} // namespace sintra::test::copying_mark
