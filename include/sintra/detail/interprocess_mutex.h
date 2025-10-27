#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <system_error>
#include <thread>

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
#elif defined(__unix__) || defined(__APPLE__)
  #include <errno.h>
  #include <pthread.h>
  #include <time.h>
#else
  #error "Unsupported platform for interprocess_mutex"
#endif

#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
#endif

namespace sintra::detail
{
namespace interprocess_mutex_detail
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

        HANDLE handle = ::OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "OpenMutexW");
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
#endif
} // namespace interprocess_mutex_detail

class interprocess_mutex
{
public:
    interprocess_mutex()
    {
#if defined(_WIN32)
        initialise_windows();
#else
        initialise_posix();
#endif
    }

    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    ~interprocess_mutex() noexcept
    {
#if defined(_WIN32)
        teardown_windows();
#else
        teardown_posix();
#endif
    }

    void release_local_handle() noexcept
    {
#if defined(_WIN32)
        interprocess_mutex_detail::close_handle(m_windows.id);
#endif
    }

    void lock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#else
        int res = ::pthread_mutex_lock(&m_mutex);
    #if defined(PTHREAD_MUTEX_ROBUST) || defined(PTHREAD_MUTEX_ROBUST_NP)
        if (res == EOWNERDEAD) {
            make_mutex_consistent();
            return;
        }
    #endif
        if (res == 0) {
            return;
        }
        throw std::system_error(res, std::generic_category(), "pthread_mutex_lock");
#endif
    }

    bool try_lock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#else
        int res = ::pthread_mutex_trylock(&m_mutex);
    #if defined(PTHREAD_MUTEX_ROBUST) || defined(PTHREAD_MUTEX_ROBUST_NP)
        if (res == EOWNERDEAD) {
            make_mutex_consistent();
            return true;
        }
    #endif
        if (res == 0) {
            return true;
        }
        if (res == EBUSY) {
            return false;
        }
        throw std::system_error(res, std::generic_category(), "pthread_mutex_trylock");
#endif
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& duration)
    {
        return try_lock_until(std::chrono::steady_clock::now() + duration);
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        return timed_lock(abs_time);
    }

    template <typename Clock, typename Duration>
    bool timed_lock(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        auto now = Clock::now();
        if (abs_time <= now) {
            DWORD result = ::WaitForSingleObject(handle, 0);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
                return true;
            }
            if (result == WAIT_TIMEOUT) {
                return false;
            }
            throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
        }

        auto remaining = abs_time - now;
        auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining);
        DWORD timeout;
        if (ms.count() <= 0) {
            timeout = 0;
        }
        else if (ms.count() >= static_cast<long long>(std::numeric_limits<DWORD>::max())) {
            timeout = INFINITE;
        }
        else {
            timeout = static_cast<DWORD>(ms.count());
        }

        DWORD result = ::WaitForSingleObject(handle, timeout);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#else
        auto now = Clock::now();
        if (abs_time <= now) {
            return try_lock();
        }

    #if defined(__APPLE__)
        while (true) {
            if (try_lock()) {
                return true;
            }
            now = Clock::now();
            if (now >= abs_time) {
                return false;
            }
            auto remaining = abs_time - now;
            auto sleep_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
            const auto max_sleep = std::chrono::nanoseconds(5'000'000);
            if (sleep_ns <= std::chrono::nanoseconds::zero()) {
                std::this_thread::yield();
            }
            else {
                if (sleep_ns > max_sleep) {
                    sleep_ns = max_sleep;
                }
                std::this_thread::sleep_for(sleep_ns);
            }
        }
    #else
        auto ts = make_abs_timespec(abs_time);
        int res = ::pthread_mutex_timedlock(&m_mutex, &ts);
    #if defined(PTHREAD_MUTEX_ROBUST) || defined(PTHREAD_MUTEX_ROBUST_NP)
        if (res == EOWNERDEAD) {
            make_mutex_consistent();
            return true;
        }
    #endif
        if (res == 0) {
            return true;
        }
        if (res == ETIMEDOUT) {
            return false;
        }
        throw std::system_error(res, std::generic_category(), "pthread_mutex_timedlock");
    #endif
