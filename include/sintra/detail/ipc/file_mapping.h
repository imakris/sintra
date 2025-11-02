// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

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
#    include "../sintra_windows.h"
#else
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace sintra::detail::ipc {

enum map_mode_t
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

    file_mapping(const char* filename, map_mode_t mode)
    {
        if (!filename) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "file_mapping: null filename");
        }
        open_impl(std::filesystem::path(filename), mode);
    }

    file_mapping(const std::string& filename, map_mode_t mode)
    {
        open_impl(std::filesystem::path(filename), mode);
    }

    file_mapping(const std::filesystem::path& filename, map_mode_t mode)
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
    map_mode_t mode() const noexcept { return m_mode; }

    // Flush underlying file contents to durable storage (best-effort).
    void flush_file()
    {
        // Only flush if the file is opened for writing
        if (m_mode != read_write) {
            return;
        }

#if defined(_WIN32)
        if (m_file != INVALID_HANDLE_VALUE) {
            if (!::FlushFileBuffers(m_file)) {
                DWORD err = ::GetLastError();
                // Treat ACCESS_DENIED as no-op (best-effort semantics)
                if (err != ERROR_ACCESS_DENIED) {
                    throw std::system_error(err, std::system_category(), "FlushFileBuffers failed");
                }
            }
        }
#else
        if (m_fd != -1) {
#if defined(_POSIX_SYNCHRONIZED_IO) && (_POSIX_SYNCHRONIZED_IO > 0)
            int rc;
            do {
                rc = ::fdatasync(m_fd);
            }
            while (rc != 0 && errno == EINTR);
            if (rc != 0) {
                throw std::system_error(errno, std::system_category(), "fdatasync failed");
            }
#else
            int rc;
            do {
                rc = ::fsync(m_fd);
            } 
            while (rc != 0 && errno == EINTR);
            if (rc != 0) {
                throw std::system_error(errno, std::system_category(), "fsync failed");
            }
#endif
        }
#endif
    }

#if defined(_WIN32)
    using native_handle_type = HANDLE;
    native_handle_type native_handle() const noexcept { return m_file; }
#else
    using native_handle_type = int;
    native_handle_type native_handle() const noexcept { return m_fd; }
#endif

private:
    void open_impl(const std::filesystem::path& filename, map_mode_t mode)
    {
        close_impl();

        if (filename.empty()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "file_mapping: empty filename");
        }

#if defined(_WIN32)
        auto wide = filename.wstring();
        DWORD desired_access = (mode == read_write) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
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
        m_mode = mode;
#else
#ifdef O_CLOEXEC
        int flags = (mode == read_write) ? (O_RDWR | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
#else
        int flags = (mode == read_write) ? O_RDWR : O_RDONLY;
#endif
        const auto* native = filename.c_str();
        int fd;
        do { fd = ::open(native, flags); } while (fd == -1 && errno == EINTR);
#ifndef O_CLOEXEC
        if (fd != -1) {
            (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
#endif
        if (fd == -1) {
            throw std::system_error(errno, std::system_category(), "open failed");
        }

        struct stat st;
        int fstat_ret;
        do { fstat_ret = ::fstat(fd, &st); } while (fstat_ret == -1 && errno == EINTR);
        if (fstat_ret != 0) {
            int err = errno;
            ::close(fd);
            throw std::system_error(err, std::system_category(), "fstat failed");
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(fd);
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "file_mapping: not a regular file");
        }

        m_fd = fd;
        m_size = static_cast<size_type>(st.st_size);
        m_mode = mode;
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
    map_mode_t    m_mode = read_only;

#if defined(_WIN32)
    HANDLE m_file = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

#ifndef _WIN32
namespace {
// Cache the page size to avoid repeated sysconf() calls
inline long get_page_size()
{
    static const long page_size = []() {
        long sz = ::sysconf(_SC_PAGE_SIZE);
        if (sz <= 0) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "could not determine page size");
        }
        return sz;
    }();
    return page_size;
}
} // anonymous namespace
#endif

