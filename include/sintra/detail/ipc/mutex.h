// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once
/*
interprocess_mutex.h

PROCESS-ROBUST MUTEX WITH OWNER-DEATH RECOVERY
Provides a compact mutual-exclusion primitive suitable for interprocess use.
The mutex is implemented entirely with lock-free atomic operations and an
adaptive spinning strategy. It can recover automatically when the owning
process terminates unexpectedly.

ROBUSTNESS MODEL
This mutex is process-robust: it detects and recovers from owner-process death.
If the owning thread exits while its process continues to run, the mutex
remains locked until the process terminates. Recovery preemption is supported:
if a recovery attempt stalls beyond a configurable timeout window, another
thread may safely take over and complete the recovery.

MEMORY & ORDERING
Ownership transitions rely on 64-bit atomic compare-exchange operations using
acquire and release semantics, ensuring full visibility of writes before and
after lock transitions. Recovery clears the owner token using acq_rel ordering.
All atomic members are required to be lock-free.

USAGE CONTRACT
The mutex object must reside in shared memory accessible by all participant
processes. The platform utilities defined in `ipc_platform_utils.h` must
provide:
  - get_current_pid()
  - get_current_tid()
  - is_process_alive(uint32_t)
No explicit initialization routine is required.

COMPATIBILITY & PORTABILITY
The implementation is portable to any platform that supports lock-free 64-bit
atomics and basic thread yielding. It uses adaptive spinning with exponential
backoff and occasional sleeps under contention. No operating system–specific
kernel synchronization primitives are required.

RECOVERY FLAG
The method recovered_last_acquire() reports whether the most recent successful
acquisition followed an owner-death recovery. The flag is set to true only when
a lock is taken after recovery and reset to false on the next normal acquisition.
Failed try_* attempts do not affect the flag.

CAVEATS
- Thread death within a still-running process is not detected.
- Recovery preemption may result in short overlapping recovery attempts,
  but only one can succeed in resetting ownership.
- Timed waits (try_lock_for, try_lock_until) may exceed their timeout by
  up to one backoff interval (~16 ms).
- Recursive lock attempts throw resource_deadlock_would_occur.
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

#include "../ipc/platform_utils.h"  // expected to provide: get_current_pid(), get_current_tid(), is_process_alive(uint32_t)

namespace sintra { namespace detail {

class interprocess_mutex
{
public:
    interprocess_mutex() = default;
    ~interprocess_mutex() = default;

    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    // Blocks until the lock is acquired. Throws on recursive acquisition (same thread).
    void lock()
    {
        const owner_token self = make_owner_token();
        if (try_acquire(self, /*throw_on_recursive=*/true)) {
            return;
        }

        std::size_t iteration = 0;
        for (;;) {
            adaptive_wait(iteration++);
            if (try_acquire(self, /*throw_on_recursive=*/true)) {
                return;
            }
        }
    }

    // Non-blocking attempt. Returns false on contention or recursive attempt by the same thread.
    bool try_lock()
    {
        const owner_token self = make_owner_token();
        return try_acquire(self, /*throw_on_recursive=*/false);
    }


    // Tries to acquire within a steady_clock-relative duration.
    // Uses adaptive spinning, then sleeps with exponential backoff capped and
    // clamped to the remaining time budget.
    bool try_lock_for(std::chrono::steady_clock::duration rel)
    {
        const owner_token self = make_owner_token();

        // Fast attempt
        if (try_acquire(self, /*throw_on_recursive=*/false)) {
            return true;
        }
        if (rel <= std::chrono::steady_clock::duration::zero()) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + rel;

        // Backoff: brief yield phase, then sleep with exponential growth.
        std::uint32_t spins = 0;
        auto sleep_us = std::chrono::microseconds(1);
        const auto sleep_cap = std::chrono::microseconds(16000); // ~16 ms upper bound

        for (;;)
        {
            // Short spinning/yield phase helps light contention without oversleeping.
            if (spins < 16) {
                std::this_thread::yield();
                ++spins;
            }
            else {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return false;
                }

                // Clamp the sleep to the remaining budget to reduce overshoot.
                auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
                auto to_sleep = (sleep_us < remaining) ? sleep_us : remaining;
                std::this_thread::sleep_for(to_sleep);

                // Exponential backoff with cap; next sleep remains clamped each loop.
                if (sleep_us < sleep_cap) {
                    sleep_us *= 2;
                    if (sleep_us > sleep_cap) {
                        sleep_us = sleep_cap;
                    }
                }
            }

            if (try_acquire(self, /*throw_on_recursive=*/false)) {
                return true;
            }
        }
    }


    // Steady-clock absolute deadline overload (preferred).
    bool try_lock_until(const std::chrono::time_point<std::chrono::steady_clock>& abs_time) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        if (abs_time <= now) return try_lock_for(std::chrono::steady_clock::duration::zero());
        return try_lock_for(abs_time - now);
    }


    // Unlock; throws if called by a non-owner thread.
    void unlock()
    {
        const owner_token self = make_owner_token();
        owner_token expected = self;
        if (!m_owner.compare_exchange_strong(
            expected, k_unowned, std::memory_order_release, std::memory_order_relaxed))
        {
            // Either unlocked by someone else (after recovery) or not owned by us.
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                                    "interprocess_mutex unlock by non-owner");
        }
        // Clearing flag here is optional; leave as-is so that a subsequent query still
        // reflects whether the *last* successful acquire was via recovery.
    }

    // Indicates whether the last successful acquire of *this mutex instance* was via recovery
    bool recovered_last_acquire() const noexcept
    {
        return m_last_recovered.load(std::memory_order_relaxed) != 0u;
    }

