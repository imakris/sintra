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
  #include <synchapi.h>
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
#if defined(_WIN32)
        ::WakeByAddressSingle(raw_address(m_count));
#elif defined(__APPLE__)
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
#if defined(_WIN32)
        if (timeout == infinite_timeout()) {
            for (;;) {
                count_type compare = expected;
                if (::WaitOnAddress(raw_address(m_count), &compare, sizeof(compare), INFINITE)) {
                    return true;
                }
                DWORD err = ::GetLastError();
                if (err == ERROR_ALERTED) {
                    continue;
                }
                throw std::system_error(err, std::system_category(), "WaitOnAddress");
            }
        }

        if (timeout <= std::chrono::nanoseconds::zero()) {
            return false;
        }

        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
        DWORD wait_ms;
        if (millis.count() >= static_cast<long long>(std::numeric_limits<DWORD>::max())) {
            wait_ms = INFINITE;
        }
        else {
            wait_ms = static_cast<DWORD>(millis.count());
            if (wait_ms == 0) {
                wait_ms = 1;
            }
        }

        count_type compare = expected;
        if (::WaitOnAddress(raw_address(m_count), &compare, sizeof(compare), wait_ms)) {
            return true;
        }

        DWORD err = ::GetLastError();
        if (err == ERROR_TIMEOUT) {
            return false;
        }
        if (err == ERROR_ALERTED) {
            return true;
        }
        throw std::system_error(err, std::system_category(), "WaitOnAddress");
#elif defined(__APPLE__)
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

} // namespace sintra::detail
