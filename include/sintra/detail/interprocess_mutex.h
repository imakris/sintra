#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <random>
#include <system_error>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <cwchar>
  #include <mutex>
  #include <unordered_map>
#elif defined(__APPLE__)
  #include <cerrno>
  #include <cstdio>
  #include <fcntl.h>
  #include <mutex>
  #include <semaphore.h>
  #include <unordered_map>
#else
  #include <cerrno>
  #include <pthread.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
#endif

namespace sintra::detail
{
namespace interprocess_mutex_detail
{
    inline std::uint64_t generate_global_identifier()
    {
        static const std::uint64_t process_entropy = [] {
            std::uint64_t value = 0;

#if defined(_WIN32)
            value ^= static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32;
#endif

#if defined(__unix__) || defined(__APPLE__)
            value ^= static_cast<std::uint64_t>(::getpid()) << 32;
#endif

            std::random_device rd;
            value ^= (static_cast<std::uint64_t>(rd()) << 32);
            value ^= static_cast<std::uint64_t>(rd());

            value ^= static_cast<std::uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());

            if (value == 0) {
                value = 0x8000000000000000ULL;
            }

            return value;
        }();

        static std::atomic<std::uint64_t> counter{0};
        return process_entropy + counter.fetch_add(1, std::memory_order_relaxed);
    }

#if defined(_WIN32)
    inline std::mutex& handle_mutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    inline std::unordered_map<std::uint64_t, HANDLE>& handle_map()
    {
        static std::unordered_map<std::uint64_t, HANDLE> map;
        return map;
    }

    inline HANDLE register_handle(std::uint64_t id, HANDLE handle)
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        handle_map()[id] = handle;
        return handle;
    }

    inline HANDLE ensure_handle(std::uint64_t id, const wchar_t* name)
    {
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                return it->second;
            }
        }

        HANDLE handle = ::OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "OpenMutexW");
        }

        std::lock_guard<std::mutex> lock(handle_mutex());
        return handle_map().emplace(id, handle).first->second;
    }

    inline void close_handle(std::uint64_t id)
    {
        HANDLE handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                handle = it->second;
                handle_map().erase(it);
            }
        }

        if (handle) {
            ::CloseHandle(handle);
        }
    }
#elif defined(__APPLE__)
    inline std::mutex& handle_mutex()
    {
        static std::mutex mtx;
        return mtx;
    }

    inline std::unordered_map<std::uint64_t, sem_t*>& handle_map()
    {
        static std::unordered_map<std::uint64_t, sem_t*> map;
        return map;
    }

    inline sem_t* register_handle(std::uint64_t id, sem_t* handle)
    {
        std::lock_guard<std::mutex> lock(handle_mutex());
        handle_map()[id] = handle;
        return handle;
    }

    inline sem_t* ensure_handle(std::uint64_t id, const char* name)
    {
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                return it->second;
            }
        }

        sem_t* sem = sem_open(name, 0);
        if (sem == SEM_FAILED) {
            throw std::system_error(errno, std::generic_category(), "sem_open");
        }

        std::lock_guard<std::mutex> lock(handle_mutex());
        return handle_map().emplace(id, sem).first->second;
    }

    inline void close_handle(std::uint64_t id)
    {
        sem_t* sem = nullptr;
        {
            std::lock_guard<std::mutex> lock(handle_mutex());
            auto it = handle_map().find(id);
            if (it != handle_map().end()) {
                sem = it->second;
                handle_map().erase(it);
            }
        }

        if (sem) {
            while (sem_close(sem) == -1 && errno == EINTR) {
            }
        }
    }
#endif
} // namespace interprocess_mutex_detail

namespace ipc
{

class interprocess_mutex
{
public:
    interprocess_mutex()
    {
#if defined(_WIN32)
        initialise_windows();
#elif defined(__APPLE__)
        initialise_named();
#else
        initialise_posix();
#endif
    }

