// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "observation.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#if __has_include(<source_location>)
    #include <source_location>
#endif
#if defined(__cpp_lib_source_location) && __cpp_lib_source_location >= 201907L
    #define SINTRA_SPINLOCK_SOURCE_LOCATION 1
#else
    #define SINTRA_SPINLOCK_SOURCE_LOCATION 0
#endif

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

// Where a waiter acquires a lock. Diagnostics name it; the lock word does not.
struct spinlock_site
{
    const char* file = nullptr;
    uint32_t    line = 0;
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
// Supplies the complete waiter-effort sample; backoff credit is not added.
using spinlock_cpu_callback = bool (*)(spinlock_cpu_sample&);
// Replaces one backoff sleep and returns its measured duration in ns.
using spinlock_backoff_callback = uint64_t (*)(const void*);
inline std::atomic<spinlock_event_callback> s_spinlock_event{nullptr};
inline std::atomic<spinlock_cpu_callback> s_spinlock_cpu{nullptr};
inline std::atomic<spinlock_backoff_callback> s_spinlock_backoff{nullptr};
}

inline void spinlock_event_for_test(const void* lock, test_hooks::spinlock_event event) noexcept
{
    if (auto callback = test_hooks::s_spinlock_event.load(std::memory_order_acquire)) {
        observe_without_canceling("spinlock_event", [&] { callback(lock, event); });
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
        try {
            return callback(sample);
        }
        catch (...) {
            defer_observation_failure("spinlock_cpu");
            return false;
        }
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

// A waiter's effort: its OS-reported thread CPU plus the credit for its
// backoff sleeps. A test CPU provider supplies the whole sample.
inline bool read_spinlock_waiter_effort(spinlock_cpu_sample& sample, uint64_t backoff_ns) noexcept
{
    if (!read_spinlock_thread_cpu(sample)) {
        return false;
    }
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (test_hooks::s_spinlock_cpu.load(std::memory_order_acquire)) {
        return true;
    }
#endif
    if (sample.ns > UINT64_MAX - backoff_ns) {
        return false;
    }
    sample.ns += backoff_ns;
    return true;
}

// Sleeps once and returns the measured duration in ns. Waiters that outnumber
// the processors would otherwise keep a preempted holder from running.
inline uint64_t spinlock_backoff_sleep(const void* lock) noexcept
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback = test_hooks::s_spinlock_backoff.load(std::memory_order_acquire)) {
        try {
            return callback(lock);
        }
        catch (...) {
            defer_observation_failure("spinlock_backoff");
            return 0;
        }
    }
#endif
    (void)lock;
    const auto before = std::chrono::steady_clock::now();
#if defined(_WIN32)
    ::Sleep(1);
#else
    timespec request{0, 100'000};
    (void)::nanosleep(&request, nullptr);
#endif
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before).count();
    return elapsed > 0 ? uint64_t(elapsed) : 0;
}

// Builds one spinlock diagnostic line and writes it with a single native
// write, without allocating or invoking an application log callback.
class Spinlock_diagnostic
{
public:
    Spinlock_diagnostic& text(const char* part) noexcept
    {
        while (part && *part && m_length + 1 < sizeof(m_line)) {
            m_line[m_length++] = *part++;
        }
        return *this;
    }

