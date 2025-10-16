#pragma once

#include <atomic>

namespace sintra::ipc_atomic {

// Release-store helper for interprocess/shared control blocks.
// Accepts std::atomic values to provide a consistent API even
// when atomic_ref is unavailable on the target standard library.
// (sintra currently targets C++17.)

template <class T>
inline void store_release(std::atomic<T>& location, T value) noexcept {
    location.store(value, std::memory_order_release);
}

template <class T>
inline T load_acquire(const std::atomic<T>& location) noexcept {
    return location.load(std::memory_order_acquire);
}

template <class T>
inline T fetch_add_release(std::atomic<T>& location, T delta = 1) noexcept {
    return location.fetch_add(delta, std::memory_order_release);
}

} // namespace sintra::ipc_atomic
