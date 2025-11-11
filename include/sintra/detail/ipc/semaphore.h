// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once
/*
interprocess_semaphore.h

Header-only, single file. Platform conditionals are **constrained**:
- One #if ladder selects Windows vs POSIX.
- On POSIX, a single #if ladder selects Linux vs Darwin; all other code is #if-free.

OVERVIEW
- Cross-platform semaphore with a 32-bit counter.
- POSIX uses a wait-on-address shim; Windows uses a named kernel semaphore.
- Single monotonic deadline timeline (ns). No fairness guarantee.

SUPPORTED OS (intentional; no fallbacks)
- Linux (futex), macOS 14.4+ (<os/sync_wait_on_address.h>), Windows 8+ (named kernel semaphore).
- All platforms support a polling backend via SINTRA_USE_SEMAPHORE_POLLING (1ms sleep between checks).
- Platform notes:
  * macOS: wake-all is used; may over-wake. If Apple adds wake-one/wake-N, prefer bounded waking.
  * POSIX wait(): implemented via a very large relative wait on a monotonic clock; effectively "infinite".
  * Windows try_wait(): returns false when no token is available and does not set errno; timed waits set errno=ETIMEDOUT.
  * Polling backend: Uses atomic counter in shared memory with nanosleep fallback.

INTERPROCESS SEMANTICS
- POSIX: the 32-bit wait word must be in shared memory (e.g., mmap MAP_SHARED).
- Windows: HANDLEs are per-process. Use the same explicit name in each process or place the object in shared memory; a lazy per-process open attaches to the embedded name.

API
  interprocess_semaphore(unsigned int initial = 0)
  interprocess_semaphore(unsigned int initial, const wchar_t* name, uint32_t max_count = 0x7fffffff)
  bool try_wait(); void wait(); bool timed_wait(...); bool try_wait_for(...); void post();
  void release_local_handle() (no-op on current backends)

NOTES
- Windows: max_count applies at creation; opening an existing named semaphore cannot change its kernel max.
- Instances are non-movable to avoid corrupting the POSIX shared counter. Copy is deleted as well.

ERROR SEMANTICS
- wait(): loops forever until acquired or unrecoverable backend error (errno == EINVAL);
  never returns false in normal use
- Timed waits: false on timeout or error. Check errno; ETIMEDOUT means timeout.
- POSIX post: post(0) is a no-op; overflow sets errno=EOVERFLOW and does not wake.
- Windows post: post(0) is a no-op.

BUILD REQUIREMENTS
- The wait word is a naturally aligned 32-bit word accessed atomically (lock-free required).

ROBUSTNESS MODEL
- Semaphores are ownerless counters. If a process crashes after wait(), the counter remains decremented; there is no "owner" to recover.
- Windows (named semaphore): the kernel object persists independently of handle lifetimes; process termination does not roll back the count.
- POSIX (shared memory): the counter lives in shared memory visible to other processes and continues to reflect the current value.

MEMORY & ORDERING
- The 32-bit counter is accessed with acquire/release semantics around successful decrement/increment.
- Spurious wake-ups are possible on POSIX, callers must always recheck state after any wake.

USAGE CONTRACT (shared memory requirements)
- POSIX: The internal 32-bit wait word must reside in shared memory (e.g., MAP_SHARED). Placing the object in process-local memory yields intra-process behavior only.
- Windows: HANDLEs are per-process. Use a common name across processes or place this object in shared memory to carry the generated name to other processes.

COMPATIBILITY & PORTABILITY
- This API mirrors the sibling `interprocess_mutex` style: steady-clock deadlines, bool-returning timed waits, and errno for error/timeout discrimination.
- Intended for Linux (futex), macOS 14.4+ (wait-on-address), Windows 8+. No fallback shims for older platforms.

CAVEATS
- Overflow: post(n) where (current + n) > max sets errno=EOVERFLOW and does not wake.
- Timed waits: return false on timeout and set errno=ETIMEDOUT; other errors also return false with errno set.
*/

#include <atomic>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <limits>
#include <thread>

#include "../time_utils.h"

