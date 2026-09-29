// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <string>

namespace sintra { namespace test {

class Windows_liveness_child
{
public:
    explicit Windows_liveness_child(const wchar_t* mode)
    {
        wchar_t executable[32768];
        const DWORD length = ::GetModuleFileNameW(nullptr, executable, 32768);
        if (length == 0 || length >= 32768) {
            return;
        }
        std::wstring command = L"\"" + std::wstring(executable, length) + L"\" " + mode;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &m_process))
        {
            ::CloseHandle(m_process.hThread);
            m_process.hThread = nullptr;
        }
    }

    Windows_liveness_child(const Windows_liveness_child&) = delete;
    Windows_liveness_child& operator=(const Windows_liveness_child&) = delete;

    ~Windows_liveness_child()
    {
        if (m_process.hProcess) {
            if (::WaitForSingleObject(m_process.hProcess, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(m_process.hProcess, 1);
                ::WaitForSingleObject(m_process.hProcess, 5000);
            }
            ::CloseHandle(m_process.hProcess);
        }
    }

    bool valid() const { return m_process.hProcess != nullptr; }
    uint32_t pid() const { return m_process.dwProcessId; }

    bool terminate(DWORD code)
    {
        return ::TerminateProcess(m_process.hProcess, code) &&
            ::WaitForSingleObject(m_process.hProcess, 5000) == WAIT_OBJECT_0;
    }

private:
    PROCESS_INFORMATION m_process{};
};

}} // namespace sintra::test
#endif