    Spinlock_diagnostic& number(uint64_t value) noexcept
    {
        char digits[20];
        size_t count = 0;
        do {
            digits[count++] = char('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0 && m_length + 1 < sizeof(m_line)) {
            m_line[m_length++] = digits[--count];
        }
        return *this;
    }

    // Names the waiter's acquisition site by file name and line, if known.
    void emit(const spinlock_site& site) noexcept
    {
        if (site.file) {
            const char* file = site.file;
            for (const char* p = site.file; *p; ++p) {
                if (*p == '/' || *p == '\\') {
                    file = p + 1;
                }
            }
            text(" Waiter at ").text(file).text(":").number(site.line).text(".");
        }
        text("\n");
        m_line[m_length] = '\0';
        native_diagnostic(m_line, 0, "", false);
    }

private:
    char   m_line[512];
    size_t m_length = 0;
};

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
#if SINTRA_SPINLOCK_SOURCE_LOCATION
        locker(spinlock& sl, const std::source_location& where = std::source_location::current())
        :
            m_sl(sl), m_pid(uint32_t(get_current_pid()))
        { m_sl.lock(detail::spinlock_site{where.file_name(), uint32_t(where.line())}); }
#else
        locker(spinlock& sl)
        :
            m_sl(sl), m_pid(uint32_t(get_current_pid()))
        { m_sl.lock(); }
#endif
        ~locker() noexcept
        {
            if (m_pid == uint32_t(get_current_pid())) {
                m_sl.unlock();
            }
        }
        locker(const locker&) = delete;
        locker& operator=(const locker&) = delete;
        locker(locker&&) = delete;
        locker& operator=(locker&&) = delete;
        spinlock& m_sl;
        uint32_t m_pid;
    };

    void lock() { lock(detail::spinlock_site{}); }

    // Spins briefly, then sleeps between attempts. The live-stall witness
    // counts the waiter's effort: thread CPU plus each sleep, capped.
    void lock(const detail::spinlock_site& site)
    {
        const uint64_t self = detail::current_process_instance();
        const auto wait_start = std::chrono::steady_clock::now();
        auto next_poll = wait_start;
        witness_t witness{};
        bool reported_unknown = false;
        size_t spin_count = 0;
        uint64_t backoff_ns = 0;

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

            auto now = std::chrono::steady_clock::now();
            if (now - wait_start < k_spin_phase) {
                detail::spin_pause();
                if ((++spin_count & k_spin_yield_mask) == 0) {
                    std::this_thread::yield();
                }
            }
            else {
                const uint64_t slept = detail::spinlock_backoff_sleep(this);
                const uint64_t credit = slept < k_backoff_credit_cap_ns
                    ? slept : k_backoff_credit_cap_ns;
                backoff_ns = backoff_ns < UINT64_MAX - credit
                    ? backoff_ns + credit : UINT64_MAX;
                now = std::chrono::steady_clock::now();
            }
            if (now < next_poll) {
                continue;
            }
            next_poll = now + k_owner_liveness_poll;
            SINTRA_SPINLOCK_HOOK(poll);

            if (try_take_over_exited_owner(self, site)) {
                return;
            }
            if (!witness.active) {
                start_witness(witness, backoff_ns);
                continue;
            }
            uint64_t elapsed_effort_ns = 0;
            if (!advance_witness(witness, backoff_ns, elapsed_effort_ns)) {
                witness.active = false;
                continue;
            }
            if (elapsed_effort_ns < k_live_owner_timeout_ns) {
                continue;
            }

            SINTRA_SPINLOCK_HOOK(before_liveness);
            const uint32_t owner_pid = detail::process_instance_pid(witness.owner);
            // A foreign PID can have been reused. Without a published start
            // stamp, only our own exact instance confirms a live owner.
            const bool live = witness.owner == self;
            SINTRA_SPINLOCK_HOOK(after_liveness);
            if (detail::is_debug_pause_active()) {
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
                if (live) {
                    report_live_owner_stall(owner_pid, elapsed_effort_ns, site);
                }
                if (!reported_unknown) {
                    reported_unknown = true;
                    // Do not invoke an application log callback from this wait.
                    detail::Spinlock_diagnostic()
                        .text("[sintra][spinlock] Owner PID ").number(owner_pid)
                        .text(" incarnation cannot be confirmed after sustained waiter effort; "
                              "continuing to wait without taking over the shared spinlock.")
                        .emit(site);
                }
            }
            witness.active = false;
        }
    }

    void unlock() noexcept
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
    // A wait longer than this sleeps between attempts instead of spinning.
    static constexpr auto k_spin_phase = std::chrono::microseconds(200);
    // One sleep credits at most this much effort, so a waiter suspended in
    // its sleep cannot reach the live-stall timeout from that sleep alone.
    // It exceeds the default 15.6 ms Windows timer period of Sleep(1).
    static constexpr uint64_t k_backoff_credit_cap_ns = 20'000'000;
    static constexpr auto k_owner_liveness_poll = std::chrono::milliseconds(5);
    static constexpr uint64_t k_live_owner_timeout_ns = 2'000'000'000;

    struct witness_t
    {
        uint64_t generation = 0;
        uint64_t owner = 0;
        uint64_t start_effort_ns = 0;
        uint64_t last_effort_ns = 0;
        bool active = false;
    };

    void start_witness(witness_t& witness, uint64_t backoff_ns)
    {
        SINTRA_SPINLOCK_HOOK(before_start_generation);
        const uint64_t generation = m_words.generation.load(std::memory_order_acquire);
        SINTRA_SPINLOCK_HOOK(after_start_generation);
        SINTRA_SPINLOCK_HOOK(before_start_owner);
        const uint64_t owner = m_words.owner.fetch_add(0, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_start_owner);
        SINTRA_SPINLOCK_HOOK(before_start_cpu);
        detail::spinlock_cpu_sample effort{};
        const bool valid = detail::read_spinlock_waiter_effort(effort, backoff_ns);
        SINTRA_SPINLOCK_HOOK(after_start_cpu);
        if ((generation & 1) && owner != 0 && valid) {
            witness = {generation, owner, effort.ns, effort.ns, true};
        }
    }

    bool advance_witness(witness_t& witness, uint64_t backoff_ns, uint64_t& elapsed_effort_ns)
    {
        SINTRA_SPINLOCK_HOOK(before_end_cpu);
        detail::spinlock_cpu_sample effort{};
        const bool valid = detail::read_spinlock_waiter_effort(effort, backoff_ns);
        SINTRA_SPINLOCK_HOOK(after_end_cpu);
        SINTRA_SPINLOCK_HOOK(before_end_owner);
        const uint64_t owner = m_words.owner.fetch_add(0, std::memory_order_acq_rel);
        SINTRA_SPINLOCK_HOOK(after_end_owner);
        SINTRA_SPINLOCK_HOOK(before_end_generation);
        const uint64_t generation = m_words.generation.load(std::memory_order_acquire);
        SINTRA_SPINLOCK_HOOK(after_end_generation);
        if (!valid || effort.ns < witness.last_effort_ns || owner != witness.owner ||
            generation != witness.generation)
        {
            return false;
        }
        witness.last_effort_ns = effort.ns;
        elapsed_effort_ns = effort.ns - witness.start_effort_ns;
        return true;
    }

    bool try_take_over_exited_owner(uint64_t self, const detail::spinlock_site& site)
    {
        const uint64_t owner = m_words.owner.load(std::memory_order_acquire);
        if (owner == 0 || !detail::process_instance_has_exited(owner, self)) {
            return false;
        }
        detail::Spinlock_diagnostic()
            .text("[sintra][spinlock] Exited owner PID ")
            .number(detail::process_instance_pid(owner))
            .text("; attempting shared-lock recovery.")
            .emit(site);
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

    [[noreturn]] void report_live_owner_stall(
        uint32_t owner, uint64_t elapsed_effort_ns, const detail::spinlock_site& site) const
    {
        detail::Spinlock_diagnostic()
            .text("[sintra][spinlock] Shared spinlock stuck after waiter effort ns ")
            .number(elapsed_effort_ns)
            .text("; owner PID ").number(owner)
            .text(" is still alive. Aborting to avoid corruption.")
            .emit(site);
        detail::native_debug_aware_abort();
    }

    detail::spinlock_words m_words;
};

static_assert(sizeof(spinlock) == 16 && alignof(spinlock) == 16);

} // namespace sintra

#undef SINTRA_SPINLOCK_HOOK
#undef SINTRA_SPINLOCK_SOURCE_LOCATION
