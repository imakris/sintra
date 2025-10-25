#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>
#include <type_traits>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <synchapi.h>
  #include <cerrno>
  #include <cwchar>
#elif defined(__APPLE__)
  #include <errno.h>
  #include <fcntl.h>
  #include <semaphore.h>
  #include <sched.h>
  #include <sys/stat.h>
  #include <time.h>
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
        return wait_semaphore(expected, deadline);
#else
        return wait_futex(expected, deadline);
#endif
    }

    void wake_one()
    {
#ifdef _WIN32
        HANDLE handle = get_semaphore_handle();
        if (!ReleaseSemaphore(handle, 1, nullptr)) {
            throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "ReleaseSemaphore failed");
        }
#elif defined(__APPLE__)
        sem_t* sem = get_semaphore_handle();
        if (sem_post(sem) == -1) {
            int err = errno;
            throw std::system_error(err, std::system_category(), "sem_post failed");
        }
#else
        int op = FUTEX_WAKE;
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
        HANDLE handle = get_semaphore_handle();
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

            DWORD result = WaitForSingleObject(handle, timeout);
            if (result == WAIT_OBJECT_0) {
                return true;
            }
            if (result == WAIT_TIMEOUT) {
                return false;
            }
            if (result == WAIT_FAILED) {
                throw std::system_error(
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                        "WaitForSingleObject failed");
            }
        }
        return true;
    }
