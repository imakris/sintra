#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <timeapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif

  #if defined(__FreeBSD__)
    #include <pthread_np.h>
    #include <sys/sysctl.h>
    #include <sys/user.h>
  #elif defined(__APPLE__)
    #include <libproc.h>
    #include <mach/mach.h>
    #include <mach/mach_time.h>
    #include <sys/sysctl.h>
  #endif
#endif

namespace sintra {

namespace detail {

#ifdef _WIN32
inline std::size_t query_system_page_size() noexcept
{
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);

    std::size_t granularity = static_cast<std::size_t>(info.dwAllocationGranularity);
    if (granularity == 0) {
        granularity = static_cast<std::size_t>(info.dwPageSize);
    }

    return granularity != 0 ? granularity : std::size_t(4096);
}
#else
inline std::size_t query_system_page_size() noexcept
{
    long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
#ifdef _SC_PAGE_SIZE
        page = ::sysconf(_SC_PAGE_SIZE);
#endif
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
    if (page <= 0) {
        page = ::getpagesize();
    }
#endif

    if (page <= 0) {
        page = 4096;
    }

    return static_cast<std::size_t>(page);
}
#endif

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

inline std::size_t system_page_size() noexcept
{
    static const std::size_t cached = detail::query_system_page_size();
    return cached;
}

#if defined(__APPLE__)
inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return;
    }

    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();

    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    if (nanos.count() <= 0) {
        return;
    }

    unsigned __int128 absolute_delta = static_cast<unsigned __int128>(nanos.count()) *
                                       static_cast<unsigned __int128>(timebase.denom);
    absolute_delta += static_cast<unsigned __int128>(timebase.numer - 1);
    absolute_delta /= static_cast<unsigned __int128>(timebase.numer);

    if (absolute_delta == 0) {
        std::this_thread::sleep_for(duration);
        return;
    }

    const uint64_t target = mach_absolute_time() + static_cast<uint64_t>(absolute_delta);

    auto status = mach_wait_until(target);
    while (status == KERN_ABORTED) {
        status = mach_wait_until(target);
    }
}
#else
inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    std::this_thread::sleep_for(duration);
}
#endif

#ifdef _WIN32
class Scoped_timer_resolution {
public:
    explicit Scoped_timer_resolution(UINT period)
        : m_period(period), m_active(::timeBeginPeriod(period) == TIMERR_NOERROR) {}

    ~Scoped_timer_resolution()
    {
        if (m_active) {
            ::timeEndPeriod(m_period);
        }
    }

private:
    UINT m_period;
    bool m_active;
};
#else
class Scoped_timer_resolution {
public:
    explicit Scoped_timer_resolution(unsigned int) {}
};
#endif

inline uint32_t get_current_pid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<uint32_t>(::getpid());
#endif
}

inline uint32_t get_current_tid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid = 0;
    if (::pthread_threadid_np(nullptr, &tid) == 0) {
        return static_cast<uint32_t>(tid);
    }
    return static_cast<uint32_t>(::mach_thread_self());
#elif defined(__linux__)
    return static_cast<uint32_t>(::syscall(SYS_gettid));
#elif defined(__FreeBSD__)
    return static_cast<uint32_t>(::pthread_getthreadid_np());
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

inline bool is_process_alive(uint32_t pid)
{
#ifdef _WIN32
    if (pid == 0) {
        return false;
    }

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return true;
        }
        return false;
    }

    DWORD code = 0;
    bool alive = false;
    if (::GetExitCodeProcess(h, &code)) {
        alive = (code == STILL_ACTIVE);
    }
    ::CloseHandle(h);
    return alive;
#else
    if (!pid) {
        return false;
    }

    if (::kill(static_cast<pid_t>(pid), 0) != 0) {
        return errno != ESRCH;
    }