class mapped_region
{
public:
    mapped_region(const file_mapping& file, map_mode_t mode,
                  std::uint64_t offset, std::size_t size,
                  void* address = nullptr, map_options_t options = 0)
    {
        // Validate view mode vs file open mode
        if (mode == read_write && file.mode() != read_write) {
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    "mapped_region: read_write view requires read_write file");
        }
        // POSIX COW does not require a write-open file descriptor; do not reject read_only here.

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

        // Calculate allocation granularity and alignment for the mapping.
        // Both Windows and POSIX require the file offset to be aligned to a specific boundary.
#if defined(_WIN32)
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        const std::size_t allocation_granularity = static_cast<std::size_t>(si.dwAllocationGranularity);
#else
        const long page_size_long = get_page_size();
        const std::size_t allocation_granularity = static_cast<std::size_t>(page_size_long);
#endif

        const std::uint64_t misalignment = offset % static_cast<std::uint64_t>(allocation_granularity);
        const std::uint64_t aligned_offset = offset - misalignment;
        const std::size_t   offset_delta = static_cast<std::size_t>(misalignment);

        if (mapping_size > std::numeric_limits<std::size_t>::max() - offset_delta) {
            throw std::system_error(std::make_error_code(std::errc::value_too_large),
                                    "mapped_region: adjusted mapping size does not fit in size_t");
        }

        const std::size_t adjusted_size = mapping_size + offset_delta;

        m_mode = mode;

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
            // Use FILE_MAP_COPY for a COW view. Reads are allowed; writes are private to the process.
            protect = PAGE_WRITECOPY;
            desired_access = FILE_MAP_COPY;
            break;
        }

        HANDLE mapping = ::CreateFileMappingW(file.native_handle(), nullptr, protect, 0, 0, nullptr);
        if (!mapping) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateFileMappingW failed");
        }

        ULARGE_INTEGER off;
        off.QuadPart = aligned_offset;
        (void)options;
        // NOTE: Windows-specific options (if any) are handled via desired_access/protect above.
        //       The 'options' parameter is currently ignored on Windows.

        // Calculate the base address for MapViewOfFileEx.
        // If the user requested a specific address, we must account for the offset delta to ensure
        // the final user-visible address aligns correctly.
        void* requested_base = nullptr;
        if (address) {
            requested_base = reinterpret_cast<void*>(reinterpret_cast<char*>(address) - offset_delta);
        }

        void* view = ::MapViewOfFileEx(mapping, desired_access,
            static_cast<DWORD>(off.HighPart), static_cast<DWORD>(off.LowPart), adjusted_size, requested_base);
        if (!view) {
            DWORD err = ::GetLastError();
            ::CloseHandle(mapping);
            throw std::system_error(err, std::system_category(),
                                    "MapViewOfFileEx failed");
        }

        m_mapping_handle = mapping;
        m_base = view;
        m_base_size = adjusted_size;
        m_address = static_cast<char*>(view) + offset_delta;
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
            prot = PROT_READ | PROT_WRITE; // COW writes are private.
            break;
        }

        int flags = (mode == copy_on_write) ? MAP_PRIVATE : MAP_SHARED;
        if (options != 0) {
            flags |= options;
        }
#if defined(MAP_ANONYMOUS)
        if (options & MAP_ANONYMOUS) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: MAP_ANONYMOUS not valid with file-backed mapping");
        }
#endif
#if defined(MAP_ANON)
        if (options & MAP_ANON) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: MAP_ANON not valid with file-backed mapping");
        }
#endif

        // Verify that the aligned offset fits in off_t for mmap.
        if (aligned_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region: aligned offset does not fit in off_t");
        }

        // Calculate the base address for mmap.
        // If the user requested a specific address, we must account for the offset delta to ensure
        // the final user-visible address aligns correctly.
        void* requested_base = nullptr;
        if (address) {
            requested_base = reinterpret_cast<void*>(reinterpret_cast<char*>(address) - offset_delta);
        }

        void* mapped = ::mmap(requested_base, adjusted_size, prot, flags,
                              file.native_handle(), static_cast<off_t>(aligned_offset));
        if (mapped == MAP_FAILED) {
            throw std::system_error(errno, std::system_category(), "mmap failed");
        }

        m_base = mapped;
        m_base_size = adjusted_size;
        m_address = static_cast<char*>(mapped) + offset_delta;
        m_size = mapping_size;
