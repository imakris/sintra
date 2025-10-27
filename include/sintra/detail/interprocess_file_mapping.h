#pragma once

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace sintra::detail::ipc {

enum mode_t
{
    read_only,
    read_write,
    copy_on_write
};

using map_options_t = int;

class file_mapping
{
public:
    using size_type = std::uint64_t;

    file_mapping(const char* filename, mode_t mode)
    {
        if (!filename) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "file_mapping: null filename");
        }
        open_impl(std::filesystem::path(filename), mode);
    }

    file_mapping(const std::string& filename, mode_t mode)
    {
        open_impl(std::filesystem::path(filename), mode);
    }

    file_mapping(const std::filesystem::path& filename, mode_t mode)
    {
        open_impl(filename, mode);
    }

    file_mapping(file_mapping&& other) noexcept
    {
        *this = std::move(other);
    }

    file_mapping& operator=(file_mapping&& other) noexcept
    {
        if (this != &other) {
            close_impl();
            m_size = other.m_size;
            m_mode = other.m_mode;
#if defined(_WIN32)
            m_file = other.m_file;
            other.m_file = INVALID_HANDLE_VALUE;
#else
            m_fd = other.m_fd;
            other.m_fd = -1;
#endif
            other.m_size = 0;
            other.m_mode = read_only;
        }
        return *this;
    }

    file_mapping(const file_mapping&) = delete;
    file_mapping& operator=(const file_mapping&) = delete;

    ~file_mapping()
    {
        close_impl();
    }

    size_type size() const noexcept { return m_size; }
    mode_t mode() const noexcept { return m_mode; }

#if defined(_WIN32)
    using native_handle_type = void*;
    native_handle_type native_handle() const noexcept { return m_file; }
#else
    using native_handle_type = int;
    native_handle_type native_handle() const noexcept { return m_fd; }
#endif

private:
    void open_impl(const std::filesystem::path& filename, mode_t mode)
    {
        close_impl();

        if (filename.empty()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "file_mapping: empty filename");
        }

        m_mode = mode;

#if defined(_WIN32)
        auto wide = filename.wstring();
        DWORD desired_access = (mode == read_only) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE file = ::CreateFileW(wide.c_str(), desired_access, share_mode, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::system_error(::GetLastError(), std::system_category(),
                                    "CreateFileW failed");
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size)) {
            DWORD err = ::GetLastError();
            ::CloseHandle(file);
            throw std::system_error(err, std::system_category(), "GetFileSizeEx failed");
        }

        m_file = file;
        m_size = static_cast<size_type>(size.QuadPart);
#else
        int flags = (mode == read_only) ? O_RDONLY : O_RDWR;
        const auto* native = filename.c_str();
        int fd = ::open(native, flags);
        if (fd == -1) {
            throw std::system_error(errno, std::system_category(), "open failed");
        }

        struct stat st;
        if (::fstat(fd, &st) != 0) {
            int err = errno;
            ::close(fd);
            throw std::system_error(err, std::system_category(), "fstat failed");
        }

        m_fd = fd;
        m_size = static_cast<size_type>(st.st_size);
#endif
    }

    void close_impl() noexcept
    {
#if defined(_WIN32)
        if (m_file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd != -1) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
        m_size = 0;
        m_mode = read_only;
    }

    size_type m_size = 0;
    mode_t    m_mode = read_only;

#if defined(_WIN32)
    HANDLE m_file = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

class mapped_region
{
public:
    mapped_region(const file_mapping& file, mode_t mode,
                  std::uint64_t offset, std::size_t size,
                  void* address = nullptr, map_options_t options = 0)
    {
        if (offset > file.size()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: offset beyond end of file");
        }

        const std::uint64_t available = file.size() - offset;

        std::size_t mapping_size = 0;
        if (size == 0) {
            if (available > std::numeric_limits<std::size_t>::max()) {
                throw std::system_error(std::make_error_code(std::errc::value_too_large),
                                        "mapped_region: mapping size does not fit in size_t");
            }
            mapping_size = static_cast<std::size_t>(available);
        }
        else {
            if (size > available) {
                throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                        "mapped_region: mapping extends beyond file size");
            }
            mapping_size = size;
        }

