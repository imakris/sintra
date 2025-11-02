#pragma once

#include "interprocess_mutex.h"
#include <cassert>
#include <chrono>
#include <system_error>
#include <utility>

namespace sintra { namespace detail {

// Helper to throw operation_not_permitted
[[noreturn]] inline void throw_op_not_permitted(const char* msg) {
    throw std::system_error(
        std::make_error_code(std::errc::operation_not_permitted), msg);
}

// Tag types for constructor overloading
struct defer_lock_t { explicit defer_lock_t() = default; };
struct try_to_lock_t { explicit try_to_lock_t() = default; };
struct adopt_lock_t { explicit adopt_lock_t() = default; };

inline constexpr defer_lock_t defer_lock{};
inline constexpr try_to_lock_t try_to_lock{};
inline constexpr adopt_lock_t adopt_lock{};

template <typename Mutex>
class interprocess_unique_lock
{
public:
    using mutex_type = Mutex;

    // Default constructor (no mutex)
    interprocess_unique_lock() noexcept
        : m_mtx(nullptr)
        , m_owns(false)
    {
    }

    // Lock immediately
    explicit interprocess_unique_lock(mutex_type& mtx)
        : m_mtx(&mtx)
        , m_owns(false)
    {
        lock();
    }

    // Defer lock
    interprocess_unique_lock(mutex_type& mtx, defer_lock_t) noexcept
        : m_mtx(&mtx)
        , m_owns(false)
    {
    }

    // Try to lock
    interprocess_unique_lock(mutex_type& mtx, try_to_lock_t)
        : m_mtx(&mtx)
        , m_owns(false)
    {
        m_owns = m_mtx->try_lock();
    }

    // Adopt existing lock
    interprocess_unique_lock(mutex_type& mtx, adopt_lock_t) noexcept
        : m_mtx(&mtx)
        , m_owns(true)
    {
    }

    // Timed lock with duration
    template <class Rep, class Period>
    interprocess_unique_lock(mutex_type& mtx,
                            const std::chrono::duration<Rep, Period>& timeout_duration)
        : m_mtx(&mtx)
        , m_owns(false)
    {
        m_owns = m_mtx->try_lock_for(timeout_duration);
    }

    // Timed lock with time_point
    template <class Clock, class Duration>
    interprocess_unique_lock(mutex_type& mtx,
                            const std::chrono::time_point<Clock, Duration>& timeout_time)
        : m_mtx(&mtx)
        , m_owns(false)
    {
        m_owns = m_mtx->try_lock_until(timeout_time);
    }

    // Destructor
    ~interprocess_unique_lock() {
        if (m_owns) {
            m_mtx->unlock();
        }
    }

    // Move constructor
    interprocess_unique_lock(interprocess_unique_lock&& other) noexcept
        : m_mtx(other.m_mtx)
        , m_owns(other.m_owns)
    {
        other.m_mtx = nullptr;
        other.m_owns = false;
    }

    // Move assignment
    interprocess_unique_lock& operator=(interprocess_unique_lock&& other) noexcept {
        if (this != &other) {
            if (m_owns) {
                m_mtx->unlock();
            }
            m_mtx = other.m_mtx;
            m_owns = other.m_owns;
            other.m_mtx = nullptr;
            other.m_owns = false;
        }
        return *this;
    }

    // Delete copy
    interprocess_unique_lock(const interprocess_unique_lock&) = delete;
    interprocess_unique_lock& operator=(const interprocess_unique_lock&) = delete;

    // Lock
    void lock() {
        assert(m_mtx && "interprocess_unique_lock::lock: null mutex");
        if (!m_mtx) {
            throw_op_not_permitted("interprocess_unique_lock::lock: null mutex");
        }
        if (m_owns) {
            throw_op_not_permitted("interprocess_unique_lock::lock: already owns");
        }
        m_mtx->lock();
        m_owns = true;
    }

    // Try lock
    bool try_lock() {
        assert(m_mtx && "interprocess_unique_lock::try_lock: null mutex");
        if (!m_mtx) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock: null mutex");
        }
        if (m_owns) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock: already owns");
        }
        m_owns = m_mtx->try_lock();
        return m_owns;
    }

    // Try lock for duration
    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout_duration) {
        assert(m_mtx && "interprocess_unique_lock::try_lock_for: null mutex");
        if (!m_mtx) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock_for: null mutex");
        }
        if (m_owns) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock_for: already owns");
        }
        m_owns = m_mtx->try_lock_for(timeout_duration);
        return m_owns;
    }

    // Try lock until time_point
    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& timeout_time) {
        assert(m_mtx && "interprocess_unique_lock::try_lock_until: null mutex");
        if (!m_mtx) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock_until: null mutex");
        }
        if (m_owns) {
            throw_op_not_permitted("interprocess_unique_lock::try_lock_until: already owns");
        }
        m_owns = m_mtx->try_lock_until(timeout_time);
        return m_owns;
    }

    // Unlock
    void unlock() {
        assert(m_owns && "interprocess_unique_lock::unlock: not locked");
        if (!m_owns) {
            throw_op_not_permitted("interprocess_unique_lock::unlock: not locked");
        }
        assert(m_mtx && "interprocess_unique_lock::unlock: null mutex");
        if (!m_mtx) {
            throw_op_not_permitted("interprocess_unique_lock::unlock: null mutex");
        }
        m_mtx->unlock();
        m_owns = false;
    }

    // Swap
    void swap(interprocess_unique_lock& other) noexcept {
        std::swap(m_mtx, other.m_mtx);
        std::swap(m_owns, other.m_owns);
    }

    // Release
    mutex_type* release() noexcept {
        mutex_type* ret = m_mtx;
        m_mtx = nullptr;
        m_owns = false;
        return ret;
    }

    // Observers
    bool owns_lock() const noexcept { return m_owns; }
    explicit operator bool() const noexcept { return m_owns; }
    mutex_type* mutex() const noexcept { return m_mtx; }

private:
    mutex_type* m_mtx;
    bool m_owns;
};

// Non-member swap
template <typename Mutex>
void swap(interprocess_unique_lock<Mutex>& lhs,
          interprocess_unique_lock<Mutex>& rhs) noexcept {
    lhs.swap(rhs);
}

}} // namespace sintra::detail
