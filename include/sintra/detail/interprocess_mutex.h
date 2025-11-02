#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

#include "ipc_platform_utils.h"

namespace sintra { namespace detail {

class interprocess_mutex
{
public:
    interprocess_mutex() noexcept = default;
    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    void lock() {
        const owner_token self = make_owner_token();
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

    bool try_lock() {
        const owner_token self = make_owner_token();
        return try_acquire(self);
    }

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_timeout) noexcept {
        const owner_token self = make_owner_token();
        if (try_acquire(self)) {
            return true;
        }

        auto deadline = std::chrono::steady_clock::now() + rel_timeout;
        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self)) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }
    }

    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) noexcept {
        const owner_token self = make_owner_token();
        if (try_acquire(self)) {
            return true;
        }

        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self)) {
                return true;
            }
            if (Clock::now() >= abs_time) {
                return false;
            }
        }
    }

    void unlock() {
        const owner_token self = make_owner_token();
        owner_token       expected = self;
        if (!m_owner.compare_exchange_strong(expected,
                                              k_unowned,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    "interprocess_mutex unlock by non-owner");
        }
    }

    void release_local_handle() noexcept {
        // No-op for mutex
    }

private:
    static void adaptive_wait(std::size_t iteration) {
        if (iteration < 16) {
            std::this_thread::yield();
            return;
        }

        iteration = std::min<std::size_t>(iteration - 16, 10);
        const auto sleep_us = std::chrono::microseconds(1u << iteration);
        std::this_thread::sleep_for(sleep_us);
    }

    using owner_token = std::uint64_t;
    static constexpr owner_token k_unowned = 0;

    static owner_token make_owner_token() {
        const owner_token pid = static_cast<owner_token>(get_current_pid());
        const owner_token tid = static_cast<owner_token>(get_current_tid());
        return (pid << 32u) | (tid & 0xFFFFFFFFull);
    }

    static uint32_t owner_pid(owner_token token) {
        return static_cast<uint32_t>(token >> 32u);
    }

    bool try_acquire(owner_token self) {
        owner_token expected = k_unowned;
        if (m_owner.compare_exchange_strong(expected,
                                             self,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
            return true;
        }

        if (expected == self) {
            throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur),
                                    "interprocess_mutex recursive locking detected");
        }

        if (expected != k_unowned && try_recover(expected, self)) {
            expected = k_unowned;
            if (m_owner.compare_exchange_strong(expected,
                                                 self,
                                                 std::memory_order_acquire,
                                                 std::memory_order_relaxed)) {
                return true;
            }
        }

        return false;
    }

    bool try_recover(owner_token observed_owner, owner_token self) {
        if (observed_owner == k_unowned) {
            return false;
        }

        uint32_t expected = 0;
        const uint32_t caller_pid = owner_pid(self);
        if (!m_recovering.compare_exchange_strong(expected,
                                                   caller_pid,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            return false;
        }

        bool recovered = false;
        owner_token current_owner = m_owner.load(std::memory_order_acquire);
        if (current_owner == k_unowned) {
            recovered = true;
        } else if (current_owner == observed_owner &&
                   !is_process_alive(owner_pid(observed_owner))) {
            recovered = m_owner.compare_exchange_strong(current_owner,
                                                        k_unowned,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_relaxed);
        }

        m_recovering.store(0, std::memory_order_release);
        return recovered;
    }

    std::atomic<owner_token> m_owner{k_unowned};
    std::atomic<uint32_t> m_recovering{0};
};

}} // namespace sintra::detail
