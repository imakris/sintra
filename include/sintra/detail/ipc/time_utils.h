// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

// Shared monotonic timeline utilities used by IPC primitives that coordinate
// across multiple processes.  The functions exposed here intentionally mirror
// the helpers embedded in the semaphore backend so that every component that
// needs a monotonic timestamp can rely on the same cross-process epoch.

#include <cstdint>
#include <chrono>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
#elif defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(__unix__) || defined(__APPLE__)
  #include <time.h>
#endif

namespace sintra { namespace detail {

inline uint64_t monotonic_now_ns() noexcept
{
#if defined(_WIN32)
    static const struct frequency_wrapper {
        LARGE_INTEGER value{};
        frequency_wrapper()
        {
            ::QueryPerformanceFrequency(&value);
        }
    } freq;

    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);

    if (freq.value.QuadPart == 0) {
        return 0;
    }

    const long double ticks = static_cast<long double>(counter.QuadPart);
    const long double per_second = static_cast<long double>(freq.value.QuadPart);
    return static_cast<uint64_t>((ticks * 1000000000.0L) / per_second);
#elif defined(__APPLE__)
    static const struct timebase_wrapper {
        mach_timebase_info_data_t info{};
        timebase_wrapper()
        {
            (void)::mach_timebase_info(&info);
        }
    } timebase;

    const uint64_t now = ::mach_absolute_time();
    return (now * static_cast<uint64_t>(timebase.info.numer)) /
           static_cast<uint64_t>(timebase.info.denom);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
#else
    // As a last resort fall back to the steady clock.  This should only happen
    // on platforms that do not expose CLOCK_MONOTONIC.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

inline uint64_t monotonic_now_us() noexcept
{
    return monotonic_now_ns() / 1000u;
}

}} // namespace sintra::detail

