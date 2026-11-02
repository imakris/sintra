// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sintra::test {

// Redirects the native standard error handle, which spinlock diagnostics use.
class Native_stderr_capture
{
public:
    explicit Native_stderr_capture(const std::filesystem::path& path)
    {
        std::fflush(stderr);
#ifdef _WIN32
        m_saved = ::GetStdHandle(STD_ERROR_HANDLE);
        m_file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        m_active = m_file != INVALID_HANDLE_VALUE &&
            ::SetStdHandle(STD_ERROR_HANDLE, m_file) != FALSE;
#else
        const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (file >= 0) {
            m_saved = ::dup(STDERR_FILENO);
            m_active = m_saved >= 0 && ::dup2(file, STDERR_FILENO) >= 0;
            ::close(file);
        }
#endif
    }

    ~Native_stderr_capture()
    {
#ifdef _WIN32
        if (m_active) {
            ::SetStdHandle(STD_ERROR_HANDLE, m_saved);
        }
        if (m_file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_file);
        }
#else
        if (m_saved >= 0) {
            ::dup2(m_saved, STDERR_FILENO);
            ::close(m_saved);
        }
#endif
    }

    Native_stderr_capture(const Native_stderr_capture&) = delete;
    Native_stderr_capture& operator=(const Native_stderr_capture&) = delete;

    bool active() const { return m_active; }

private:
#ifdef _WIN32
    HANDLE m_saved = nullptr;
    HANDLE m_file = INVALID_HANDLE_VALUE;
#else
    int m_saved = -1;
#endif
    bool m_active = false;
};

} // namespace sintra::test
