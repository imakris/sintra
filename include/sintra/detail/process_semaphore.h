#pragma once

#include <atomic>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <array>
#    include <mutex>
#    include <string>
#    include <unordered_map>
#elif defined(__linux__)
#    include <semaphore.h>
#    include <errno.h>
#elif defined(__APPLE__)
#    include <array>
#    include <cerrno>
#    include <fcntl.h>
#    include <mutex>
#    include <semaphore.h>
#    include <string>
#    include <sys/stat.h>
#endif

namespace sintra::detail {

class process_semaphore
{
public:
    explicit process_semaphore(unsigned int initial = 0);
    ~process_semaphore();

    process_semaphore(const process_semaphore&)            = delete;
    process_semaphore& operator=(const process_semaphore&) = delete;

    process_semaphore(process_semaphore&&)            = delete;
    process_semaphore& operator=(process_semaphore&&) = delete;

    void post();
    void wait();
    bool try_wait();

private:
#if defined(_WIN32)
    using name_storage = std::array<wchar_t, 64>;

    HANDLE acquire_handle() const;
    void   ensure_created(unsigned int initial);

    enum class state : uint32_t
    {
        uninitialized = 0,
        initializing,
        ready
    };

    mutable std::atomic<uint32_t> m_state{static_cast<uint32_t>(state::uninitialized)};
    unsigned int                  m_initial{0};
    name_storage                  m_name{};

#elif defined(__linux__)
    sem_t m_sem;

#elif defined(__APPLE__)
    using name_storage = std::array<char, 64>;

    enum class state : uint32_t
    {
        uninitialized = 0,
        initializing,
        ready
    };

    void ensure_created(unsigned int initial);
    sem_t* acquire_handle() const;

    mutable std::atomic<uint32_t> m_state{static_cast<uint32_t>(state::uninitialized)};
    unsigned int                  m_initial{0};
    name_storage                  m_name{};
#else
#    error "process_semaphore is not implemented for this platform"
#endif
};

#if defined(_WIN32)

namespace ps_detail
{
struct win32_handle_cache
{
    static win32_handle_cache& instance()
    {
        static win32_handle_cache cache;
        return cache;
    }

    HANDLE acquire(const wchar_t* name, unsigned int initial)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto                        key = std::wstring{name};
        auto                        it  = m_handles.find(key);
        if (it != m_handles.end()) {
            return it->second;
        }

        HANDLE handle = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name);
        if (!handle) {
            handle = CreateSemaphoreW(nullptr,
                                      static_cast<LONG>(initial),
                                      LONG_MAX,
                                      name);
        }

        if (!handle) {
            throw std::runtime_error("Failed to create or open Win32 semaphore");
        }

        m_handles.emplace(std::move(key), handle);
        return handle;
    }

    ~win32_handle_cache()
    {
        for (auto& entry : m_handles) {
            CloseHandle(entry.second);
        }
    }

private:
    std::mutex                               m_mutex;
    std::unordered_map<std::wstring, HANDLE> m_handles;
};
} // namespace ps_detail

inline void process_semaphore::ensure_created(unsigned int initial)
{
    uint32_t expected = static_cast<uint32_t>(state::uninitialized);
    if (m_state.compare_exchange_strong(expected, static_cast<uint32_t>(state::initializing),
                                        std::memory_order_acq_rel)) {
        m_initial = initial;

        constexpr wchar_t alphabet[] = L"0123456789abcdef";
        std::random_device rd;
        std::uniform_int_distribution<int> dist(0, 15);

        m_name.fill(L'\0');
        std::wstring generated = L"Local\\sintra_sem_";
        for (size_t i = 0; i < 32; ++i) {
            generated.push_back(alphabet[dist(rd)]);
        }
        generated.copy(m_name.data(), m_name.size() - 1);

        HANDLE handle = CreateSemaphoreW(nullptr,
                                         static_cast<LONG>(initial),
                                         LONG_MAX,
                                         m_name.data());
        if (!handle) {
            m_state.store(static_cast<uint32_t>(state::uninitialized), std::memory_order_release);
            throw std::runtime_error("Failed to create Win32 semaphore");
        }
        CloseHandle(handle);

        m_state.store(static_cast<uint32_t>(state::ready), std::memory_order_release);
    }
    else {
        while (m_state.load(std::memory_order_acquire) != static_cast<uint32_t>(state::ready)) {
            std::this_thread::yield();
        }
    }
}

inline HANDLE process_semaphore::acquire_handle() const
{
    if (m_state.load(std::memory_order_acquire) != static_cast<uint32_t>(state::ready)) {
        throw std::logic_error("Semaphore not initialized");
    }
    return ps_detail::win32_handle_cache::instance().acquire(m_name.data(), m_initial);
}

inline process_semaphore::process_semaphore(unsigned int initial)
{
    ensure_created(initial);
}

inline process_semaphore::~process_semaphore() = default;

inline void process_semaphore::post()
{
    HANDLE handle = acquire_handle();
    if (!ReleaseSemaphore(handle, 1, nullptr)) {
        throw std::runtime_error("Failed to release Win32 semaphore");
    }
}

