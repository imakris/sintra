#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace sintra::detail::interprocess
{

#if defined(_WIN32)
using map_options_t = DWORD;
#else
using map_options_t = int;
#endif

enum mode_t
{
    read_only,
    read_write
};

class file_mapping
{
public:
    file_mapping() noexcept = default;

    file_mapping(const char* filename, mode_t mode)
    {
        open(filename, mode);
    }

    file_mapping(file_mapping&& other) noexcept
    {
        *this = std::move(other);
    }

    file_mapping& operator=(file_mapping&& other) noexcept
    {
        if (this != &other) {
            close();
            m_mode = other.m_mode;
            m_size = other.m_size;
#if defined(_WIN32)
            m_file_handle = other.m_file_handle;
            m_mapping_handle = other.m_mapping_handle;
            other.m_file_handle = INVALID_HANDLE_VALUE;
            other.m_mapping_handle = nullptr;
#else
            m_fd = other.m_fd;
            other.m_fd = -1;
#endif
        }
        return *this;
    }

    file_mapping(const file_mapping&) = delete;
    file_mapping& operator=(const file_mapping&) = delete;

    ~file_mapping()
    {
        close();
    }

    void open(const char* filename, mode_t mode)
    {
        if (!filename) {
            throw std::invalid_argument("file_mapping::open requires a valid filename");
        }

        close();

        m_mode = mode;
#if defined(_WIN32)
        const std::wstring wide = utf8_to_wide(filename);

        DWORD desired_access = (mode == read_only) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

        HANDLE file = ::CreateFileW(wide.c_str(), desired_access, share_mode, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateFileW");
        }

        LARGE_INTEGER file_size{};
        if (!::GetFileSizeEx(file, &file_size)) {
            DWORD err = ::GetLastError();
            ::CloseHandle(file);
            throw std::system_error(err, std::system_category(), "GetFileSizeEx");
        }

        DWORD protection = (mode == read_only) ? PAGE_READONLY : PAGE_READWRITE;
        HANDLE mapping = ::CreateFileMappingW(file, nullptr, protection, 0, 0, nullptr);
        if (!mapping) {
            DWORD err = ::GetLastError();
            ::CloseHandle(file);
            throw std::system_error(err, std::system_category(), "CreateFileMappingW");
        }

        m_file_handle = file;
        m_mapping_handle = mapping;
        m_size = static_cast<std::size_t>(file_size.QuadPart);
#else
        int flags = (mode == read_only) ? O_RDONLY : O_RDWR;
        int fd = ::open(filename, flags);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "open");
        }

        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "fstat");
        }

        m_fd = fd;
        m_size = static_cast<std::size_t>(st.st_size);
#endif
    }

#if defined(_WIN32)
    HANDLE native_handle() const noexcept { return m_file_handle; }
    HANDLE mapping_handle() const noexcept { return m_mapping_handle; }
#else
    int native_handle() const noexcept { return m_fd; }
#endif

    std::size_t size() const noexcept { return m_size; }
    mode_t mode() const noexcept { return m_mode; }

    static bool remove(const char* filename)
    {
#if defined(_WIN32)
        const std::wstring wide = utf8_to_wide(filename);
        return ::DeleteFileW(wide.c_str()) != 0;
#else
        return ::unlink(filename) == 0;
#endif
    }

private:
#if defined(_WIN32)
    static std::wstring utf8_to_wide(const char* filename)
    {
        if (!filename) {
            return {};
        }
        int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0);
        if (wide_length <= 0) {
            throw std::system_error(::GetLastError(), std::system_category(), "MultiByteToWideChar");
        }
        std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
        int result = ::MultiByteToWideChar(CP_UTF8, 0, filename, -1, wide.data(), wide_length);
        if (result <= 0) {
            throw std::system_error(::GetLastError(), std::system_category(), "MultiByteToWideChar");
        }
        if (!wide.empty() && wide.back() == L'\0') {
            wide.pop_back();
        }
        return wide;
    }
