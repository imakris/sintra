#pragma once

#include <cstdint>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace sintra::ipc_sync {
namespace detail {

template <typename T>
struct atomic_support;

#if defined(__GNUC__) || defined(__clang__)

template <typename T>
struct atomic_support {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    static void store_release(T& location, T value)
    {
        __atomic_store_n(&location, value, __ATOMIC_RELEASE);
    }

    static T load_acquire(const T& location)
    {
        return __atomic_load_n(&location, __ATOMIC_ACQUIRE);
    }

    static T fetch_add_release(T& location, T delta)
    {
        return __atomic_fetch_add(&location, delta, __ATOMIC_RELEASE);
    }
};

#elif defined(_MSC_VER)

template <>
struct atomic_support<std::uint32_t> {
    using value_type = std::uint32_t;

    static void store_release(value_type& location, value_type value)
    {
        _InterlockedExchange(reinterpret_cast<volatile long*>(&location), static_cast<long>(value));
    }

    static value_type load_acquire(const value_type& location)
    {
        return static_cast<value_type>(
            _InterlockedCompareExchange(reinterpret_cast<volatile long*>(const_cast<value_type*>(&location)), 0L, 0L));
    }

    static value_type fetch_add_release(value_type& location, value_type delta)
    {
        return static_cast<value_type>(
            _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&location), static_cast<long>(delta)));
    }
};

#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM64)

template <>
struct atomic_support<std::uint64_t> {
    using value_type = std::uint64_t;

    static void store_release(value_type& location, value_type value)
    {
        _InterlockedExchange64(reinterpret_cast<volatile long long*>(&location), static_cast<long long>(value));
    }

    static value_type load_acquire(const value_type& location)
    {
        return static_cast<value_type>(
            _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(const_cast<value_type*>(&location)), 0LL, 0LL));
    }

    static value_type fetch_add_release(value_type& location, value_type delta)
    {
        return static_cast<value_type>(
            _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(&location), static_cast<long long>(delta)));
    }
};

#else
#error "64-bit atomic operations require a 64-bit Microsoft toolchain"
#endif

#else
#error "Unsupported compiler for sintra::ipc_sync atomics"
#endif

} // namespace detail

template <typename T>
inline void store_release(T& location, T value)
{
    detail::atomic_support<T>::store_release(location, value);
}

template <typename T>
inline T load_acquire(const T& location)
{
    return detail::atomic_support<T>::load_acquire(location);
}

template <typename T>
inline T fetch_add_release(T& location, T delta)
{
    return detail::atomic_support<T>::fetch_add_release(location, delta);
}

} // namespace sintra::ipc_sync
