#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
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
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0602
  #endif
  #include <Windows.h>
  #include <climits>
  #include <cwchar>
  #include <limits>
  #include <mutex>
  #include <unordered_map>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <cstdint>
  #include <limits>
  #include <sys/ulock.h>

  extern "C" {
    int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
    int __ulock_wait2(uint32_t operation, void* addr, uint64_t value, uint64_t timeout, uint64_t value2);
    int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
  }
#else
  #include <cerrno>
  #include <limits>
  #include <semaphore.h>
#endif

namespace sintra::detail
{

#if defined(_WIN32)
namespace interprocess_semaphore_detail
{
    inline uint64_t generate_global_identifier()
    {
        static const uint64_t process_entropy = [] {
            uint64_t value = static_cast<uint64_t>(::GetCurrentProcessId()) << 32;

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
} // namespace interprocess_semaphore_detail
#endif

class interprocess_semaphore
{
public:
    explicit interprocess_semaphore(unsigned int initial_count = 0)
    {
#if defined(_WIN32)
        initialise_windows(initial_count);
#elif defined(__APPLE__)
        initialise_ulock(initial_count);
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
        teardown_ulock();
#else
        teardown_posix();
#endif
    }

    void release_local_handle() noexcept
    {
#if defined(_WIN32)
        interprocess_semaphore_detail::close_handle(m_windows.id);
#elif defined(__APPLE__)
        // All state lives in shared memory; nothing to release locally.
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
        post_ulock();
#else
        while (sem_post(&m_sem) == -1) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "sem_post");
            }
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
        wait_ulock();
#else
        while (sem_wait(&m_sem) == -1) {
            if (errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "sem_wait");
            }
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
        return try_wait_ulock();
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
        return timed_wait_ulock(abs_time);
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
        timespec ts{};
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
    struct ulock_storage
    {
        std::atomic<int32_t>  count{0};
        std::atomic<uint32_t> sequence{0};
    };

    ulock_storage m_ulock;

    static void* wait_address(std::atomic<uint32_t>& value)
    {
        return static_cast<void*>(std::addressof(value));
    }

    void initialise_ulock(unsigned int initial_count)
    {
        m_ulock.count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
        m_ulock.sequence.store(0, std::memory_order_relaxed);
    }

    void teardown_ulock() noexcept {}

    void post_ulock()
    {
        m_ulock.count.fetch_add(1, std::memory_order_release);
        m_ulock.sequence.fetch_add(1, std::memory_order_release);
        int rc = __ulock_wake(
            UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
            wait_address(m_ulock.sequence),
            0);
        if (rc != 0 && rc != EALREADY && rc != ENOENT) {
            throw std::system_error(rc, std::system_category(), "__ulock_wake");
        }
    }

    bool try_wait_ulock()
    {
        int32_t expected = m_ulock.count.load(std::memory_order_acquire);
        while (expected > 0) {
            if (m_ulock.count.compare_exchange_weak(
                    expected,
                    expected - 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void wait_ulock()
    {
        while (true) {
            int32_t expected = m_ulock.count.load(std::memory_order_acquire);
            while (expected > 0) {
                if (m_ulock.count.compare_exchange_weak(
                        expected,
                        expected - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return;
                }
            }

            uint32_t sequence = m_ulock.sequence.load(std::memory_order_acquire);
            int rc = __ulock_wait(
                UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
                wait_address(m_ulock.sequence),
                sequence,
                0);
            if (rc == 0 || rc == EINTR) {
                continue;
            }
            throw std::system_error(rc, std::system_category(), "__ulock_wait");
        }
    }

    template <typename Clock, typename Duration>
    bool timed_wait_ulock(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        auto now = Clock::now();
        if (abs_time <= now) {
            return try_wait_ulock();
        }

        while (true) {
            int32_t expected = m_ulock.count.load(std::memory_order_acquire);
            while (expected > 0) {
                if (m_ulock.count.compare_exchange_weak(
                        expected,
                        expected - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return true;
                }
            }

            now = Clock::now();
            if (abs_time <= now) {
                return false;
            }

            auto remaining = abs_time - now;
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(remaining);
            int64_t clamped = std::clamp<int64_t>(
                micros.count(),
                int64_t{0},
                static_cast<int64_t>(std::numeric_limits<uint32_t>::max()));
            uint32_t timeout = static_cast<uint32_t>(clamped);
            if (timeout == 0 && micros.count() > 0) {
                timeout = 1;
            }

            uint32_t sequence = m_ulock.sequence.load(std::memory_order_acquire);
            int rc = __ulock_wait(
                UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
                wait_address(m_ulock.sequence),
                sequence,
                timeout);
            if (rc == 0 || rc == EINTR) {
                continue;
            }
            if (rc == ETIMEDOUT) {
                return false;
            }
            throw std::system_error(rc, std::system_category(), "__ulock_wait");
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

