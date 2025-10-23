#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

namespace sintra {
namespace detail {

class deterministic_delay_fuzzer {
public:
    static void set_enabled(bool enabled) noexcept
    {
        s_enabled.store(enabled, std::memory_order_release);
    }

    static bool enabled() noexcept
    {
        return s_enabled.load(std::memory_order_acquire);
    }

    static void set_seed(std::uint64_t seed) noexcept
    {
        s_seed.store(seed, std::memory_order_release);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
    }

    static std::uint64_t seed() noexcept
    {
        return s_seed.load(std::memory_order_acquire);
    }

    static void set_delay_bounds(std::uint32_t min_delay_us, std::uint32_t max_delay_us) noexcept
    {
        if (max_delay_us < min_delay_us) {
            max_delay_us = min_delay_us;
        }
        s_min_delay.store(min_delay_us, std::memory_order_release);
        s_max_delay.store(max_delay_us, std::memory_order_release);
    }

    static std::uint32_t min_delay() noexcept
    {
        return s_min_delay.load(std::memory_order_acquire);
    }

    static std::uint32_t max_delay() noexcept
    {
        return s_max_delay.load(std::memory_order_acquire);
    }

    static void set_injection_rate(unsigned rate) noexcept
    {
        if (rate == 0U) {
            rate = 1U;
        }
        s_injection_rate.store(rate, std::memory_order_release);
    }

    static unsigned injection_rate() noexcept
    {
        return s_injection_rate.load(std::memory_order_acquire);
    }

    static void maybe_inject_delay(const char* /*tag*/ = nullptr) noexcept
    {
        if (!enabled()) {
            return;
        }

        auto& state = thread_state();
        const std::uint64_t generation = s_generation.load(std::memory_order_acquire);
        if (state.generation != generation) {
            initialise_state(state, generation);
        }

        if (!state.initialised) {
            return;
        }

        const unsigned rate = injection_rate();
        if (rate > 1U) {
            const unsigned decision = static_cast<unsigned>(state.prng());
            if (decision % rate != 0U) {
                return;
            }
        }

        const std::uint32_t min_delay_us = min_delay();
        const std::uint32_t max_delay_us = max_delay();
        if (max_delay_us == 0U && min_delay_us == 0U) {
            return;
        }

        const std::uint32_t span = (max_delay_us >= min_delay_us) ? (max_delay_us - min_delay_us) : 0U;
        std::uint32_t delay_us = min_delay_us;
        if (span > 0U) {
            delay_us += static_cast<std::uint32_t>(state.prng() % (static_cast<std::uint64_t>(span) + 1U));
        }

        if (delay_us == 0U) {
            return;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }

private:
    struct thread_state_data {
        std::mt19937_64 prng{};
        std::uint64_t   generation = 0;
        std::uint64_t   thread_index = 0;
        bool            has_thread_index = false;
        bool            initialised = false;
    };

    static thread_state_data& thread_state() noexcept
    {
        thread_local thread_state_data state{};
        return state;
    }

    static void initialise_state(thread_state_data& state, std::uint64_t generation) noexcept
    {
        state.generation = generation;
        const std::uint64_t base_seed = seed();

        if (!state.has_thread_index) {
            state.thread_index = s_thread_counter.fetch_add(1, std::memory_order_relaxed);
            state.has_thread_index = true;
        }

        std::seed_seq sequence{
            static_cast<std::uint32_t>(base_seed & 0xFFFF'FFFFULL),
            static_cast<std::uint32_t>((base_seed >> 32) & 0xFFFF'FFFFULL),
            static_cast<std::uint32_t>(state.thread_index & 0xFFFF'FFFFULL),
            static_cast<std::uint32_t>((state.thread_index >> 32) & 0xFFFF'FFFFULL),
            static_cast<std::uint32_t>(generation & 0xFFFF'FFFFULL)
        };
        state.prng.seed(sequence);
        state.initialised = true;
    }

    static std::atomic<bool>           s_enabled;
    static std::atomic<std::uint64_t>  s_seed;
    static std::atomic<std::uint64_t>  s_generation;
    static std::atomic<std::uint32_t>  s_min_delay;
    static std::atomic<std::uint32_t>  s_max_delay;
    static std::atomic<unsigned>       s_injection_rate;
    static std::atomic<std::uint64_t>  s_thread_counter;
};

inline std::atomic<bool>          deterministic_delay_fuzzer::s_enabled{false};
inline std::atomic<std::uint64_t> deterministic_delay_fuzzer::s_seed{0};
inline std::atomic<std::uint64_t> deterministic_delay_fuzzer::s_generation{1};
inline std::atomic<std::uint32_t> deterministic_delay_fuzzer::s_min_delay{0};
inline std::atomic<std::uint32_t> deterministic_delay_fuzzer::s_max_delay{30};
inline std::atomic<unsigned>      deterministic_delay_fuzzer::s_injection_rate{1};
inline std::atomic<std::uint64_t> deterministic_delay_fuzzer::s_thread_counter{0};

} // namespace detail

inline void set_delay_fuzzing_enabled(bool enabled) noexcept
{
    detail::deterministic_delay_fuzzer::set_enabled(enabled);
}

inline void set_delay_fuzzing_seed(std::uint64_t seed) noexcept
{
    detail::deterministic_delay_fuzzer::set_seed(seed);
}

inline void set_delay_fuzzing_run_index(std::uint64_t run_index) noexcept
{
    set_delay_fuzzing_seed(run_index);
    set_delay_fuzzing_enabled(true);
}

inline void set_delay_fuzzing_bounds(std::uint32_t min_delay_us, std::uint32_t max_delay_us) noexcept
{
    detail::deterministic_delay_fuzzer::set_delay_bounds(min_delay_us, max_delay_us);
}

inline void set_delay_fuzzing_injection_rate(unsigned rate) noexcept
{
    detail::deterministic_delay_fuzzer::set_injection_rate(rate);
}

namespace detail {

struct delay_fuzz_scope {
    explicit delay_fuzz_scope(bool enable) noexcept
        : m_previous(detail::deterministic_delay_fuzzer::enabled())
    {
        detail::deterministic_delay_fuzzer::set_enabled(enable);
    }

    ~delay_fuzz_scope() noexcept
    {
        detail::deterministic_delay_fuzzer::set_enabled(m_previous);
    }

    delay_fuzz_scope(const delay_fuzz_scope&) = delete;
    delay_fuzz_scope& operator=(const delay_fuzz_scope&) = delete;
    delay_fuzz_scope(delay_fuzz_scope&&) = delete;
    delay_fuzz_scope& operator=(delay_fuzz_scope&&) = delete;

private:
    bool m_previous;
};

inline void inject_delay_if_enabled(const char* tag = nullptr) noexcept
{
    deterministic_delay_fuzzer::maybe_inject_delay(tag);
}

} // namespace detail

} // namespace sintra

#define SINTRA_DELAY_FUZZ(tag) ::sintra::detail::inject_delay_if_enabled(tag)

