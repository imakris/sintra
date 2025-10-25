#pragma once

// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <random>
#include <type_traits>
#include <unordered_map>
#if defined(_WIN32)
#    include <algorithm>
#    include <climits>
#    include <cwchar>
#endif

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <Windows.h>
#  include <synchapi.h>
#elif defined(__linux__)
  #include <errno.h>
  #include <linux/futex.h>
  #include <sched.h>
  #include <sys/syscall.h>
  #include <time.h>
  #include <unistd.h>
#elif defined(__APPLE__)
  #include <errno.h>
#  if __has_include(<sys/ulock.h>)
    #include <sys/ulock.h>
#    define SINTRA_HAS_ULOCK 1
#  else
    #include <fcntl.h>
    #include <semaphore.h>
    #include <time.h>
    #include <unistd.h>
#    define SINTRA_HAS_ULOCK 0
#  endif
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <semaphore.h>
  #include <time.h>
  #include <unistd.h>
#endif

#ifndef SINTRA_HAS_ULOCK
#define SINTRA_HAS_ULOCK 0
#endif

namespace sintra {
namespace ipc {

namespace detail {

enum class wait_status { value_changed, timed_out };

inline std::chrono::steady_clock::time_point to_steady(
    const std::chrono::steady_clock::time_point& tp)
{
    return tp;
}

template <class Clock, class Duration>
inline std::chrono::steady_clock::time_point to_steady(
    const std::chrono::time_point<Clock, Duration>& tp)
{
    const auto now_clock  = Clock::now();
    const auto now_steady = std::chrono::steady_clock::now();
    const auto delta      = tp - now_clock;
    return now_steady + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delta);
}

#if defined(_WIN32)

namespace win32 {

struct semaphore_state
{
    std::atomic<int32_t> initialized{0};
    wchar_t              name[64]{};
};

inline std::unordered_map<void*, HANDLE>& handle_map()
{
    static std::unordered_map<void*, HANDLE> map;
    return map;
}

inline std::mutex& handle_mutex()
{
    static std::mutex mtx;
    return mtx;
}

inline HANDLE open_or_create_handle(semaphore_state& state)
{
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        auto                        it = handle_map().find(&state);
        if (it != handle_map().end()) {
            return it->second;
        }
    }

    int init_state = state.initialized.load(std::memory_order_acquire);
    if (init_state != 2) {
        int expected = 0;
        if (state.initialized.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            std::random_device rd;
            const auto         unique_hi = static_cast<unsigned long long>(::GetCurrentProcessId());
            const auto         unique_lo = static_cast<unsigned long long>(rd());
            const auto         unique_id = (unique_hi << 32) ^ unique_lo ^
                                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&state));

            std::swprintf(state.name,
                          sizeof(state.name) / sizeof(wchar_t),
                          L"Global\\sintra_ipc_sem_%016llx",
                          unique_id);

            HANDLE created = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, state.name);
            if (!created) {
                state.initialized.store(0, std::memory_order_release);
                return nullptr;
            }

            {
                std::lock_guard<std::mutex> lock(handle_mutex());
                handle_map()[&state] = created;
            }

            state.initialized.store(2, std::memory_order_release);
            return created;
        }

        while (state.initialized.load(std::memory_order_acquire) == 1) {
            ::Sleep(0);
        }
    }

    HANDLE opened = ::OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, state.name);
    if (!opened) {
        opened = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, state.name);
        if (!opened) {
            return nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        auto [it, inserted]         = handle_map().emplace(&state, opened);
        if (!inserted) {
            ::CloseHandle(opened);
            return it->second;
        }
    }

    return opened;
}

inline void close_handle(semaphore_state& state)
{
    std::lock_guard<std::mutex> lock(handle_mutex());
    auto                        it = handle_map().find(&state);
    if (it != handle_map().end()) {
        ::CloseHandle(it->second);
        handle_map().erase(it);
    }
}

} // namespace win32

