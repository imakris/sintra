// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <shared_mutex>

#ifdef _WIN32
#include "detail/sintra_windows.h"
#include <synchapi.h>
#endif

namespace sintra {

#ifdef _WIN32

class shared_mutex
{
public:
    using native_handle_type = PSRWLOCK;

    shared_mutex() = default;
    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;

    void lock() { AcquireSRWLockExclusive(&m_lock); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&m_lock) != FALSE; }
    void unlock() { ReleaseSRWLockExclusive(&m_lock); }

    void lock_shared() { AcquireSRWLockShared(&m_lock); }
    bool try_lock_shared() { return TryAcquireSRWLockShared(&m_lock) != FALSE; }
    void unlock_shared() { ReleaseSRWLockShared(&m_lock); }

    native_handle_type native_handle() { return &m_lock; }

private:
    SRWLOCK m_lock = SRWLOCK_INIT;
};

#else

using shared_mutex = std::shared_mutex;

#endif

} // namespace sintra