    interprocess_mutex(const interprocess_mutex&) = delete;
    interprocess_mutex& operator=(const interprocess_mutex&) = delete;

    ~interprocess_mutex() noexcept
    {
#if defined(_WIN32)
        teardown_windows();
#elif defined(__APPLE__)
        teardown_named();
#else
        teardown_posix();
#endif
    }

    void lock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#elif defined(__APPLE__)
        sem_t* sem = named_handle();
        while (sem_wait(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_wait");
        }
#else
        int rc = ::pthread_mutex_lock(&m_mutex);
#if defined(PTHREAD_MUTEX_ROBUST)
        if (rc == EOWNERDEAD) {
            int rc_consistent = ::pthread_mutex_consistent(&m_mutex);
            if (rc_consistent != 0) {
                throw std::system_error(rc_consistent,
                                       std::generic_category(),
                                       "pthread_mutex_consistent");
            }
            return;
        }
#endif
        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_lock");
        }
#endif
    }

    bool try_lock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        DWORD result = ::WaitForSingleObject(handle, 0);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#elif defined(__APPLE__)
        sem_t* sem = named_handle();
        while (sem_trywait(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_trywait");
        }
        return true;
#else
        int rc = ::pthread_mutex_trylock(&m_mutex);
#if defined(PTHREAD_MUTEX_ROBUST)
        if (rc == EOWNERDEAD) {
            int rc_consistent = ::pthread_mutex_consistent(&m_mutex);
            if (rc_consistent != 0) {
                throw std::system_error(rc_consistent,
                                       std::generic_category(),
                                       "pthread_mutex_consistent");
            }
            return true;
        }
#endif
        if (rc == EBUSY) {
            return false;
        }
        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_trylock");
        }
        return true;
#endif
    }

    template <typename Clock, typename Duration>
    bool timed_lock(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        auto now = Clock::now();
        if (abs_time <= now) {
            return try_lock();
        }
        auto delta = abs_time - now;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
        if (ms.count() <= 0) {
            return try_lock();
        }
        constexpr std::uint64_t infinite_value = static_cast<std::uint64_t>(INFINITE);
        std::uint64_t clamped = static_cast<std::uint64_t>(ms.count());
        if (clamped >= infinite_value) {
            clamped = infinite_value - 1;
        }
        DWORD timeout = static_cast<DWORD>(clamped);
        DWORD result = ::WaitForSingleObject(handle, timeout);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
            return true;
        }
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        throw std::system_error(::GetLastError(), std::system_category(), "WaitForSingleObject");
#elif defined(__APPLE__)
        sem_t* sem = named_handle();
        auto ts = make_abs_timespec(abs_time);
        while (sem_timedwait(sem, &ts) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ETIMEDOUT) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "sem_timedwait");
        }
        return true;
#else
        auto ts = make_abs_timespec(abs_time);
        int rc = ::pthread_mutex_timedlock(&m_mutex, &ts);
#if defined(PTHREAD_MUTEX_ROBUST)
        if (rc == EOWNERDEAD) {
            int rc_consistent = ::pthread_mutex_consistent(&m_mutex);
            if (rc_consistent != 0) {
                throw std::system_error(rc_consistent,
                                       std::generic_category(),
                                       "pthread_mutex_consistent");
            }
            return true;
        }
#endif
        if (rc == 0) {
            return true;
        }
        if (rc == ETIMEDOUT) {
            return false;
        }
        throw std::system_error(rc, std::generic_category(), "pthread_mutex_timedlock");
#endif
    }

    void unlock()
    {
#if defined(_WIN32)
        HANDLE handle = windows_handle();
        if (!::ReleaseMutex(handle)) {
            throw std::system_error(::GetLastError(), std::system_category(), "ReleaseMutex");
        }
#elif defined(__APPLE__)
        sem_t* sem = named_handle();
        while (sem_post(sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "sem_post");
        }
#else
        int rc = ::pthread_mutex_unlock(&m_mutex);
        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_unlock");
        }