#if defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc kip;
    std::memset(&kip, 0, sizeof(kip));
    size_t len = sizeof(kip);
    if (::sysctl(mib, 4, &kip, &len, nullptr, 0) != 0) {
        return true;
    }

    if (len == 0) {
        return false;
    }

    switch (kip.ki_stat) {
        case SZOMB:
#ifdef SDEAD
        case SDEAD:
#endif
            return false;
        default:
            return true;
    }
#elif defined(__APPLE__)
    struct proc_bsdinfo bsd_info;
    std::memset(&bsd_info, 0, sizeof(bsd_info));
    int result = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &bsd_info, sizeof(bsd_info));
    if (result <= 0 || static_cast<size_t>(result) < sizeof(bsd_info)) {
        return true;
    }

    constexpr int kProcStatusZombie = 5; // corresponds to SZOMB in <sys/proc.h>
    return bsd_info.pbi_status != kProcStatusZombie;
#else
    std::ifstream stat_file;
    stat_file.open(std::string("/proc/") + std::to_string(pid) + "/stat");
    if (!stat_file.is_open()) {
        return true;
    }

    std::string stat_line;
    std::getline(stat_file, stat_line);

    auto closing_paren = stat_line.rfind(')');
    if (closing_paren == std::string::npos) {
        return true;
    }

    auto state_pos = stat_line.find_first_not_of(' ', closing_paren + 1);
    if (state_pos == std::string::npos) {
        return true;
    }

    char state = stat_line[state_pos];
    return state != 'Z' && state != 'X';
#endif
#endif
}

#if defined(__linux__)
static inline size_t sintra_detect_cache_line_size_linux()
{
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
    long v = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v > 0) return static_cast<size_t>(v);
#endif
    auto read_size = [](const char* path) -> size_t {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return 0;
        char buf[64] = {0};
        size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
        std::fclose(f);
        if (n == 0) return 0;
        char* endp = nullptr;
        long val = std::strtol(buf, &endp, 10);
        return val > 0 ? static_cast<size_t>(val) : 0;
    };

    const char* paths[] = {
        "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index1/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index2/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index3/coherency_line_size",
    };
    for (const char* p : paths) {
        size_t s = read_size(p);
        if (s) return s;
    }
    return 64;
}

static inline void sintra_warn_if_cacheline_mismatch_linux(size_t assumed_cache_line_size)
{
    size_t detected = sintra_detect_cache_line_size_linux();
    if (detected && detected != assumed_cache_line_size) {
        std::fprintf(stderr,
            "sintra(ipc_rings): warning: detected L1D line %zu != assumed %zu; "
            "performance may be suboptimal.\n",
            detected, assumed_cache_line_size);
    }
}
#elif defined(__APPLE__)
static inline size_t sintra_detect_cache_line_size_macos()
{
    auto query_size = [](const char* name) -> size_t {
        size_t value = 0;
        size_t len = sizeof(value);
        if (::sysctlbyname(name, &value, &len, nullptr, 0) == 0 && value > 0) {
            return value;
        }
        return 0;
    };

    if (size_t s = query_size("hw.cachelinesize")) {
        return s;
    }
    if (size_t s = query_size("machdep.cpu.cache.linesize")) {
        return s;
    }

    return 64;
}

static inline void sintra_warn_if_cacheline_mismatch_macos(size_t assumed_cache_line_size)
{
    size_t detected = sintra_detect_cache_line_size_macos();
    if (detected && detected != assumed_cache_line_size) {
        std::fprintf(stderr,
            "sintra(ipc_rings): warning: detected L1D line %zu != assumed %zu; "
            "performance may be suboptimal.\n",
            detected, assumed_cache_line_size);
    }
}
#endif

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

inline size_t mod_pos_i64(int64_t x, size_t m)
{
    const int64_t mm = static_cast<int64_t>(m);
    int64_t r = x % mm;
    if (r < 0) r += mm;
    return static_cast<size_t>(r);
}

inline size_t mod_u64(uint64_t x, size_t m)
{
    return static_cast<size_t>(x % static_cast<uint64_t>(m));
}

} // namespace sintra

