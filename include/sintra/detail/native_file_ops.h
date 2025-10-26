#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <limits>
#include <algorithm>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace sintra {
namespace detail {
namespace native_file_ops {

#ifdef _WIN32
using handle_type = HANDLE;

inline handle_type invalid_handle() noexcept
{
    return INVALID_HANDLE_VALUE;
}

inline handle_type create_new_file(const std::string& path)
{
    return ::CreateFileA(path.c_str(),
                         GENERIC_READ | GENERIC_WRITE,
                         0,
                         nullptr,
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

inline bool truncate_file(handle_type handle, std::uintmax_t size)
{
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!::SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) {
        return false;
    }
    if (!::SetEndOfFile(handle)) {
        return false;
    }
    return true;
}

inline bool write_file(handle_type handle, const void* data, std::size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!::WriteFile(handle, ptr, chunk, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        ptr += written;
        size -= written;
    }
    return true;
}

inline bool close_file(handle_type handle) noexcept
{
    if (handle == invalid_handle()) {
        return true;
    }
    return ::CloseHandle(handle) != 0;
}

#else
using handle_type = int;

inline handle_type invalid_handle() noexcept
{
    return -1;
}

inline handle_type create_new_file(const std::string& path)
{
    return ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, static_cast<mode_t>(0666));
}

inline bool truncate_file(handle_type handle, std::uintmax_t size)
{
    return ::ftruncate(handle, static_cast<off_t>(size)) == 0;
}

inline bool write_file(handle_type handle, const void* data, std::size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t written = ::write(handle, ptr, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        ptr += static_cast<std::size_t>(written);
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

inline bool close_file(handle_type handle) noexcept
{
    if (handle == invalid_handle()) {
        return true;
    }
    return ::close(handle) == 0;
}

#endif

} // namespace native_file_ops
} // namespace detail
} // namespace sintra