private:
    // === Types & constants ===
    using owner_token = std::uint64_t; // upper 32 bits: pid, lower 32 bits: tid
    static constexpr owner_token k_unowned = 0;

    // Recovery coordination token packs {recoverer_pid (hi32), ticks_ms (lo32)}
    using recover_token = std::uint64_t;

    // We require a lock-free 64-bit atomic for interprocess usage.
    static_assert(std::atomic<owner_token>::is_always_lock_free,
                  "interprocess_mutex requires lock-free 64-bit atomics");
    static_assert(std::atomic<recover_token>::is_always_lock_free,
                  "interprocess_mutex requires lock-free 64-bit atomics for recovery");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "interprocess_mutex requires lock-free 32-bit atomics for flags");

    // If a recoverer stalls while still "alive", let others preempt after this many ms.
    static constexpr std::uint32_t k_recovery_stale_ms = 10000; // 10s; conservative

    static owner_token make_owner_token()
    {
        const owner_token pid = static_cast<owner_token>(get_current_pid());
        const owner_token tid = static_cast<owner_token>(get_current_tid());
        return (pid << 32u) | (tid & 0xFFFFFFFFull);
    }

    static std::uint32_t owner_pid(owner_token token)
    {
        return static_cast<std::uint32_t>(token >> 32u);
    }

    static recover_token make_recover_token(std::uint32_t pid, std::uint32_t ticks)
    {
        return (static_cast<recover_token>(pid) << 32u) | static_cast<recover_token>(ticks);
    }

    static std::uint32_t recover_pid(recover_token tok)
    {
        return static_cast<std::uint32_t>(tok >> 32u);
    }

    static std::uint32_t recover_ticks(recover_token tok)
    {
        return static_cast<std::uint32_t>(tok & 0xFFFFFFFFull);
    }

    static std::uint32_t now_ticks32() noexcept
    {
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        return static_cast<std::uint32_t>(ms);
    }

    static void adaptive_wait(std::size_t iteration)
    {
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

    // Attempts to acquire the mutex for 'self' with recursion detection.
    bool try_acquire(owner_token self)
    {
        return try_acquire(self, /*throw_on_recursive=*/true);
    }

    // Internal helper lets timed/try APIs avoid throwing on recursion.
    bool try_acquire(owner_token self, bool throw_on_recursive)
    {
        owner_token expected = k_unowned;
        if (m_owner.compare_exchange_strong(
            expected, self, std::memory_order_acquire, std::memory_order_relaxed))
        {
            // Successful normal acquisition -> clear recovery flag
            m_last_recovered.store(0u, std::memory_order_relaxed);
            return true;
        }

        // Recursive acquisition by the same thread
        if (expected == self) {
            if (throw_on_recursive) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_deadlock_would_occur),
                    "interprocess_mutex: recursive lock detected");
            }
            return false;
        }

        // Recovery path: previous owner is gone (process crashed/exited).
        if (expected != k_unowned && try_recover(expected, self)) {
            expected = k_unowned;
            if (m_owner.compare_exchange_strong(
                expected, self, std::memory_order_acquire, std::memory_order_relaxed))
            {
                // Successful post-recovery acquisition -> set recovery flag
                m_last_recovered.store(1, std::memory_order_relaxed);
                return true;
            }
        }

        return false;
    }

    // Attempt robust recovery if the observed owner appears to be dead.
    bool try_recover(owner_token observed_owner, owner_token self)
    {
        if (observed_owner == k_unowned) {
            return false;
        }

        // If someone is recovering but that process is dead or stalled, clear it first.
        recover_token rec = m_recovering.load(std::memory_order_acquire);
        if (rec != 0) {
            const auto rp = recover_pid(rec);
            const auto rt = recover_ticks(rec);
            const auto nowt = now_ticks32();
            
            // uint32_t subtraction is wrap-safe for tick comparisons
            const bool stalled = static_cast<std::uint32_t>(nowt - rt) > k_recovery_stale_ms;

            if ((rp != 0 && !is_process_alive(rp)) || stalled) {
                m_recovering.compare_exchange_strong(
                    rec, static_cast<recover_token>(0), std::memory_order_acq_rel, std::memory_order_relaxed);
            }
        }

        // Try to become the recoverer for a short critical sequence.
        const recover_token want = make_recover_token(get_current_pid(), now_ticks32());
        recover_token zero = 0;
        if (!m_recovering.compare_exchange_strong(
            zero, want, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            return false; // someone else is (still) recovering
        }

        // We are the recoverer now.
        bool recovered = false;
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
        m_recovering.store(static_cast<recover_token>(0), std::memory_order_release);
        return recovered;
    }

private:
    // Owner token in shared memory: who currently owns the mutex.
    alignas(64) std::atomic<owner_token> m_owner{ k_unowned };

    // Recovery gate. Packs {recoverer_pid, ticks_ms}. Used to serialize robust recovery and
    // to allow preemption if a recoverer stalls without dying.
    alignas(64) std::atomic<recover_token> m_recovering{ 0 };

    // Per-instance flag indicating if the last successful acquire recovered from a dead owner.
    alignas(64) std::atomic<std::uint32_t> m_last_recovered{ 0u };
};

}} // namespace sintra::detail