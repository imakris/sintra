#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
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
  #include <mutex>
  #include <random>
  #include <synchapi.h>
  #include <unordered_map>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <sys/ulock.h>
  #include <unistd.h>
#else
  #include <cerrno>
  #include <linux/futex.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#endif

namespace sintra::detail
{
#if defined(_WIN32)
namespace interprocess_semaphore_detail
{
    inline uint64_t generate_global_identifier()
    {
        static const uint64_t process_entropy = [] {
            uint64_t value = 0;
            value ^= static_cast<uint64_t>(::GetCurrentProcessId()) << 32;

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

class interprocess_semaphore
{
public:
    using count_type = uint32_t;

    explicit interprocess_semaphore(unsigned int initial_count = 0)
    {
        if (initial_count > max_count) {
            throw std::invalid_argument("interprocess_semaphore initial count too large");
        }
        initialise_windows(initial_count);
    }

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    ~interprocess_semaphore() noexcept
    {
        teardown_windows();
    }

    void release_local_handle() noexcept
    {
        interprocess_semaphore_detail::close_handle(m_windows.id);
    }

    void post()
    {
        HANDLE handle = windows_handle();
        // WaitOnAddress only synchronizes threads within a single process, so we rely on
        // the named kernel semaphore to ensure posts in one process release waiters in
        // another process that shares the ring buffer.
        if (!::ReleaseSemaphore(handle, 1, nullptr)) {
            throw std::system_error(::GetLastError(), std::system_category(), "ReleaseSemaphore");
        }
    }

    void wait()
    {
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, INFINITE);
        if (result != WAIT_OBJECT_0) {
            throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
        }
    }

    bool try_wait()
    {
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, 0);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
    }

    template <typename Clock, typename Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
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
    }

private:
    static constexpr count_type max_count = static_cast<count_type>(LONG_MAX);

    struct windows_storage
    {
        uint64_t id = 0;
        wchar_t  name[64]{};
    };

    windows_storage m_windows{};

    void initialise_windows(unsigned int initial_count)
    {
        m_windows.id = interprocess_semaphore_detail::generate_global_identifier();
        std::swprintf(m_windows.name,
                      sizeof(m_windows.name) / sizeof(m_windows.name[0]),
                      L"SintraSemaphore_%016llX",
                      static_cast<unsigned long long>(m_windows.id));

        HANDLE handle = ::CreateSemaphoreW(nullptr,
                                           static_cast<LONG>(initial_count),
                                           LONG_MAX,
                                           m_windows.name);
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
};

#else // defined(_WIN32)

class interprocess_semaphore
{
public:
    using count_type = uint32_t;

    explicit interprocess_semaphore(unsigned int initial_count = 0)
    {
        if (initial_count > max_count) {
            throw std::invalid_argument("interprocess_semaphore initial count too large");
        }
        m_count.store(static_cast<count_type>(initial_count), std::memory_order_relaxed);
    }

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    ~interprocess_semaphore() = default;

    void release_local_handle() noexcept {}

    void post()
    {
        count_type value = m_count.load(std::memory_order_relaxed);
        while (true) {
            if (value == max_count) {
                throw std::system_error(EOVERFLOW, std::generic_category(), "interprocess_semaphore overflow");
            }
            if (m_count.compare_exchange_weak(value,
                                              static_cast<count_type>(value + 1),
                                              std::memory_order_release,
                                              std::memory_order_relaxed))
            {
                break;
            }
        }

        wake_one();
    }

