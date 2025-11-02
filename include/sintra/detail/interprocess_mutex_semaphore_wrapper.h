#pragma once

#include <chrono>
#include "interprocess_semaphore.h"

namespace sintra { namespace detail {

// Backup mutex implementation using a binary semaphore.
// Heavier than the regular interprocess_mutex (has full semaphore overhead).
// Does not track ownership or support recursion detection.
// Use this only as a fallback if the regular interprocess_mutex has issues.
class interprocess_mutex_semaphore_wrapper
{
public:
    interprocess_mutex_semaphore_wrapper() noexcept
        : m_sem(1, nullptr, 1) // initial=1 (unlocked), name=nullptr, max=1 (prevents double-unlock)
    {
    }

    explicit interprocess_mutex_semaphore_wrapper(const wchar_t* name) noexcept
        : m_sem(1, name, 1)
    {
    }

    ~interprocess_mutex_semaphore_wrapper() = default;

    interprocess_mutex_semaphore_wrapper(const interprocess_mutex_semaphore_wrapper&) = delete;
    interprocess_mutex_semaphore_wrapper& operator=(const interprocess_mutex_semaphore_wrapper&) = delete;
    interprocess_mutex_semaphore_wrapper(interprocess_mutex_semaphore_wrapper&&) = delete;
    interprocess_mutex_semaphore_wrapper& operator=(interprocess_mutex_semaphore_wrapper&&) = delete;

    void lock() noexcept { m_sem.wait(); }
    bool try_lock() noexcept { return m_sem.try_wait(); }

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_timeout) noexcept {
        using ns = std::chrono::nanoseconds;
        return m_sem.try_wait_for(std::chrono::duration_cast<ns>(rel_timeout));
    }

    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) noexcept {
        return m_sem.timed_wait(abs_time);
    }

    void unlock() noexcept { m_sem.post(); }
    void release_local_handle() noexcept { m_sem.release_local_handle(); }

private:
    interprocess_semaphore m_sem;
};

}} // namespace sintra::detail
