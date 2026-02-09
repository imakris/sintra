// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

// Shared monotonic timeline utilities used by IPC primitives that coordinate
// across multiple processes.  The functions exposed here intentionally mirror
// the helpers embedded in the semaphore backend so that every component that
// needs a monotonic timestamp can rely on the same cross-process epoch.

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(_WIN32)
  #include "sintra_windows.h"
  #include <timeapi.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_time.h>
#elif defined(__unix__) || defined(__APPLE__)
  #include <time.h>
#endif

namespace sintra {

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

inline double get_wtime() noexcept
{
    return static_cast<double>(monotonic_now_ns()) * 1e-9;
}

#if defined(__APPLE__)
inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return;
    }

    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();

    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    if (nanos.count() <= 0) {
        return;
    }

    unsigned __int128 absolute_delta = static_cast<unsigned __int128>(nanos.count()) *
                                       static_cast<unsigned __int128>(timebase.denom);
    absolute_delta += static_cast<unsigned __int128>(timebase.numer - 1);
    absolute_delta /= static_cast<unsigned __int128>(timebase.numer);

    if (absolute_delta == 0) {
        std::this_thread::sleep_for(duration);
        return;
    }

    const uint64_t target = mach_absolute_time() + static_cast<uint64_t>(absolute_delta);

    auto status = mach_wait_until(target);
    while (status == KERN_ABORTED) {
        status = mach_wait_until(target);
    }
}
#else
inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    std::this_thread::sleep_for(duration);
}
#endif

#ifdef _WIN32
class Scoped_timer_resolution {
public:
    explicit Scoped_timer_resolution(UINT period)
        : m_period(period), m_active(::timeBeginPeriod(period) == TIMERR_NOERROR) {}

    ~Scoped_timer_resolution()
    {
        if (m_active) {
            ::timeEndPeriod(m_period);
        }
    }

private:
    UINT m_period;
    bool m_active;
};
#else
class Scoped_timer_resolution {
public:
    explicit Scoped_timer_resolution(unsigned int) {}
};
#endif

} // namespace sintra