// Platform headers MUST be included BEFORE opening namespaces to avoid polluting them
#if defined(SINTRA_USE_SEMAPHORE_POLLING)
  // User explicitly requested polling backend on all platforms
  #include <time.h>
  #define SINTRA_BACKEND_POLLING 1
#elif defined(_WIN32)
  #include "../sintra_windows.h"
  #include <synchapi.h>
  #include <mutex>
  #include <unordered_map>
  #include <string>
  #include <shared_mutex>
#else
  #include <time.h>
  // Single selection ladder for sub-platform specifics
  #if defined(__APPLE__) && defined(__MACH__) && defined(__has_include) && __has_include(<os/sync_wait_on_address.h>)
    #define SINTRA_BACKEND_DARWIN 1
    #include <os/clock.h>
    #include <os/sync_wait_on_address.h>
  #elif defined(__linux__)
    #define SINTRA_BACKEND_LINUX 1
    #include <sys/syscall.h>
    #include <linux/futex.h>
    #include <unistd.h>
  #else
    #define SINTRA_BACKEND_POLLING 1
  #endif
#endif

namespace sintra { namespace detail {

static_assert(std::atomic<uint32_t>::is_always_lock_free, "requires lock-free 32-bit atomic");
static_assert(sizeof(std::atomic<uint32_t>) == 4, "Assume 4-byte object representation");

// ========================== Backend (single #if ladder) ==========================
struct ips_backend
{
    void   init_default(uint32_t initial) noexcept;
    void   init_named(uint32_t initial, const wchar_t* name, uint32_t max_count) noexcept;
    bool   try_wait() noexcept;
    bool   wait() noexcept;
    bool   try_wait_for(std::chrono::nanoseconds d) noexcept;
    void   post(uint32_t n) noexcept;
    void   destroy() noexcept;

    alignas(8) unsigned char storage[256]{}; // zero-initialized
};

// ----------------------------- Windows backend -----------------------------
#if defined(_WIN32)

