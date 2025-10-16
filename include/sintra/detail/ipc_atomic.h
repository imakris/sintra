#pragma once

#include <atomic>

namespace sintra::ipc {

// Release store through an atomic_ref wrapper. This provides cross-process
// visibility without changing the underlying storage type.
template <class T>
inline void store_release(T& location, T value)
{
    std::atomic_ref<T> ref(location);
    ref.store(value, std::memory_order_release);
}

// Acquire load through an atomic_ref wrapper.
template <class T>
inline T load_acquire(const T& location)
{
    std::atomic_ref<const T> ref(location);
    return ref.load(std::memory_order_acquire);
}

// Fetch-add with release semantics via atomic_ref.
template <class T>
inline T fetch_add_release(T& location, T delta = 1)
{
    std::atomic_ref<T> ref(location);
    return ref.fetch_add(delta, std::memory_order_release);
}

} // namespace sintra::ipc
