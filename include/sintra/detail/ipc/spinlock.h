// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
    #include "../sintra_windows.h"
    #if defined(_MSC_VER)
        #include <intrin.h>
    #endif
#else
    #include <time.h>
#endif

#include "../debug_pause.h"
#include "../logging.h"
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
    // Unsupported architectures still have a correct, if slower, spin loop.
#endif
}

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

struct spinlock_cpu_sample
{
    uint64_t ns = 0;
};

#if defined(SINTRA_ENABLE_TEST_HOOKS)
namespace test_hooks {
enum class spinlock_event {
    before_acquire_cas, after_acquire_cas, before_odd_mark, after_odd_mark,
    before_even_mark, after_even_mark, before_zero_store, after_zero_store,
    before_start_generation, after_start_generation, before_start_owner,
    after_start_owner, before_start_cpu, after_start_cpu,
    before_end_cpu, after_end_cpu, before_end_owner, after_end_owner,
    before_end_generation, after_end_generation, before_takeover_bump,
    after_takeover_bump, before_takeover_cas, after_takeover_cas,
    before_takeover_mark, after_takeover_mark, before_liveness,
    after_liveness, before_final_owner, after_final_owner,
    before_final_generation, after_final_generation, poll
};
using spinlock_event_callback = void (*)(const void*, spinlock_event);
using spinlock_cpu_callback = bool (*)(spinlock_cpu_sample&);
inline std::atomic<spinlock_event_callback> s_spinlock_event{nullptr};
inline std::atomic<spinlock_cpu_callback> s_spinlock_cpu{nullptr};
}

inline void spinlock_event_for_test(const void* lock, test_hooks::spinlock_event event)
{
    if (auto callback = test_hooks::s_spinlock_event.load(std::memory_order_acquire)) {
        callback(lock, event);
    }
}
#define SINTRA_SPINLOCK_HOOK(event) \
    detail::spinlock_event_for_test(this, detail::test_hooks::spinlock_event::event)
#else
#define SINTRA_SPINLOCK_HOOK(event) ((void)0)
#endif

inline bool read_spinlock_thread_cpu(spinlock_cpu_sample& sample) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_spinlock_cpu.load(std::memory_order_acquire)) {
        return callback(sample);
    }
