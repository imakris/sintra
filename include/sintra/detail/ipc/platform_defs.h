// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

#include "../process/process_id.h"

#ifdef _WIN32
  #include "../sintra_windows.h"
#else
  #include <unistd.h>
  #include <pthread.h>

  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif

  #if defined(__FreeBSD__)
    #include <sys/thr.h>
    #include <pthread_np.h>
  #endif
#endif

namespace sintra {

namespace detail {

#ifdef _WIN32
inline std::size_t query_system_page_size() noexcept
{
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);

    std::size_t granularity = static_cast<std::size_t>(info.dwAllocationGranularity);
    if (granularity == 0) {
        granularity = static_cast<std::size_t>(info.dwPageSize);
    }

    return granularity != 0 ? granularity : std::size_t(4096);
}
#else
inline std::size_t query_system_page_size() noexcept
{
    long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
#ifdef _SC_PAGE_SIZE
        page = ::sysconf(_SC_PAGE_SIZE);
#endif
    }

#if defined(__APPLE__) || defined(__FreeBSD__)
    if (page <= 0) {
        page = ::getpagesize();
    }
#endif

    if (page <= 0) {
        page = 4096;
    }

    return static_cast<std::size_t>(page);
}
#endif

} // namespace detail

inline std::size_t system_page_size() noexcept
{
    static const std::size_t cached = detail::query_system_page_size();
    return cached;
}

inline uint32_t get_current_tid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t tid = 0;
    (void)pthread_threadid_np(nullptr, &tid);
    return static_cast<uint32_t>(tid);
#elif defined(__FreeBSD__)
    long tid = 0;
    if (::thr_self(&tid) == 0) {
        return static_cast<uint32_t>(tid);
    }
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#elif defined(__linux__)
#ifdef SYS_gettid
    return static_cast<uint32_t>(::syscall(SYS_gettid));
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

inline uint32_t get_current_pid()
{
    return static_cast<uint32_t>(detail::get_current_process_id());
}

inline size_t mod_pos_i64(int64_t x, size_t m)
{
    const int64_t mm = static_cast<int64_t>(m);
    int64_t       r  = x % mm;
    if (r < 0) {
        r += mm;
    }
    return static_cast<size_t>(r);
}

inline size_t mod_u64(uint64_t x, size_t m)
{
    return static_cast<size_t>(x % static_cast<uint64_t>(m));
}

} // namespace sintra
