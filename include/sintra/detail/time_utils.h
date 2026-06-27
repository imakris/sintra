// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

// Shared monotonic timeline utilities used by IPC primitives that coordinate
// across multiple processes.  The functions exposed here intentionally mirror
// the helpers embedded in the semaphore backend so that every component that
// needs a monotonic timestamp can rely on the same cross-process epoch.

#include <cstdint>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#if defined(_WIN32)
  #include "sintra_windows.h"
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_time.h>
#elif defined(__unix__) || defined(__APPLE__)
  #include <time.h>
#endif

namespace sintra {

#if defined(__APPLE__)
inline const mach_timebase_info_data_t& mach_timebase_info_cached() noexcept
{
    static const mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t cached{};
        (void)mach_timebase_info(&cached);
        return cached;
    }();
    return info;
}
#endif

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

    const long double ticks      = static_cast<long double>(counter.QuadPart);
    const long double per_second = static_cast<long double>(freq.value.QuadPart);
    return static_cast<uint64_t>((ticks * 1000000000.0L) / per_second);
#elif defined(__APPLE__)
    const auto&    timebase = mach_timebase_info_cached();
    const uint64_t now      = ::mach_absolute_time();
    return (now * static_cast<uint64_t>(timebase.numer)) /
           static_cast<uint64_t>(timebase.denom);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return
        static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
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

#if defined(_WIN32)
constexpr DWORD k_max_finite_sleep_ms = INFINITE - 1u;
constexpr LONGLONG k_max_finite_waitable_timer_intervals =
    static_cast<LONGLONG>(k_max_finite_sleep_ms) * 10000LL;

inline void coarse_sleep_for(std::chrono::duration<double> duration)
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return;
    }

    const double seconds = duration.count();
    if (std::isnan(seconds)) {
        return;
    }
    if (!std::isfinite(seconds)) {
        ::Sleep(k_max_finite_sleep_ms);
        return;
    }

    constexpr std::chrono::duration<double, std::milli> max_duration(
        k_max_finite_sleep_ms);
    if (duration >= max_duration) {
        ::Sleep(k_max_finite_sleep_ms);
        return;
    }

    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    if (millis < duration) {
        ++millis;
    }

    if (millis.count() <= 0) {
        ::Sleep(0);
        return;
    }

    ::Sleep(static_cast<DWORD>(millis.count()));
}

inline HANDLE create_precision_waitable_timer() noexcept
{
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
    constexpr DWORD high_resolution_flag = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
#else
    constexpr DWORD high_resolution_flag = 0x00000002u;
#endif

    constexpr DWORD access = SYNCHRONIZE | TIMER_MODIFY_STATE;
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, high_resolution_flag, access);
    if (timer != nullptr) {
        return timer;
    }
    return ::CreateWaitableTimerExW(nullptr, nullptr, 0, access);
}

inline LONGLONG waitable_timer_100ns_intervals(
    std::chrono::duration<double> duration) noexcept
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return 1;
    }

    const double seconds_value = duration.count();
    if (std::isnan(seconds_value)) {
        return 1;
    }

    const long double seconds = static_cast<long double>(seconds_value);
    if (!std::isfinite(seconds)) {
        return k_max_finite_waitable_timer_intervals;
    }

    constexpr long double intervals_per_second = 10000000.0L;
    const long double intervals = std::ceil(seconds * intervals_per_second);

    if (!std::isfinite(intervals) ||
        intervals >= static_cast<long double>(k_max_finite_waitable_timer_intervals))
    {
        return k_max_finite_waitable_timer_intervals;
    }
    if (intervals <= 0.0L) {
        return 1;
    }
    return static_cast<LONGLONG>(intervals);
}

inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return;
    }
    if (std::isnan(duration.count())) {
        return;
    }

    class Precision_timer
    {
    public:
        ~Precision_timer()
        {
            if (m_handle != nullptr) {
                ::CloseHandle(m_handle);
            }
        }

        HANDLE handle() const noexcept { return m_handle; }

    private:
        HANDLE m_handle = create_precision_waitable_timer();
    };

    static thread_local Precision_timer timer;
    if (timer.handle() == nullptr) {
        coarse_sleep_for(duration);
        return;
    }

    LARGE_INTEGER due_time{};
    due_time.QuadPart = -waitable_timer_100ns_intervals(duration);

    if (::SetWaitableTimer(timer.handle(), &due_time, 0, nullptr, nullptr, FALSE)) {
        const DWORD wait_result = ::WaitForSingleObject(timer.handle(), INFINITE);
        if (wait_result != WAIT_FAILED) {
            return;
        }
    }

    coarse_sleep_for(duration);
}
#elif defined(__APPLE__)
inline void precision_sleep_for(std::chrono::duration<double> duration)
{
    if (duration <= std::chrono::duration<double>::zero()) {
        return;
    }

    const auto& timebase = mach_timebase_info_cached();

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

} // namespace sintra
