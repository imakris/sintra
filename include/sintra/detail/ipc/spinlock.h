// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "../debug_pause.h"
#include "../process/process_id.h"
#include "platform_utils.h"

namespace sintra {

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
        const uint32_t self_pid = static_cast<uint32_t>(detail::get_current_process_id());
        auto next_liveness_check = std::chrono::steady_clock::now();
        auto live_owner_deadline = next_liveness_check + k_live_owner_timeout;
        size_t spin_count = 0;

        while (true) {
            if (!m_locked.test_and_set(std::memory_order_acquire)) {
                m_owner_pid.store(self_pid, std::memory_order_release);
                m_last_progress_ns.store(now_ns(), std::memory_order_relaxed);
                return;
            }

            if ((++spin_count & k_spin_yield_mask) == 0) {
                std::this_thread::yield();
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_liveness_check) {
                if (try_recover_dead_owner(self_pid)) {
                    continue;
                }
                next_liveness_check = now + k_owner_liveness_poll;
            }

            if (now >= live_owner_deadline) {
                if (try_recover_dead_owner(self_pid)) {
                    live_owner_deadline = std::chrono::steady_clock::now() + k_live_owner_timeout;
                    continue;
                }

                const auto owner = m_owner_pid.load(std::memory_order_acquire);
                if (owner != 0 && owner != self_pid && is_process_alive(owner)) {
                    report_live_owner_stall(owner);
                }

                // Owner unknown or became self - forcefully release and continue trying.
                force_unlock();
                live_owner_deadline = std::chrono::steady_clock::now() + k_live_owner_timeout;
            }
        }
    }

    void unlock()
    {
        m_owner_pid.store(0, std::memory_order_release);
        m_last_progress_ns.store(now_ns(), std::memory_order_relaxed);
        m_locked.clear(std::memory_order_release);
    }

private:
    static constexpr size_t k_spin_yield_mask = 0x3FF; // yield every 1024 spins
    static constexpr auto k_owner_liveness_poll = std::chrono::milliseconds(5);
    static constexpr auto k_live_owner_timeout  = std::chrono::milliseconds(2000);

    static uint64_t now_ns()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool try_recover_dead_owner(uint32_t self_pid)
    {
        const auto owner = m_owner_pid.load(std::memory_order_acquire);
        if (owner == 0 || owner == self_pid) {
            return false;
        }

        if (is_process_alive(owner)) {
            return false;
        }

        log_recovery(owner);
        force_unlock();
        return true;
    }

    void log_recovery(uint32_t owner) const
    {
        const uint64_t last_ns = m_last_progress_ns.load(std::memory_order_relaxed);
        std::fprintf(
            stderr,
            "[sintra][spinlock] Owner PID %u disappeared while holding a shared spinlock "
            "(last progress %llu ns ago). Forcibly releasing lock.\n",
            owner,
            static_cast<unsigned long long>(
                now_ns() > last_ns ? (now_ns() - last_ns) : 0));
    }

    [[noreturn]] void report_live_owner_stall(uint32_t owner) const
    {
        const uint64_t acquired_ns = m_last_progress_ns.load(std::memory_order_relaxed);
        const uint64_t held_ns = now_ns() - acquired_ns;
        std::fprintf(
            stderr,
            "[sintra][spinlock] Shared spinlock stuck for %.3f ms while owner PID %u is still alive. "
            "Aborting to avoid corruption.\n",
            static_cast<double>(held_ns) / 1'000'000.0,
            owner);
        detail::debug_aware_abort();
    }

    void force_unlock()
    {
        m_owner_pid.store(0, std::memory_order_release);
        m_locked.clear(std::memory_order_release);
        m_last_progress_ns.store(now_ns(), std::memory_order_relaxed);
    }

    std::atomic_flag    m_locked = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> m_owner_pid{0};
    std::atomic<uint64_t> m_last_progress_ns{0};
};

} // namespace sintra