inline void platform_wake(std::atomic<int32_t>& value, win32::semaphore_state& state)
{
    (void)value;
    if (HANDLE handle = win32::open_or_create_handle(state)) {
        ::ReleaseSemaphore(handle, 1, nullptr);
    }
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected, win32::semaphore_state& state)
{
    HANDLE handle = win32::open_or_create_handle(state);
    if (!handle) {
        while (value.load(std::memory_order_acquire) == expected) {
            ::Sleep(1);
        }
        return;
    }

    while (value.load(std::memory_order_acquire) == expected) {
        DWORD wait_result = ::WaitForSingleObject(handle, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            return;
        }
        if (wait_result == WAIT_FAILED) {
            ::Sleep(1);
        }
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout,
                                       win32::semaphore_state& state)
{
    HANDLE handle = win32::open_or_create_handle(state);
    if (!handle) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (value.load(std::memory_order_acquire) != expected) {
                return wait_status::value_changed;
            }
            ::Sleep(1);
        }
        return wait_status::timed_out;
    }

    while (value.load(std::memory_order_acquire) == expected) {
        DWORD wait_ms = INFINITE;
        if (timeout <= std::chrono::nanoseconds::zero()) {
            wait_ms = 0;
        } else {
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
            if (millis.count() <= 0) {
                wait_ms = 1;
            } else if (millis.count() >= static_cast<long long>(INFINITE - 1)) {
                wait_ms = INFINITE - 1;
            } else {
                wait_ms = static_cast<DWORD>(millis.count());
            }
        }

        DWORD wait_result = ::WaitForSingleObject(handle, wait_ms);
        if (wait_result == WAIT_OBJECT_0) {
            return wait_status::value_changed;
        }
        if (wait_result == WAIT_TIMEOUT) {
            return wait_status::timed_out;
        }
        if (wait_result == WAIT_FAILED) {
            return wait_status::timed_out;
        }
    }

    return wait_status::value_changed;
}

#elif defined(__APPLE__) && SINTRA_HAS_ULOCK

#ifndef UL_COMPARE_AND_WAIT_SHARED
#define UL_COMPARE_AND_WAIT_SHARED (UL_COMPARE_AND_WAIT | ULF_SHARED)
#endif

inline void platform_wake(std::atomic<int32_t>& value)
{
    __ulock_wake(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), 0);
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected)
{
    while (true) {
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), expected, 0);
        if (ret == 0) {
            return;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN) {
            return;
        }
        // Other errors break out to avoid spinning.
        return;
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) {
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, static_cast<void*>(&value), expected, 0);
        if (ret == 0) {
            return wait_status::value_changed;
        }
        int err = errno;
        if (err == EAGAIN || err == EINTR) {
            return wait_status::value_changed;
        }
        return wait_status::timed_out;
    }

    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    uint64_t us = static_cast<uint64_t>(micros.count());
    if (us == 0) {
        us = 1;
    }
    if (us > std::numeric_limits<uint32_t>::max()) {
        us = std::numeric_limits<uint32_t>::max();
    }

    int ret = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED,
                           static_cast<void*>(&value),
                           expected,
                           static_cast<uint32_t>(us));
    if (ret == 0) {
        return wait_status::value_changed;
    }
    int err = errno;
    if (err == ETIMEDOUT) {
        return wait_status::timed_out;
    }
    if (err == EINTR) {
        return wait_status::value_changed;
    }
    return (err == EAGAIN) ? wait_status::value_changed : wait_status::timed_out;
}

#elif defined(__linux__)

inline void platform_wake(std::atomic<int32_t>& value)
{
    syscall(SYS_futex, static_cast<int32_t*>(static_cast<void*>(&value)), FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected)
{
    while (true) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          nullptr,
                          nullptr,
                          0);
        if (res == 0) {
            return;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN) {
            return;
        }
        return;
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout)
{
    if (timeout <= std::chrono::nanoseconds::zero()) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          nullptr,
                          nullptr,
                          0);
        if (res == 0) {
            return wait_status::value_changed;
        }
        return (errno == EAGAIN) ? wait_status::value_changed : wait_status::timed_out;
    }

    timespec ts;
    auto      secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto      nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs);
    ts.tv_sec  = static_cast<long>(secs.count());
    ts.tv_nsec = static_cast<long>(nanos.count());

    while (true) {
        int res = syscall(SYS_futex,
                          static_cast<int32_t*>(static_cast<void*>(&value)),
                          FUTEX_WAIT,
                          expected,
                          &ts,
                          nullptr,
                          0);
        if (res == 0) {
            return wait_status::value_changed;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == ETIMEDOUT) {
            return wait_status::timed_out;
        }
        if (err == EAGAIN) {
            return wait_status::value_changed;
        }
        return wait_status::timed_out;
    }
}

