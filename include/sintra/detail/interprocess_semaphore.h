#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
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
  #include <unordered_map>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <cstdio>
  #include <ctime>
  #include <dlfcn.h>
  #include <fcntl.h>
  #include <mutex>
  #include <semaphore.h>
  #include <unordered_map>

  #if defined(__has_include) && __has_include(<os/clock.h>)
    #include <os/clock.h>
  #else
    #include <time.h>
    using os_clockid_t = clockid_t;
    #ifndef OS_CLOCK_REALTIME
      #define OS_CLOCK_REALTIME CLOCK_REALTIME
    #endif
  #endif

  #if defined(__has_include) && __has_include(<os/os_sync_wait_on_address.h>)
    #include <os/os_sync_wait_on_address.h>
  #else
    using os_sync_wait_on_address_flags_t = uint32_t;
    using os_sync_wake_by_address_flags_t = uint32_t;

    constexpr os_sync_wait_on_address_flags_t OS_SYNC_WAIT_ON_ADDRESS_NONE = 0x00000000u;
    constexpr os_sync_wait_on_address_flags_t OS_SYNC_WAIT_ON_ADDRESS_SHARED = 0x00000001u;
    constexpr os_sync_wake_by_address_flags_t OS_SYNC_WAKE_BY_ADDRESS_NONE = 0x00000000u;
    constexpr os_sync_wake_by_address_flags_t OS_SYNC_WAKE_BY_ADDRESS_SHARED = 0x00000001u;
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

        HANDLE handle = ::OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "OpenSemaphoreW");
        }

        std::lock_guard<std::mutex> lock(handle_mutex());
        return handle_map().emplace(id, handle).first->second;
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
#elif defined(__APPLE__)
    inline std::mutex& handle_mutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    inline std::unordered_map<uint64_t, sem_t*>& handle_map()
    {
        static std::unordered_map<uint64_t, sem_t*> map;
        return map;
    }

    inline sem_t* register_handle(uint64_t id, sem_t* handle)
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        handle_map()[id] = handle;
        return handle;
    }

    inline sem_t* ensure_handle(uint64_t id, const char* name)
    {
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                return it->second;
            }
        }

        sem_t* sem = sem_open(name, 0);
        if (sem == SEM_FAILED) {
            throw std::system_error(errno, std::generic_category(), "sem_open");
        }

        std::lock_guard<std::mutex> lock(handle_mutex());
        return handle_map().emplace(id, sem).first->second;
    }

    inline void close_handle(uint64_t id)
    {
        sem_t* sem = nullptr;
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                sem = it->second;
                handle_map().erase(it);
            }
        }

        if (sem) {
            while (sem_close(sem) == -1 && errno == EINTR) {}
        }
    }

    struct os_sync_symbols
    {
        using wait_on_address_fn = int (*)(void*, uint64_t, size_t, os_sync_wait_on_address_flags_t);
        using wait_on_address_with_timeout_fn = int (*)(void*, uint64_t, size_t, os_sync_wait_on_address_flags_t, os_clockid_t, uint64_t);
        using wake_by_address_any_fn = int (*)(void*, size_t, os_sync_wake_by_address_flags_t);
        using wake_by_address_all_fn = int (*)(void*, size_t, os_sync_wake_by_address_flags_t);

        wait_on_address_fn wait_on_address = nullptr;
        wait_on_address_with_timeout_fn wait_on_address_with_timeout = nullptr;
        wake_by_address_any_fn wake_by_address_any = nullptr;
        wake_by_address_all_fn wake_by_address_all = nullptr;
        bool available = false;
    };

    inline os_sync_symbols load_os_sync_symbols()
    {
        os_sync_symbols symbols{};

        auto load = [](const char* name) -> void*
        {
            dlerror();
            return dlsym(RTLD_DEFAULT, name);
        };

        symbols.wait_on_address = reinterpret_cast<os_sync_symbols::wait_on_address_fn>(load("os_sync_wait_on_address"));
        symbols.wait_on_address_with_timeout =
            reinterpret_cast<os_sync_symbols::wait_on_address_with_timeout_fn>(load("os_sync_wait_on_address_with_timeout"));
        symbols.wake_by_address_any =
            reinterpret_cast<os_sync_symbols::wake_by_address_any_fn>(load("os_sync_wake_by_address_any"));
        symbols.wake_by_address_all =
            reinterpret_cast<os_sync_symbols::wake_by_address_all_fn>(load("os_sync_wake_by_address_all"));

        symbols.available = symbols.wait_on_address && symbols.wait_on_address_with_timeout &&
                            symbols.wake_by_address_any && symbols.wake_by_address_all;
        return symbols;
    }

    inline const os_sync_symbols& resolved_os_sync_symbols()
    {
        static const os_sync_symbols symbols = load_os_sync_symbols();
        return symbols;
    }

    inline bool has_os_sync_support()
    {
        return resolved_os_sync_symbols().available;
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
        if (interprocess_semaphore_detail::has_os_sync_support()) {
            initialise_os_sync(initial_count);
            m_use_os_sync = true;
        } else {
            initialise_named(initial_count);
        }
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
        if (m_use_os_sync) {
            teardown_os_sync();
        } else {
            teardown_named();
        }
#else
        teardown_posix();
#endif
    }

    void release_local_handle() noexcept
    {
#if defined(_WIN32)
        interprocess_semaphore_detail::close_handle(m_windows.id);
#elif defined(__APPLE__)
        if (!m_use_os_sync) {
            interprocess_semaphore_detail::close_handle(m_named.id);
        }
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
        if (m_use_os_sync) {
            post_os_sync();
        } else {
            post_named();
        }
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
        if (m_use_os_sync) {
            wait_os_sync();
        } else {
            wait_named();
        }
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
        if (m_use_os_sync) {
            return try_acquire_os_sync();
        }

        return try_wait_named();
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
        if (m_use_os_sync) {
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
        }

        auto ts = make_abs_timespec(abs_time);
        sem_t* sem = named_handle();
        while (sem_timedwait(sem, &ts) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ETIMEDOUT) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_timedwait");
        }
        return true;
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

    struct named_storage
    {
        uint64_t id = 0;
        char     name[32]{};
    };

    os_sync_storage m_os_sync{};
    named_storage   m_named{};
    bool            m_use_os_sync = false;

    static constexpr os_sync_wait_on_address_flags_t wait_flags = OS_SYNC_WAIT_ON_ADDRESS_SHARED;
    static constexpr os_sync_wake_by_address_flags_t wake_flags = OS_SYNC_WAKE_BY_ADDRESS_SHARED;

