#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

#include "ipc_platform_utils.h"

namespace sintra::detail
{

class interprocess_mutex
{
public:
    interprocess_mutex() noexcept = default;
    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    void lock()
    {
        const uint32_t self_pid = get_current_pid();
        const uint32_t self_tid = get_current_tid();
        if (try_acquire(self_pid, self_tid)) {
            return;
        }

        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self_pid, self_tid)) {
                return;
            }
        }
    }

    bool try_lock()
    {
        const uint32_t self_pid = get_current_pid();
        const uint32_t self_tid = get_current_tid();
        return try_acquire(self_pid, self_tid);
    }

    void unlock()
    {
        const uint64_t self_owner = make_owner_id(get_current_pid(), get_current_tid());
        uint64_t       expected = self_owner;
        if (!m_owner.compare_exchange_strong(expected,
                                              0,
                                              std::memory_order_release,
                                              std::memory_order_relaxed))
        {
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    "interprocess_mutex unlock by non-owner");
        }
    }

private:
    static void adaptive_wait(std::size_t iteration)
    {
        if (iteration < 16) {
            std::this_thread::yield();
            return;
        }

        iteration = std::min<std::size_t>(iteration - 16, 10);
        const auto sleep_us = std::chrono::microseconds(1u << iteration);
        std::this_thread::sleep_for(sleep_us);
    }

    static uint64_t make_owner_id(uint32_t pid, uint32_t tid) noexcept
    {
        return (static_cast<uint64_t>(pid) << 32) | static_cast<uint64_t>(tid);
    }

    bool try_acquire(uint32_t self_pid, uint32_t self_tid)
    {
        const uint64_t self_owner = make_owner_id(self_pid, self_tid);
        uint64_t       expected = 0;
        if (m_owner.compare_exchange_strong(expected,
                                             self_owner,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed))
        {
            return true;
        }

        if (expected == self_owner) {
            throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
                                    "interprocess_mutex recursive locking detected");
        }

        if (expected != 0 && try_recover(expected, self_pid)) {
            expected = 0;
            if (m_owner.compare_exchange_strong(expected,
                                                 self_owner,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed))
            {
                return true;
            }
        }

        return false;
    }

    static uint32_t owner_pid(uint64_t owner) noexcept
    {
        return static_cast<uint32_t>(owner >> 32);
    }

    bool try_recover(uint64_t observed_owner, uint32_t self_pid)
    {
        if (observed_owner == 0) {
            return false;
        }

        uint32_t expected = 0;
        if (!m_recovering.compare_exchange_strong(expected,
                                                   self_pid,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
        {
            return false;
        }

        bool recovered = false;
        uint64_t current_owner = m_owner.load(std::memory_order_acquire);
        if (current_owner == 0) {
            recovered = true;
        } else if (current_owner == observed_owner) {
            const uint32_t observed_pid = owner_pid(observed_owner);
            if (!is_process_alive(observed_pid)) {
                recovered = m_owner.compare_exchange_strong(current_owner,
                                                            0,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_relaxed);
            }
        }

        m_recovering.store(0, std::memory_order_release);
        return recovered;
    }

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "sintra::detail::interprocess_mutex requires lock-free 64-bit atomics");

    std::atomic<uint64_t> m_owner{0};
    std::atomic<uint32_t> m_recovering{0};
};

} // namespace sintra::detail

