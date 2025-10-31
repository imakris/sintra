#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <climits>
  #include <cwchar>
  #include <limits>
  #include <mutex>
  #include <thread>
  #include <unordered_map>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <ctime>
  #include <pthread.h>
  #if defined(__has_include)
    #if __has_include(<os/os_sync_wait_on_address.h>) && __has_include(<os/clock.h>)
      #include <os/os_sync_wait_on_address.h>
      #include <os/clock.h>
      // macOS builds require os_sync_wait_on_address; no named-semaphore fallback is provided.
    #else
      #error "sintra requires <os/os_sync_wait_on_address.h> and <os/clock.h>. Install recent Xcode Command Line Tools (macOS 13+/Xcode 15+) to provide os_sync_wait_on_address."
    #endif
  #else
    #error "sintra requires compiler support for __has_include to verify os_sync_wait_on_address availability on macOS."
  #endif
  #ifdef OS_CLOCK_MACH_ABSOLUTE_TIME
    #include <mach/mach_time.h>
  #endif
#else
  #include <cerrno>
  #include <semaphore.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
#endif

namespace sintra::detail
{
namespace interprocess_semaphore_detail
{
#if defined(__APPLE__)
    class os_sync_trace
    {
    public:
        static bool enabled()
        {
            static const bool value = [] {
                if (const char* env = std::getenv("SINTRA_OS_SYNC_TRACE")) {
                    if (env[0] == '\0') {
                        return false;
                    }
                    if (matches_ignore_case(env, "0") || matches_ignore_case(env, "false") ||
                        matches_ignore_case(env, "off") || matches_ignore_case(env, "no")) {
                        return false;
                    }
                    return true;
                }
                return true;
            }();
            return value;
        }

        static void log(const char* fmt, ...)
        {
            if (!enabled()) {
                return;
            }

            std::lock_guard<std::mutex> guard(mutex());
            std::FILE* destination = stream();

            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);

            std::fprintf(
                destination,
                "[%lld.%09lld pid=%ld tid=%p] ",
                static_cast<long long>(seconds.count()),
                static_cast<long long>(nanos.count()),
                static_cast<long>(::getpid()),
                reinterpret_cast<void*>(pthread_self()));

            va_list args;
            va_start(args, fmt);
            std::vfprintf(destination, fmt, args);
            va_end(args);

            std::fprintf(destination, "\n");
            std::fflush(destination);
        }

    private:
        static bool matches_ignore_case(const char* value, const char* literal)
        {
            while (*value && *literal) {
                if (std::tolower(static_cast<unsigned char>(*value)) !=
                    std::tolower(static_cast<unsigned char>(*literal))) {
                    return false;
                }
                ++value;
                ++literal;
            }
            return *value == '\0' && *literal == '\0';
        }

        static std::mutex& mutex()
        {
            static std::mutex instance;
            return instance;
        }

        static std::FILE* stream()
        {
            static std::FILE* instance = [] {
                if (const char* path = std::getenv("SINTRA_OS_SYNC_TRACE_FILE")) {
                    if (path[0] != '\0') {
                        if (std::FILE* file = std::fopen(path, "a")) {
                            ::setvbuf(file, nullptr, _IOLBF, 0);
                            return file;
                        }
                    }
                }
                return stderr;
            }();
            return instance;
        }
    };

    inline void ensure_os_sync_timeout_support()
    {
        static std::once_flag probe_once;
        std::call_once(probe_once, [] {
            alignas(4) std::atomic<int32_t> probe_value{0};
            constexpr uint64_t probe_timeout_ns = 1'000'000; // 1 ms

#if defined(OS_CLOCK_MACH_ABSOLUTE_TIME)
            constexpr os_clockid_t probe_clock = OS_CLOCK_MACH_ABSOLUTE_TIME;
#elif defined(OS_CLOCK_MONOTONIC)
            constexpr os_clockid_t probe_clock = OS_CLOCK_MONOTONIC;
#elif defined(CLOCK_MONOTONIC)
            constexpr os_clockid_t probe_clock = static_cast<os_clockid_t>(CLOCK_MONOTONIC);
#else
#   error "No supported monotonic clock id available for os_sync_wait_on_address_with_timeout"
#endif

            errno = 0;
            uint64_t expected = static_cast<uint32_t>(probe_value.load(std::memory_order_relaxed));
            int rc = os_sync_wait_on_address_with_timeout(
                reinterpret_cast<void*>(&probe_value),
                expected,
                sizeof(int32_t),
                OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                probe_clock,
                probe_timeout_ns);
            int saved_errno = errno;

            interprocess_semaphore_detail::os_sync_trace::log(
                "os_sync_wait_on_address_with_timeout probe rc=%d errno=%d",
                rc,
                saved_errno);

            if (rc == -1 && saved_errno == ETIMEDOUT) {
                return;
            }

            if (rc == -1 && saved_errno == EINVAL) {
                throw std::runtime_error(
                    "macOS kernel rejected nanosecond timeouts for os_sync_wait_on_address_with_timeout; "
                    "nanosecond support is required. Please update macOS or install the latest Xcode Command Line Tools.");
            }

            throw std::runtime_error(
                "os_sync_wait_on_address_with_timeout probe failed; nanosecond timeout support could not be verified.");
        });
    }
#endif