        if (mapping_size == 0) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: zero-length mapping");
        }

#if defined(_WIN32)
        DWORD protect = PAGE_READONLY;
        DWORD desired_access = FILE_MAP_READ;
        switch (mode) {
        case read_only:
            protect = PAGE_READONLY;
            desired_access = FILE_MAP_READ;
            break;
        case read_write:
            protect = PAGE_READWRITE;
            desired_access = FILE_MAP_READ | FILE_MAP_WRITE;
            break;
        case copy_on_write:
            protect = PAGE_WRITECOPY;
            desired_access = FILE_MAP_READ | FILE_MAP_COPY;
            break;
        }

        HANDLE mapping = ::CreateFileMappingW(static_cast<HANDLE>(file.native_handle()), nullptr,
                                              protect, 0, 0, nullptr);
        if (!mapping) {
            throw std::system_error(::GetLastError(), std::system_category(),
                                    "CreateFileMappingW failed");
        }

        ULARGE_INTEGER off;
        off.QuadPart = offset;
        (void)options; // Windows-specific flags are handled via desired_access/protect.
        void* view = ::MapViewOfFileEx(mapping, desired_access,
                                       static_cast<DWORD>(off.HighPart),
                                       static_cast<DWORD>(off.LowPart),
                                       mapping_size, address);
        if (!view) {
            DWORD err = ::GetLastError();
            ::CloseHandle(mapping);
            throw std::system_error(err, std::system_category(),
                                    "MapViewOfFileEx failed");
        }

        m_mapping_handle = mapping;
        m_address = view;
        m_size = mapping_size;
#else
        int prot = PROT_READ;
        switch (mode) {
        case read_only:
            prot = PROT_READ;
            break;
        case read_write:
            prot = PROT_READ | PROT_WRITE;
            break;
        case copy_on_write:
            prot = PROT_READ | PROT_WRITE;
            break;
        }

        int flags = (mode == copy_on_write) ? MAP_PRIVATE : MAP_SHARED;
        if (options != 0) {
            flags |= options;
        }

        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: offset does not fit in off_t");
        }

        void* mapped = ::mmap(address, mapping_size, prot, flags,
                              file.native_handle(), static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::system_error(errno, std::system_category(), "mmap failed");
        }

        m_address = mapped;
        m_size = mapping_size;
#endif
    }

    mapped_region(mapped_region&& other) noexcept
    {
        *this = std::move(other);
    }

    mapped_region& operator=(mapped_region&& other) noexcept
    {
        if (this != &other) {
            release();
            m_address = other.m_address;
            m_size = other.m_size;
#if defined(_WIN32)
            m_mapping_handle = other.m_mapping_handle;
            other.m_mapping_handle = nullptr;
#endif
            other.m_address = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    mapped_region(const mapped_region&) = delete;
    mapped_region& operator=(const mapped_region&) = delete;

    ~mapped_region()
    {
        release();
    }

    void*       get_address() const noexcept { return m_address; }
    std::size_t get_size() const noexcept { return m_size; }

private:
    void release() noexcept
    {
#if defined(_WIN32)
        if (m_address) {
            ::UnmapViewOfFile(m_address);
        }
        m_address = nullptr;
        if (m_mapping_handle) {
            ::CloseHandle(static_cast<HANDLE>(m_mapping_handle));
            m_mapping_handle = nullptr;
        }
#else
        if (m_address) {
            ::munmap(m_address, m_size);
        }
        m_address = nullptr;
#endif
        m_size = 0;
    }

#if defined(_WIN32)
    HANDLE      m_mapping_handle = nullptr;
#endif
    void*       m_address = nullptr;
    std::size_t m_size = 0;
};

} // namespace sintra::detail::ipc

