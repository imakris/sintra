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
remains locked until the process terminates. A recovery/publication gate left
by a dead process may be reclaimed; live stalled gate holders are not
preempted.

MEMORY & ORDERING
Ownership transitions rely on 64-bit atomic compare-exchange operations using
acquire and release semantics, ensuring full visibility of writes before and
after lock transitions. Recovery clears the owner token using acq_rel ordering.
All atomic members are required to be lock-free.

USAGE CONTRACT
The mutex object must reside in shared memory accessible by all participant
processes. The platform utilities defined in `platform_defs.h` and
`process_utils.h` must provide:
  - get_current_pid()
  - get_current_tid()
  - is_process_alive(uint32_t)
  - query_process_start_stamp(uint32_t)
  - current_process_start_stamp()
No explicit initialization routine is required.

PORTABILITY
The implementation is portable to any platform that supports lock-free 64-bit
atomics and basic thread yielding. It uses adaptive spinning with exponential
backoff and occasional sleeps under contention. No operating system-specific
kernel synchronization primitives are required.

CAVEATS
- Thread death within a still-running process is not detected.
- Recovery/publication gates are reclaimed only when the gate-owner process is
  dead.
- Timed waits (try_lock_for, try_lock_until) may exceed their timeout by
  up to one backoff interval (~16 ms).
- Recursive lock attempts throw resource_deadlock_would_occur.
- If process start-stamp evidence is unavailable, recovery stays conservative
  and falls back to PID-liveness only.
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

#include "../ipc/platform_defs.h"
#include "../ipc/process_utils.h"

namespace sintra { namespace detail {

// The alignas(64) members intentionally request padding for cache-line isolation;
// silence the MSVC padding warning for this type.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable:4324)
#endif

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
        if (try_acquire(self, /*throw_on_recursive=*/false))    { return true;  }
        if (rel <= std::chrono::steady_clock::duration::zero()) { return false; }

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
        if (abs_time <= now) {
            return try_lock_for(std::chrono::steady_clock::duration::zero());
        }
        return try_lock_for(abs_time - now);
    }


    // Unlock; throws if called by a non-owner thread.
    void unlock()
    {
        const owner_token self = make_owner_token();
        owner_token expected = self;
        if (!m_owner.compare_exchange_strong(expected, k_unowned)) {
            // Either unlocked by someone else (after recovery) or not owned by us.
            throw std::system_error(std::make_error_code(std::errc::operation_not_permitted),
                "interprocess_mutex unlock by non-owner");
        }
    }

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    struct test_owner_fixture
    {
        std::uint32_t pid = 0;
        std::uint32_t tid = 0;
        std::uint64_t start_stamp = 0;
    };

    void test_install_owner_fixture(test_owner_fixture owner) noexcept
    {
        const auto token =
            (static_cast<std::uint64_t>(owner.pid) << 32u) |
            (static_cast<std::uint64_t>(owner.tid) & 0xFFFFFFFFull);
        m_recovering.store(0, std::memory_order_release);
        m_owner_start_stamp.store(owner.start_stamp, std::memory_order_release);
        m_owner.store(token, std::memory_order_release);
    }

    std::uint64_t test_owner_token() const noexcept
    {
        return m_owner.load(std::memory_order_acquire);
    }
#endif

private:
    // === Types & constants ===
    using owner_token = std::uint64_t; // upper 32 bits: pid, lower 32 bits: tid
    static constexpr owner_token   k_unowned = 0;

    // Recovery coordination token packs {recoverer_pid (hi32), ticks_ms (lo32)}
    using recover_token = std::uint64_t;