    inline uint64_t generate_global_identifier()
    {
        static const uint64_t process_entropy = [] {
            uint64_t value = 0;

#if defined(_WIN32)
            value ^= static_cast<uint64_t>(::GetCurrentProcessId()) << 32;
#endif

#if defined(__unix__) || defined(__APPLE__)
            value ^= static_cast<uint64_t>(::getpid()) << 32;
#endif

            std::random_device rd;
            value ^= (static_cast<uint64_t>(rd()) << 32);
            value ^= static_cast<uint64_t>(rd());

            value ^= static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());

            if (value == 0) {
                value = 0x8000000000000000ULL;
            }

            return value;
        }();

        static std::atomic<uint64_t> counter{0};
        return process_entropy + counter.fetch_add(1, std::memory_order_relaxed);
    }

#if defined(_WIN32)
    inline std::mutex& handle_mutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    inline std::unordered_map<uint64_t, HANDLE>& handle_map()
    {
        static std::unordered_map<uint64_t, HANDLE> map;
        return map;
    }

    inline HANDLE register_handle(uint64_t id, HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        handle_map()[id] = handle;
        return handle;
    }

    inline HANDLE ensure_handle(uint64_t id, const wchar_t* name)
    {
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                return it->second;
            }
        }

        // Opening a named semaphore can occasionally race with the creator on
        // Windows when the object has just been published.  Retry a few times
        // on transient errors instead of failing immediately so that newly
        // spawned processes don't abort before joining the barrier network.
        constexpr unsigned kMaxAttempts = 64;
        constexpr auto kRetryDelay = std::chrono::milliseconds(1);

        DWORD last_error = ERROR_SUCCESS;
        for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
            HANDLE handle = ::OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name);
            if (handle) {
                std::lock_guard<std::mutex> lock(handle_mutex());
                return handle_map().emplace(id, handle).first->second;
            }

            last_error = ::GetLastError();
            const bool transient =
                (last_error == ERROR_FILE_NOT_FOUND) ||
                (last_error == ERROR_INVALID_HANDLE) ||
                (last_error == ERROR_ACCESS_DENIED);

            if (!transient) {
                break;
            }

            std::this_thread::sleep_for(kRetryDelay);
        }

        throw std::system_error(last_error, std::system_category(), "OpenSemaphoreW");
    }

    inline void close_handle(uint64_t id)
    {
        HANDLE handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                handle = it->second;
                handle_map().erase(it);
            }
        }

        if (handle) {
            ::CloseHandle(handle);
        }
    }
#endif
}

class interprocess_semaphore
{
public:
    explicit interprocess_semaphore(unsigned int initial_count = 0)
    {
#if defined(_WIN32)
        initialise_windows(initial_count);
#elif defined(__APPLE__)
        initialise_os_sync(initial_count);
#else
        initialise_posix(initial_count);
#endif
    }

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    ~interprocess_semaphore() noexcept
    {
#if defined(_WIN32)
        teardown_windows();
#elif defined(__APPLE__)
        teardown_os_sync();
#else
        teardown_posix();
#endif
    }

    void release_local_handle() noexcept
    {
#if defined(_WIN32)
        interprocess_semaphore_detail::close_handle(m_windows.id);
#elif defined(__APPLE__)
        // Nothing to do for the os_sync based implementation.
#else
        // Nothing to do for POSIX unnamed semaphores.
#endif
    }

    void post()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        if (!::ReleaseSemaphore(handle, 1, nullptr)) {
            throw std::system_error(::GetLastError(), std::system_category(), "ReleaseSemaphore");
        }
#elif defined(__APPLE__)
        post_os_sync();
#else
        while (sem_post(&m_sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_post");
        }
#endif
    }

