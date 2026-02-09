// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

#ifdef _WIN32
  #include "../sintra_windows.h"
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace sintra {

namespace detail {

#ifdef _WIN32
using native_file_handle = HANDLE;

inline native_file_handle invalid_file() noexcept
{
    return INVALID_HANDLE_VALUE;
}

inline native_file_handle create_new_file(const char* path)
{
    SECURITY_ATTRIBUTES  sa{};
    SECURITY_DESCRIPTOR  sd{};
    SECURITY_ATTRIBUTES* psa = nullptr;

    if (::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        ::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
        psa = &sa;
    }

    return ::CreateFileA(path,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         psa,
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

inline bool truncate_file(native_file_handle handle, std::uint64_t size)
{
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    return ::SetEndOfFile(handle) != 0;
}

inline bool write_file(native_file_handle handle, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::byte*>(data);
    while (size > 0) {
        const DWORD chunk = size > std::numeric_limits<DWORD>::max()
                                ? std::numeric_limits<DWORD>::max()
                                : static_cast<DWORD>(size);
        DWORD written = 0;
        if (!::WriteFile(handle, bytes, chunk, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += written;
        size -= written;
    }
    return true;
}

inline bool close_file(native_file_handle handle)
{
    return ::CloseHandle(handle) != 0;
}
#else
using native_file_handle = int;

inline native_file_handle invalid_file() noexcept
{
    return -1;
}

inline native_file_handle create_new_file(const char* path)
{
    native_file_handle fd = ::open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) {
        (void)::fchmod(fd, 0666);
    }
    return fd;
}

inline bool truncate_file(native_file_handle handle, std::uint64_t size)
{
    return ::ftruncate(handle, static_cast<off_t>(size)) == 0;
}

inline bool write_file(native_file_handle handle, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::byte*>(data);
    while (size > 0) {
        ssize_t written = ::write(handle, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

inline bool close_file(native_file_handle handle)
{
    return ::close(handle) == 0;
}
#endif

} // namespace detail

inline bool check_or_create_directory(const std::string& dir_name)
{
    std::error_code ec;
    std::filesystem::path ps(dir_name);
    if (!std::filesystem::exists(ps)) {
        return std::filesystem::create_directory(ps, ec);
    }
    if (std::filesystem::is_regular_file(ps)) {
        (void)std::filesystem::remove(ps, ec);
        return std::filesystem::create_directory(ps, ec);
    }
    return true;
}

inline bool remove_directory(const std::string& dir_name)
{
    std::error_code ec;
    auto removed = std::filesystem::remove_all(dir_name.c_str(), ec);
    if (ec) {
        return false;
    }
    return removed > 0;
}

} // namespace sintra