#endif
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return false;
    }
    const auto units = [](const FILETIME& time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const uint64_t kernel_ticks = units(kernel);
    const uint64_t user_ticks = units(user);
    if (kernel_ticks > UINT64_MAX - user_ticks) {
        return false;
    }
    const uint64_t ticks = kernel_ticks + user_ticks;
    if (ticks > UINT64_MAX / 100) {
        return false;
    }
    sample = {ticks * 100};
    return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0 ||
        time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1'000'000'000 ||
        uint64_t(time.tv_sec) >
            (UINT64_MAX - uint64_t(time.tv_nsec)) / 1'000'000'000)
    {
        return false;
    }
    sample = {uint64_t(time.tv_sec) * 1'000'000'000 + uint64_t(time.tv_nsec)};
    return true;
#else
    (void)sample;
    return false;
#endif
}

struct alignas(16) spinlock_words
{
    std::atomic<uint64_t> owner{0};
    std::atomic<uint64_t> generation{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free,
    "spinlock requires lock-free 64-bit atomics");
static_assert(offsetof(spinlock_words, owner) == 0);
static_assert(offsetof(spinlock_words, generation) == 8);
static_assert(sizeof(spinlock_words) == 16 && alignof(spinlock_words) == 16);

} // namespace detail

struct alignas(16) spinlock
{
    struct locker
    {
        locker(spinlock& sl): m_sl(sl) { m_sl.lock(); }
        ~locker() { m_sl.unlock(); }
        locker(const locker&) = delete;
        locker& operator=(const locker&) = delete;
        locker(locker&&) = delete;
        locker& operator=(locker&&) = delete;
        spinlock& m_sl;
    };

    void lock()
    {
        const uint64_t self = detail::current_process_instance();
        auto next_poll = std::chrono::steady_clock::now();
        witness_t witness{};
        size_t spin_count = 0;

        while (true) {
            uint64_t unowned = 0;
            if (m_words.owner.load(std::memory_order_relaxed) == 0) {
                SINTRA_SPINLOCK_HOOK(before_acquire_cas);
                const bool acquired = m_words.owner.compare_exchange_strong(
                    unowned, self, std::memory_order_acquire, std::memory_order_relaxed);
                SINTRA_SPINLOCK_HOOK(after_acquire_cas);
                if (acquired) {
                    SINTRA_SPINLOCK_HOOK(before_odd_mark);
                    m_words.generation.fetch_add(1, std::memory_order_acq_rel);
                    SINTRA_SPINLOCK_HOOK(after_odd_mark);
                    return;
                }
            }

            detail::spin_pause();
            if ((++spin_count & k_spin_yield_mask) == 0) {
                std::this_thread::yield();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < next_poll) {
                continue;
            }
            next_poll = now + k_owner_liveness_poll;
            SINTRA_SPINLOCK_HOOK(poll);

            if (try_take_over_exited_owner(self)) {
                return;
            }
            if (!witness.active) {
                start_witness(witness);
                continue;
            }
            uint64_t elapsed_cpu_ns = 0;
            if (!advance_witness(witness, elapsed_cpu_ns)) {
                witness.active = false;
                continue;
            }
            if (elapsed_cpu_ns < k_live_owner_timeout_ns) {
                continue;
            }

            SINTRA_SPINLOCK_HOOK(before_liveness);
            const uint32_t owner_pid = detail::process_instance_pid(witness.owner);
#ifdef _WIN32
            const bool live = witness.owner == self ||
                (owner_pid != detail::process_instance_pid(self) &&
                    detail::probe_process_liveness(owner_pid) == detail::Process_liveness::LIVE);
#else
            const bool live = witness.owner == self ||
                (owner_pid != detail::process_instance_pid(self) && is_process_alive(owner_pid));
#endif
            SINTRA_SPINLOCK_HOOK(after_liveness);
            if (!live || detail::is_debug_pause_active()) {
                witness.active = false;
                continue;
            }
            SINTRA_SPINLOCK_HOOK(before_final_owner);
            const uint64_t final_owner =
                m_words.owner.fetch_add(0, std::memory_order_acq_rel);
            SINTRA_SPINLOCK_HOOK(after_final_owner);
            SINTRA_SPINLOCK_HOOK(before_final_generation);
            const uint64_t final_generation =
                m_words.generation.load(std::memory_order_acquire);
            SINTRA_SPINLOCK_HOOK(after_final_generation);
            if (final_owner == witness.owner && final_generation == witness.generation) {
                report_live_owner_stall(owner_pid, elapsed_cpu_ns);
            }
            witness.active = false;
        }
    }

    void unlock()
    {
        SINTRA_SPINLOCK_HOOK(before_even_mark);
        m_words.generation.fetch_add(1, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_even_mark);
        SINTRA_SPINLOCK_HOOK(before_zero_store);
        m_words.owner.store(0, std::memory_order_release);
        SINTRA_SPINLOCK_HOOK(after_zero_store);
    }

#if defined(SINTRA_ENABLE_TEST_HOOKS)
    void test_install_owner(uint64_t owner, uint64_t generation)
    {
        m_words.generation.store(generation, std::memory_order_relaxed);
        m_words.owner.store(owner, std::memory_order_release);
    }
    uint64_t test_owner() const { return m_words.owner.load(std::memory_order_acquire); }
    uint64_t test_generation() const { return m_words.generation.load(std::memory_order_acquire); }
#endif

private:
    static constexpr size_t k_spin_yield_mask = 0x3ff;
    static constexpr auto k_owner_liveness_poll = std::chrono::milliseconds(5);
    static constexpr uint64_t k_live_owner_timeout_ns = 2'000'000'000;

    struct witness_t
    {
        uint64_t generation = 0;
        uint64_t owner = 0;
        uint64_t start_cpu_ns = 0;
        uint64_t last_cpu_ns = 0;
        bool active = false;
    };

    void start_witness(witness_t& witness)
    {
        SINTRA_SPINLOCK_HOOK(before_start_generation);
        const uint64_t generation = m_words.generation.load(std::memory_order_acquire);
        SINTRA_SPINLOCK_HOOK(after_start_generation);
        SINTRA_SPINLOCK_HOOK(before_start_owner);
        const uint64_t owner = m_words.owner.fetch_add(0, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_start_owner);
        SINTRA_SPINLOCK_HOOK(before_start_cpu);
        detail::spinlock_cpu_sample cpu{};
        const bool valid = detail::read_spinlock_thread_cpu(cpu);
        SINTRA_SPINLOCK_HOOK(after_start_cpu);
        if ((generation & 1) && owner != 0 && valid) {
            witness = {generation, owner, cpu.ns, cpu.ns, true};
        }
    }

    bool advance_witness(witness_t& witness, uint64_t& elapsed_cpu_ns)
    {
        SINTRA_SPINLOCK_HOOK(before_end_cpu);
        detail::spinlock_cpu_sample cpu{};
        const bool valid = detail::read_spinlock_thread_cpu(cpu);
        SINTRA_SPINLOCK_HOOK(after_end_cpu);
        SINTRA_SPINLOCK_HOOK(before_end_owner);
        const uint64_t owner = m_words.owner.fetch_add(0, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_end_owner);
        SINTRA_SPINLOCK_HOOK(before_end_generation);
        const uint64_t generation = m_words.generation.load(std::memory_order_acquire);
        SINTRA_SPINLOCK_HOOK(after_end_generation);
        if (!valid || cpu.ns < witness.last_cpu_ns || owner != witness.owner ||
            generation != witness.generation)
        {
            return false;
        }
        witness.last_cpu_ns = cpu.ns;
        elapsed_cpu_ns = cpu.ns - witness.start_cpu_ns;
        return true;
    }

    bool try_take_over_exited_owner(uint64_t self)
    {
        const uint64_t owner = m_words.owner.load(std::memory_order_acquire);
        if (owner == 0 || !detail::process_instance_has_exited(owner, self)) {
            return false;
        }
        log_recovery(detail::process_instance_pid(owner));
        SINTRA_SPINLOCK_HOOK(before_takeover_bump);
        m_words.generation.fetch_add(2, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_takeover_bump);
        uint64_t expected = owner;
        SINTRA_SPINLOCK_HOOK(before_takeover_cas);
        const bool acquired = m_words.owner.compare_exchange_strong(
            expected, self, std::memory_order_acq_rel, std::memory_order_acquire);
        SINTRA_SPINLOCK_HOOK(after_takeover_cas);
        if (!acquired) {
            return false;
        }
        SINTRA_SPINLOCK_HOOK(before_takeover_mark);
        uint64_t actual = m_words.generation.load(std::memory_order_acquire);
        while (!m_words.generation.compare_exchange_weak(
            actual, (actual & 1) ? actual + 2 : actual + 1,
            std::memory_order_acq_rel, std::memory_order_acquire)) {}
        SINTRA_SPINLOCK_HOOK(after_takeover_mark);
        return true;
    }

    void log_recovery(uint32_t owner) const
    {
        Log_stream(log_level::warning)
            << "[sintra][spinlock] Owner PID " << owner
            << " disappeared while holding a shared spinlock. Attempting to take over the lock.\n";
    }

    [[noreturn]] void report_live_owner_stall(uint32_t owner, uint64_t elapsed_cpu_ns) const
    {
        Log_stream(log_level::error)
            << "[sintra][spinlock] Shared spinlock stuck after approximately "
            << (double(elapsed_cpu_ns) / 1'000'000.0)
            << " ms of OS-reported waiter thread CPU while owner PID " << owner
            << " is still alive. Aborting to avoid corruption.\n";
        detail::debug_aware_abort();
    }

    detail::spinlock_words m_words;
};

static_assert(sizeof(spinlock) == 16 && alignof(spinlock) == 16);

} // namespace sintra

#undef SINTRA_SPINLOCK_HOOK