#elif defined(__APPLE__)
    bool wait_semaphore(int expected, std::optional<steady_clock::time_point> deadline)
    {
        sem_t* sem = get_semaphore_handle();
        while (count_.load(std::memory_order_relaxed) == expected) {
            int rc = 0;
            if (deadline) {
                while (true) {
                    rc = sem_trywait(sem);
                    if (rc == 0) {
                        return true;
                    }

                    int err = errno;
                    if (err == EINTR) {
                        continue;
                    }
                    if (err != EAGAIN) {
                        throw std::system_error(err, std::system_category(), "semaphore wait failed");
                    }

                    auto now = steady_clock::now();
                    if (now >= *deadline) {
                        return false;
                    }

                    auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
                    if (remaining.count() <= 0) {
                        return false;
                    }

                    constexpr long long kBillion = 1000000000LL;
                    long long sleep_ns = remaining.count();
                    if (sleep_ns > 1000000LL) {
                        sleep_ns = 1000000LL;
                    }

                    struct timespec ts;
                    ts.tv_sec  = static_cast<time_t>(sleep_ns / kBillion);
                    ts.tv_nsec = static_cast<long>(sleep_ns % kBillion);
                    nanosleep(&ts, nullptr);
                }
            }
            else {
                rc = sem_wait(sem);
                if (rc == 0) {
                    return true;
                }

                int err = errno;
                if (err == EINTR) {
                    continue;
                }
                if (err == EAGAIN) {
                    return true;
                }
                throw std::system_error(err, std::system_category(), "semaphore wait failed");
            }
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

#ifdef _WIN32
    void ensure_name_initialized() const
    {
        int state = name_state_.load(std::memory_order_acquire);
        if (state == kNameInitialized) {
            return;
        }
        initialize_name();
    }

    void initialize_name() const
    {
        int expected = kNameUninitialized;
        if (name_state_.compare_exchange_strong(
                    expected,
                    kNameInitializing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
        {
            generate_unique_name();
            name_state_.store(kNameInitialized, std::memory_order_release);
        }
        else {
            while (name_state_.load(std::memory_order_acquire) != kNameInitialized) {
                Sleep(0);
            }
        }
    }

    void generate_unique_name() const
    {
        static std::atomic<uint64_t> local_counter{0};
        uint64_t counter_value = local_counter.fetch_add(1, std::memory_order_relaxed);
        unsigned long process_id = GetCurrentProcessId();
        unsigned long long ticks = GetTickCount64();
        uintptr_t address       = reinterpret_cast<uintptr_t>(this);

        errno       = 0;
        int written = swprintf(
                name_,
                kNameLength,
                L"SintraIPS_%08lx_%016llx_%016llx_%016llx",
                process_id,
                static_cast<unsigned long long>(ticks),
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(counter_value));
        if (written < 0) {
            int err = errno ? errno : EINVAL;
            throw std::system_error(
                    err,
                    std::system_category(),
                    "swprintf failed to generate semaphore name");
        }
        name_[kNameLength - 1] = L'\0';
    }

    HANDLE get_semaphore_handle() const
    {
        ensure_name_initialized();
        return win32_handle_registry::instance().get(this, name_);
    }

    struct win32_handle_registry
    {
        static win32_handle_registry& instance()
        {
            static win32_handle_registry registry;
            return registry;
        }

        HANDLE get(const interprocess_semaphore* semaphore, const wchar_t* name)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handles_.find(semaphore);
            if (it != handles_.end()) {
                return it->second;
            }

            HANDLE handle = CreateSemaphoreW(nullptr, 0, std::numeric_limits<LONG>::max(), name);
            if (!handle) {
                throw std::system_error(
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                        "CreateSemaphoreW failed");
            }

            handles_.emplace(semaphore, handle);
            return handle;
        }

        ~win32_handle_registry()
        {
            for (auto& entry : handles_) {
                if (entry.second) {
                    CloseHandle(entry.second);
                }
            }
        }

    private:
        win32_handle_registry() = default;

        std::mutex mutex_;
        std::unordered_map<const interprocess_semaphore*, HANDLE> handles_;
    };

    static constexpr int kNameUninitialized = 0;
    static constexpr int kNameInitializing  = 1;
    static constexpr int kNameInitialized   = 2;

    mutable std::atomic<int> name_state_{kNameUninitialized};
    static constexpr size_t  kNameLength = 128;
    mutable wchar_t          name_[kNameLength]{};
#elif defined(__APPLE__)
    void ensure_name_initialized() const
    {
        int state = name_state_.load(std::memory_order_acquire);
        if (state == kNameInitialized) {
            return;
        }
        initialize_name();
    }

    void initialize_name() const
    {
        int expected = kNameUninitialized;
        if (name_state_.compare_exchange_strong(
                    expected,
                    kNameInitializing,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
        {
            generate_unique_name();
            name_state_.store(kNameInitialized, std::memory_order_release);
        }
        else {
            while (name_state_.load(std::memory_order_acquire) != kNameInitialized) {
                sched_yield();
            }
        }
    }

    void generate_unique_name() const
    {
        // POSIX named semaphores on macOS are limited to 31 characters (including the
        // leading slash and excluding the null terminator). Longer names cause
        // sem_open to fail with ENAMETOOLONG. The previous implementation attempted to
        // embed several identifiers which routinely exceeded that limit on real-world
        // workloads. To keep the names short while still providing enough entropy to
        // avoid collisions, we condense the data we care about into two hexadecimal
        // components: the process id and a 64-bit mix of the address, timestamp and
        // a local counter.
        static std::atomic<uint64_t> local_counter{0};
        uint64_t counter_value = local_counter.fetch_add(1, std::memory_order_relaxed);
        pid_t    process_id    = getpid();
        uintptr_t address      = reinterpret_cast<uintptr_t>(this);
        uint64_t  timestamp    = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());

        uint64_t entropy = counter_value;
        entropy ^= static_cast<uint64_t>(process_id) << 32;
        entropy ^= static_cast<uint64_t>(address);
        entropy ^= timestamp;

        int written = std::snprintf(
                name_,
                kNameLength,
                "/SIPS_%08x_%016llx",
                static_cast<unsigned int>(process_id),
                static_cast<unsigned long long>(entropy));
        if (written < 0) {
            int err = errno ? errno : EINVAL;
            throw std::system_error(err, std::system_category(), "snprintf failed to generate semaphore name");
        }
        name_[kNameLength - 1] = '\0';
    }

    sem_t* get_semaphore_handle() const
    {
        ensure_name_initialized();
        return darwin_handle_registry::instance().get(this, name_);
    }

    struct darwin_handle_registry
    {
        static darwin_handle_registry& instance()
        {
            static darwin_handle_registry registry;
            return registry;
        }

        sem_t* get(const interprocess_semaphore* semaphore, const char* name)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handles_.find(semaphore);
            if (it != handles_.end()) {
                return it->second;
            }

            sem_t* handle = sem_open(name, O_CREAT, S_IRUSR | S_IWUSR, 0);
            if (handle == SEM_FAILED) {
                int err = errno;
                throw std::system_error(err, std::system_category(), "sem_open failed");
            }

            handles_.emplace(semaphore, handle);
            return handle;
        }

        ~darwin_handle_registry()
        {
            for (auto& entry : handles_) {
                if (entry.second && entry.second != SEM_FAILED) {
                    sem_close(entry.second);
                }
            }
        }

    private:
        darwin_handle_registry() = default;

        std::mutex mutex_;
        std::unordered_map<const interprocess_semaphore*, sem_t*> handles_;
    };

    static constexpr int kNameUninitialized = 0;
    static constexpr int kNameInitializing  = 1;
    static constexpr int kNameInitialized   = 2;

    mutable std::atomic<int> name_state_{kNameUninitialized};
    static constexpr size_t  kNameLength = 128;
    mutable char             name_[kNameLength]{};
#endif

    std::atomic<int> count_;
};

} // namespace detail
} // namespace sintra

