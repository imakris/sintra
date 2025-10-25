#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <climits>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <cerrno>
  #include <linux/futex.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <fcntl.h>
  #include <semaphore.h>
  #include <sys/stat.h>
  #include <unistd.h>
#elif defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <cwchar>
  #include <limits>
#else
  #error "interprocess_semaphore is not implemented for this platform"
#endif

namespace sintra {

namespace detail {

#if defined(__linux__)
inline int futex_wait(std::atomic<int>* addr, int expected, const struct timespec* ts)
{
    return syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAIT_PRIVATE, expected, ts, nullptr, 0);
}

inline int futex_wake(std::atomic<int>* addr, int count)
{
    return syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}
#endif

#if defined(__APPLE__)
class posix_handle_registry {
public:
    ~posix_handle_registry()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.handle != SEM_FAILED) {
                sem_close(entry.handle);
            }
        }
    }

    void adopt(const void* owner, sem_t* handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back({owner, handle});
    }

    sem_t* ensure(const void* owner, const char* name)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& entry : m_entries) {
                if (entry.owner == owner) {
                    return entry.handle;
                }
            }
        }

        sem_t* opened = sem_open(name, 0);
        if (opened == SEM_FAILED) {
            throw std::system_error(errno, std::generic_category(), "sem_open");
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.owner == owner) {
                sem_close(opened);
                return entry.handle;
            }
        }
        m_entries.push_back({owner, opened});
        return opened;
    }

    void release(const void* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->owner == owner) {
                if (it->handle != SEM_FAILED) {
                    sem_close(it->handle);
                }
                m_entries.erase(it);
                break;
            }
        }
    }

    static posix_handle_registry& instance()
    {
        static posix_handle_registry registry;
        return registry;
    }

private:
    struct entry_t {
        const void* owner;
        sem_t*      handle;
    };

    std::mutex            m_mutex;
    std::vector<entry_t>  m_entries;
};
#endif

#if defined(_WIN32)
class win32_handle_registry {
public:
    ~win32_handle_registry()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.handle) {
                CloseHandle(entry.handle);
            }
        }
    }

    void adopt(const void* owner, HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back({owner, handle});
    }

    HANDLE ensure(const void* owner, LPCWSTR name)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& entry : m_entries) {
                if (entry.owner == owner) {
                    return entry.handle;
                }
            }
        }

        HANDLE opened = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
        if (!opened) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "OpenSemaphoreW");
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.owner == owner) {
                CloseHandle(opened);
                return entry.handle;
            }
        }
        m_entries.push_back({owner, opened});
        return opened;
    }

    void release(const void* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->owner == owner) {
                if (it->handle) {
                    CloseHandle(it->handle);
                }
                m_entries.erase(it);
                break;
            }
        }
    }

    static win32_handle_registry& instance()
    {
        static win32_handle_registry registry;
        return registry;
    }

private:
    struct entry_t {
        const void* owner;
        HANDLE      handle;
    };

    std::mutex            m_mutex;
    std::vector<entry_t>  m_entries;
};
#endif

} // namespace detail

class interprocess_semaphore {
public:
    explicit interprocess_semaphore(unsigned int initialCount = 0);
    ~interprocess_semaphore();

    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;

    void post();
    void wait();
    bool try_wait();
    template <typename Clock, typename Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        return m_impl.timed_wait(abs_time);
    }