    void wait()
    {
        count_type expected = m_count.load(std::memory_order_acquire);
        for (;;) {
            while (expected == 0) {
                wait_for_value(expected, infinite_timeout());
                expected = m_count.load(std::memory_order_acquire);
            }

            if (m_count.compare_exchange_weak(expected,
                                              static_cast<count_type>(expected - 1),
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
            {
                return;
            }
        }
    }

    bool try_wait()
    {
        count_type expected = m_count.load(std::memory_order_acquire);
        while (expected != 0) {
            if (m_count.compare_exchange_weak(expected,
                                              static_cast<count_type>(expected - 1),
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    template <typename Clock, typename Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        while (true) {
            if (try_wait()) {
                return true;
            }

            auto now = Clock::now();
            if (now >= abs_time) {
                return false;
            }

            auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(abs_time - now);
            if (!wait_for_value(0, remaining)) {
                return try_wait();
            }
        }
    }

private:
    static constexpr count_type max_count = std::numeric_limits<count_type>::max();

    static count_type* raw_address(std::atomic<count_type>& value) noexcept
    {
        return reinterpret_cast<count_type*>(std::addressof(value));
    }

    static const count_type* raw_address(const std::atomic<count_type>& value) noexcept
    {
        return reinterpret_cast<const count_type*>(std::addressof(value));
    }

    static std::chrono::nanoseconds infinite_timeout() noexcept
    {
        return std::chrono::nanoseconds::max();
    }

    void wake_one()
    {
#if defined(__APPLE__)
        const uint32_t op = UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ONE | ULF_WAKE_ALLOW_NON_OWNER;
        int rc = ::ulock_wake(op, raw_address(m_count), 0);
        if (rc == -1) {
            int err = errno;
            if (err == ENOENT) {
                return;
            }
            throw std::system_error(err, std::generic_category(), "ulock_wake");
        }
#else
        int rc = static_cast<int>(::syscall(SYS_futex,
                                            raw_address(m_count),
                                            FUTEX_WAKE,
                                            1,
                                            nullptr,
                                            nullptr,
                                            0));
        if (rc == -1) {
            throw std::system_error(errno, std::generic_category(), "futex(FUTEX_WAKE)");
        }
#endif
    }

    bool wait_for_value(count_type expected, std::chrono::nanoseconds timeout)
    {
#if defined(__APPLE__)
        const uint32_t wait_op = UL_COMPARE_AND_WAIT_SHARED;
        if (timeout == infinite_timeout()) {
            for (;;) {
                int rc = ::ulock_wait(wait_op, raw_address(m_count), expected, 0);
                if (rc == 0) {
                    return true;
                }
                int err = errno;
                if (err == EINTR) {
                    continue;
                }
                if (err == EBUSY) {
                    return true;
                }
                throw std::system_error(err, std::generic_category(), "ulock_wait");
            }
        }

        if (timeout <= std::chrono::nanoseconds::zero()) {
            return false;
        }

        uint64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
        if (micros == 0) {
            micros = 1;
        }

        uint32_t capped = static_cast<uint32_t>(std::min<uint64_t>(micros, std::numeric_limits<uint32_t>::max()));
        int rc = ::ulock_wait(wait_op, raw_address(m_count), expected, capped);
        if (rc == 0) {
            return true;
        }

        int err = errno;
        if (err == ETIMEDOUT) {
            return false;
        }
        if (err == EINTR || err == EBUSY) {
            return true;
        }
        throw std::system_error(err, std::generic_category(), "ulock_wait");
#else
        if (timeout == infinite_timeout()) {
            for (;;) {
                int rc = static_cast<int>(::syscall(SYS_futex,
                                                    raw_address(m_count),
                                                    FUTEX_WAIT,
                                                    expected,
                                                    nullptr,
                                                    nullptr,
                                                    0));
                if (rc == 0) {
                    return true;
                }
                int err = errno;
                if (err == EINTR) {
                    continue;
                }
                if (err == EAGAIN) {
                    return true;
                }
                throw std::system_error(err, std::generic_category(), "futex(FUTEX_WAIT)");
            }
        }

        if (timeout <= std::chrono::nanoseconds::zero()) {
            return false;
        }

        struct timespec ts;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
        ts.tv_sec = static_cast<time_t>(secs.count());
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
        ts.tv_nsec = static_cast<long>(ns.count());

        int rc = static_cast<int>(::syscall(SYS_futex,
                                            raw_address(m_count),
                                            FUTEX_WAIT,
                                            expected,
                                            &ts,
                                            nullptr,
                                            0));
        if (rc == 0) {
            return true;
        }

        int err = errno;
        if (err == ETIMEDOUT) {
            return false;
        }
        if (err == EINTR || err == EAGAIN) {
            return true;
        }
        throw std::system_error(err, std::generic_category(), "futex(FUTEX_WAIT)");
#endif
        return true;
    }

    alignas(sizeof(count_type)) std::atomic<count_type> m_count{0};
};

#endif // defined(_WIN32)

} // namespace sintra::detail
