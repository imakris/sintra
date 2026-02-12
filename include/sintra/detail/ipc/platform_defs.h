// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

#include "../logging.h"
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
  #elif defined(__APPLE__)
    #include <sys/sysctl.h>
  #endif
#endif

#if defined(__linux__) || defined(__APPLE__)
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
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

#if defined(__linux__)
static inline size_t sintra_detect_cache_line_size()
{
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
    long v = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v > 0) return static_cast<size_t>(v);
#endif
    auto read_size = [](const char* path) -> size_t {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return 0;
        char buf[64] = {0};
        size_t n = std::fread(buf, 1, sizeof(buf)-1, f);
        std::fclose(f);
        if (n == 0) return 0;
        char* endp = nullptr;
        long val = std::strtol(buf, &endp, 10);
        return val > 0 ? static_cast<size_t>(val) : 0;
    };

    const char* paths[] = {
        "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index1/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index2/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index3/coherency_line_size",
    };
    for (const char* p : paths) {
        size_t s = read_size(p);
        if (s) return s;
    }
    return 64;
}
#elif defined(__APPLE__)
static inline size_t sintra_detect_cache_line_size()
{
    auto query_size = [](const char* name) -> size_t {
        size_t value = 0;
        size_t len = sizeof(value);
        if (::sysctlbyname(name, &value, &len, nullptr, 0) == 0 && value > 0) {
            return value;
        }
        return 0;
    };

    if (size_t s = query_size("hw.cachelinesize")) {
        return s;
    }
    if (size_t s = query_size("machdep.cpu.cache.linesize")) {
        return s;
    }

    return 64;
}
#endif

#if defined(__linux__) || defined(__APPLE__)
static inline void sintra_warn_if_cacheline_mismatch(size_t assumed_cache_line_size)
{
    size_t detected = sintra_detect_cache_line_size();
    if (detected && detected != assumed_cache_line_size) {
        Log_stream(log_level::warning)
            << "sintra(ipc_rings): warning: detected L1D line " << detected
            << " != assumed " << assumed_cache_line_size
            << "; performance may be suboptimal.\n";
    }
}
#endif

inline size_t mod_pos_i64(int64_t x, size_t m)
{
    const int64_t mm = static_cast<int64_t>(m);
    int64_t r = x % mm;
    if (r < 0) r += mm;
    return static_cast<size_t>(r);
}

inline size_t mod_u64(uint64_t x, size_t m)
{
    return static_cast<size_t>(x % static_cast<uint64_t>(m));
}

} // namespace sintra
