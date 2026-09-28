// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include "../process/process_id.h"
#include "private_resources.h"

#if defined(SINTRA_ENABLE_TEST_HOOKS)
#include <functional>
#endif

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

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline std::function<void(const std::filesystem::path&)> before_directory_create_for_test;
// Runs once publish_private_file has written all of the contents under their
// staged name, before the staged file takes the published name.
inline std::function<void(const std::filesystem::path& staged, const std::filesystem::path& path)>
    after_private_file_staged_for_test;
#endif

enum class publish_file_result
{
    published,
    already_exists,
    failed
};

#ifdef _WIN32
using native_file_handle = HANDLE;

inline native_file_handle invalid_file() noexcept
{
    return INVALID_HANDLE_VALUE;
}

inline native_file_handle create_new_file(const char* path)
{
    Private_security security;

    return ::CreateFileA(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        security.attributes(),
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
        if (!::WriteFile(handle, bytes, chunk, &written, nullptr)) { return false; }
        if (written == 0)                                          { return false; }
        bytes += written;
        size -= written;
    }
    return true;
}

inline bool close_file(native_file_handle handle)
{
    return ::CloseHandle(handle) != 0;
}

inline publish_file_result publish_file_if_absent(
    const std::filesystem::path& source,
    const std::filesystem::path& target)
{
    if (::MoveFileExW(
            source.wstring().c_str(),
            target.wstring().c_str(),
            MOVEFILE_WRITE_THROUGH))
    {
        return publish_file_result::published;
    }

    const DWORD error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return publish_file_result::already_exists;
    }

    return publish_file_result::failed;
}
#else
using native_file_handle = int;

inline native_file_handle invalid_file() noexcept
{
    return -1;
}

inline native_file_handle create_new_file(const char* path)
{
    return ::open(path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
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

inline publish_file_result publish_file_if_absent(
    const std::filesystem::path& source,
    const std::filesystem::path& target)
{
    int rc;
    do {
        rc = ::link(source.c_str(), target.c_str());
    }
    while (rc != 0 && errno == EINTR);

    if (rc == 0) {
        return publish_file_result::published;
    }
    if (errno == EEXIST) {
        return publish_file_result::already_exists;
    }
    return publish_file_result::failed;
}
#endif

inline bool write_private_file(const std::filesystem::path& path, const std::string& contents)
{
    try {
        const auto created = create_new_file(path.string().c_str());
        if (created != invalid_file()) {
            const bool written = write_file(created, contents.data(), contents.size());
            const bool closed = close_file(created);
            return written && closed;
        }
        auto existing = open_private_file(path, sintra::ipc::read_write);
        return truncate_file(existing.native_handle(), 0) &&
            write_file(existing.native_handle(), contents.data(), contents.size());
    }
    catch (...) {
        return false;
    }
}

// Publishes contents as the private file at path in one step: a reader finds no
// file or all of the contents, never part of them. They are written to a private
// file beside path, which then takes path's name and keeps its owner and access.
// Replacing an existing file works on POSIX; on Windows it fails while another
// process has that file open without delete sharing, so publish each path once.
inline bool publish_private_file(const std::filesystem::path& path, const std::string& contents)
{
    try {
        static std::atomic<std::uint64_t> s_staged_files{0};
        auto staged = path;
        staged += ".staged." + std::to_string(get_current_process_id()) + '.' +
            std::to_string(++s_staged_files);
        const auto created = create_new_file(staged.string().c_str());
        if (created == invalid_file()) {
            return false;
        }
        const bool written = write_file(created, contents.data(), contents.size());
        bool published = close_file(created) && written;
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        if (published && after_private_file_staged_for_test) {
            after_private_file_staged_for_test(staged, path);
        }
#endif
#ifdef _WIN32
        published = published && ::MoveFileExW(staged.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        published = published && ::rename(staged.c_str(), path.c_str()) == 0;
#endif
        if (!published) {
            std::error_code ignored;
            std::filesystem::remove(staged, ignored);
        }
        return published;
    }
    catch (...) {
        return false;
    }
}

} // namespace detail

inline bool check_or_create_directory(
    const std::string& dir_name,
    std::error_code*   out_error = nullptr)
{
    std::error_code ec;
    if (out_error) {
        out_error->clear();
    }
    std::filesystem::path ps(dir_name);
    const auto create = [&]() {
#if defined(SINTRA_ENABLE_TEST_HOOKS)
        if (detail::before_directory_create_for_test) {
            detail::before_directory_create_for_test(ps);
        }
#endif
        // A concurrent creator may have won after our existence check.
        // create_directory returns false with no error for an existing directory.
        (void)std::filesystem::create_directory(ps, ec);
        if (out_error) {
            *out_error = ec;
        }
        return !ec;
    };
    if (!std::filesystem::exists(ps)) {
        return create();
    }
    if (std::filesystem::is_regular_file(ps)) {
        (void)std::filesystem::remove(ps, ec);
        return create();
    }
    return true;
}

} // namespace sintra
