#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace sintra::detail
{

/**
 * Lightweight interprocess mutex built on top of an address-free atomic flag.
 *
 * The implementation is intentionally self-contained so it can operate inside
 * shared memory without relying on OS specific kernel primitives. This keeps
 * the object trivially relocatable and avoids per-process handle management,
 * which simplifies crash recovery logic in the ring buffer control block.
 */
class interprocess_mutex
{
public:
    interprocess_mutex() noexcept = default;
    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    ~interprocess_mutex() noexcept = default;

    void lock() noexcept
    {
        std::size_t attempt = 0;
        while (!try_lock()) {
            backoff(attempt++);
        }
    }

    bool try_lock() noexcept
    {
        uint32_t expected = unlocked;
        return m_state.compare_exchange_strong(
            expected, locked, std::memory_order_acquire, std::memory_order_relaxed);
    }

    template <class Clock, class Duration>
    bool timed_lock(const std::chrono::time_point<Clock, Duration>& abs_time) noexcept
    {
        if (try_lock()) {
            return true;
        }

        std::size_t attempt = 0;
        while (Clock::now() < abs_time) {
            backoff(attempt++);
            if (try_lock()) {
                return true;
            }
        }

        return false;
    }

    template <class TimePoint>
    bool try_lock_until(const TimePoint& abs_time) noexcept
    {
        return timed_lock(abs_time);
    }

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_time) noexcept
    {
        if (rel_time <= rel_time.zero()) {
            return try_lock();
        }

        return timed_lock(std::chrono::steady_clock::now() + rel_time);
    }

    void unlock() noexcept
    {
        m_state.store(unlocked, std::memory_order_release);
    }

    void reset() noexcept
    {
        m_state.store(unlocked, std::memory_order_release);
    }

private:
    static constexpr uint32_t unlocked = 0;
    static constexpr uint32_t locked   = 1;

    static void backoff(std::size_t iteration) noexcept
    {
        if (iteration < 32) {
            std::this_thread::yield();
        }
        else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    std::atomic<uint32_t> m_state{unlocked};
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "sintra::detail::interprocess_mutex requires lock-free 32-bit atomics");
// All supported platforms (Windows, Linux, macOS, FreeBSD on mainstream
// architectures) provide lock-free 32-bit atomics, which makes the mutex
// address-free and safe to place in shared memory mappings.

} // namespace sintra::detail