inline void process_semaphore::wait()
{
    HANDLE handle = acquire_handle();
    DWORD  result = WaitForSingleObject(handle, INFINITE);
    if (result != WAIT_OBJECT_0) {
        throw std::runtime_error("Failed to wait on Win32 semaphore");
    }
}

inline bool process_semaphore::try_wait()
{
    HANDLE handle = acquire_handle();
    DWORD  result = WaitForSingleObject(handle, 0);
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        return false;
    }
    throw std::runtime_error("Failed to try_wait on Win32 semaphore");
}

#elif defined(__linux__)

inline process_semaphore::process_semaphore(unsigned int initial)
{
    if (sem_init(&m_sem, 1, initial) != 0) {
        throw std::runtime_error("Failed to initialize POSIX semaphore");
    }
}

inline process_semaphore::~process_semaphore()
{
    sem_destroy(&m_sem);
}

inline void process_semaphore::post()
{
    if (sem_post(&m_sem) != 0) {
        throw std::runtime_error("Failed to post POSIX semaphore");
    }
}

inline void process_semaphore::wait()
{
    while (sem_wait(&m_sem) != 0) {
        if (errno != EINTR) {
            throw std::runtime_error("Failed to wait on POSIX semaphore");
        }
    }
}

inline bool process_semaphore::try_wait()
{
    while (sem_trywait(&m_sem) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error("Failed to try_wait on POSIX semaphore");
    }
    return true;
}

#elif defined(__APPLE__)

namespace ps_detail
{
struct named_sem_cache
{
    static named_sem_cache& instance()
    {
        static named_sem_cache cache;
        return cache;
    }

    sem_t* acquire(const char* name, unsigned int initial)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto                        key = std::string{name};
        auto                        it  = m_handles.find(key);
        if (it != m_handles.end()) {
            return it->second;
        }

        sem_t* handle = sem_open(name, 0);
        if (handle == SEM_FAILED && errno == ENOENT) {
            handle = sem_open(name, O_CREAT, S_IRUSR | S_IWUSR, initial);
        }

        if (handle == SEM_FAILED) {
            throw std::runtime_error("Failed to create or open named semaphore");
        }

        m_handles.emplace(std::move(key), handle);
        return handle;
    }

    ~named_sem_cache()
    {
        for (auto& entry : m_handles) {
            sem_close(entry.second);
        }
    }

private:
    std::mutex                              m_mutex;
    std::unordered_map<std::string, sem_t*> m_handles;
};
} // namespace ps_detail

inline void process_semaphore::ensure_created(unsigned int initial)
{
    uint32_t expected = static_cast<uint32_t>(state::uninitialized);
    if (m_state.compare_exchange_strong(expected, static_cast<uint32_t>(state::initializing),
                                        std::memory_order_acq_rel)) {
        m_initial = initial;

        constexpr char alphabet[] = "0123456789abcdef";
        std::random_device         rd;
        std::uniform_int_distribution<int> dist(0, 15);

        m_name.fill('\0');
        std::string generated = "/sintra_sem_";
        for (size_t i = 0; i < 32; ++i) {
            generated.push_back(alphabet[dist(rd)]);
        }

        generated.copy(m_name.data(), m_name.size() - 1);

        sem_t* handle = sem_open(m_name.data(), O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, initial);
        if (handle == SEM_FAILED) {
            if (errno == EEXIST) {
                handle = sem_open(m_name.data(), 0);
            }
        }

        if (handle == SEM_FAILED) {
            m_state.store(static_cast<uint32_t>(state::uninitialized), std::memory_order_release);
            throw std::runtime_error("Failed to create named semaphore");
        }

        sem_close(handle);
        m_state.store(static_cast<uint32_t>(state::ready), std::memory_order_release);
    }
    else {
        while (m_state.load(std::memory_order_acquire) != static_cast<uint32_t>(state::ready)) {
            std::this_thread::yield();
        }
    }
}

inline sem_t* process_semaphore::acquire_handle() const
{
    if (m_state.load(std::memory_order_acquire) != static_cast<uint32_t>(state::ready)) {
        throw std::logic_error("Semaphore not initialized");
    }
    return ps_detail::named_sem_cache::instance().acquire(m_name.data(), m_initial);
}

inline process_semaphore::process_semaphore(unsigned int initial)
{
    ensure_created(initial);
}

inline process_semaphore::~process_semaphore()
{
    if (m_state.load(std::memory_order_acquire) == static_cast<uint32_t>(state::ready)) {
        sem_unlink(m_name.data());
    }
}

inline void process_semaphore::post()
{
    sem_t* handle = acquire_handle();
    if (sem_post(handle) != 0) {
        throw std::runtime_error("Failed to post named semaphore");
    }
}

inline void process_semaphore::wait()
{
    sem_t* handle = acquire_handle();
    while (sem_wait(handle) != 0) {
        if (errno != EINTR) {
            throw std::runtime_error("Failed to wait on named semaphore");
        }
    }
}

inline bool process_semaphore::try_wait()
{
    sem_t* handle = acquire_handle();
    while (sem_trywait(handle) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return false;
        }
        throw std::runtime_error("Failed to try_wait on named semaphore");
    }
    return true;
}

#else
#    error "process_semaphore is not implemented for this platform"
#endif

} // namespace sintra::detail

