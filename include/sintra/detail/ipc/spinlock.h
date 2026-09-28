// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_MSC_VER)
    #include "../sintra_windows.h"
    #include <intrin.h>
#endif

#include "../debug_pause.h"
#include "../logging.h"
#include "../time_utils.h"
#include "process_utils.h"

namespace sintra {

namespace detail {

inline void spin_pause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__arm__)
    __asm__ __volatile__("yield");
#else
    #if defined(_MSC_VER)
        #pragma message("Sintra: unsupported architecture; spin_pause is a no-op and performance may degrade.")
    #elif defined(__GNUC__) || defined(__clang__)
        #warning "Sintra: unsupported architecture; spin_pause is a no-op and performance may degrade."
    #endif
    // No-op fallback for other architectures
#endif
}

// Cooperative backoff for spin loops around short critical sections: pause
// the CPU each iteration and yield the thread after sustained spinning, so a
// preempted lock holder is not starved on oversubscribed machines.
struct Spin_backoff
{
    void spin() noexcept
    {
        spin_pause();
        if (++m_spin_count >= 1024) {
            std::this_thread::yield();
            m_spin_count = 0;
        }
    }

private:
    unsigned m_spin_count = 0;
};

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
// Reports each contended liveness poll with the time the waiter read for it
// and its decision on whether the hold it observes has outlasted the
// live-owner timeout by then. It runs on the waiting thread, before the stall
// handling that a timeout leads to.
using Spinlock_poll_callback = void (*)(
    const void*                            lock,
    std::chrono::steady_clock::time_point  poll_time,
    bool                                   timed_out);
inline std::atomic<Spinlock_poll_callback> s_spinlock_poll{nullptr};
}
#endif

inline void spinlock_poll_for_test(
    const void*                            lock,
    std::chrono::steady_clock::time_point  poll_time,
    bool                                   timed_out)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_spinlock_poll.load(std::memory_order_acquire)) {
        callback(lock, poll_time, timed_out);
    }
#else
    (void)lock;
    (void)poll_time;
    (void)timed_out;
#endif
}

} // namespace detail

struct spinlock
{
    struct locker
    {
        locker(spinlock& sl): m_sl(sl) { m_sl.lock();   }
        ~locker()                      { m_sl.unlock(); }
        locker(const locker&) = delete;
        locker& operator=(const locker&) = delete;
        locker(locker&&) = delete;
        locker& operator=(locker&&) = delete;
        spinlock& m_sl;
    };

    void lock()
    {
        const uint64_t self = detail::current_process_instance();
        auto next_liveness_check = std::chrono::steady_clock::now();
        hold_t observed_hold{};
        auto   hold_observed_since = next_liveness_check;
        bool   hold_observed       = false;
        size_t spin_count          = 0;

        while (true) {
            if (!m_locked.test_and_set(std::memory_order_acquire)) {
                m_owner.store(self, std::memory_order_release);
                m_last_progress_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
                return;
            }

            detail::spin_pause();
            if ((++spin_count & k_spin_yield_mask) == 0) {
                std::this_thread::yield();
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_liveness_check) {
                continue;
            }
            const bool waiter_was_stopped = now - next_liveness_check > k_live_owner_timeout;
            next_liveness_check = now + k_owner_liveness_poll;

            if (try_take_over_exited_owner(self)) {
                return;
            }

            // The live-owner timeout bounds one hold, not this wait. A waiter can
            // lose every race against a stream of short holds, or resume from a
            // system suspend, although no hold stalled. A hold is identified by its
            // owner and by the progress stamp that lock(), unlock() and takeover
            // write, so a new hold is recognized even when the owner repeats, as it
            // does for threads of one process. The stamp is not the hold's start
            // time: the flag, owner and stamp are written separately, so a snapshot
            // taken between those writes pairs a new hold with an older stamp. The
            // hold is timed from this waiter's first observation of it instead, and
            // only while this waiter runs: a poll overdue by more than the timeout
            // means it was stopped, as a suspend also stops the holder.
            const hold_t hold = current_hold();
            if (!hold_observed || waiter_was_stopped || hold != observed_hold) {
                observed_hold       = hold;
                hold_observed_since = now;
                hold_observed       = true;
            }
            const bool timed_out = now - hold_observed_since >= k_live_owner_timeout;
            detail::spinlock_poll_for_test(this, now, timed_out);
            if (!timed_out) {
                continue;
            }

            const auto owner     = hold.owner;
            const auto owner_pid = detail::process_instance_pid(owner);
            if (owner == self) {
                report_live_owner_stall(owner_pid);
            }
            if (owner != 0 &&
                owner_pid != detail::process_instance_pid(self) &&
                is_process_alive(owner_pid))
            {
                if (detail::is_debug_pause_active()) {
                    Log_stream(log_level::warning)
                        << "[sintra][spinlock] Owner PID " << owner_pid
                        << " is paused under debug control; "
                        << "taking over the spinlock to allow shutdown to proceed.\n";
                    if (take_over_owner(owner, self)) {
                        return;
                    }
                    hold_observed = false;
                    continue;
                }
                report_live_owner_stall(owner_pid);
            }

            // A dead owner is recovered only by takeover. A zero owner has no
            // identity to take over from: the holder is between the flag and owner
            // updates of lock() or unlock(), or died there.
            if (owner == 0) {
                force_unlock();
            }
            hold_observed = false;
        }
    }

