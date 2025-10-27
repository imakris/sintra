#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
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
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace sintra::detail::ipc
{

enum mode_t
{
    read_only,
    read_write,
    copy_on_write
};

#if defined(_WIN32)
using map_options_t = unsigned long;
constexpr map_options_t default_map_options = static_cast<map_options_t>(-1);
#else
using map_options_t = int;
constexpr map_options_t default_map_options = static_cast<map_options_t>(-1);
#endif

class file_mapping
{
public:
    file_mapping(const char* path, mode_t mode)
        : m_mode(mode)
    {
        if (!path) {
            throw std::invalid_argument("file_mapping path cannot be null");
        }

#if defined(_WIN32)
        std::wstring wide_path = utf8_to_wide(path);

        DWORD desired_access = 0;
        DWORD protect = 0;
        switch (mode) {
        case read_only:
            desired_access = GENERIC_READ;
            protect = PAGE_READONLY;
            break;
        case read_write:
            desired_access = GENERIC_READ | GENERIC_WRITE;
            protect = PAGE_READWRITE;
            break;
        case copy_on_write:
            desired_access = GENERIC_READ;
            protect = PAGE_WRITECOPY;
            break;
        default:
            throw std::invalid_argument("invalid file_mapping mode");
        }

        HANDLE file = ::CreateFileW(wide_path.c_str(), desired_access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateFileW");
        }

        LARGE_INTEGER file_size{};
        if (!::GetFileSizeEx(file, &file_size)) {
            DWORD err = ::GetLastError();
            ::CloseHandle(file);
            throw std::system_error(err, std::system_category(), "GetFileSizeEx");
        }
        if (file_size.QuadPart < 0) {
            ::CloseHandle(file);
            throw std::runtime_error("negative file size");
        }

        m_file = file;
        m_size = static_cast<std::uint64_t>(file_size.QuadPart);

        HANDLE mapping = ::CreateFileMappingW(m_file, nullptr, protect, 0, 0, nullptr);
        if (!mapping) {
            DWORD err = ::GetLastError();
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
            throw std::system_error(err, std::system_category(), "CreateFileMappingW");
        }
        m_mapping = mapping;
#else
        int flags = 0;
        switch (mode) {
        case read_only:
            flags = O_RDONLY;
            break;
        case read_write:
            flags = O_RDWR;
            break;
        case copy_on_write:
            flags = O_RDONLY;
            break;
        default:
            throw std::invalid_argument("invalid file_mapping mode");
        }

        int fd = ::open(path, flags);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "open");
        }

        struct stat st;
        if (::fstat(fd, &st) != 0) {
            int err = errno;
            ::close(fd);
            throw std::system_error(err, std::system_category(), "fstat");
        }

        if (st.st_size < 0) {
            ::close(fd);
            throw std::runtime_error("negative file size");
        }

        m_fd = fd;
        m_size = static_cast<std::uint64_t>(st.st_size);
#endif
    }

    file_mapping(file_mapping&& other) noexcept
        : m_mode(other.m_mode), m_size(other.m_size)
    {
#if defined(_WIN32)
        m_file = other.m_file;
        m_mapping = other.m_mapping;
        other.m_file = INVALID_HANDLE_VALUE;
        other.m_mapping = nullptr;
#else
        m_fd = other.m_fd;
        other.m_fd = -1;
#endif
        other.m_size = 0;
    }

    file_mapping& operator=(file_mapping&& other) noexcept
    {
        if (this != &other) {
            close();
            m_mode = other.m_mode;
            m_size = other.m_size;
#if defined(_WIN32)
            m_file = other.m_file;
            m_mapping = other.m_mapping;
            other.m_file = INVALID_HANDLE_VALUE;
            other.m_mapping = nullptr;
#else
            m_fd = other.m_fd;
            other.m_fd = -1;
#endif
            other.m_size = 0;
        }
        return *this;
    }

    ~file_mapping()
    {
        close();
    }

    file_mapping(const file_mapping&) = delete;
    file_mapping& operator=(const file_mapping&) = delete;

#if defined(_WIN32)
    HANDLE native_handle() const { return m_mapping; }
    HANDLE file_handle() const { return m_file; }
#else
    int native_handle() const { return m_fd; }
#endif

    std::uint64_t size() const { return m_size; }
    mode_t mode() const { return m_mode; }