    // We require a lock-free 64-bit atomic for interprocess usage.
    static_assert(std::atomic<owner_token>::is_always_lock_free,
        "interprocess_mutex requires lock-free 64-bit atomics");
    static_assert(std::atomic<recover_token>::is_always_lock_free,
        "interprocess_mutex requires lock-free 64-bit atomics for recovery");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
        "interprocess_mutex requires lock-free 32-bit atomics for flags");

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

    bool acquire_recovery_gate(recover_token& owned_token)
    {
        recover_token rec = m_recovering.load(std::memory_order_acquire);
        if (rec != 0) {
            const auto rp = recover_pid(rec);
            if (rp != 0 && !is_process_alive(rp)) {
                m_recovering.compare_exchange_strong(rec, static_cast<recover_token>(0));
            }
        }

        owned_token = make_recover_token(get_current_pid(), now_ticks32());
        recover_token zero = 0;
        return m_recovering.compare_exchange_strong(zero, owned_token);
    }

    void release_recovery_gate(recover_token owned_token) noexcept
    {
        recover_token expected = owned_token;
        m_recovering.compare_exchange_strong(expected, static_cast<recover_token>(0));
    }

    static bool owner_generation_is_stale(owner_token token, std::uint64_t stored_stamp)
    {
        if (stored_stamp == 0) {
            return false;
        }

        const auto current_stamp = query_process_start_stamp(owner_pid(token));
        return current_stamp && *current_stamp != stored_stamp;
    }

    bool try_acquire_unowned_when_no_recovery(owner_token self)
    {
        recover_token gate = 0;
        if (!acquire_recovery_gate(gate)) {
            return false;
        }

        if (m_owner.load(std::memory_order_acquire) != k_unowned) {
            release_recovery_gate(gate);
            return false;
        }

        m_owner_start_stamp.store(0, std::memory_order_release);
        owner_token expected = k_unowned;
        if (!m_owner.compare_exchange_strong(expected, self)) {
            release_recovery_gate(gate);
            return false;
        }

        m_owner_start_stamp.store(
            current_process_start_stamp().value_or(0),
            std::memory_order_release);
        release_recovery_gate(gate);
        return true;
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

    // Internal helper lets timed/try APIs avoid throwing on recursion.
    bool try_acquire(owner_token self, bool throw_on_recursive)
    {
        if (try_acquire_unowned_when_no_recovery(self)) {
            return true;
        }

        owner_token expected = m_owner.load(std::memory_order_acquire);

        // Recovery path: previous owner is gone (process crashed/exited).
        if (expected != k_unowned && try_recover(expected, self)) {
            if (try_acquire_unowned_when_no_recovery(self)) {
                return true;
            }
        }

        // Recursive acquisition by the same current-generation thread. A same-token
        // stale generation is recoverable, so do not classify it as recursion.
        if (expected == self) {
            const owner_token current_owner = m_owner.load(std::memory_order_acquire);
            const recover_token current_recovering = m_recovering.load(std::memory_order_acquire);
            if (current_recovering != 0 || current_owner != self) {
                return false;
            }

            if (owner_generation_is_stale(
                    current_owner,
                    m_owner_start_stamp.load(std::memory_order_acquire)))
            {
                return false;
            }

            if (throw_on_recursive) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_deadlock_would_occur),
                    "interprocess_mutex: recursive lock detected");
            }
            return false;
        }

        return false;
    }

    // Attempt robust recovery if the observed owner appears to be dead.
    bool try_recover(owner_token observed_owner, owner_token self)
    {
        (void)self; // self is currently unused but kept for symmetry/diagnostics
        if (observed_owner == k_unowned) {
            return false;
        }

        recover_token gate = 0;
        if (!acquire_recovery_gate(gate)) {
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
        if (current_owner == observed_owner) {
            const auto stored_stamp = m_owner_start_stamp.load(std::memory_order_acquire);
            if (!is_process_alive(owner_pid(observed_owner)) ||
                owner_generation_is_stale(observed_owner, stored_stamp))
            {
                // Owner process is dead or belongs to a stale process generation.
                recovered = m_owner.compare_exchange_strong(current_owner, k_unowned);
                if (recovered &&
                    m_recovering.load(std::memory_order_acquire) == gate)
                {
                    auto stamp_to_clear = stored_stamp;
                    m_owner_start_stamp.compare_exchange_strong(
                        stamp_to_clear,
                        static_cast<std::uint64_t>(0),
                        std::memory_order_release,
                        std::memory_order_acquire);
                }
            }
        }

        release_recovery_gate(gate);
        return recovered;
    }

private:
    // Owner token in shared memory: who currently owns the mutex.
    alignas(64) std::atomic<owner_token> m_owner{ k_unowned };

    // Start stamp for the current owner process. Zero means unknown.
    std::atomic<std::uint64_t> m_owner_start_stamp{ 0 };

    // Recovery/publication gate. Packs {recoverer_pid, ticks_ms}. Used to
    // serialize robust recovery and owner-stamp publication. Dead gate-owner
    // PIDs are recoverable; live stalled gates are not preempted.
    alignas(64) std::atomic<recover_token> m_recovering{ 0 };

};

#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

}} // namespace sintra::detail