private:
#if defined(__linux__)
    class futex_semaphore {
    public:
        explicit futex_semaphore(unsigned int initialCount)
            : m_count(static_cast<int>(initialCount))
        {
        }

        void post()
        {
            m_count.fetch_add(1, std::memory_order_release);
            detail::futex_wake(&m_count, 1);
        }

        void wait()
        {
            while (true) {
                int value = m_count.load(std::memory_order_acquire);
                while (value > 0) {
                    if (m_count.compare_exchange_weak(value, value - 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                        return;
                    }
                }

                if (detail::futex_wait(&m_count, 0, nullptr) == -1) {
                    if (errno == EAGAIN || errno == EINTR) {
                        continue;
                    }
                    throw std::system_error(errno, std::generic_category(), "futex_wait");
                }
            }
        }

        bool try_wait()
        {
            int value = m_count.load(std::memory_order_acquire);
            while (value > 0) {
                if (m_count.compare_exchange_weak(value, value - 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    return true;
                }
            }
            return false;
        }

        template <typename Clock, typename Duration>
        bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
        {
            if (try_wait()) {
                return true;
            }

            while (true) {
                auto now = Clock::now();
                if (now >= abs_time) {
                    return false;
                }

                int value = m_count.load(std::memory_order_acquire);
                while (value > 0) {
                    if (m_count.compare_exchange_weak(value, value - 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                        return true;
                    }
                }

                auto remaining = abs_time - now;
                auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
                if (nanos.count() < 0) {
                    nanos = std::chrono::nanoseconds::zero();
                }

                struct timespec ts;
                ts.tv_sec  = static_cast<time_t>(nanos.count() / 1000000000LL);
                ts.tv_nsec = static_cast<long>(nanos.count() % 1000000000LL);

                if (detail::futex_wait(&m_count, 0, &ts) == -1) {
                    if (errno == ETIMEDOUT) {
                        return false;
                    }
                    if (errno == EAGAIN || errno == EINTR) {
                        continue;
                    }
                    throw std::system_error(errno, std::generic_category(), "futex_wait");
                }
            }
        }

    private:
        std::atomic<int> m_count;
    };

    futex_semaphore m_impl;
#elif defined(__APPLE__)
    class posix_semaphore {
    public:
        explicit posix_semaphore(unsigned int initialCount)
            : m_state(k_uninitialized)
            , m_initial(initialCount)
        {
            initialize();
        }

        ~posix_semaphore()
        {
            detail::posix_handle_registry::instance().release(this);
            sem_unlink(m_name);
        }

        void post()
        {
            sem_t* handle = get_handle();
            if (sem_post(handle) != 0) {
                throw std::system_error(errno, std::generic_category(), "sem_post");
            }
        }

        void wait()
        {
            sem_t* handle = get_handle();
            while (sem_wait(handle) != 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "sem_wait");
            }
        }

        bool try_wait()
        {
            sem_t* handle = get_handle();
            while (sem_trywait(handle) != 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN) {
                    return false;
                }
                throw std::system_error(errno, std::generic_category(), "sem_trywait");
            }
            return true;
        }

        template <typename Clock, typename Duration>
        bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
        {
            if (try_wait()) {
                return true;
            }

            auto now = Clock::now();
            if (now >= abs_time) {
                return false;
            }

            auto remaining = abs_time - now;
            auto system_deadline = std::chrono::system_clock::now() +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);

            struct timespec ts;
            auto secs = std::chrono::time_point_cast<std::chrono::seconds>(system_deadline);
            ts.tv_sec = static_cast<time_t>(secs.time_since_epoch().count());
            auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(system_deadline - secs);
            ts.tv_nsec = static_cast<long>(nsecs.count());

            sem_t* handle = get_handle();
            while (sem_timedwait(handle, &ts) != 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ETIMEDOUT) {
                    return false;
                }
                throw std::system_error(errno, std::generic_category(), "sem_timedwait");
            }
            return true;
        }

    private:
        void initialize()
        {
            m_state.store(k_initializing, std::memory_order_relaxed);

            static std::atomic<uint64_t> counter{0};
            uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
            pid_t pid = getpid();
            int written = std::snprintf(
                m_name,
                sizeof(m_name),
                "/sintra_sem_%d_%llu_%p",
                static_cast<int>(pid),
                static_cast<unsigned long long>(id),
                static_cast<const void*>(this));
            if (written <= 0 || static_cast<size_t>(written) >= sizeof(m_name)) {
                throw std::runtime_error("Failed to generate semaphore name");
            }

            sem_t* created = sem_open(m_name, O_CREAT | O_EXCL, 0666, m_initial);
            if (created == SEM_FAILED) {
                throw std::system_error(errno, std::generic_category(), "sem_open");
            }

            detail::posix_handle_registry::instance().adopt(this, created);
            m_state.store(k_ready, std::memory_order_release);
        }

        void ensure_ready() const
        {
            while (m_state.load(std::memory_order_acquire) != k_ready) {
                std::this_thread::yield();
            }
        }

        sem_t* get_handle() const
        {
            ensure_ready();
            return detail::posix_handle_registry::instance().ensure(this, m_name);
        }

        static constexpr uint32_t k_uninitialized = 0;
        static constexpr uint32_t k_initializing  = 1;
        static constexpr uint32_t k_ready         = 2;

        mutable std::atomic<uint32_t> m_state;
        unsigned int                   m_initial;
        char                           m_name[64]{};
    };

    posix_semaphore m_impl;