#else // POSIX named semaphore fallback

namespace posix {

struct semaphore_state
{
    std::atomic<int32_t> initialized{0};
    char                  name[64]{};
};

inline std::unordered_map<void*, sem_t*>& handle_map()
{
    static std::unordered_map<void*, sem_t*> map;
    return map;
}

inline std::mutex& handle_mutex()
{
    static std::mutex mtx;
    return mtx;
}

inline sem_t* open_or_create_handle(semaphore_state& state)
{
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        auto                        it = handle_map().find(&state);
        if (it != handle_map().end()) {
            return it->second;
        }
    }

    int init_state = state.initialized.load(std::memory_order_acquire);
    if (init_state != 2) {
        int expected = 0;
        if (state.initialized.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            std::random_device rd;
            const auto         unique_hi = static_cast<unsigned long long>(::getpid());
            const auto         unique_lo = static_cast<unsigned long long>(rd());
            const auto         unique_id = (unique_hi << 32) ^ unique_lo ^
                                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&state));

            std::snprintf(state.name,
                          sizeof(state.name),
                          "/sintra_ipc_sem_%016llx",
                          unique_id);

            sem_t* created = sem_open(state.name, O_CREAT | O_EXCL, 0600, 0);
            if (created == SEM_FAILED) {
                if (errno == EEXIST) {
                    created = sem_open(state.name, 0);
                }
            }
            if (created == SEM_FAILED) {
                state.initialized.store(0, std::memory_order_release);
                return nullptr;
            }

            {
                std::lock_guard<std::mutex> lock(handle_mutex());
                handle_map()[&state] = created;
            }

            state.initialized.store(2, std::memory_order_release);
            return created;
        }

        while (state.initialized.load(std::memory_order_acquire) == 1) {
            ::usleep(1000);
        }
    }

    sem_t* opened = sem_open(state.name, 0);
    if (opened == SEM_FAILED) {
        opened = sem_open(state.name, O_CREAT, 0600, 0);
        if (opened == SEM_FAILED) {
            return nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        auto [it, inserted]         = handle_map().emplace(&state, opened);
        if (!inserted) {
            sem_close(opened);
            return it->second;
        }
    }

    return opened;
}

inline void close_handle(semaphore_state& state)
{
    std::lock_guard<std::mutex> lock(handle_mutex());
    auto                        it = handle_map().find(&state);
    if (it != handle_map().end()) {
        sem_close(it->second);
        handle_map().erase(it);
    }
}

} // namespace posix

inline void platform_wake(std::atomic<int32_t>& value, posix::semaphore_state& state)
{
    (void)value;
    if (sem_t* sem = posix::open_or_create_handle(state)) {
        sem_post(sem);
    }
}

inline void platform_wait(std::atomic<int32_t>& value, int32_t expected, posix::semaphore_state& state)
{
    sem_t* sem = posix::open_or_create_handle(state);
    if (!sem) {
        while (value.load(std::memory_order_acquire) == expected) {
            ::usleep(1000);
        }
        return;
    }

    while (value.load(std::memory_order_acquire) == expected) {
        if (sem_wait(sem) == 0) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
    }
}

inline wait_status platform_wait_until(std::atomic<int32_t>& value,
                                       int32_t                expected,
                                       std::chrono::nanoseconds timeout,
                                       posix::semaphore_state& state)
{
    sem_t* sem = posix::open_or_create_handle(state);
    if (!sem) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (value.load(std::memory_order_acquire) != expected) {
                return wait_status::value_changed;
            }
            ::usleep(1000);
        }
        return wait_status::timed_out;
    }

    if (timeout <= std::chrono::nanoseconds::zero()) {
        if (sem_trywait(sem) == 0) {
            return wait_status::value_changed;
        }
        return (errno == EAGAIN) ? wait_status::timed_out : wait_status::value_changed;
    }

    timespec ts{};
    auto     now       = std::chrono::system_clock::now().time_since_epoch();
    auto     now_secs  = std::chrono::duration_cast<std::chrono::seconds>(now);
    auto     now_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - now_secs);
    ts.tv_sec          = static_cast<time_t>(now_secs.count());
    ts.tv_nsec         = static_cast<long>(now_nanos.count());

    auto secs  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto nanos = timeout - secs;
    ts.tv_sec += static_cast<time_t>(secs.count());
    ts.tv_nsec += static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(nanos).count());
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
    }

    while (value.load(std::memory_order_acquire) == expected) {
        if (sem_timedwait(sem, &ts) == 0) {
            return wait_status::value_changed;
        }
        if (errno == ETIMEDOUT) {
            return wait_status::timed_out;
        }
        if (errno != EINTR) {
            return wait_status::timed_out;
        }
    }

    return wait_status::value_changed;
}

