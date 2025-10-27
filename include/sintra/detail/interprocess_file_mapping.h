#pragma once

#include <cerrno>
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
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace sintra::detail::interprocess
{
    enum mode_t
    {
        read_only,
        read_write
    };

#if defined(_WIN32)
    using offset_t = std::uint64_t;
    using map_options_t = unsigned long;
    inline constexpr map_options_t default_map_options = static_cast<map_options_t>(-1);
#else
    using offset_t = std::uint64_t;
    using map_options_t = int;
    inline constexpr map_options_t default_map_options = static_cast<map_options_t>(-1);
#endif

    class file_mapping
    {
    public:
        file_mapping() = default;

        file_mapping(const char* filename, mode_t mode)
            : m_mode(mode)
        {
            if (filename == nullptr)
            {
                throw std::invalid_argument("filename");
            }

            m_name = filename;

#if defined(_WIN32)
            DWORD desired_access = (mode == read_only) ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
            DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;

            HANDLE file = ::CreateFileA(filename, desired_access, share_mode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                throw std::system_error(::GetLastError(), std::system_category(), "CreateFileA");
            }

            LARGE_INTEGER size{};
            if (!::GetFileSizeEx(file, &size))
            {
                auto err = ::GetLastError();
                ::CloseHandle(file);
                throw std::system_error(err, std::system_category(), "GetFileSizeEx");
            }

            DWORD protection = (mode == read_only) ? PAGE_READONLY : PAGE_READWRITE;
            HANDLE mapping = ::CreateFileMappingA(file, nullptr, protection, 0, 0, nullptr);
            if (!mapping)
            {
                auto err = ::GetLastError();
                ::CloseHandle(file);
                throw std::system_error(err, std::system_category(), "CreateFileMappingA");
            }

            m_file_handle = file;
            m_mapping_handle = mapping;
            m_size = static_cast<std::uint64_t>(size.QuadPart);
#else
            int flags = (mode == read_only) ? O_RDONLY : O_RDWR;
            int fd = ::open(filename, flags);
            if (fd == -1)
            {
                throw std::system_error(errno, std::generic_category(), "open");
            }

            struct stat info{};
            if (::fstat(fd, &info) == -1)
            {
                auto err = errno;
                ::close(fd);
                throw std::system_error(err, std::generic_category(), "fstat");
            }

            m_fd = fd;
            m_size = static_cast<std::uint64_t>(info.st_size);
#endif
        }

        file_mapping(const file_mapping&) = delete;
        file_mapping& operator=(const file_mapping&) = delete;

        file_mapping(file_mapping&& other) noexcept
            : m_mode(other.m_mode),
              m_name(std::move(other.m_name)),
              m_size(other.m_size)
        {
#if defined(_WIN32)
            m_file_handle = other.m_file_handle;
            m_mapping_handle = other.m_mapping_handle;
            other.m_file_handle = INVALID_HANDLE_VALUE;
            other.m_mapping_handle = nullptr;
#else
            m_fd = other.m_fd;
            other.m_fd = -1;
#endif
            other.m_size = 0;
        }

        file_mapping& operator=(file_mapping&& other) noexcept
        {
            if (this != &other)
            {
                close();

                m_mode = other.m_mode;
                m_name = std::move(other.m_name);
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
                other.m_size = 0;
            }
            return *this;
        }

        ~file_mapping()
        {
            close();
        }

        mode_t get_mode() const noexcept
        {
            return m_mode;
        }

        const std::string& get_name() const noexcept
        {
            return m_name;
        }

#if defined(_WIN32)
        HANDLE mapping_handle() const noexcept
        {
            return m_mapping_handle;
        }

        HANDLE file_handle() const noexcept
        {
            return m_file_handle;
        }
#else
        int native_handle() const noexcept
        {
            return m_fd;
        }
#endif

        std::uint64_t size() const noexcept
        {
            return m_size;
        }

        static bool remove(const char* filename) noexcept
        {
            if (!filename)
            {
                return false;
            }
#if defined(_WIN32)
            return ::DeleteFileA(filename) != 0;
#else
            return ::unlink(filename) == 0;
#endif
        }

    private:
        void close() noexcept
        {
#if defined(_WIN32)
            if (m_mapping_handle)
            {
                ::CloseHandle(m_mapping_handle);
                m_mapping_handle = nullptr;
            }
            if (m_file_handle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(m_file_handle);
                m_file_handle = INVALID_HANDLE_VALUE;
            }
#else
            if (m_fd != -1)
            {
                ::close(m_fd);
                m_fd = -1;
            }
#endif
            m_size = 0;
        }

        mode_t m_mode = read_only;
        std::string m_name;
        std::uint64_t m_size = 0;

#if defined(_WIN32)
        HANDLE m_file_handle = INVALID_HANDLE_VALUE;
        HANDLE m_mapping_handle = nullptr;
#else
        int m_fd = -1;
#endif
    };

    class mapped_region
    {
    public:
        mapped_region(const file_mapping& mapping,
                      mode_t mode,
                      offset_t offset = 0,
                      std::size_t length = 0,
                      void* address = nullptr,
                      map_options_t options = 0)
        {
            if (offset > mapping.size())
            {
                throw std::system_error(EINVAL, std::generic_category(), "mapped_region offset");
            }

            const std::uint64_t remaining = mapping.size() - offset;
            if (length != 0 && static_cast<std::uint64_t>(length) > remaining)
            {
                throw std::system_error(EINVAL, std::generic_category(), "mapped_region length");
            }

            if (remaining == 0 && length == 0)
            {
                throw std::system_error(EINVAL, std::generic_category(), "mapped_region size");
            }

            if (remaining > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                throw std::system_error(EINVAL, std::generic_category(), "mapped_region range");
            }

            const std::size_t requested = (length == 0)
                ? static_cast<std::size_t>(remaining)
                : length;

#if defined(_WIN32)
            (void)options;

            DWORD desired_access = (mode == read_only) ? FILE_MAP_READ : (FILE_MAP_READ | FILE_MAP_WRITE);

            ULARGE_INTEGER file_offset;
            file_offset.QuadPart = offset;

            SIZE_T bytes_to_map = (length == 0) ? 0 : static_cast<SIZE_T>(requested);

            void* result = ::MapViewOfFileEx(
                mapping.mapping_handle(),
                desired_access,
                file_offset.HighPart,
                file_offset.LowPart,
                bytes_to_map,
                address);

            if (!result)
            {
                throw std::system_error(::GetLastError(), std::system_category(), "MapViewOfFileEx");
            }
#else
            int prot = PROT_READ;
            if (mode == read_write)
            {
                prot |= PROT_WRITE;
            }

            int flags = MAP_SHARED;
            if (options != default_map_options && options != 0)
            {
                flags |= options;
            }

            void* result = ::mmap(address, requested, prot, flags, mapping.native_handle(), static_cast<off_t>(offset));
            if (result == MAP_FAILED)
            {
                throw std::system_error(errno, std::generic_category(), "mmap");
            }
#endif

            m_address = result;
            m_length = requested;
        }

        mapped_region(const mapped_region&) = delete;
        mapped_region& operator=(const mapped_region&) = delete;

        mapped_region(mapped_region&& other) noexcept
            : m_address(other.m_address), m_length(other.m_length)
        {
            other.m_address = nullptr;
            other.m_length = 0;
        }

        mapped_region& operator=(mapped_region&& other) noexcept
        {
            if (this != &other)
            {
                unmap();
                m_address = other.m_address;
                m_length = other.m_length;
                other.m_address = nullptr;
                other.m_length = 0;
            }
            return *this;
        }

        ~mapped_region()
        {
            unmap();
        }

        void* get_address() const noexcept
        {
            return m_address;
        }

        std::size_t get_size() const noexcept
        {
            return m_length;
        }

    private:
        void unmap() noexcept
        {
#if defined(_WIN32)
            if (m_address)
            {
                ::UnmapViewOfFile(m_address);
                m_address = nullptr;
            }
#else
            if (m_address)
            {
                ::munmap(m_address, m_length);
                m_address = nullptr;
            }
#endif
            m_length = 0;
        }

        void* m_address = nullptr;
        std::size_t m_length = 0;
    };
}