#elif defined(_WIN32)
    class win32_semaphore {
    public:
        explicit win32_semaphore(unsigned int initialCount)
            : m_state(k_uninitialized)
            , m_initial(initialCount)
        {
            initialize();
        }

        ~win32_semaphore()
        {
            detail::win32_handle_registry::instance().release(this);
        }

        void post()
        {
            HANDLE handle = get_handle();
            if (!ReleaseSemaphore(handle, 1, nullptr)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "ReleaseSemaphore");
            }
        }

        void wait()
        {
            HANDLE handle = get_handle();
            DWORD result = WaitForSingleObject(handle, INFINITE);
            if (result != WAIT_OBJECT_0) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "WaitForSingleObject");
            }
        }

        bool try_wait()
        {
            HANDLE handle = get_handle();
            DWORD result = WaitForSingleObject(handle, 0);
            if (result == WAIT_OBJECT_0) {
                return true;
            }
            if (result == WAIT_TIMEOUT) {
                return false;
            }
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "WaitForSingleObject");
        }

        template <typename Clock, typename Duration>
        bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time)
        {
            if (try_wait()) {
                return true;
            }

            auto now = Clock::now();
            if (now >= abs_time) {
                return false;
            }

            auto remaining = abs_time - now;
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            if (millis.count() < 0) {
                millis = std::chrono::milliseconds::zero();
            }

            DWORD timeout = static_cast<DWORD>(std::min<int64_t>(millis.count(), static_cast<int64_t>(INFINITE - 1)));
            HANDLE handle = get_handle();
            DWORD result = WaitForSingleObject(handle, timeout);
            if (result == WAIT_OBJECT_0) {
                return true;
            }
            if (result == WAIT_TIMEOUT) {
                return false;
            }
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "WaitForSingleObject");
        }

    private:
        void initialize()
        {
            m_state.store(k_initializing, std::memory_order_relaxed);

            static std::atomic<uint64_t> counter{0};
            uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
            DWORD pid = GetCurrentProcessId();
            int written = _snwprintf(
                m_name,
                sizeof(m_name) / sizeof(m_name[0]),
                L"Global\\sintra_sem_%lu_%llu_%p",
                static_cast<unsigned long>(pid),
                static_cast<unsigned long long>(id),
                this);
            if (written <= 0 || static_cast<size_t>(written) >= (sizeof(m_name) / sizeof(m_name[0]))) {
                throw std::runtime_error("Failed to generate semaphore name");
            }
            m_name[sizeof(m_name) / sizeof(m_name[0]) - 1] = L'\0';

            HANDLE created = CreateSemaphoreW(nullptr, static_cast<LONG>(m_initial), LONG_MAX, m_name);
            if (!created) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateSemaphoreW");
            }

            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(created);
                throw std::runtime_error("Semaphore name collision");
            }

            detail::win32_handle_registry::instance().adopt(this, created);
            m_state.store(k_ready, std::memory_order_release);
        }

        void ensure_ready() const
        {
            while (m_state.load(std::memory_order_acquire) != k_ready) {
                std::this_thread::yield();
            }
        }

        HANDLE get_handle() const
        {
            ensure_ready();
            return detail::win32_handle_registry::instance().ensure(this, m_name);
        }

        static constexpr uint32_t k_uninitialized = 0;
        static constexpr uint32_t k_initializing  = 1;
        static constexpr uint32_t k_ready         = 2;

        mutable std::atomic<uint32_t> m_state;
        unsigned int                   m_initial;
        wchar_t                        m_name[64]{};
    };

    win32_semaphore m_impl;
#endif
};

inline interprocess_semaphore::interprocess_semaphore(unsigned int initialCount)
#if defined(__linux__)
    : m_impl(initialCount)
#elif defined(__APPLE__)
    : m_impl(initialCount)
#elif defined(_WIN32)
    : m_impl(initialCount)
#endif
{
}

inline interprocess_semaphore::~interprocess_semaphore() = default;

inline void interprocess_semaphore::post()
{
    m_impl.post();
}

inline void interprocess_semaphore::wait()
{
    m_impl.wait();
}

inline bool interprocess_semaphore::try_wait()
{
    return m_impl.try_wait();
}

} // namespace sintra