  #if defined(_MSC_VER)
    #define bounded_swprintf(buf, cch, fmt, x)  _snwprintf_s(buf, cch, _TRUNCATE, fmt, x)
    #define bounded_swprintf2(buf, cch, fmt, s) _snwprintf_s(buf, cch, _TRUNCATE, fmt, s)
    #define backoff_yield() YieldProcessor()
  #else
    #define bounded_swprintf(buf, cch, fmt, x)  swprintf(buf, cch, fmt, x)
    #define bounded_swprintf2(buf, cch, fmt, s) swprintf(buf, cch, fmt, s)
    #if defined(__x86_64__) || defined(__i386__)
      #define backoff_yield() __asm__ __volatile__("pause")
    #elif defined(__aarch64__) || defined(__arm__)
      #define backoff_yield() __asm__ __volatile__("yield")
    #else
      #define backoff_yield() ((void)0)  // No-op on other archs
    #endif
  #endif


struct ips_backend_win_state
{
    // 0=uninit, 1=initializing, 2=ready
    volatile long init_flag = 0;
    uint32_t initial = 0;
    uint32_t max = 0x7fffffff;
    bool     was_autogenerated = false;
    wchar_t  name[64] = {0};
};
static_assert(sizeof(ips_backend_win_state) <= sizeof(((ips_backend*)0)->storage),
              "ips_backend::storage too small for Windows backend");
static_assert(alignof(ips_backend) >= alignof(ips_backend_win_state),
              "backend align too small");

static ips_backend_win_state& W(ips_backend& b) noexcept
{
    return *reinterpret_cast<ips_backend_win_state*>(b.storage);
}


struct ips_win_handle_cache
{
    std::shared_mutex m; // a shared mutex, for this read-mostly workload
    std::unordered_map<std::wstring, HANDLE> map;
    ~ips_win_handle_cache()
    {
        for (auto& kv : map) {
            if (kv.second) {
                CloseHandle(kv.second);
            }
        }
    }
};

static inline ips_win_handle_cache& ips_win_handles()
{
    static ips_win_handle_cache cache;
    return cache;
}

static bool ips_win_copy_name(ips_backend& b, const wchar_t* s) noexcept
{
    auto& st = W(b);
    if (!s || !s[0]) {
        st.name[0] = L'\0'; return true;
    }
    // Do not modify st.name unless it fits
    size_t len = 0;
    while (s[len] != L'\0') {
        ++len;
    }
    if (len >= 63) {
        errno = ENAMETOOLONG; return false;
    }
    for (size_t i=0; i<=len; ++i) {
        st.name[i] = s[i];
    }
    return true;
}


// NOTE ON WINDOWS HANDLE & SHARED MEMORY:
// A Windows HANDLE is per-process and cannot be safely stored in bytes shared across
// processes. This per-process cache intentionally keeps HANDLEs *out of shared memory*.
// The shared object contains only the generated name/flags; each process lazily
// opens/creates its own HANDLE and stores it here.
static HANDLE ips_win_local_handle(ips_backend& b) noexcept
{
    auto& st = W(b);
    if (st.name[0] == L'\0') {
        errno = EINVAL; return nullptr;
    }

    auto& cache = ips_win_handles();
    std::wstring key(st.name);

    // FAST PATH: Read-only lock
    {
        std::shared_lock<std::shared_mutex> lock(cache.m);
        auto it = cache.map.find(key);
        if (it != cache.map.end()) {
            return it->second;
        }
    }

    // SLOW PATH: Exclusive write lock
    {
        std::unique_lock<std::shared_mutex> lock(cache.m);

        // Re-assign key from the canonical name to prevent a stale read from before the lock.
        key.assign(st.name);

        // Re-check the map with the canonical key in case another thread just created it.
        auto it = cache.map.find(key);
        if (it != cache.map.end()) {
            return it->second;
        }

        // We are the first. Create (or open) the handle.
        HANDLE h = CreateSemaphoreW(nullptr, (LONG)st.initial, (LONG)st.max, st.name);
        if (!h) {
            DWORD last = GetLastError();

            // Last resort: try opening if creation failed for other reasons (e.g., permissions).
            h = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, st.name);
        }

        if (!h) {
            errno = EINVAL; // Set errno on final failure
            return nullptr;
        }

        cache.map.emplace(std::move(key), h);
        return h;
    }
}



static void ips_win_ensure_ready(ips_backend& b) noexcept
{
    auto& st = W(b);
    if (st.init_flag == 2) {
        return;
    }
    if (InterlockedCompareExchange(&st.init_flag, 1, 0) == 0) {
        // We are the initializer
        if (st.name[0] == L'\0') {
            if (st.was_autogenerated) {
                FILETIME ft; GetSystemTimeAsFileTime(&ft);
                unsigned pid = GetCurrentProcessId();
                unsigned tid = GetCurrentThreadId();
                uintptr_t self = (uintptr_t)&b;
                unsigned long long x =
                    ((unsigned long long)ft.dwLowDateTime) ^
                    ((unsigned long long)ft.dwHighDateTime<<32) ^
                    ((unsigned long long)pid<<13) ^
                    ((unsigned long long)tid<<7) ^
                    ((unsigned long long)self);
                wchar_t buf[64];
                bounded_swprintf(buf, 64, L"Local\\SintraSem_%016llX", x);
                ips_win_copy_name(b, buf);
            }
            else {
                errno = EINVAL;
            }
        }
        MemoryBarrier();
        InterlockedExchange(&st.init_flag, 2);
    }
    else {
        // Wait until published (state==2)
        unsigned spins = 0;
        while (st.init_flag != 2) {
            backoff_yield();
            if ((++spins & 0x3FFF) == 0) {
                Sleep(0);
            }
        }
    }
}


// 1) Make sure default ctor actually autogenerates a name
inline void ips_backend::init_default(uint32_t initial) noexcept
{
    auto& st = W(*this);
    st.initial = initial;
    st.max = 0x7fffffff;
    if (initial > st.max)  {
        errno = EINVAL;
        InterlockedExchange(&st.init_flag, 2);
        return;
    }
    st.was_autogenerated = true;
    ips_win_ensure_ready(*this);   // <-- generate Global\SintraSem_… name now
}

// 2) If name is absent, treat as autogenerated and generate it now
inline void ips_backend::init_named(
    uint32_t initial, const wchar_t* name, uint32_t max_count) noexcept
{
    auto& st = W(*this);
    st.initial = initial;
    st.max = max_count;
    if (initial > max_count) {
        errno = EINVAL;
        InterlockedExchange(&st.init_flag, 2);
        return;
    }

    if (name && name[0] != L'\0') {
        st.was_autogenerated = false;
        if (!ips_win_copy_name(*this, name)) {
            InterlockedExchange(&st.init_flag, 2);
            return;
        }
    }
    else {
        st.was_autogenerated = true;
        ips_win_ensure_ready(*this);   // <-- generate a name
    }
}

// 3) Ensure name exists before opening/creating a handle
static HANDLE ips_win_ensure_handle(ips_backend& b) noexcept // Return HANDLE
{
    ips_win_ensure_ready(b);           // <-- guarantees st.name is set if autogen was intended
    return ips_win_local_handle(b);
}

// 4) Avoid double lookup in try_wait (similar in wait/try_wait_for)
inline bool ips_backend::try_wait() noexcept
{
    HANDLE h = ips_win_ensure_handle(*this);
    if (!h) {
        errno = EINVAL;
        return false;
    }
    
    DWORD rc = WaitForSingleObject(h, 0);
    if (rc == WAIT_OBJECT_0) return true;
    if (rc == WAIT_FAILED) {
        errno = EINVAL; // keeps "no errno on empty" invariant for normal false
        return false;
    }
    return false;
}


inline bool ips_backend::wait() noexcept
{
    HANDLE h = ips_win_ensure_handle(*this);
    if (!h) {
        errno = EINVAL;
        return false;
    }
    DWORD rc = WaitForSingleObject(h, INFINITE);
    if (rc == WAIT_OBJECT_0) {
        return true;
    }
    errno = EINVAL;
    return false;
}

inline bool ips_backend::try_wait_for(std::chrono::nanoseconds d) noexcept
{
    HANDLE h = ips_win_ensure_handle(*this);
    if (!h) {
        errno = EINVAL;
        return false;
    }
    uint64_t add = d.count() <= 0 ? 0ULL : (uint64_t)d.count();
    DWORD ms;
    if (add/1000000ULL >= 0xFFFFFFFFULL) {
        ms = INFINITE - 1;
    }
    else {
        ms = (DWORD)(add/1000000ULL);
        if (ms == 0 && add > 0) {
            ms = 1;
        }
    }

    DWORD rc = WaitForSingleObject(h, ms);
    if (rc == WAIT_OBJECT_0) {
        return true;
    }
    if (rc == WAIT_TIMEOUT) {
        errno = ETIMEDOUT; return false;
    }
    errno = EINVAL;
    return false;
}

inline void ips_backend::post(uint32_t n) noexcept
{
    if (n == 0) {
        return;
    }
    HANDLE h = ips_win_ensure_handle(*this);
    if (!h) {
        errno = EINVAL; return;
    }
    if (n > (uint32_t)LONG_MAX) {
        errno = EOVERFLOW; return;
    }
    if (!ReleaseSemaphore(h, (LONG)n, nullptr)) {
        DWORD e = GetLastError();
        errno = (e == ERROR_TOO_MANY_POSTS) ? EOVERFLOW : EINVAL;
    }
}

inline void ips_backend::destroy() noexcept
{
    // Per-process cached handles are kept for the life of the process.
}

#else // ----------------------------- POSIX backend -----------------------------



#if SINTRA_BACKEND_LINUX
static inline void ns_to_timespec(uint64_t ns, struct timespec& ts)
{
    uint64_t sec = ns / 1000000000ULL;
    uint64_t tmax = (uint64_t)std::numeric_limits<time_t>::max();
    if (sec > tmax) {
        sec = tmax;
    }
    ts.tv_sec  = (time_t)sec;
    ts.tv_nsec = (long)(ns % 1000000000ULL);
}

static inline int futex_wait(int* addr, int val, const struct timespec* rel)
{
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT, val, rel, nullptr, 0);
}