#    ifdef OS_CLOCK_REALTIME
    static constexpr os_clockid_t wait_clock = OS_CLOCK_REALTIME;
#    elif defined(CLOCK_REALTIME)
    static constexpr os_clockid_t wait_clock = static_cast<os_clockid_t>(CLOCK_REALTIME);
#    elif defined(OS_CLOCK_MACH_ABSOLUTE_TIME)
    static constexpr os_clockid_t wait_clock = OS_CLOCK_MACH_ABSOLUTE_TIME;
#    else
#      error "No supported clock id available for os_sync_wait_on_address_with_timeout"
#    endif

    void initialise_os_sync(unsigned int initial_count)
    {
        m_os_sync.count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
    }

    void initialise_named(unsigned int initial_count)
    {
        m_named.id = interprocess_semaphore_detail::generate_global_identifier();
        std::snprintf(m_named.name,
                      sizeof(m_named.name) / sizeof(m_named.name[0]),
                      "/sintra_sem_%016llx",
                      static_cast<unsigned long long>(m_named.id));

        sem_unlink(m_named.name);
        sem_t* sem = sem_open(m_named.name, O_CREAT | O_EXCL, 0600, initial_count);
        if (sem == SEM_FAILED) {
            throw std::system_error(errno, std::generic_category(), "sem_open");
        }

        interprocess_semaphore_detail::register_handle(m_named.id, sem);
    }

    void teardown_os_sync() noexcept {}

    void teardown_named() noexcept
    {
        interprocess_semaphore_detail::close_handle(m_named.id);
        if (m_named.name[0] != '\0') {
            sem_unlink(m_named.name);
        }
    }

    sem_t* named_handle() const
    {
        return interprocess_semaphore_detail::ensure_handle(m_named.id, m_named.name);
    }

    void post_os_sync()
    {
        int32_t previous = m_os_sync.count.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
            wake_one_waiter();
        }
    }

    void post_named()
    {
        sem_t* sem = named_handle();
        while (sem_post(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_post");
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

    void wait_named()
    {
        sem_t* sem = named_handle();
        while (sem_wait(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_wait");
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

    bool try_wait_named()
    {
        sem_t* sem = named_handle();
        while (sem_trywait(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_trywait");
        }
        return true;
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

        const auto& symbols = interprocess_semaphore_detail::resolved_os_sync_symbols();

        while (true) {
            int rc = symbols.wait_on_address_with_timeout(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected,
                sizeof(int32_t),
                wait_flags,
                wait_clock,
                timeout_ns);
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
            throw std::system_error(errno, std::generic_category(), "os_sync_wait_on_address_with_timeout");
        }
    }

    void cancel_wait_os_sync()
    {
        m_os_sync.count.fetch_add(1, std::memory_order_acq_rel);
    }

    int32_t wait_on_address_blocking(int32_t expected)
    {
        const auto& symbols = interprocess_semaphore_detail::resolved_os_sync_symbols();

        while (true) {
            int rc = symbols.wait_on_address(
                reinterpret_cast<void*>(&m_os_sync.count),
                expected,
                sizeof(int32_t),
                wait_flags);
            if (rc >= 0) {
                return m_os_sync.count.load(std::memory_order_acquire);
            }
            if (errno == EINTR || errno == EFAULT) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "os_sync_wait_on_address");
        }
    }

    void wake_one_waiter()
    {
        const auto& symbols = interprocess_semaphore_detail::resolved_os_sync_symbols();

        while (true) {
            int rc = symbols.wake_by_address_any(
                reinterpret_cast<void*>(&m_os_sync.count),
                sizeof(int32_t),
                wake_flags);
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
            }
            throw std::system_error(errno, std::generic_category(), "os_sync_wake_by_address_any");
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

