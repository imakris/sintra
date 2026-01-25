// Crash capture test: self-crash after a randomized delay.
#include "test_environment.h"
#include "sintra/detail/debug_pause.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

int clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

int read_env_int(const char* name, int default_value)
{
    return sintra::test::read_env_int(name, default_value);
}

std::uint64_t make_seed()
{
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(getpid());
#endif
    return now ^ (pid << 1U);
}

int pick_delay_ms()
{
    int min_ms = read_env_int("SINTRA_CRASH_CAPTURE_MIN_MS", 5);
    int max_ms = read_env_int("SINTRA_CRASH_CAPTURE_MAX_MS", 250);
    if (max_ms < min_ms) {
        std::swap(max_ms, min_ms);
    }
    min_ms = clamp_int(min_ms, 0, 60000);
    max_ms = clamp_int(max_ms, min_ms, 60000);

    std::mt19937 rng(static_cast<unsigned>(make_seed()));
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return dist(rng);
}

} // namespace

int main()
{
    sintra::detail::install_debug_pause_handlers();

    const int delay_ms = pick_delay_ms();
    std::fprintf(stderr, "[crash_capture_self] delay=%dms\n", delay_ms);
    std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    sintra::test::precrash_pause("self-precrash");
    sintra::test::emit_self_stack_trace();
    sintra::test::trigger_stack_capture_crash("self");
}
