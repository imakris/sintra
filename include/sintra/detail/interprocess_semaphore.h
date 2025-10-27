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
  #include <limits>
  #include <sys/ulock.h>
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
        initialise_darwin(initial_count);
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
        teardown_darwin();
#else
        teardown_posix();
#endif
    }

    void release_local_handle() noexcept
    {
#if defined(_WIN32)
        interprocess_semaphore_detail::close_handle(m_windows.id);
#elif defined(__APPLE__)
        // Nothing to do for the Darwin wait-based backend.
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
        darwin_post();
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
        darwin_wait();
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
        return darwin_try_wait();
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
        return darwin_timed_wait(abs_time);
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
    struct darwin_storage
    {
        std::atomic<int32_t> count{0};
        std::atomic<uint32_t> wake_sequence{0};
        std::atomic<int32_t> waiters{0};
    };

    darwin_storage m_darwin;

    void initialise_darwin(unsigned int initial_count)
    {
        if (initial_count > static_cast<unsigned int>(std::numeric_limits<int32_t>::max())) {
            throw std::system_error(EINVAL, std::generic_category(), "initial_count too large");
        }
        m_darwin.count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
        m_darwin.wake_sequence.store(0, std::memory_order_relaxed);
        m_darwin.waiters.store(0, std::memory_order_relaxed);
    }

    void teardown_darwin() noexcept {}

    struct darwin_waiter_guard
    {
        explicit darwin_waiter_guard(std::atomic<int32_t>& waiters) noexcept
            : m_waiters(waiters)
        {
            m_waiters.fetch_add(1, std::memory_order_acq_rel);
        }

        darwin_waiter_guard(const darwin_waiter_guard&) = delete;
        darwin_waiter_guard& operator=(const darwin_waiter_guard&) = delete;

        ~darwin_waiter_guard()
        {
            release();
        }

        void release() noexcept
        {
            if (m_active) {
                m_waiters.fetch_sub(1, std::memory_order_acq_rel);
                m_active = false;
            }
        }

    private:
        std::atomic<int32_t>& m_waiters;
        bool                  m_active{true};
    };

    bool darwin_try_acquire()
    {
        int32_t available = m_darwin.count.load(std::memory_order_acquire);
        while (available > 0) {
            if (m_darwin.count.compare_exchange_weak(available,
                                                     available - 1,
                                                     std::memory_order_acquire,
                                                     std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void darwin_post()
    {
        m_darwin.count.fetch_add(1, std::memory_order_release);
        if (m_darwin.waiters.load(std::memory_order_acquire) > 0) {
            m_darwin.wake_sequence.fetch_add(1, std::memory_order_acq_rel);
            int rc = __ulock_wake(UL_COMPARE_AND_WAIT | ULF_SHARED,
                                   reinterpret_cast<void*>(&m_darwin.wake_sequence),
                                   0);
            if (rc != 0) {
                int err = errno;
                if (err != ENOENT) {
                    throw std::system_error(err, std::generic_category(), "__ulock_wake");
                }
            }
        }
    }

    void darwin_wait()
    {
        if (darwin_try_acquire()) {
            return;
        }

        while (true) {
            const uint32_t seq = m_darwin.wake_sequence.load(std::memory_order_acquire);
            darwin_waiter_guard guard(m_darwin.waiters);

            if (darwin_try_acquire()) {
                guard.release();
                return;
            }

            int rc;
            do {
                rc = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_SHARED,
                                   reinterpret_cast<void*>(&m_darwin.wake_sequence),
                                   seq,
                                   0);
            } while (rc == -1 && errno == EINTR);

            guard.release();

            if (rc == 0 || (rc == -1 && errno == EAGAIN)) {
                continue;
            }

            if (rc == -1) {
                throw std::system_error(errno, std::generic_category(), "__ulock_wait");
            }
        }
    }

    bool darwin_try_wait()
    {
        return darwin_try_acquire();
    }

    template <typename Clock, typename Duration>
    bool darwin_timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        if (darwin_try_acquire()) {
            return true;
        }

        while (true) {
            auto now = Clock::now();
            if (abs_time <= now) {
                return false;
            }

            const uint32_t seq = m_darwin.wake_sequence.load(std::memory_order_acquire);
            darwin_waiter_guard guard(m_darwin.waiters);

            if (darwin_try_acquire()) {
                guard.release();
                return true;
            }

            now = Clock::now();
            if (abs_time <= now) {
                guard.release();
                return false;
            }

            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(abs_time - now);
            if (remaining <= std::chrono::microseconds::zero()) {
                guard.release();
                return false;
            }

            uint64_t timeout_us = static_cast<uint64_t>(remaining.count());
            if (timeout_us == 0) {
                timeout_us = 1;
            }
            if (timeout_us > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                timeout_us = std::numeric_limits<uint32_t>::max();
            }

            int rc;
            do {
                rc = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_SHARED,
                                   reinterpret_cast<void*>(&m_darwin.wake_sequence),
                                   seq,
                                   static_cast<uint32_t>(timeout_us));
            } while (rc == -1 && errno == EINTR);

            guard.release();

            if (rc == 0 || (rc == -1 && errno == EAGAIN)) {
                continue;
            }

            if (rc == -1 && errno == ETIMEDOUT) {
                if (Clock::now() >= abs_time) {
                    return false;
                }
                continue;
            }

            if (rc == -1) {
                throw std::system_error(errno, std::generic_category(), "__ulock_wait");
            }
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