    void wait()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, INFINITE);
        if (result != WAIT_OBJECT_0) {
            throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
        }
#elif defined(__APPLE__)
        wait_os_sync();
#else
        while (sem_wait(&m_sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_wait");
        }
#endif
    }

    bool try_wait()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, 0);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#elif defined(__APPLE__)
        return try_acquire_os_sync();
#else
        while (sem_trywait(&m_sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_trywait");
        }
        return true;
#endif
    }

    template <typename Clock, typename Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        auto now = Clock::now();
        if (abs_time <= now) {
            return try_wait();
        }
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(abs_time - now);
        DWORD timeout;
        if (remaining.count() < 0) {
            timeout = 0;
        }
        else if (remaining.count() >= static_cast<long long>(std::numeric_limits<DWORD>::max())) {
            timeout = INFINITE;
        }
        else {
            timeout = static_cast<DWORD>(remaining.count());
        }

        DWORD result = ::WaitForSingleObject(handle, timeout);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#elif defined(__APPLE__)
        int32_t previous = m_os_sync.count.fetch_sub(1, std::memory_order_acq_rel);
        if (previous > 0) {
            return true;
        }

        int32_t expected = previous - 1;
        while (true) {
            auto now = Clock::now();
            if (abs_time <= now) {
                cancel_wait_os_sync();
                return false;
            }

            auto remaining = std::chrono::ceil<std::chrono::nanoseconds>(abs_time - now);
            int32_t observed = expected;
            if (!wait_os_sync_with_timeout(expected, remaining, observed)) {
                if (observed >= 0) {
                    return true;
                }
                cancel_wait_os_sync();
                return false;
            }

            if (observed >= 0) {
                return true;
            }

            expected = observed;
        }
#else
        auto ts = make_abs_timespec(abs_time);
        while (sem_timedwait(&m_sem, &ts) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ETIMEDOUT) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_timedwait");
        }
        return true;
#endif
    }