static inline int futex_wake(int* addr, int n)
{
    return (int)syscall(SYS_futex, addr, FUTEX_WAKE, n, nullptr, nullptr, 0);
}
#endif

// Unified POSIX helpers (no further #ifs elsewhere)
static inline int posix_wait_equal_until(
    uint32_t* addr, uint32_t expected, uint64_t deadline) noexcept
{
#if SINTRA_BACKEND_DARWIN
    const uint32_t flags = OS_SYNC_WAIT_ON_ADDRESS_SHARED;
    for (;;) {
        int rc = os_sync_wait_on_address_with_deadline(
            (void*)addr, (uint64_t)expected, 4, flags, OS_CLOCK_MONOTONIC, deadline);
        if (rc >= 0)                           return  0;
        if (errno == ETIMEDOUT)                return -1;
        if (errno == EINTR || errno == EAGAIN) continue;
        return -1;
    }
#elif SINTRA_BACKEND_LINUX
    for (;;) {
        const uint64_t now = monotonic_now_ns();
        if (now >= deadline) {
            errno = ETIMEDOUT; return -1;
        }
        uint64_t rel = deadline - now;
        int rc;
        {
            uint64_t tmax = (uint64_t)std::numeric_limits<time_t>::max();
            if (rel / 1000000000ULL > tmax) {
                rc = futex_wait((int*)addr, (int)expected, nullptr);
            }
            else {
                struct timespec ts;
                ns_to_timespec(rel, ts);
                rc = futex_wait((int*)addr, (int)expected, &ts);
            }
        }
        if (rc == 0)                           return  0; // value changed or spuriously woke; caller rechecks
        if (errno == ETIMEDOUT)                return -1;
        if (errno == EINTR || errno == EAGAIN) continue;
        return -1;
    }
#elif SINTRA_BACKEND_POLLING
    // Polling backend: sleep 1ms and return spurious wake
    (void)addr; (void)expected; (void)deadline;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 0;  // Treat as spurious wake, caller will recheck condition
#endif
}

