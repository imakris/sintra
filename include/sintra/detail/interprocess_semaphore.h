#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <type_traits>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <synchapi.h>
#elif defined(__APPLE__)
  #include <errno.h>
  #include <sys/ulock.h>
  #include <unistd.h>
#else
  #include <cerrno>
  #include <linux/futex.h>
  #include <sys/syscall.h>
  #include <time.h>
  #include <unistd.h>
#endif

namespace sintra {
namespace detail {

class interprocess_semaphore
{
public:
    explicit interprocess_semaphore(unsigned int initial = 0) noexcept
        : count_(static_cast<int>(initial))
    {}

    interprocess_semaphore(const interprocess_semaphore&)            = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    void post()
    {
        int previous = count_.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
            wake_one();
        }
    }

    void wait()
    {
        int previous = count_.fetch_sub(1, std::memory_order_acquire);
        if (previous <= 0) {
            wait_slow(previous - 1, std::nullopt);
        }
    }

    bool try_wait()
    {
        int current = count_.load(std::memory_order_acquire);
        while (current > 0) {
            if (count_.compare_exchange_weak(
                        current,
                        current - 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    template<class Clock, class Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        if (try_wait()) {
            return true;
        }

        auto now_clock = Clock::now();
        if (now_clock >= abs_time) {
            return false;
        }

        auto deadline = make_steady_deadline(abs_time, now_clock);

        int previous = count_.fetch_sub(1, std::memory_order_acquire);
        if (previous > 0) {
            return true;
        }

        if (wait_slow(previous - 1, deadline)) {
            return true;
        }

        return false;
    }

    int get_count() const noexcept { return count_.load(std::memory_order_acquire); }

private:
    using steady_clock = std::chrono::steady_clock;

    template<class Clock, class Duration>
    static steady_clock::time_point make_steady_deadline(
            const std::chrono::time_point<Clock, Duration>& abs_time,
            const std::chrono::time_point<Clock, Duration>& now_clock)
    {
        if constexpr (std::is_same_v<Clock, steady_clock>) {
            return abs_time;
        }
        else {
            auto diff = abs_time - now_clock;
            if (diff <= Duration::zero()) {
                return steady_clock::now();
            }
            return steady_clock::now() + std::chrono::duration_cast<steady_clock::duration>(diff);
        }
    }

    bool wait_slow(int expected, std::optional<steady_clock::time_point> deadline)
    {
        int value = expected;
        while (value < 0) {
            if (!wait_for_value(value, deadline)) {
                // Timed out. Restore the decrement and report failure.
                int previous = count_.fetch_add(1, std::memory_order_release);
                (void)previous;
                return false;
            }
            value = count_.load(std::memory_order_acquire);
        }
        return true;
    }

    bool wait_for_value(int expected, std::optional<steady_clock::time_point> deadline)
    {
#ifdef _WIN32
        return wait_on_address(expected, deadline);
#elif defined(__APPLE__)
        return wait_ulock(expected, deadline);
#else
        return wait_futex(expected, deadline);
#endif
    }

    void wake_one()
    {
#ifdef _WIN32
        WakeByAddressSingle(reinterpret_cast<volatile VOID*>(&count_));
#elif defined(__APPLE__)
        __ulock_wake(UL_COMPARE_AND_WAIT | ULF_SHARED, reinterpret_cast<void*>(&count_), 0);
#else
        int op = FUTEX_WAKE;
#ifdef FUTEX_PRIVATE_FLAG
        op |= FUTEX_PRIVATE_FLAG;
#endif
        futex(reinterpret_cast<int*>(&count_), op, 1);
#endif
    }

#ifndef _WIN32
    static int futex(int* addr, int op, int val, const struct timespec* ts = nullptr)
    {
#if defined(__linux__)
        int result = syscall(SYS_futex, addr, op, val, ts, nullptr, 0);
        if (result == -1) {
            return -errno;
        }
        return result;
#else
        (void)addr;
        (void)op;
        (void)val;
        (void)ts;
        return -ENOSYS;
#endif
    }
#endif

#ifdef _WIN32
    bool wait_on_address(int expected, std::optional<steady_clock::time_point> deadline)
    {
        while (count_.load(std::memory_order_relaxed) == expected) {
            DWORD timeout = INFINITE;
            if (deadline) {
                auto now = steady_clock::now();
                if (now >= *deadline) {
                    return false;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
                if (remaining.count() <= 0) {
                    timeout = 0;
                }
                else if (remaining.count() >= static_cast<long long>(std::numeric_limits<DWORD>::max())) {
                    timeout = INFINITE;
                }
                else {
                    timeout = static_cast<DWORD>(remaining.count());
                }
            }

            BOOL ok = WaitOnAddress(
                    reinterpret_cast<volatile VOID*>(&count_),
                    &expected,
                    sizeof(expected),
                    timeout);
            if (ok) {
                return true;
            }

            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT) {
                return false;
            }
        }
        return true;
    }
#elif defined(__APPLE__)
    bool wait_ulock(int expected, std::optional<steady_clock::time_point> deadline)
    {
        while (count_.load(std::memory_order_relaxed) == expected) {
            uint32_t op = UL_COMPARE_AND_WAIT | ULF_SHARED;
            uint64_t timeout = 0;
            if (deadline) {
                auto now = steady_clock::now();
                if (now >= *deadline) {
                    return false;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
                if (remaining.count() <= 0) {
                    timeout = 0;
                }
                else {
                    timeout = static_cast<uint64_t>(remaining.count());
                }
            }

            int rc = __ulock_wait(op, reinterpret_cast<void*>(&count_), static_cast<uint64_t>(expected), timeout);
            if (rc == 0) {
                return true;
            }

            int err = errno;
            if (err == ETIMEDOUT) {
                return false;
            }
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN) {
                return true;
            }
            throw std::system_error(err, std::system_category(), "__ulock_wait failed");
        }
        return true;
    }
#else
    bool wait_futex(int expected, std::optional<steady_clock::time_point> deadline)
    {
        while (count_.load(std::memory_order_relaxed) == expected) {
            const struct timespec* ts_ptr = nullptr;
            struct timespec ts;
            int op = FUTEX_WAIT;
#ifdef FUTEX_PRIVATE_FLAG
            op |= FUTEX_PRIVATE_FLAG;
#endif

            if (deadline) {
                auto now = steady_clock::now();
                if (now >= *deadline) {
                    return false;
                }
                auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
                long long total_ns = remaining.count();
                if (total_ns <= 0) {
                    return false;
                }
                constexpr long long kBillion = 1000000000LL;
                long long sec_part = total_ns / kBillion;
                if (sec_part > static_cast<long long>(std::numeric_limits<time_t>::max())) {
                    ts.tv_sec  = std::numeric_limits<time_t>::max();
                    ts.tv_nsec = 999999999L;
                }
                else {
                    ts.tv_sec  = static_cast<time_t>(sec_part);
                    long long nano_part = total_ns - sec_part * kBillion;
                    ts.tv_nsec = static_cast<long>(nano_part);
                }
                ts_ptr     = &ts;
            }

            int rc = futex(reinterpret_cast<int*>(&count_), op, expected, ts_ptr);
            if (rc == 0) {
                return true;
            }

            int err = -rc;
            if (err == ETIMEDOUT) {
                return false;
            }
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN) {
                return true;
            }
            throw std::system_error(err, std::system_category(), "futex wait failed");
        }
        return true;
    }
#endif

    std::atomic<int> count_;
};

} // namespace detail
} // namespace sintra