private:
    void close() noexcept
    {
#if defined(_WIN32)
        if (m_mapping) {
            ::CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        if (m_file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
        m_size = 0;
    }

#if defined(_WIN32)
    static std::wstring utf8_to_wide(const char* path)
    {
        if (!path) {
            return std::wstring();
        }

        int required = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (required == 0) {
            throw std::system_error(::GetLastError(), std::system_category(), "MultiByteToWideChar");
        }

        std::wstring buffer(static_cast<std::size_t>(required), L'\0');
        int converted = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, buffer.data(), required);
        if (converted == 0) {
            throw std::system_error(::GetLastError(), std::system_category(), "MultiByteToWideChar");
        }
        // Remove the terminating null written by MultiByteToWideChar
        if (!buffer.empty() && buffer.back() == L'\0') {
            buffer.pop_back();
        }
        return buffer;
    }
#endif

private:
    mode_t m_mode;
    std::uint64_t m_size = 0;
#if defined(_WIN32)
    HANDLE m_file = INVALID_HANDLE_VALUE;
    HANDLE m_mapping = nullptr;
#else
    int m_fd = -1;
#endif
};

class mapped_region
{
public:
    mapped_region(const file_mapping& mapping,
                  mode_t mode,
                  std::uint64_t offset = 0,
                  std::size_t length = 0,
                  const void* address = nullptr,
                  map_options_t map_options = default_map_options)
        : m_mode(mode)
    {
        if (offset > mapping.size()) {
            throw std::out_of_range("offset exceeds mapped file size");
        }

        std::uint64_t available = mapping.size() - offset;
        std::uint64_t desired = (length == 0) ? available : static_cast<std::uint64_t>(length);
        if (desired > available) {
            throw std::out_of_range("requested mapping exceeds file size");
        }
        if (desired == 0) {
            throw std::invalid_argument("mapped_region length must be non-zero");
        }
        if (desired > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw std::length_error("mapped_region length too large for size_t");
        }

        m_size = static_cast<std::size_t>(desired);

#if defined(_WIN32)
        DWORD desired_access = 0;
        switch (mode) {
        case read_only:
            desired_access = FILE_MAP_READ;
            break;
        case read_write:
            desired_access = FILE_MAP_READ | FILE_MAP_WRITE;
            break;
        case copy_on_write:
            desired_access = FILE_MAP_COPY;
            break;
        default:
            throw std::invalid_argument("invalid mapped_region mode");
        }

        DWORD extra = (map_options == default_map_options)
                       ? 0
                       : static_cast<DWORD>(map_options);
        DWORD access = desired_access | extra;

        ULARGE_INTEGER native_offset;
        native_offset.QuadPart = offset;

        void* base = ::MapViewOfFileEx(mapping.native_handle(), access,
                                       native_offset.HighPart, native_offset.LowPart,
                                       m_size, const_cast<void*>(address));
        if (!base) {
            throw std::system_error(::GetLastError(), std::system_category(), "MapViewOfFileEx");
        }
        m_address = base;
#else
        int prot = PROT_READ;
        int flags = 0;
        switch (mode) {
        case read_only:
            flags = MAP_SHARED;
            break;
        case read_write:
            prot |= PROT_WRITE;
            flags = MAP_SHARED;
            break;
        case copy_on_write:
            prot |= PROT_WRITE;
            flags = MAP_PRIVATE;
            break;
        default:
            throw std::invalid_argument("invalid mapped_region mode");
        }

        map_options_t opts;
        if (map_options == default_map_options) {
        #if defined(MAP_NOSYNC)
            opts = MAP_NOSYNC;
        #else
            opts = 0;
        #endif
        }
        else {
            opts = map_options;
        }

        flags |= opts;

        void* base = ::mmap(const_cast<void*>(address), m_size, prot, flags,
                            mapping.native_handle(), static_cast<off_t>(offset));
        if (base == MAP_FAILED) {
            throw std::system_error(errno, std::system_category(), "mmap");
        }
        m_address = base;
#endif
    }

    mapped_region(mapped_region&& other) noexcept
        : m_address(other.m_address), m_size(other.m_size), m_mode(other.m_mode)
    {
        other.m_address = nullptr;
        other.m_size = 0;
    }

    mapped_region& operator=(mapped_region&& other) noexcept
    {
        if (this != &other) {
            unmap();
            m_address = other.m_address;
            m_size = other.m_size;
            m_mode = other.m_mode;
            other.m_address = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    ~mapped_region()
    {
        unmap();
    }

    mapped_region(const mapped_region&) = delete;
    mapped_region& operator=(const mapped_region&) = delete;

    void* get_address() const noexcept { return m_address; }
    std::size_t get_size() const noexcept { return m_size; }
    mode_t get_mode() const noexcept { return m_mode; }

private:
    void unmap() noexcept
    {
#if defined(_WIN32)
        if (m_address) {
            ::UnmapViewOfFile(m_address);
            m_address = nullptr;
        }
#else
        if (m_address) {
            ::munmap(m_address, m_size);
            m_address = nullptr;
        }
#endif
        m_size = 0;
    }

private:
    void* m_address = nullptr;
    std::size_t m_size = 0;
    mode_t m_mode = read_only;
};

} // namespace sintra::detail::ipc