private:
#if !defined(_WIN32)
    template <typename Clock, typename Duration>
    static timespec make_abs_timespec(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        using namespace std::chrono;
        auto now = Clock::now();
        auto delta = abs_time > now ? abs_time - now : Clock::duration::zero();
        auto sys_time = system_clock::now() + duration_cast<system_clock::duration>(delta);
        auto ns = duration_cast<nanoseconds>(sys_time.time_since_epoch());
        timespec ts;
        ts.tv_sec = static_cast<time_t>(ns.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(ns.count() % 1000000000);
        return ts;
    }
#endif

#if defined(_WIN32)
    struct windows_storage
    {
        uint64_t id = 0;
        wchar_t  name[64]{};
    };

    windows_storage m_windows;

    void initialise_windows(unsigned int initial_count)
    {
        m_windows.id = interprocess_semaphore_detail::generate_global_identifier();
        std::swprintf(m_windows.name,
                      sizeof(m_windows.name) / sizeof(m_windows.name[0]),
                      L"SintraSemaphore_%016llX",
                      static_cast<unsigned long long>(m_windows.id));

        HANDLE handle = ::CreateSemaphoreW(nullptr, static_cast<LONG>(initial_count), LONG_MAX, m_windows.name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateSemaphoreW");
        }

        interprocess_semaphore_detail::register_handle(m_windows.id, handle);
    }

    HANDLE windows_handle() const
    {
        return interprocess_semaphore_detail::ensure_handle(m_windows.id, m_windows.name);
    }

    void teardown_windows() noexcept
    {
        interprocess_semaphore_detail::close_handle(m_windows.id);
    }
#elif defined(__APPLE__)
    struct os_sync_storage
    {
        std::atomic<int32_t> count{0};
    };

    os_sync_storage m_os_sync{};

    static constexpr os_sync_wait_on_address_flags_t wait_flags = OS_SYNC_WAIT_ON_ADDRESS_SHARED;
    static constexpr os_sync_wake_by_address_flags_t wake_flags = OS_SYNC_WAKE_BY_ADDRESS_SHARED;

#ifdef OS_CLOCK_MACH_ABSOLUTE_TIME
    static constexpr os_clockid_t wait_clock = OS_CLOCK_MACH_ABSOLUTE_TIME;
#elif defined(OS_CLOCK_MONOTONIC)
    static constexpr os_clockid_t wait_clock = OS_CLOCK_MONOTONIC;
#elif defined(CLOCK_MONOTONIC)
    static constexpr os_clockid_t wait_clock = static_cast<os_clockid_t>(CLOCK_MONOTONIC);
#else
#   error "No supported monotonic clock id available for os_sync_wait_on_address_with_timeout"
#endif

    void initialise_os_sync(unsigned int initial_count)
    {
        interprocess_semaphore_detail::ensure_os_sync_timeout_support();
        m_os_sync.count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
#if defined(__APPLE__)
        interprocess_semaphore_detail::os_sync_trace::log(
            "initialise_os_sync initial_count=%u address=%p",
            initial_count,
            reinterpret_cast<void*>(&m_os_sync.count));
#endif
    }

    void teardown_os_sync() noexcept {}

    void post_os_sync()
    {
        int32_t previous = m_os_sync.count.fetch_add(1, std::memory_order_release);
#if defined(__APPLE__)
        interprocess_semaphore_detail::os_sync_trace::log(
            "post_os_sync previous=%d new_value=%d address=%p",
            static_cast<int>(previous),
            static_cast<int>(previous + 1),
            reinterpret_cast<void*>(&m_os_sync.count));
#endif
        if (previous < 0) {
            wake_one_waiter();
        }
    }

    void wait_os_sync()
    {
        int32_t previous = m_os_sync.count.fetch_sub(1, std::memory_order_acq_rel);
#if defined(__APPLE__)
        interprocess_semaphore_detail::os_sync_trace::log(
            "wait_os_sync previous=%d new_value=%d address=%p",
            static_cast<int>(previous),
            static_cast<int>(previous - 1),
            reinterpret_cast<void*>(&m_os_sync.count));
#endif
        if (previous > 0) {
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log("wait_os_sync satisfied_without_wait previous=%d", static_cast<int>(previous));
#endif
            return;
        }

        int32_t expected = previous - 1;
        while (true) {
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync waiting expected=%d",
                static_cast<int>(expected));
#endif
            int32_t observed = wait_on_address_blocking(expected);
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync wake observed=%d",
                static_cast<int>(observed));
#endif
            if (observed >= 0) {
                return;
            }
            expected = observed;
        }
    }

    bool try_acquire_os_sync()
    {
        int32_t expected = m_os_sync.count.load(std::memory_order_acquire);
        while (expected > 0) {
            if (m_os_sync.count.compare_exchange_weak(expected,
                                                      expected - 1,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    bool wait_os_sync_with_timeout(int32_t expected, std::chrono::nanoseconds remaining, int32_t& observed)
    {
        if (remaining <= std::chrono::nanoseconds::zero()) {
            observed = m_os_sync.count.load(std::memory_order_acquire);
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync_with_timeout immediate_timeout expected=%d observed=%d remaining_ns=%lld",
                static_cast<int>(expected),
                static_cast<int>(observed),
                static_cast<long long>(remaining.count()));
#endif
            return false;
        }

        auto count = remaining.count();
        if (count <= 0) {
            observed = m_os_sync.count.load(std::memory_order_acquire);
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync_with_timeout non_positive_duration expected=%d observed=%d duration_ns=%lld",
                static_cast<int>(expected),
                static_cast<int>(observed),
                static_cast<long long>(count));
#endif
            return false;
        }

        const uint64_t timeout_ns = static_cast<uint64_t>(count);

        while (true) {
            uint64_t expected_value = static_cast<uint32_t>(expected);
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync_with_timeout attempt expected=%d expected_value=%u timeout_ns=%llu address=%p",
                static_cast<int>(expected),
                static_cast<unsigned>(expected_value),
                static_cast<unsigned long long>(timeout_ns),
                reinterpret_cast<void*>(&m_os_sync.count));
#endif
            int rc = os_sync_wait_on_address_with_timeout(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected_value,
                sizeof(int32_t),
                wait_flags,
                wait_clock,
                timeout_ns);
            int saved_errno = errno;
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync_with_timeout result rc=%d errno=%d count_snapshot=%d",
                rc,
                saved_errno,
                static_cast<int>(m_os_sync.count.load(std::memory_order_acquire)));
#endif
            if (rc >= 0) {
                observed = m_os_sync.count.load(std::memory_order_acquire);
                return true;
            }
            if (saved_errno == ETIMEDOUT) {
                observed = m_os_sync.count.load(std::memory_order_acquire);
#if defined(__APPLE__)
                interprocess_semaphore_detail::os_sync_trace::log(
                    "wait_os_sync_with_timeout timeout observed=%d",
                    static_cast<int>(observed));
#endif
                return false;
            }
            if (saved_errno == EINTR || saved_errno == EFAULT) {
                errno = saved_errno;
#if defined(__APPLE__)
                interprocess_semaphore_detail::os_sync_trace::log(
                    "wait_os_sync_with_timeout retry errno=%d",
                saved_errno);
#endif
                continue;
            }
            errno = saved_errno;
#if defined(__APPLE__)
            if (saved_errno == EINVAL) {
                interprocess_semaphore_detail::os_sync_trace::log(
                    "wait_os_sync_with_timeout fatal_einval expected=%d timeout_ns=%llu",
                    static_cast<int>(expected),
                    static_cast<unsigned long long>(timeout_ns));
                throw std::runtime_error(
                    "os_sync_wait_on_address_with_timeout returned EINVAL despite nanosecond probe success; "
                    "this macOS kernel does not honour the documented timeout contract.");
            }
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_os_sync_with_timeout throwing errno=%d expected=%d",
                saved_errno,
                static_cast<int>(expected));
#endif
            throw std::system_error(saved_errno, std::generic_category(), "os_sync_wait_on_address_with_timeout");
        }
    }

    void cancel_wait_os_sync()
    {
        m_os_sync.count.fetch_add(1, std::memory_order_acq_rel);
#if defined(__APPLE__)
        interprocess_semaphore_detail::os_sync_trace::log(
            "cancel_wait_os_sync new_value=%d",
            static_cast<int>(m_os_sync.count.load(std::memory_order_acquire)));
#endif
    }

    int32_t wait_on_address_blocking(int32_t expected)
    {
        while (true) {
            uint64_t expected_value = static_cast<uint32_t>(expected);
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_on_address_blocking attempt expected=%d expected_value=%u address=%p",
                static_cast<int>(expected),
                static_cast<unsigned>(expected_value),
                reinterpret_cast<void*>(&m_os_sync.count));
#endif
            int rc = os_sync_wait_on_address(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected_value,
                sizeof(int32_t),
                wait_flags);
            int saved_errno = errno;
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_on_address_blocking result rc=%d errno=%d count_snapshot=%d",
                rc,
                saved_errno,
                static_cast<int>(m_os_sync.count.load(std::memory_order_acquire)));