#endif
    }

    // Flush changes to the underlying file (no-op for read_only and copy_on_write). If len==0, flush to end.
    void flush(std::size_t offset = 0, std::size_t len = 0)
    {
        if (offset > m_size) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region::flush: offset beyond mapping size");
        }
        if (len == 0) {
            len = m_size - offset;
        }
        if (len > m_size - offset) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "mapped_region::flush: length exceeds mapping size");
        }

        // No-op for modes that cannot persist to the underlying file.
        if (m_mode != read_write || len == 0 || !m_address) {
            return;
        }

#if defined(_WIN32)
        // Simplified flush: pass any address inside the view; Windows aligns internally to page boundaries.
        std::uintptr_t start_addr = reinterpret_cast<std::uintptr_t>(m_address) + offset;
        if (!::FlushViewOfFile(reinterpret_cast<void*>(start_addr), len)) {
            throw std::system_error(::GetLastError(), std::system_category(), "FlushViewOfFile failed");
        }
#else
        const long page = get_page_size();
        std::uintptr_t base = reinterpret_cast<std::uintptr_t>(m_address);
        if (base > std::numeric_limits<std::uintptr_t>::max() - offset) {
            throw std::system_error(std::make_error_code(std::errc::value_too_large),
                                    "mapped_region::flush: address overflow (base+offset)");
        }
        std::uintptr_t start = base + offset;
        if (start > std::numeric_limits<std::uintptr_t>::max() - len) {
            throw std::system_error(std::make_error_code(std::errc::value_too_large),
                                    "mapped_region::flush: address overflow (start+len)");
        }
        std::uintptr_t end = start + len;
        const std::uintptr_t page_sz = static_cast<std::uintptr_t>(page);
        std::uintptr_t aligned = (start / page_sz) * page_sz;
        if (end - aligned > std::numeric_limits<std::size_t>::max()) {
            throw std::system_error(std::make_error_code(std::errc::value_too_large),
                                    "mapped_region::flush: length overflows size_t");
        }
        std::size_t adj_len = static_cast<std::size_t>(end - aligned);

        int rc;
        do {
            rc = ::msync(reinterpret_cast<void*>(aligned), adj_len, MS_SYNC);
        }
        while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            throw std::system_error(errno, std::system_category(), "msync failed");
        }
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
            m_base = other.m_base;
            m_base_size = other.m_base_size;
            m_address = other.m_address;
            m_size = other.m_size;
            m_mode = other.m_mode;
#if defined(_WIN32)
            m_mapping_handle = other.m_mapping_handle;
            other.m_mapping_handle = nullptr;
#endif
            other.m_base = nullptr;
            other.m_base_size = 0;
            other.m_address = nullptr;
            other.m_size = 0;
            other.m_mode = read_only;
        }
        return *this;
    }

    mapped_region(const mapped_region&) = delete;
    mapped_region& operator=(const mapped_region&) = delete;

    ~mapped_region()
    {
        release();
    }

    // STL-style accessors
    void*       data()           noexcept { return m_address; }
    const void* data()     const noexcept { return m_address; }
    std::size_t size()     const noexcept { return m_size;    }

private:
    void release() noexcept
    {
#if defined(_WIN32)
        if (m_base) {
            ::UnmapViewOfFile(m_base);
        }
        m_base = nullptr;
        m_address = nullptr;
        if (m_mapping_handle) {
            ::CloseHandle(m_mapping_handle);
            m_mapping_handle = nullptr;
        }
#else
        if (m_base) {
            ::munmap(m_base, m_base_size);
        }
        m_base = nullptr;
        m_address = nullptr;
#endif
        m_size = 0;
        m_mode = read_only;
    }

#if defined(_WIN32)
    HANDLE      m_mapping_handle = nullptr;
#endif
    void*       m_base = nullptr;
    std::size_t m_base_size = 0;
    void*       m_address = nullptr;
    std::size_t m_size = 0;
    map_mode_t  m_mode = read_only;
};

} // namespace sintra::detail::ipc