#endif
    }

    void unlock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        if (!::ReleaseMutex(handle)) {
            throw std::system_error(::GetLastError(), std::system_category(), "ReleaseMutex");
        }
#else
        int res = ::pthread_mutex_unlock(&m_mutex);
        if (res != 0) {
            throw std::system_error(res, std::generic_category(), "pthread_mutex_unlock");
        }
#endif
    }

private:
#if defined(_WIN32)
    struct windows_storage
    {
        uint64_t id = 0;
        wchar_t  name[64]{};
    };

    windows_storage m_windows{};

    void initialise_windows()
    {
        m_windows.id = interprocess_mutex_detail::generate_global_identifier();
        std::swprintf(m_windows.name,
                      sizeof(m_windows.name) / sizeof(m_windows.name[0]),
                      L"SintraMutex_%016llX",
                      static_cast<unsigned long long>(m_windows.id));

        HANDLE handle = ::CreateMutexW(nullptr, FALSE, m_windows.name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateMutexW");
        }

        interprocess_mutex_detail::register_handle(m_windows.id, handle);
    }

    HANDLE windows_handle() const
    {
        return interprocess_mutex_detail::ensure_handle(m_windows.id, m_windows.name);
    }

    void teardown_windows() noexcept
    {
        interprocess_mutex_detail::close_handle(m_windows.id);
    }
#else
    pthread_mutex_t m_mutex{};

    void initialise_posix()
    {
        pthread_mutexattr_t attr;
        int res = ::pthread_mutexattr_init(&attr);
        if (res != 0) {
            throw std::system_error(res, std::generic_category(), "pthread_mutexattr_init");
        }

        res = ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (res != 0) {
            ::pthread_mutexattr_destroy(&attr);
            throw std::system_error(res, std::generic_category(), "pthread_mutexattr_setpshared");
        }

#if defined(PTHREAD_MUTEX_ROBUST) || defined(PTHREAD_MUTEX_ROBUST_NP)
    #if defined(PTHREAD_MUTEX_ROBUST)
        res = ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    #else
        res = ::pthread_mutexattr_setrobust_np(&attr, PTHREAD_MUTEX_ROBUST_NP);
    #endif
        if (res != 0) {
            ::pthread_mutexattr_destroy(&attr);
            throw std::system_error(res, std::generic_category(), "pthread_mutexattr_setrobust");
        }
#endif

        res = ::pthread_mutex_init(&m_mutex, &attr);
        ::pthread_mutexattr_destroy(&attr);
        if (res != 0) {
            throw std::system_error(res, std::generic_category(), "pthread_mutex_init");
        }
    }

    void teardown_posix() noexcept
    {
        ::pthread_mutex_destroy(&m_mutex);
    }

#if defined(PTHREAD_MUTEX_ROBUST) || defined(PTHREAD_MUTEX_ROBUST_NP)
    void make_mutex_consistent()
    {
    #if defined(PTHREAD_MUTEX_ROBUST)
        int res = ::pthread_mutex_consistent(&m_mutex);
    #else
        int res = ::pthread_mutex_consistent_np(&m_mutex);
    #endif
        if (res != 0) {
            throw std::system_error(res, std::generic_category(), "pthread_mutex_consistent");
        }
    }
#endif

    template <typename Clock, typename Duration>
    static timespec make_abs_timespec(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        using namespace std::chrono;
        auto now = Clock::now();
        auto delta = abs_time > now ? abs_time - now : Clock::duration::zero();
        auto sys_time = system_clock::now() + duration_cast<system_clock::duration>(delta);
        auto ns = duration_cast<nanoseconds>(sys_time.time_since_epoch());
        timespec ts;
        ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000);
        ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000);
        return ts;
    }
#endif
};

} // namespace sintra::detail

