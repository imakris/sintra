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
        const uint32_t self = get_current_pid();
        if (try_acquire(self)) {
            return;
        }

        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self)) {
                return;
            }
        }
    }

    bool try_lock()
    {
        const uint32_t self = get_current_pid();
        return try_acquire(self);
    }

    void unlock()
    {
        const uint32_t self = get_current_pid();
        uint32_t       expected = self;
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

    bool try_acquire(uint32_t self)
    {
        uint32_t expected = 0;
        if (m_owner.compare_exchange_strong(expected,
                                             self,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed))
        {
            return true;
        }

        if (expected == self) {
            throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
                                    "interprocess_mutex recursive locking detected");
        }

        if (expected != 0 && try_recover(expected, self)) {
            expected = 0;
            if (m_owner.compare_exchange_strong(expected,
                                                 self,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed))
            {
                return true;
            }
        }

        return false;
    }

    bool try_recover(uint32_t observed_owner, uint32_t self)
    {
        if (observed_owner == 0) {
            return false;
        }

        uint32_t expected = 0;
        if (!m_recovering.compare_exchange_strong(expected,
                                                   self,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
        {
            return false;
        }

        bool recovered = false;
        uint32_t current_owner = m_owner.load(std::memory_order_acquire);
        if (current_owner == 0) {
            recovered = true;
        } else if (current_owner == observed_owner && !is_process_alive(observed_owner)) {
            recovered = m_owner.compare_exchange_strong(current_owner,
                                                        0,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_relaxed);
        }

        m_recovering.store(0, std::memory_order_release);
        return recovered;
    }

    std::atomic<uint32_t> m_owner{0};
    std::atomic<uint32_t> m_recovering{0};
};

} // namespace sintra::detail

