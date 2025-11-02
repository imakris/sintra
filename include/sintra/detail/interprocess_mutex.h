#pragma once


// NOTE: This should compile on C++17


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
    interprocess_mutex() = default;
    ~interprocess_mutex() = default;

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
        return try_acquire(self, /*throw_on_recursive=*/false);
    }

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_timeout) noexcept {
        const owner_token self = make_owner_token();
        if (try_acquire(self, /*throw_on_recursive=*/false)) {
            return true;
        }

        const auto deadline = std::chrono::steady_clock::now() + rel_timeout;
        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self, /*throw_on_recursive=*/false)) {
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
        if (try_acquire(self, /*throw_on_recursive=*/false)) {
            return true;
        }

        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self, /*throw_on_recursive=*/false)) {
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
            k_unowned, std::memory_order_release, std::memory_order_relaxed))
        {
            // Someone else (or no one) owns it -> error.
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    "interprocess_mutex unlock by non-owner");
        }
    }

    void release_local_handle() noexcept {
        // No-op for mutex; kept for API symmetry with other IPC handles.
    }

    bool recovered_last_acquire() const noexcept {
        return s_last_lock_recovered;
    }


private:
    inline static thread_local bool s_last_lock_recovered = false;

    static void adaptive_wait(std::size_t iteration) {
        // Short phase: yield a few times to let other threads run.
        if (iteration < 16) {
            std::this_thread::yield();
            return;
        }
        // Then exponential backoff in microseconds, capped.
        iteration = std::min<std::size_t>(iteration - 16, 14); // cap at ~16ms
        const auto sleep_us = std::chrono::microseconds(1u << iteration);
        std::this_thread::sleep_for(sleep_us);
    }

    using owner_token = std::uint64_t;
    static constexpr owner_token k_unowned = 0;

    // We require a lock-free 64-bit atomic for interprocess usage.
    static_assert(std::atomic<owner_token>::is_always_lock_free,
                  "interprocess_mutex requires lock-free 64-bit atomics");

    static owner_token make_owner_token() {
        const owner_token pid = static_cast<owner_token>(get_current_pid());
        const owner_token tid = static_cast<owner_token>(get_current_tid());
        return (pid << 32u) | (tid & 0xFFFFFFFFull);
    }

    static uint32_t owner_pid(owner_token token) {
        return static_cast<uint32_t>(token >> 32u);
    }

    // Original internal entry-point keeps throwing behavior for lock()
    // (i.e., recursive lock -> throw).
    bool try_acquire(owner_token self) {
        return try_acquire(self, /*throw_on_recursive=*/true);
    }

    // New internal helper lets timed/try APIs avoid throwing on recursion.
    bool try_acquire(owner_token self, bool throw_on_recursive) {
        s_last_lock_recovered = false;
        owner_token expected = k_unowned;
        if (m_owner.compare_exchange_strong(
            expected, self, std::memory_order_acquire, std::memory_order_relaxed))
        {
            return true;
        }

        // Recursive acquisition by the same thread
        if (expected == self) {
            if (throw_on_recursive) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_deadlock_would_occur),
                    "interprocess_mutex recursive locking detected");
            }
            else {
                return false;
            }
        }

        // Recovery path: previous owner is gone (process crashed/exited).
        if (expected != k_unowned && try_recover(expected, self)) {
            expected = k_unowned;
            if (m_owner.compare_exchange_strong(
                expected, self, std::memory_order_acquire, std::memory_order_relaxed))
            {
                s_last_lock_recovered = true;
                return true;
            }
        }

        return false;
    }

    bool try_recover(owner_token observed_owner, owner_token self) {
        if (observed_owner == k_unowned) {
            return false;
        }

        // If someone is recovering but that process is dead, clear it first.
        uint32_t rec = m_recovering.load(std::memory_order_acquire);
        if (rec != 0 && !is_process_alive(rec)) {
            m_recovering.compare_exchange_strong(
                rec, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // Try to become the single recovering process (store caller PID).
        uint32_t expected = 0;
        const uint32_t caller_pid = owner_pid(self);
        if (!m_recovering.compare_exchange_strong(
            expected, caller_pid, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return false; // Someone else is handling recovery.
        }

        bool        recovered     = false;
        owner_token current_owner = m_owner.load(std::memory_order_acquire);

        // If unlocked meanwhile, consider it recovered.
        if (current_owner == k_unowned) {
            recovered = true;
        }
        else
        if (current_owner == observed_owner && !is_process_alive(owner_pid(observed_owner))) {
            // Owner process is dead -> forcibly clear ownership.
            recovered = m_owner.compare_exchange_strong(
                current_owner, k_unowned, std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // Release the recovery lock.
        m_recovering.store(0, std::memory_order_release);
        return recovered;
    }

    alignas(64) std::atomic<owner_token> m_owner{k_unowned};
    alignas(64) std::atomic<uint32_t>    m_recovering{0};
};

}} // namespace sintra::detail