#endif

    void close() noexcept
    {
#if defined(_WIN32)
        if (m_mapping_handle) {
            ::CloseHandle(m_mapping_handle);
            m_mapping_handle = nullptr;
        }
        if (m_file_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_file_handle);
            m_file_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
        m_size = 0;
    }

    mode_t       m_mode = read_only;
    std::size_t  m_size = 0;
#if defined(_WIN32)
    HANDLE       m_file_handle    = INVALID_HANDLE_VALUE;
    HANDLE       m_mapping_handle = nullptr;
#else
    int          m_fd             = -1;
#endif
};

class mapped_region
{
public:
    mapped_region() noexcept = default;

    mapped_region(const file_mapping& mapping,
                  mode_t             mode,
                  std::size_t        offset,
                  std::size_t        size,
                  void*              address = nullptr,
                  map_options_t      options = 0)
    {
        map(mapping, mode, offset, size, address, options);
    }

    mapped_region(mapped_region&& other) noexcept
    {
        *this = std::move(other);
    }

    mapped_region& operator=(mapped_region&& other) noexcept
    {
        if (this != &other) {
            unmap();
            m_address = other.m_address;
            m_length = other.m_length;
            other.m_address = nullptr;
            other.m_length = 0;
        }
        return *this;
    }

    mapped_region(const mapped_region&) = delete;
    mapped_region& operator=(const mapped_region&) = delete;

    ~mapped_region()
    {
        unmap();
    }

    void* get_address() const noexcept { return m_address; }
    std::size_t get_size() const noexcept { return m_length; }

    void swap(mapped_region& other) noexcept
    {
        std::swap(m_address, other.m_address);
        std::swap(m_length, other.m_length);
    }

private:
    void map(const file_mapping& mapping,
             mode_t             mode,
             std::size_t        offset,
             std::size_t        size,
             void*              address,
             map_options_t      options)
    {
        if (offset > mapping.size()) {
            throw std::out_of_range("mapped_region: offset beyond file size");
        }

        std::size_t remaining = mapping.size() - offset;
        std::size_t length = size == 0 ? remaining : size;
        if (length == 0 || length > remaining) {
            throw std::out_of_range("mapped_region: requested size exceeds file size");
        }

#if defined(_WIN32)
        DWORD desired_access = (mode == read_only) ? FILE_MAP_READ : (FILE_MAP_READ | FILE_MAP_WRITE);
        desired_access |= options;

        ULONGLONG off = static_cast<ULONGLONG>(offset);
        DWORD offset_low = static_cast<DWORD>(off & 0xFFFFFFFFULL);
        DWORD offset_high = static_cast<DWORD>((off >> 32) & 0xFFFFFFFFULL);

        void* mapped = ::MapViewOfFileEx(mapping.mapping_handle(), desired_access,
                                         offset_high, offset_low,
                                         static_cast<SIZE_T>(length), address);
        if (!mapped) {
            throw std::system_error(::GetLastError(), std::system_category(), "MapViewOfFileEx");
        }
#else
        if (offset > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
            throw std::out_of_range("mapped_region: offset exceeds off_t range");
        }

        int prot = (mode == read_only) ? PROT_READ : (PROT_READ | PROT_WRITE);
        int flags = MAP_SHARED;
        if (options != 0) {
            flags |= options;
        }

        void* mapped = ::mmap(address, length, prot, flags, mapping.native_handle(),
                               static_cast<off_t>(offset));
        if (mapped == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category(), "mmap");
        }
#endif

        m_address = mapped;
        m_length = length;
    }

    void unmap() noexcept
    {
#if defined(_WIN32)
        if (m_address) {
            ::UnmapViewOfFile(m_address);
            m_address = nullptr;
            m_length = 0;
        }
#else
        if (m_address) {
            ::munmap(m_address, m_length);
            m_address = nullptr;
            m_length = 0;
        }
#endif
    }

    void*       m_address = nullptr;
    std::size_t m_length  = 0;
};

} // namespace sintra::detail::interprocess