#endif
    }

private:
#if !defined(_WIN32)
    template <typename Clock, typename Duration>
    static timespec make_abs_timespec(const std::chrono::time_point<Clock, Duration>& abs_time)
    {
        using namespace std::chrono;
        auto now = Clock::now();
        auto delta = abs_time > now ? abs_time - now : Clock::duration::zero();
        auto sys_time = std::chrono::system_clock::now() +
                        duration_cast<std::chrono::system_clock::duration>(delta);
        auto ns = duration_cast<std::chrono::nanoseconds>(sys_time.time_since_epoch());
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(ns.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(ns.count() % 1000000000);
        return ts;
    }
#endif

#if defined(_WIN32)
    struct windows_storage
    {
        std::uint64_t id = 0;
        wchar_t       name[64]{};
    };

    windows_storage m_windows{};

    void initialise_windows()
    {
        m_windows.id = interprocess_mutex_detail::generate_global_identifier();
        std::swprintf(m_windows.name,
                      sizeof(m_windows.name) / sizeof(m_windows.name[0]),
                      L"SintraMutex_%016llX",
                      static_cast<unsigned long long>(m_windows.id));

        HANDLE handle = ::CreateMutexW(nullptr, FALSE, m_windows.name);
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateMutexW");
        }

        interprocess_mutex_detail::register_handle(m_windows.id, handle);
    }

    HANDLE windows_handle() const
    {
        return interprocess_mutex_detail::ensure_handle(m_windows.id, m_windows.name);
    }

    void teardown_windows() noexcept
    {
        interprocess_mutex_detail::close_handle(m_windows.id);
    }
#elif defined(__APPLE__)
    struct named_storage
    {
        std::uint64_t id = 0;
        char          name[32]{};
    };

    named_storage m_named{};

    void initialise_named()
    {
        m_named.id = interprocess_mutex_detail::generate_global_identifier();
        std::snprintf(m_named.name,
                      sizeof(m_named.name) / sizeof(m_named.name[0]),
                      "/sintra_mutex_%016llx",
                      static_cast<unsigned long long>(m_named.id));

        sem_unlink(m_named.name);
        sem_t* sem = sem_open(m_named.name, O_CREAT | O_EXCL, 0600, 1);
        if (sem == SEM_FAILED) {
            throw std::system_error(errno, std::generic_category(), "sem_open");
        }

        interprocess_mutex_detail::register_handle(m_named.id, sem);
    }

    sem_t* named_handle() const
    {
        return interprocess_mutex_detail::ensure_handle(m_named.id, m_named.name);
    }

    void teardown_named() noexcept
    {
        interprocess_mutex_detail::close_handle(m_named.id);
        if (m_named.name[0] != '\0') {
            sem_unlink(m_named.name);
        }
    }
#else
    pthread_mutex_t m_mutex{};

    void initialise_posix()
    {
        pthread_mutexattr_t attr;
        int rc = ::pthread_mutexattr_init(&attr);
        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutexattr_init");
        }

        rc = ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc != 0) {
            ::pthread_mutexattr_destroy(&attr);
            throw std::system_error(rc, std::generic_category(), "pthread_mutexattr_setpshared");
        }

#if defined(PTHREAD_MUTEX_ROBUST)
        rc = ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (rc != 0 && rc != ENOTSUP && rc != EINVAL) {
            ::pthread_mutexattr_destroy(&attr);
            throw std::system_error(rc, std::generic_category(), "pthread_mutexattr_setrobust");
        }
#endif

        rc = ::pthread_mutex_init(&m_mutex, &attr);
        ::pthread_mutexattr_destroy(&attr);
        if (rc != 0) {
            throw std::system_error(rc, std::generic_category(), "pthread_mutex_init");
        }
    }

    void teardown_posix() noexcept
    {
        ::pthread_mutex_destroy(&m_mutex);
    }
#endif
};

} // namespace ipc
} // namespace sintra::detail