    void unlock()
    {
        m_owner.store(0, std::memory_order_release);
        m_last_progress_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
        m_locked.clear(std::memory_order_release);
    }

private:
    static constexpr size_t    k_spin_yield_mask     = 0x3FF; // yield every 1024 spins
    static constexpr auto      k_owner_liveness_poll = std::chrono::milliseconds(5);
    static constexpr auto      k_live_owner_timeout  = std::chrono::milliseconds(2000);

    struct hold_t
    {
        uint64_t owner;
        uint64_t progress_ns;

        bool operator==(const hold_t&) const = default;
    };

    hold_t current_hold() const
    {
        const auto owner = m_owner.load(std::memory_order_acquire);
        return {owner, m_last_progress_ns.load(std::memory_order_relaxed)};
    }

    // Only a thread of this process instance can hold the lock under its
    // instance. An owner recorded under this process's PID with another token
    // was an earlier process with this PID, which has exited.
    bool try_take_over_exited_owner(uint64_t self)
    {
        const auto owner = m_owner.load(std::memory_order_acquire);
        if (owner == 0 || !detail::process_instance_has_exited(owner, self)) {
            return false;
        }

        log_recovery(detail::process_instance_pid(owner));
        return take_over_owner(owner, self);
    }

    // Recovery inherits the lock instead of releasing it. Every contender that
    // saw the same owner may act on that stale observation after another one
    // has already recovered the lock, so a release would clear the recovered
    // holder's ownership. The flag stays set, and the compare-exchange admits
    // exactly one contender per observed owner; the others keep waiting.
    bool take_over_owner(uint64_t observed_owner, uint64_t self)
    {
        if (!m_owner.compare_exchange_strong(
                observed_owner,
                self,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return false;
        }

        m_last_progress_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
        return true;
    }

    void log_recovery(uint32_t owner) const
    {
        const uint64_t last_ns = m_last_progress_ns.load(std::memory_order_relaxed);
        Log_stream(log_level::warning)
            << "[sintra][spinlock] Owner PID " << owner
            << " disappeared while holding a shared spinlock (last progress "
            << static_cast<unsigned long long>(
                monotonic_now_ns() > last_ns ? (monotonic_now_ns() - last_ns) : 0)
            << " ns ago). Attempting to take over the lock.\n";
    }

    [[noreturn]] void report_live_owner_stall(uint32_t owner) const
    {
        const uint64_t acquired_ns = m_last_progress_ns.load(std::memory_order_relaxed);
        const uint64_t held_ns = monotonic_now_ns() - acquired_ns;
        Log_stream(log_level::error)
            << "[sintra][spinlock] Shared spinlock stuck for "
            << (static_cast<double>(held_ns) / 1'000'000.0)
            << " ms while owner PID " << owner << " is still alive. "
            << "Aborting to avoid corruption.\n";
        detail::debug_aware_abort();
    }

    void force_unlock()
    {
        m_owner.store(0, std::memory_order_release);
        m_locked.clear(std::memory_order_release);
        m_last_progress_ns.store(monotonic_now_ns(), std::memory_order_relaxed);
    }

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "spinlock requires lock-free 64-bit atomics");

    std::atomic_flag           m_locked{};
    // The owner's process instance (detail::current_process_instance), or zero.
    std::atomic<uint64_t>      m_owner{0};
    std::atomic<uint64_t>      m_last_progress_ns{0};
};

} // namespace sintra
