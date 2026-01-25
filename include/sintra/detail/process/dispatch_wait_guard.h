// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <atomic>
#include <shared_mutex>

namespace sintra {

// Global depth counter avoids TLS access from signal handlers.
inline std::atomic<unsigned int> g_dispatch_critical_depth {0};

class Dispatch_wait_depth_guard {
public:
    Dispatch_wait_depth_guard()
    {
        g_dispatch_critical_depth.fetch_add(1, std::memory_order_relaxed);
    }

    ~Dispatch_wait_depth_guard()
    {
        g_dispatch_critical_depth.fetch_sub(1, std::memory_order_relaxed);
    }

    Dispatch_wait_depth_guard(const Dispatch_wait_depth_guard&) = delete;
    Dispatch_wait_depth_guard& operator=(const Dispatch_wait_depth_guard&) = delete;
};

template <typename LockT>
class Dispatch_lock_guard {
public:
    template <typename MutexT>
    explicit Dispatch_lock_guard(MutexT& mutex) : depth_guard_(), lock_(mutex) {}

    Dispatch_lock_guard(const Dispatch_lock_guard&) = delete;
    Dispatch_lock_guard& operator=(const Dispatch_lock_guard&) = delete;

private:
    Dispatch_wait_depth_guard depth_guard_;
    LockT lock_;
};

inline bool can_wait_for_signal_dispatch()
{
    return g_dispatch_critical_depth.load(std::memory_order_relaxed) == 0;
}

} // namespace sintra