static inline void posix_wake_some(uint32_t* addr, int n) noexcept
{
#if SINTRA_BACKEND_LINUX
    if (n < 0)       n = 0;
    if (n > INT_MAX) n = INT_MAX;
    (void)futex_wake((int*)addr, n);
#elif SINTRA_BACKEND_DARWIN
    (void)n;  // Darwin doesn't have wake-N, always use wake-all
    (void)os_sync_wake_by_address_all((void*)addr, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#elif SINTRA_BACKEND_POLLING
    (void)addr; (void)n;
    // Polling backend doesn't need wakes - waiters poll the atomic counter
#endif
}


struct ips_backend_posix_state
{
    std::atomic<uint32_t> count{0};
    uint32_t max = 0x7fffffff;
};

static_assert(sizeof(ips_backend_posix_state) <= sizeof(((ips_backend*)0)->storage),
              "ips_backend::storage too small for POSIX backend");
static_assert(alignof(ips_backend) >= alignof(ips_backend_posix_state),
              "backend align too small");

static ips_backend_posix_state& P(ips_backend& b) noexcept
{
    return *reinterpret_cast<ips_backend_posix_state*>(b.storage);
}

inline void ips_backend::init_default(uint32_t initial) noexcept
{
    auto& st = P(*this);
    st.max = 0x7fffffff;
    if (initial > st.max) {
        errno = EINVAL;
        st.max = 0;
        st.count = 0;
        return;
    }
    st.count = initial;
}

inline void ips_backend::init_named(
    uint32_t initial, const wchar_t*, uint32_t max_count) noexcept
{
    // Name unused on POSIX (interprocess via shared memory placement).
    auto& st = P(*this);
    st.max = max_count;
    if (initial > max_count) {
        errno = EINVAL;
        st.max = 0;
        st.count = 0;
        return;
    }
    st.count = initial;
}

inline bool ips_backend::try_wait() noexcept
{
    std::atomic<uint32_t>& c = P(*this).count;
    uint32_t v = c;
    while (v != 0) {
        if (c.compare_exchange_weak(v, v-1)) {
            return true;
        }
    }
    return false;
}

inline bool ips_backend::wait() noexcept
{
    if (try_wait()) {
        return true;
    }
    // Use very large timeout for "infinite" wait
    return try_wait_for(std::chrono::nanoseconds(1ULL<<60));
}

inline bool ips_backend::try_wait_for(std::chrono::nanoseconds d) noexcept
{
    std::atomic<uint32_t>& c = P(*this).count;
    if (try_wait()) {
        return true;
    }

    const uint64_t add = d.count() <= 0 ? 0ULL : (uint64_t)d.count();
    const uint64_t deadline = monotonic_now_ns() + add;

    for (;;) {
        uint32_t cur = c;
        if (cur != 0) {
            if (c.compare_exchange_weak(cur, cur-1))
                return true;
            continue;
        }
        if (monotonic_now_ns() >= deadline) {
            if (try_wait()) {
                return true;
            }
            errno = ETIMEDOUT;
            return false;
        }
        if (posix_wait_equal_until(reinterpret_cast<uint32_t*>(&P(*this).count), 0u, deadline) == -1) {
            if (errno == ETIMEDOUT) {
                if (try_wait()) {
                    return true;
                }
                return false;
            }
            // other errors treated as spurious; loop and recheck
        }
    }
}

inline void ips_backend::post(uint32_t n) noexcept
{
    if (n == 0) {
        return;
    }

    std::atomic<uint32_t>& c = P(*this).count;
    uint32_t v = c;
    const uint32_t maxv = P(*this).max;

    for (;;) {
        if (n > maxv - v) {
            errno = EOVERFLOW;
            return;
        }
        if (c.compare_exchange_weak(v, v + n)) {
            break;
        }
    }

    posix_wake_some(reinterpret_cast<uint32_t*>(&P(*this).count), (int)n);
}

inline void ips_backend::destroy() noexcept
{
    // Nothing to do for POSIX
}

#endif // end backend selection

// ========================== Public class (no #ifs below) ==========================
class interprocess_semaphore
{
public:
    // Constructor matching current API (unsigned int for compatibility)
    explicit interprocess_semaphore(unsigned int initial = 0) noexcept
    {
        m_impl.init_default(static_cast<uint32_t>(initial));
    }

    interprocess_semaphore(unsigned int initial, const wchar_t* name, uint32_t max_count = 0x7fffffff) noexcept
    {
        m_impl.init_named(static_cast<uint32_t>(initial), name, max_count);
    }

    ~interprocess_semaphore()
    {
        m_impl.destroy();
    }

    // Non-copyable and non-movable (critical for shared memory correctness)
    interprocess_semaphore(const interprocess_semaphore&) = delete;
    interprocess_semaphore& operator=(const interprocess_semaphore&) = delete;
    interprocess_semaphore(interprocess_semaphore&&) = delete;
    interprocess_semaphore& operator=(interprocess_semaphore&&) = delete;

    // API compatibility: void wait() (loops forever until acquired or unrecoverable backend error)
    void wait() noexcept
    {
        while (!m_impl.wait()) {
            if (errno == EINVAL) {
                assert(!"errno == EINVAL in interprocess_semaphore::wait() [backend error]");
                break;
            }
        }
    }

    // API compatibility: bool try_wait()
    bool try_wait() noexcept
    {
        return m_impl.try_wait();
    }

    // API compatibility: void post() (posts single token)
    void post() noexcept
    {
        m_impl.post(1);
    }

    // API compatibility: templated timed_wait (requires steady clock)
    template <typename Clock, typename Duration>
    bool timed_wait(const std::chrono::time_point<Clock, Duration>& abs_time) noexcept
    {
        static_assert(Clock::is_steady, "timed_wait requires a steady clock to avoid wall-clock jumps");

        auto now_chrono = Clock::now();
        if (abs_time <= now_chrono) {
            return false; // Deadline already passed
        }

        auto delta = abs_time - now_chrono;
        auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delta);

        return m_impl.try_wait_for(delta_ns);
    }

    // API compatibility: try_wait_for with duration
    bool try_wait_for(std::chrono::nanoseconds rel_timeout) noexcept
    {
        return m_impl.try_wait_for(rel_timeout);
    }

    // API compatibility: no-op on current backends
    void release_local_handle() noexcept
    {
        // No-op (Windows named semaphores handle this internally)
    }

#ifdef _WIN32
    // Friend for testing: allows test to extract Windows semaphore details without hardcoded offsets
    friend void test_get_win_semaphore_info(const interprocess_semaphore&, std::uint64_t&, std::wstring&);
#endif

private:
    ips_backend m_impl;
};

}} // namespace sintra::detail