#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <random>

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
    enum class os_sync_address_scope : uint8_t
    {
        shared,
        process_local
    };

    struct os_sync_storage
    {
        std::atomic<int32_t> count{0};
        std::atomic<os_sync_address_scope> scope{os_sync_address_scope::shared};
    };

    os_sync_storage m_os_sync{};

    static constexpr os_sync_wait_on_address_flags_t shared_wait_flag = OS_SYNC_WAIT_ON_ADDRESS_SHARED;
#ifdef OS_SYNC_WAIT_ON_ADDRESS_PRIVATE
    static constexpr os_sync_wait_on_address_flags_t local_wait_flag = OS_SYNC_WAIT_ON_ADDRESS_PRIVATE;
#else
    static constexpr os_sync_wait_on_address_flags_t local_wait_flag = static_cast<os_sync_wait_on_address_flags_t>(0);
#endif

    static constexpr os_sync_wake_by_address_flags_t shared_wake_flag = OS_SYNC_WAKE_BY_ADDRESS_SHARED;
#ifdef OS_SYNC_WAKE_BY_ADDRESS_PRIVATE
    static constexpr os_sync_wake_by_address_flags_t local_wake_flag = OS_SYNC_WAKE_BY_ADDRESS_PRIVATE;
#else
    static constexpr os_sync_wake_by_address_flags_t local_wake_flag = static_cast<os_sync_wake_by_address_flags_t>(0);
#endif

#ifdef OS_CLOCK_MACH_ABSOLUTE_TIME
    static constexpr os_clockid_t wait_clock = OS_CLOCK_MACH_ABSOLUTE_TIME;

    static uint64_t nanoseconds_to_mach_absolute_ticks(uint64_t timeout_ns)
    {
        if (timeout_ns == 0) {
            return 0;
        }

        static const mach_timebase_info_data_t timebase = [] {
            mach_timebase_info_data_t info{};
            (void)mach_timebase_info(&info);
            return info;
        }();

        unsigned __int128 absolute_delta = static_cast<unsigned __int128>(timeout_ns) *
                                           static_cast<unsigned __int128>(timebase.denom);
        absolute_delta += static_cast<unsigned __int128>(timebase.numer - 1);
        absolute_delta /= static_cast<unsigned __int128>(timebase.numer);

        return static_cast<uint64_t>(absolute_delta);
    }
#elif defined(OS_CLOCK_MONOTONIC)
    static constexpr os_clockid_t wait_clock = OS_CLOCK_MONOTONIC;

    static uint64_t nanoseconds_to_mach_absolute_ticks(uint64_t timeout_ns)
    {
        return timeout_ns;
    }
#elif defined(CLOCK_MONOTONIC)
    static constexpr os_clockid_t wait_clock = static_cast<os_clockid_t>(CLOCK_MONOTONIC);

    static uint64_t nanoseconds_to_mach_absolute_ticks(uint64_t timeout_ns)
    {
        return timeout_ns;
    }
#else
#   error "No supported monotonic clock id available for os_sync_wait_on_address_with_timeout"
#endif

    void initialise_os_sync(unsigned int initial_count)
    {
        m_os_sync.count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
    }

    void teardown_os_sync() noexcept {}

    void post_os_sync()
    {
        int32_t previous = m_os_sync.count.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
            wake_one_waiter();
        }
    }

    void wait_os_sync()
    {
        int32_t previous = m_os_sync.count.fetch_sub(1, std::memory_order_acq_rel);
        if (previous > 0) {
            return;
        }

        int32_t expected = previous - 1;
        while (true) {
            int32_t observed = wait_on_address_blocking(expected);
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
            return false;
        }

        auto count = remaining.count();
        if (count <= 0) {
            observed = m_os_sync.count.load(std::memory_order_acquire);
            return false;
        }

        uint64_t timeout_ns = static_cast<uint64_t>(count);

        while (true) {
            const auto scope = current_scope();
            uint64_t expected_value = static_cast<uint32_t>(expected);
            const uint64_t timeout_value = nanoseconds_to_mach_absolute_ticks(timeout_ns);
            int rc = os_sync_wait_on_address_with_timeout(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected_value,
                sizeof(int32_t),
                wait_flags_for_scope(scope),
                wait_clock,
                timeout_value);
            if (rc >= 0) {
                observed = m_os_sync.count.load(std::memory_order_acquire);
                return true;
            }
            if (errno == ETIMEDOUT) {
                observed = m_os_sync.count.load(std::memory_order_acquire);
                return false;
            }
            if (errno == EINTR || errno == EFAULT) {
                continue;
            }
            if (errno == EINVAL) {
                adjust_scope_after_einval(scope);
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "os_sync_wait_on_address_with_timeout");
        }
    }

    void cancel_wait_os_sync()
    {
        m_os_sync.count.fetch_add(1, std::memory_order_acq_rel);
    }

    int32_t wait_on_address_blocking(int32_t expected)
    {
        while (true) {
            const auto scope = current_scope();
            uint64_t expected_value = static_cast<uint32_t>(expected);
            int rc = os_sync_wait_on_address(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected_value,
                sizeof(int32_t),
                wait_flags_for_scope(scope));
            if (rc >= 0) {
                return m_os_sync.count.load(std::memory_order_acquire);
            }
            if (errno == EINTR || errno == EFAULT) {
                continue;
            }
            if (errno == EINVAL) {
                adjust_scope_after_einval(scope);
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "os_sync_wait_on_address");
        }
    }

    void wake_one_waiter()
    {
        while (true) {
            const auto scope = current_scope();
            int rc = os_sync_wake_by_address_any(
                reinterpret_cast<void*>(&m_os_sync.count),
                sizeof(int32_t),
                wake_flags_for_scope(scope));
            if (rc == 0) {
                return;
            }
            if (rc == -1) {
                if (errno == ENOENT) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EINVAL) {
                    adjust_scope_after_einval(scope);
                    continue;
                }
            }
            throw std::system_error(errno, std::generic_category(), "os_sync_wake_by_address_any");
        }
    }

    static os_sync_wait_on_address_flags_t wait_flags_for_scope(os_sync_address_scope scope)
    {
        return scope == os_sync_address_scope::shared ? shared_wait_flag : local_wait_flag;
    }

    static os_sync_wake_by_address_flags_t wake_flags_for_scope(os_sync_address_scope scope)
    {
        return scope == os_sync_address_scope::shared ? shared_wake_flag : local_wake_flag;
    }

    os_sync_address_scope current_scope() const
    {
        return m_os_sync.scope.load(std::memory_order_acquire);
    }

    void adjust_scope_after_einval(os_sync_address_scope observed_scope)
    {
        if (observed_scope != os_sync_address_scope::shared) {
            return;
        }

        auto expected = os_sync_address_scope::shared;
        (void)m_os_sync.scope.compare_exchange_strong(expected,
                                                      os_sync_address_scope::process_local,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
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

