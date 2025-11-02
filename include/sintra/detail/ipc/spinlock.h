// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>

namespace sintra {

struct spinlock
{
    struct locker
    {
        locker(spinlock& sl): m_sl(sl) { m_sl.lock();   }
        ~locker()                      { m_sl.unlock(); }
        locker(const locker&) = delete;
        locker& operator=(const locker&) = delete;
        locker(locker&&) = delete;
        locker& operator=(locker&&) = delete;
        spinlock& m_sl;
    };

    void lock()
    {
        while (m_locked.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() { m_locked.clear(std::memory_order_release);                  }

private:
    std::atomic_flag m_locked = ATOMIC_FLAG_INIT;
};

} // namespace sintra

