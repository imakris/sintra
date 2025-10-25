#pragma once

// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_WIN32)
  #include <Windows.h>
  #include <synchapi.h>
#elif defined(__APPLE__)
  #include <errno.h>
  #include <sys/ulock.h>
#else
  #include <errno.h>
  #include <linux/futex.h>
  #include <sys/syscall.h>
  #include <time.h>
  #include <unistd.h>
#endif

namespace sintra {
namespace ipc {

namespace detail {

enum class wait_status { value_changed, timed_out };

inline std::chrono::steady_clock::time_point to_steady(
    const std::chrono::steady_clock::time_point& tp)
{
    return tp;
}

template <class Clock, class Duration>
inline std::chrono::steady_clock::time_point to_steady(
    const std::chrono::time_point<Clock, Duration>& tp)
{
    const auto now_clock  = Clock::now();
    const auto now_steady = std::chrono::steady_clock::now();
    const auto delta      = tp - now_clock;
    return now_steady + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta);
}

#if defined(_WIN32)

inline void platform_wake(std::atomic<int32_t>& value)
{
    ::WakeByAddressSingle(static_cast<volatile void*>(static_cast<void*>(&value)));
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected)
{
    int32_t snapshot = expected;
    while (true) {
        if (::WaitOnAddress(static_cast<volatile void*>(static_cast<void*>(&value)),
                             &snapshot,
                             sizeof(snapshot),
                             INFINITE)) {
            return;
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_TIMEOUT) {
            return;
        }
        if (error != ERROR_IO_PENDING && error != ERROR_OPERATION_ABORTED) {
            return;
        }
        snapshot = expected;
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) {
        int32_t snapshot = expected;
        if (!::WaitOnAddress(static_cast<volatile void*>(static_cast<void*>(&value)),
                              &snapshot,
                              sizeof(snapshot),
                              0)) {
            return (::GetLastError() == ERROR_TIMEOUT) ? wait_status::timed_out : wait_status::value_changed;
        }
        return wait_status::value_changed;
    }

    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    DWORD wait_ms;
    if (millis.count() <= 0) {
        wait_ms = 1;
    } else if (millis.count() >= static_cast<long long>(std::numeric_limits<DWORD>::max())) {
        wait_ms = std::numeric_limits<DWORD>::max() - 1;
    } else {
        wait_ms = static_cast<DWORD>(millis.count());
    }

    int32_t snapshot = expected;
    if (!::WaitOnAddress(static_cast<volatile void*>(static_cast<void*>(&value)),
                          &snapshot,
                          sizeof(snapshot),
                          wait_ms)) {
        return (::GetLastError() == ERROR_TIMEOUT) ? wait_status::timed_out : wait_status::value_changed;
    }
    return wait_status::value_changed;
}

#elif defined(__APPLE__)

#ifndef UL_COMPARE_AND_WAIT_SHARED
#define UL_COMPARE_AND_WAIT_SHARED (UL_COMPARE_AND_WAIT | ULF_SHARED)
#endif

inline void platform_wake(std::atomic<int32_t>& value)
{
    __ulock_wake(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), 0);
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected)
{
    while (true) {
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), expected, 0);
        if (ret == 0) {
            return;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN) {
            return;
        }
        // Other errors break out to avoid spinning.
        return;
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) {
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), expected, 0);
        if (ret == 0) {
            return wait_status::value_changed;
        }
        int err = errno;
        if (err == EAGAIN || err == EINTR) {
            return wait_status::value_changed;
        }
        return wait_status::timed_out;
    }

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    uint64_t us = static_cast<uint64_t>(micros.count());
    if (us == 0) {
        us = 1;
    }
    if (us > std::numeric_limits<uint32_t>::max()) {
        us = std::numeric_limits<uint32_t>::max();
    }

    int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED,
                           static_cast<void*>(&value),
                           expected,
                           static_cast<uint32_t>(us));
    if (ret == 0) {
        return wait_status::value_changed;
    }
    int err = errno;
    if (err == ETIMEDOUT) {
        return wait_status::timed_out;
    }
    if (err == EINTR) {
        return wait_status::value_changed;
    }
    return (err == EAGAIN) ? wait_status::value_changed : wait_status::timed_out;
}

#else // Linux / POSIX fallback

inline void platform_wake(std::atomic<int32_t>& value)
{
    syscall(SYS_futex, static_cast<int32_t*>(static_cast<void*>(&value)), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected)
{
    while (true) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          nullptr,
                          nullptr,
                          0);
        if (res == 0) {
            return;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN) {
            return;
        }
        return;
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          nullptr,
                          nullptr,
                          0);
        if (res == 0) {
            return wait_status::value_changed;
        }
        return (errno == EAGAIN) ? wait_status::value_changed : wait_status::timed_out;
    }

    timespec ts;
    auto      secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto      nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
    ts.tv_sec  = static_cast<long>(secs.count());
    ts.tv_nsec = static_cast<long>(nanos.count());

    while (true) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          &ts,
                          nullptr,
                          0);
        if (res == 0) {
            return wait_status::value_changed;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == ETIMEDOUT) {
            return wait_status::timed_out;
        }
        if (err == EAGAIN) {
            return wait_status::value_changed;
        }
        return wait_status::timed_out;
    }
}

#endif

} // namespace detail

class interprocess_semaphore {
public:
    explicit interprocess_semaphore(unsigned int initial_count = 0) noexcept
        : m_count(static_cast<int32_t>(initial_count))
    {}

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    void post()
    {
        int32_t previous = m_count.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
            detail::platform_wake(m_count);
        }
    }

    void wait()
    {
        int32_t expected = m_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (expected >= 0) {
            return;
        }
        wait_slow(expected);
    }

    bool try_wait()
    {
        int32_t current = m_count.load(std::memory_order_acquire);
        while (current > 0) {
            if (m_count.compare_exchange_weak(current,
                                              current - 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    template <class Clock, class Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        const auto deadline = detail::to_steady(abs_time);
        return timed_wait_deadline(deadline);
    }

private:
    void wait_slow(int32_t expected)
    {
        while (true) {
            detail::platform_wait(m_count, expected);
            int32_t current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return;
            }
            expected = current;
        }
    }

    bool timed_wait_deadline(const std::chrono::steady_clock::time_point& deadline)
    {
        int32_t expected = m_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (expected >= 0) {
            return true;
        }

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (finalize_timeout(expected)) {
                    return false;
                }
                return true;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            detail::wait_status result = detail::platform_wait_until(m_count, expected, remaining);
            int32_t             current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return true;
            }
            if (result == detail::wait_status::timed_out) {
                if (finalize_timeout(expected)) {
                    return false;
                }
                return true;
            }
            expected = current;
        }
    }

    bool finalize_timeout(int32_t expected)
    {
        while (true) {
            int32_t current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return false; // a post occurred concurrently; treat as acquired
            }
            if (m_count.compare_exchange_strong(current,
                                                current + 1,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    std::atomic<int32_t> m_count;
};

} // namespace ipc
} // namespace sintra