#endif
            if (rc >= 0) {
                return m_os_sync.count.load(std::memory_order_acquire);
            }
            if (saved_errno == EINTR || saved_errno == EFAULT) {
                errno = saved_errno;
#if defined(__APPLE__)
                interprocess_semaphore_detail::os_sync_trace::log(
                    "wait_on_address_blocking retry errno=%d",
                    saved_errno);
#endif
                continue;
            }
            errno = saved_errno;
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wait_on_address_blocking throwing errno=%d expected=%d",
                saved_errno,
                static_cast<int>(expected));
#endif
            throw std::system_error(saved_errno, std::generic_category(), "os_sync_wait_on_address");
        }
    }

    void wake_one_waiter()
    {
        while (true) {
            int rc = os_sync_wake_by_address_any(
                reinterpret_cast<void*>(&m_os_sync.count),
                sizeof(int32_t),
                wake_flags);
            int saved_errno = errno;
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wake_one_waiter result rc=%d errno=%d count_snapshot=%d",
                rc,
                saved_errno,
                static_cast<int>(m_os_sync.count.load(std::memory_order_acquire)));
#endif
            if (rc == 0) {
                return;
            }
            if (rc == -1) {
                if (saved_errno == ENOENT) {
                    return;
                }
                if (saved_errno == EINTR) {
                    errno = saved_errno;
#if defined(__APPLE__)
                    interprocess_semaphore_detail::os_sync_trace::log("wake_one_waiter retry errno=EINTR");
#endif
                    continue;
                }
            }
            errno = saved_errno;
#if defined(__APPLE__)
            interprocess_semaphore_detail::os_sync_trace::log(
                "wake_one_waiter throwing errno=%d",
                saved_errno);
#endif
            throw std::system_error(saved_errno, std::generic_category(), "os_sync_wake_by_address_any");
        }
    }
#else
    sem_t m_sem{};

    void initialise_posix(unsigned int initial_count)
    {
        if (sem_init(&m_sem, 1, initial_count) == -1) {
            throw std::system_error(errno, std::generic_category(), "sem_init");
        }
    }

    void teardown_posix() noexcept
    {
        sem_destroy(&m_sem);
    }
#endif
}; 

} // namespace sintra::detail