#endif

} // namespace detail

class interprocess_semaphore {
public:
    explicit interprocess_semaphore(unsigned int initial_count = 0) noexcept
        : m_count(static_cast<int32_t>(initial_count))
    {}

    ~interprocess_semaphore()
    {
#if defined(_WIN32)
        detail::win32::close_handle(m_platform_state);
#elif !defined(__linux__) && !(defined(__APPLE__) && SINTRA_HAS_ULOCK)
        detail::posix::close_handle(m_platform_state);
#endif
    }

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    void post()
    {
        int32_t previous = m_count.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
#if defined(_WIN32)
            detail::platform_wake(m_count, m_platform_state);
#elif defined(__APPLE__) && SINTRA_HAS_ULOCK
            detail::platform_wake(m_count);
#elif defined(__linux__)
            detail::platform_wake(m_count);
#else
            detail::platform_wake(m_count, m_platform_state);
#endif
        }
    }

    void wait()
    {
        int32_t expected = m_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (expected >= 0) {
            return;
        }
        wait_slow(expected);
    }

    bool try_wait()
    {
        int32_t current = m_count.load(std::memory_order_acquire);
        while (current > 0) {
            if (m_count.compare_exchange_weak(current,
                                              current - 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    template <class Clock, class Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        const auto deadline = detail::to_steady(abs_time);
        return timed_wait_deadline(deadline);
    }

private:
    void wait_slow(int32_t expected)
    {
        while (true) {
#if defined(_WIN32)
            detail::platform_wait(m_count, expected, m_platform_state);
#elif defined(__APPLE__) && SINTRA_HAS_ULOCK
            detail::platform_wait(m_count, expected);
#elif defined(__linux__)
            detail::platform_wait(m_count, expected);
#else
            detail::platform_wait(m_count, expected, m_platform_state);
#endif
            int32_t current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return;
            }
            expected = current;
        }
    }

    bool timed_wait_deadline(const std::chrono::steady_clock::time_point& deadline)
    {
        int32_t expected = m_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (expected >= 0) {
            return true;
        }

        while (true) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (finalize_timeout(expected)) {
                    return false;
                }
                return true;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
#if defined(_WIN32)
            detail::wait_status result =
                detail::platform_wait_until(m_count, expected, remaining, m_platform_state);
#elif defined(__APPLE__) && SINTRA_HAS_ULOCK
            detail::wait_status result = detail::platform_wait_until(m_count, expected, remaining);
#elif defined(__linux__)
            detail::wait_status result = detail::platform_wait_until(m_count, expected, remaining);
#else
            detail::wait_status result =
                detail::platform_wait_until(m_count, expected, remaining, m_platform_state);
#endif
            int32_t             current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return true;
            }
            if (result == detail::wait_status::timed_out) {
                if (finalize_timeout(expected)) {
                    return false;
                }
                return true;
            }
            expected = current;
        }
    }

    bool finalize_timeout(int32_t expected)
    {
        while (true) {
            int32_t current = m_count.load(std::memory_order_acquire);
            if (current > expected) {
                return false; // a post occurred concurrently; treat as acquired
            }
            if (m_count.compare_exchange_strong(current,
                                                current + 1,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
                return true;
            }
        }
    }

#if defined(_WIN32)
    mutable detail::win32::semaphore_state m_platform_state{};
#elif !defined(__linux__) && !(defined(__APPLE__) && SINTRA_HAS_ULOCK)
    mutable detail::posix::semaphore_state m_platform_state{};
#endif
    std::atomic<int32_t> m_count;
};

} // namespace ipc
} // namespace sintra

