/*
Copyright 2017 Ioannis Makris

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/* ipc_rings.h  —  SINTRA SPMC IPC Ring
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS IS
 * ------------
 * A single-producer / multiple-consumer (SPMC) inter-process circular ring
 * buffer that uses the “magic ring” trick (double virtual mapping of the same
 * file back-to-back) so wrap-around reads and writes are linear in memory.
 *
 * WHEN TO USE
 * -----------
 * High-throughput, low-latency IPC where one writer emits variable-sized
 * messages and many independent readers consume with minimal contention
 * (telemetry, logging, streaming analytics).
 *
 * CONSTRAINTS & RECOMMENDATIONS
 * -----------------------------
 *  • Elements & octiles:
 *      - The ring is conceptually split into 8 equal “octiles”.
 *      - REQUIRED: num_elements % 8 == 0 (we guard per-octile).
 *  • Page / granularity alignment:
 *      - REQUIRED: data region size (num_elements * sizeof(T)) must be a
 *        multiple of the system’s mapping granularity:
 *          • Windows: allocation granularity (dwAllocationGranularity)
 *          • POSIX: page size
 *        We assert this at attach() time.
 *  • Power-of-two size:
 *      - RECOMMENDED (not strictly required): choose power-of-two sizes.
 *        Our helper that proposes configurations typically does that.
 *
 * PUBLISH / MEMORY ORDERING SEMANTICS
 * -----------------------------------
 *  • The writer first writes elements into the mapped region (plain stores),
 *    then calls done_writing(), which atomically publishes the new
 *    leading_sequence (last published element is leading_sequence - 1).
 *  • Readers use the published leading_sequence to compute their readable range.
 *
 * WRITE BOUNDS
 * ------------
 *  • Each write must fit within a single octile (<= ring_size/8 elements).
 *    This ensures the writer only needs to check & spin on at most one octile’s
 *    read guard (fast path with minimal contention).
 *
 * READER SNAPSHOTS & VALIDITY
 * ---------------------------
 *  • start_reading() returns a snapshot range into the shared memory mapping.
 *  • The snapshot remains valid until overwritten; i.e., until the writer
 *    advances far enough that the reader’s trailing guard octile would be
 *    surpassed and reclaimed by the writer.
 *  • start_reading() must be paired with done_reading().
 *    Calling start_reading() again, while a snapshot is active, throws.
 *
 * READER CAP
 * ----------
 *  • Effective maximum readers = min(255, max_process_index).
 *    - The control block allocates per-reader state arrays sized by
 *      max_process_index; and the octile guard uses an 8×1-byte pattern which
 *      caps actively guarded readers at 255.
 *
 * READER EVICTION (WHEN ENABLED)
 * ------------------------------
 *  • If SINTRA_ENABLE_SLOW_READER_EVICTION is defined, the writer can evict
 *    a reader that is blocking its progress.
 *  • A reader is considered "slow" if it holds a guard and its last-read
 *    sequence is more than one full ring buffer's length behind the writer.
 *  • Eviction forcefully clears the reader's guard and sets its status to
 *    EVICTED. The reader's next call to start_reading() will throw a
 *    ring_reader_evicted_exception, forcing it to re-attach.
 *
 * LIFECYCLE & CLEANUP
 * -------------------
 *  • Two files exist: a data file (double-mapped) and a control file
 *    (atomics, semaphores, per-reader state).
 *  • Any process may create; subsequent processes attach.
 *  • Each Ring_R object acquires a unique "reader slot" at construction and
 *    releases it on normal destruction.
 *  • The *last* detaching process removes BOTH files.
 *
 * PLATFORM MAPPING OVERVIEW
 * -------------------------
 *  • Windows:
 *      - Reserve address space (2× region + granularity) via ::VirtualAlloc.
 *      - Apply the historical rounding to preserve layout parity:
 *            char* ptr = (char*)(
 *                (uintptr_t)((char*)mem + granularity) & ~((uintptr_t)granularity - 1)
 *            );
 *      - Release the reservation and immediately map the file twice contiguously,
 *        expecting the OS to reuse the address.
 *  • Linux / POSIX:
 *      - Reserve a 2× span with mmap(NULL, 2*size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, ...).
 *      - Map the file TWICE into that span using MAP_FIXED (by design replaces
 *        the reservation). Do NOT use MAP_FIXED_NOREPLACE for this step.
 *      - Use ptr = mem (mmap returns a page-aligned address).
 *      - On ANY failure after reserving, munmap the 2× span before returning.
 */


#pragma once

// ─── Project config & utilities (kept as in original codebase) ───────────────
#include "config.h"      // defines: assumed_cache_line_size, max_process_index,
                         //          SINTRA_RING_READING_POLICY*, etc.
#include "get_wtime.h"   // high-res wall clock (used by hybrid reader policy)
#include "id_types.h"    // ID and type aliases as used by the project

// ─── STL / stdlib ────────────────────────────────────────────────────────────
#include <algorithm>     // std::reverse
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>        // std::FILE, std::fprintf
#include <cstring>       // std::strlen
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>         // std::once_flag, std::call_once
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// ─── Boost.Interprocess ──────────────────────────────────────────────────────
#include <boost/interprocess/detail/os_file_functions.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>

// ─── Platform headers (grouped) ──────────────────────────────────────────────
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>   // ::VirtualAlloc, granularity details (via Boost)
#else
  #include <sys/mman.h>  // ::mmap, ::munmap, MAP_FIXED, MAP_NOSYNC (if available)
  #include <unistd.h>    // ::sysconf
  #include <signal.h>    // ::kill
#endif


// Enables the writer to forcefully evict readers that are too slow.
#ifndef SINTRA_ENABLE_SLOW_READER_EVICTION
#define SINTRA_ENABLE_SLOW_READER_EVICTION
#endif

// If eviction is enabled, this is the approximate stall budget (in microseconds)
// the writer will tolerate before triggering an eviction scan. Override with a
// compile-time value tailored to your deployment if desired.
#ifndef SINTRA_EVICTION_SPIN_BUDGET_US
#define SINTRA_EVICTION_SPIN_BUDGET_US 500u
#endif

#ifndef SINTRA_EVICTION_LAG_RINGS
#define SINTRA_EVICTION_LAG_RINGS 1u  // reader is "slow" if > N rings behind
#endif


// ─────────────────────────────────────────────────────────────────────────────

namespace sintra {

namespace fs  = std::filesystem;
namespace ipc = boost::interprocess;

using sequence_counter_type = uint64_t;
constexpr auto invalid_sequence = ~sequence_counter_type(0);

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
namespace ring_detail {

inline uint64_t calibrate_spin_loops_per_microsecond()
{
    static std::atomic<uint64_t> cached{0};
    uint64_t loops = cached.load(std::memory_order_acquire);
    if (loops != 0) {
        return loops;
    }

    constexpr uint64_t sample_iterations = 1u << 20;
    volatile uint64_t sink = 0;

    double start = get_wtime();
    for (uint64_t i = 0; i < sample_iterations; ++i) {
        sink += i;
    }
    double elapsed = get_wtime() - start;
    (void)sink;

    uint64_t computed = sample_iterations;
    if (elapsed > 0.0) {
        double loops_per_us = static_cast<double>(sample_iterations) / (elapsed * 1e6);
        if (loops_per_us >= 1.0) {
            computed = static_cast<uint64_t>(loops_per_us);
        }
        else {
            computed = 1;
        }
    }

    uint64_t expected = 0;
    if (cached.compare_exchange_strong(expected, computed, std::memory_order_release, std::memory_order_relaxed)) {
        return computed;
    }

    return cached.load(std::memory_order_acquire);
}

inline uint64_t get_eviction_spin_loop_budget()
{
#ifdef SINTRA_EVICTION_SPIN_THRESHOLD
    // Backwards compatibility: allow callers to supply a raw iteration budget.
    return SINTRA_EVICTION_SPIN_THRESHOLD;
#else
    static std::atomic<uint64_t> cached{0};
    uint64_t loops = cached.load(std::memory_order_acquire);
    if (loops != 0) {
        return loops;
    }

    const uint64_t loops_per_us = calibrate_spin_loops_per_microsecond();
    uint64_t computed = loops_per_us * static_cast<uint64_t>(SINTRA_EVICTION_SPIN_BUDGET_US);
    if (computed == 0) {
        computed = SINTRA_EVICTION_SPIN_BUDGET_US ? 1 : loops_per_us;
    }

    uint64_t expected = 0;
    if (cached.compare_exchange_strong(expected, computed, std::memory_order_release, std::memory_order_relaxed)) {
        return computed;
    }

    return cached.load(std::memory_order_acquire);
#endif
}

} // namespace ring_detail
#endif // SINTRA_ENABLE_SLOW_READER_EVICTION

#ifdef _WIN32
static inline uint32_t get_current_pid()
{
    return static_cast<uint32_t>(::GetCurrentProcessId());
}

static inline bool is_process_alive(uint32_t pid)
{
    if (pid == 0) {
        return false;
    }

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return true;
        }
        return false;
    }

    DWORD code = 0;
    bool alive = false;
    if (::GetExitCodeProcess(h, &code)) {
        alive = (code == STILL_ACTIVE);
    }
    ::CloseHandle(h);
    return alive;
}
#else
static inline uint32_t get_current_pid()
{
    return static_cast<uint32_t>(::getpid());
}

static inline bool is_process_alive(uint32_t pid)
{
    return pid && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH);
}
#endif

//==============================================================================
// Linux-only runtime cache-line helpers (placed AFTER includes, as required)
//==============================================================================
#if defined(__linux__)
/**
 * Attempt to detect the L1 data cache line size at runtime.
 * Order:
 *   1) sysconf(_SC_LEVEL1_DCACHE_LINESIZE) if available.
 *   2) sysfs: /sys/devices/system/cpu/cpu0/cache/indexX/coherency_line_size (for X = 0..3)
 *   3) Fallback: 64 bytes (sane default on many systems).
 */
static inline size_t sintra_detect_cache_line_size_linux()
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
    // Probe a few likely indices (L1/L2/L3 order doesn’t matter; we just need a sane size)
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
    return 64; // conservative default
}

/**
 * Under SINTRA_RUNTIME_CACHELINE_CHECK, warn ONCE after a successful attach()
 * if the detected cache-line size differs from assumed_cache_line_size.
 */
static inline void sintra_warn_if_cacheline_mismatch_linux(size_t assumed_cache_line_size)
{
    size_t detected = sintra_detect_cache_line_size_linux();
    if (detected && detected != assumed_cache_line_size) {
        std::fprintf(stderr,
            "sintra(ipc_rings): warning: detected L1D line %zu != assumed %zu; "
            "performance may be suboptimal.\n",
            detected, assumed_cache_line_size);
    }
}
#endif // __linux__

//==============================================================================
// Utility: project-consistent filesystem helpers
//==============================================================================

/**
 * Ensure the directory exists and is a directory. If a same-named regular file
 * exists, remove it and create a directory in its place.
 */
static inline bool check_or_create_directory(const std::string& dir_name)
{
    std::error_code ec;
    fs::path ps(dir_name);
    if (!fs::exists(ps)) {
        return fs::create_directory(ps, ec);
    }
    if (fs::is_regular_file(ps)) {
        (void)fs::remove(ps, ec);                 // explicit fs::remove (not ::remove)
        return fs::create_directory(ps, ec);
    }
    return true;
}

/**
 * Remove a directory tree. Returns true if anything was removed.
 * Uses retry logic to handle filesystem delays on Windows.
 */
static inline bool remove_directory(const std::string& dir_name)
{
    std::error_code ec;
    auto removed = fs::remove_all(dir_name.c_str(), ec);
    if (ec) {
        return false;
    }
    return removed > 0;
}


static inline size_t mod_pos_i64(int64_t x, size_t m) {
    const int64_t mm = static_cast<int64_t>(m);
    int64_t r = x % mm;
    if (r < 0) r += mm;
    return static_cast<size_t>(r);
}


static inline size_t mod_u64(uint64_t x, size_t m) {
    return static_cast<size_t>(x % static_cast<uint64_t>(m));
}

//==============================================================================
// Suggested ring configurations helper
//==============================================================================

/**
 * Compute candidate ring sizes (in element counts) between min_elements and the
 * maximum byte size constraint, with up to max_subdivisions sizes. The algorithm
 * prefers power-of-two-like sizes aligned to the system’s page size so that
 * double-mapping constraints are naturally satisfied.
 *
 * Constraints embodied here:
 *  • The base size (bytes) is the LCM(sizeof(T), page_size).
 *  • The resulting region sizes are multiples of the page size.
 *  • The element count MUST be a multiple of 8 (octiles). We ensure the base
 *    size respects that when the caller chooses from the returned set.
 */
template <typename T>
std::vector<size_t> get_ring_configurations(
    size_t min_elements, size_t max_size, size_t max_subdivisions)
{
    auto gcd = [](size_t m, size_t n) {
        size_t tmp;
        while (m) { tmp = m; m = n % m; n = tmp; }
        return n;
    };
    auto lcm = [=](size_t m, size_t n) {
        return m / gcd(m, n) * n;
    };

    size_t page_size = ipc::mapped_region::get_page_size();
    // Round up to a page-size multiple that is also a multiple of sizeof(T)
    size_t base_size = lcm(sizeof(T), page_size);

    // Respect caller’s minimum element constraint
    size_t min_size = std::max(min_elements * sizeof(T), base_size);
    size_t tmp_size = base_size;

    std::vector<size_t> ret;

    // Find the largest power-of-two-ish size <= max_size
    while (tmp_size * 2 <= max_size) {
        tmp_size *= 2;
    }

    // Produce up to max_subdivisions sizes by halving down
    for (size_t i = 0; i < max_subdivisions && tmp_size >= min_size; i++) {
        // (Caller must still ensure multiple-of-8 elements.)
        ret.push_back(tmp_size / sizeof(T));
        tmp_size /= 2;
    }

    std::reverse(ret.begin(), ret.end());
    return ret;
}

//==============================================================================
// Lightweight range view
//==============================================================================

template <typename T>
struct Range {
    T* begin = nullptr;
    T* end   = nullptr;
};

//==============================================================================
// Small helper types
//==============================================================================

class ring_acquisition_failure_exception : public std::runtime_error {
public:
    ring_acquisition_failure_exception() : std::runtime_error("Failed to acquire ring buffer.") {}
};


class ring_reader_evicted_exception : public std::runtime_error {
public:
    ring_reader_evicted_exception() : std::runtime_error(
              "Ring reader was evicted by the writer due to being too slow.") {}
};

// A binary semaphore tailored for the ring’s reader wakeup policy.
struct sintra_ring_semaphore : ipc::interprocess_semaphore
{
    sintra_ring_semaphore() : ipc::interprocess_semaphore(0) {}

    // Wakes all readers in an ordered fashion (used by writer after publishing).
    void post_ordered()
    {
        if (unordered.load()) {
            unordered = false;
        }
        else
        if (!posted.test_and_set(std::memory_order_acquire)) {
            post();
        }
    }

    // Wakes a single reader in an unordered fashion (used by local unblocks).
    void post_unordered()
    {
        if (!posted.test_and_set(std::memory_order_acquire)) {
            post();
            unordered = true;
        }
    }

    // Wait returns true if the wakeup was unordered and no ordered post happened since.
    bool wait()
    {
        ipc::interprocess_semaphore::wait();
        posted.clear(std::memory_order_release);
        return unordered.load();
    }
private:
    std::atomic_flag posted = ATOMIC_FLAG_INIT;
    std::atomic<bool> unordered{false};
};


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_data //////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//


//==============================================================================
// Ring_data: file-backed data region with “magic” double mapping
//==============================================================================

/**
 * Manages the data file and the double virtual memory mapping. This class knows
 * nothing about control logic; it only guarantees that the same file contents
 * appear twice back-to-back in the virtual address space so wrap-around is linear.
 */
template <typename T, bool READ_ONLY_DATA>
struct Ring_data
{
    using element_t = T;
    static constexpr bool read_only_data = READ_ONLY_DATA;

    Ring_data(const std::string& directory,
              const std::string& data_filename,
              const size_t       num_elements)
    :
        m_num_elements(num_elements),
        m_data_region_size(num_elements * sizeof(T))
    {
        m_directory     = directory + "/";
        m_data_filename = m_directory + data_filename;

        fs::path pr(m_data_filename);

        const bool already_exists = fs::exists(pr) && fs::is_regular_file(pr) && fs::file_size(pr);
        const bool created_or_ok  = already_exists || create();

        if (!created_or_ok || !attach()) {
            throw ring_acquisition_failure_exception();
        }

        // Stable identity across processes for "same mapping" checks
        std::hash<std::string> hasher;
        m_data_filename_hash = hasher(m_data_filename);
    }

    ~Ring_data()
    {
        delete m_data_region_0;
        delete m_data_region_1;
        m_data_region_0 = nullptr;
        m_data_region_1 = nullptr;
        m_data          = nullptr;

        if (m_remove_files_on_destruction) {
            std::error_code ec;
            (void)fs::remove(fs::path(m_data_filename), ec);
        }
    }

    size_t        get_num_elements() const { return m_num_elements; }
    const T*      get_base_address() const { return m_data; }

private:

    // Create the backing data file (filled with a debug pattern in !NDEBUG).
    bool create()
    {
        try {
            if (!check_or_create_directory(m_directory))
                return false;

            ipc::file_handle_t fh_data =
                ipc::ipcdetail::create_new_file(m_data_filename.c_str(), ipc::read_write);
            if (fh_data == ipc::ipcdetail::invalid_file())
                return false;

#ifdef NDEBUG
            if (!ipc::ipcdetail::truncate_file(fh_data, m_data_region_size))
                return false;
#else
            // Fill with a recognizable pattern to aid debugging
            const char* ustr = "UNINITIALIZED";
            const size_t dv = std::strlen(ustr); // std::strlen: see header notes
            std::unique_ptr<char[]> tmp(new char[m_data_region_size]);
            for (size_t i = 0; i < m_data_region_size; ++i) {
                tmp[i] = ustr[i % dv];
            }
            ipc::ipcdetail::write_file(fh_data, tmp.get(), m_data_region_size);
#endif
            return ipc::ipcdetail::close_file(fh_data);
        }
        catch (...) {
        }
        return false;
    }

    /**
     * Attach the data file with a “double mapping”.
     *
     * WINDOWS
     *   • Reserve address space: 2× region + one granularity page.
     *   • Compute ptr by rounding (mem + granularity) down to granularity
     *     (historical layout parity).
     *   • Release the reservation, then map the file twice contiguously starting
     *     at ptr (Boost maps at a provided address).
     *
     * LINUX / POSIX
     *   • Reserve a 2× span with mmap(PROT_NONE). POSIX guarantees page alignment.
     *   • Map the file twice using MAP_FIXED so the mappings REPLACE the reservation.
     *   • IMPORTANT: Do NOT use MAP_FIXED_NOREPLACE here. The whole point is to
     *     overwrite the reservation.
     *   • On ANY failure after reserving, munmap the 2× span and fail cleanly.
     */
    bool attach()
    {
        assert(m_data_region_0 == nullptr && m_data_region_1 == nullptr && m_data == nullptr);

        try {
            if (fs::file_size(m_data_filename) != m_data_region_size) {
                return false; // size mismatch => refuse to map
            }

            // NOTE: On Windows, Boost's "page size" here is the allocation granularity.
            size_t page_size = ipc::mapped_region::get_page_size();

            // Enforce the “multiple of page/granularity” constraint explicitly.
            assert((m_data_region_size % page_size) == 0 &&
                   "Ring size (bytes) must be multiple of mapping granularity");

            auto data_rights = READ_ONLY_DATA ? ipc::read_only : ipc::read_write;
            ipc::file_mapping file(m_data_filename.c_str(), data_rights);

            ipc::map_options_t map_extra_options = 0;

#ifdef _WIN32
            // ── Windows path ───────────────────────────────────────────────────
            // Reserve (2× + granularity) so we can round within the reservation.
            void* mem = ::VirtualAlloc(nullptr,
                                       m_data_region_size * 2 + page_size,
                                       MEM_RESERVE, PAGE_READWRITE);
            if (!mem) {
                return false;
            }

            // Preserve historical layout: round to allocation granularity.
            char* ptr = (char*)(
                (uintptr_t)((char*)mem + page_size) & ~((uintptr_t)page_size - 1)
            );

            // Free the reservation; we expect the OS to reuse the address immediately.
            ::VirtualFree(mem, 0, MEM_RELEASE);

            // Map twice back-to-back at the chosen address.
            m_data_region_0 = new ipc::mapped_region(file, data_rights, 0,
                m_data_region_size, ptr, map_extra_options);
            m_data_region_1 = new ipc::mapped_region(file, data_rights, 0,
                0, ((char*)m_data_region_0->get_address()) + m_data_region_size, map_extra_options);
            m_data = (T*)m_data_region_0->get_address();

            // Basic sanity checks (compile-time asserts are not practical here).
#ifndef NDEBUG
            assert(m_data_region_0->get_address() == ptr);
            assert(m_data_region_1->get_address() == ptr + m_data_region_size);
            assert(m_data_region_0->get_size() == m_data_region_size);
            assert(m_data_region_1->get_size() == m_data_region_size);
#else
            if (m_data_region_0->get_address() != ptr ||
                m_data_region_1->get_address() != (ptr + m_data_region_size) ||
                m_data_region_0->get_size() != m_data_region_size ||
                m_data_region_1->get_size() != m_data_region_size)
            {
                delete m_data_region_0; m_data_region_0 = nullptr;
                delete m_data_region_1; m_data_region_1 = nullptr;
                m_data = nullptr;
                return false;
            }
#endif

#else
            // ── POSIX/Linux path ───────────────────────────────────────────────
            // Reserve a PROT_NONE span large enough for both mappings (2×).
            void* mem = ::mmap(nullptr, m_data_region_size * 2,
                               PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED) {
                return false;
            }

            // POSIX guarantees page alignment for mmap().
            char* ptr = static_cast<char*>(mem);
            assert((reinterpret_cast<uintptr_t>(ptr) % page_size) == 0 &&
                   "mmap(PROT_NONE) base not page-aligned?");

            // We will REPLACE the reservation with fixed mappings.
            #ifdef MAP_FIXED
            map_extra_options |= MAP_FIXED;   // NOTE: intentionally MAP_FIXED, not *_NOREPLACE
            #endif
            #ifdef MAP_NOSYNC
            map_extra_options |= MAP_NOSYNC;
            #endif

            try {
                m_data_region_0 = new ipc::mapped_region(
                    file, data_rights, 0, m_data_region_size, ptr, map_extra_options);
                m_data_region_1 = new ipc::mapped_region(
                    file, data_rights, 0, 0,
                    ((char*)m_data_region_0->get_address()) + m_data_region_size, map_extra_options);
                m_data = (T*)m_data_region_0->get_address();

                // Sanity — the two mappings must be exactly adjacent.
                assert(m_data_region_0->get_address() == ptr);
                assert(m_data_region_1->get_address() == ptr + m_data_region_size);
                assert(m_data_region_0->get_size() == m_data_region_size);
                assert(m_data_region_1->get_size() == m_data_region_size);
            }
            catch (...) {
                // IMPORTANT: unmap the whole reserved span on ANY failure.
                if (m_data_region_0) { delete m_data_region_0; m_data_region_0 = nullptr; }
                if (m_data_region_1) { delete m_data_region_1; m_data_region_1 = nullptr; }
                ::munmap(mem, m_data_region_size * 2);
                return false;
            }
#endif // _WIN32 vs POSIX

#if defined(__linux__) && defined(SINTRA_RUNTIME_CACHELINE_CHECK)
            // Warn ONCE per process if we detect a cache-line mismatch.
            static std::once_flag once;
            std::call_once(once, []{
                sintra_warn_if_cacheline_mismatch_linux(assumed_cache_line_size);
            });
#endif
            return true;
        }
        catch (...) {
            return false;
        }
    }


    ipc::mapped_region*                 m_data_region_0                 = nullptr;
    ipc::mapped_region*                 m_data_region_1                 = nullptr;
    std::string                         m_directory;

protected:

    const size_t                        m_num_elements;
    const size_t                        m_data_region_size;

    T*                                  m_data                          = nullptr;
    std::string                         m_data_filename;
    size_t                              m_data_filename_hash            = 0;
    bool                                m_remove_files_on_destruction   = false;

    template <typename RingT1, typename RingT2>
    friend bool has_same_mapping(const RingT1& r1, const RingT2& r2);
};

// Compare two rings by the underlying data file identity.
template <typename RingT1, typename RingT2>
bool has_same_mapping(const RingT1& r1, const RingT2& r2)
{
    return r1.m_data_filename_hash == r2.m_data_filename_hash;
}



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring_data ////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



//==============================================================================
// Ring: adds control region (atomics, semaphores, per-reader state)
//==============================================================================

/**
 * Extends Ring_data by adding the shared “control” region which holds all
 * cross-process state: publisher sequence, reader sequences, semaphores, and
 * small stacks used by the hybrid sleeping policy.
 */
template <typename T, bool READ_ONLY_DATA>
struct Ring: Ring_data<T, READ_ONLY_DATA>
{
    enum Reader_status : uint8_t {
        READER_STATE_INACTIVE = 0,
        READER_STATE_ACTIVE   = 1,
        READER_STATE_EVICTED  = 2
    };

    // Helper: pad to a cache line to reduce false sharing in Control arrays.
    struct cache_line_sized_t {
        struct Payload {
            std::atomic<sequence_counter_type> v;
            std::atomic<uint8_t> status{READER_STATE_INACTIVE};
            std::atomic<uint8_t> trailing_octile{0};
            std::atomic<uint8_t> has_guard{0};
            std::atomic<uint32_t> owner_pid{0};
        };

        Payload data;
        std::array<uint8_t, (assumed_cache_line_size - sizeof(Payload))> padding{};

        static_assert(sizeof(Payload) <= assumed_cache_line_size,
                      "The payload of cache_line_sized_t exceeds the assumed cache line size.");
    };

    /**
     * Shared control block. All fields that are concurrently modified are atomic.
     * NOTE: Atomics are chosen such that they are address-free and (ideally) lock-free.
     */
    struct Control
    {
        // This struct is always instantiated in a memory region which is shared among processes.
        // The atomics below are used for *cross-process* communication, including in a
        // double-mapped (“magic ring”) region. See the detailed note & lock-free checks
        // in Control() about address-free atomics (N4713 [atomics.lockfree]).
        std::atomic<size_t>                  num_attached{0};

        // The sequence number of the NEXT element to be written (published by the writer).
        // Publish semantics: after writing data, the writer calls done_writing(), which
        // atomically stores this value; the last written element is (leading_sequence - 1).
        std::atomic<sequence_counter_type>   leading_sequence{0};

        // Octile read guards:
        //   An 8-byte integer where each *byte* corresponds to one octile of the ring
        //   (the ring is conceptually split into 8 equal parts). Each byte is a reader
        //   counter for that octile. While a reader holds a snapshot that trails into a
        //   given octile, it increments that byte to keep the writer from reclaiming it.
        //
        // Reader cap:
        //   Because each byte is an 8-bit counter, this imposes a hard limit of 255
        //   concurrently-guarding readers per octile. Combined with control-array sizing,
        //   the effective maximum readers is min(255, max_process_index).
        std::atomic<uint64_t>                read_access{0};

        // NOTE: ONLY RELEVANT FOR TRACKING / DIAGNOSTICS
        // Should equal the sum of the eight bytes in read_access. Not used for correctness.
        std::atomic<uint32_t>                num_readers{0};

        // Per-reader currently visible (snapshot) sequence. Cache-line sized to minimize
        // false sharing between reader slots. A reader sets its slot to the snapshot head
        // and advances it as it consumes ranges.
        cache_line_sized_t                   reading_sequences[max_process_index];

        // --- Reader Sequence Stack Management ------------------------------------
        // Protects free_rs_stack and num_free_rs during slot acquisition/release.
        std::atomic_flag                     rs_stack_spinlock = ATOMIC_FLAG_INIT;
        void rs_stack_lock()   { while (rs_stack_spinlock.test_and_set(std::memory_order_acquire)) {} }
        void rs_stack_unlock() { rs_stack_spinlock.clear(std::memory_order_release); }

        // Freelist of reader-slot indices into reading_sequences[].
        int                                  free_rs_stack[max_process_index]{};
        std::atomic<int>                     num_free_rs{0};
        // --- End Reader Sequence Stack Management --------------------------------

        bool scavenge_orphans()
        {
            bool freed = false;

            rs_stack_lock();
            auto in_freelist = [&](int idx) -> bool {
                int nf = num_free_rs.load(std::memory_order_relaxed);
                for (int i = 0; i < nf; ++i) {
                    if (free_rs_stack[i] == idx) {
                        return true;
                    }
                }
                return false;
            };

            for (int i = 0; i < max_process_index; ++i) {
                auto& slot = reading_sequences[i].data;

                if (slot.status.load(std::memory_order_acquire) == READER_STATE_INACTIVE) {
                    continue;
                }

                uint32_t pid = slot.owner_pid.load(std::memory_order_relaxed);

                bool owner_unknown = (pid == 0);
                bool dead = owner_unknown || !is_process_alive(pid);

                if (dead) {
                    uint8_t expected = 1;
                    if (slot.has_guard.compare_exchange_strong(
                            expected, uint8_t{0}, std::memory_order_acq_rel))
                    {
                        uint8_t oct = slot.trailing_octile.load(std::memory_order_relaxed);
                        read_access.fetch_sub(uint64_t(1) << (8 * oct), std::memory_order_acq_rel);
                    }

                    slot.status.store(READER_STATE_INACTIVE, std::memory_order_release);
                    slot.owner_pid.store(0, std::memory_order_relaxed);

                    if (!in_freelist(i)) {
                        int idx = num_free_rs.load(std::memory_order_relaxed);
                        free_rs_stack[idx] = i;
                        num_free_rs.fetch_add(1, std::memory_order_release);
                    }
                    freed = true;
                }
            }

            rs_stack_unlock();
            return freed;
        }

        // Used to avoid accidentally having multiple writers on the same ring
        // across processes. Only one writer may hold this at a time.
        std::atomic<uint32_t>                writer_pid{0};
        ipc::interprocess_mutex              ownership_mutex;

#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        // The following synchronization structures may only be accessed between lock()/unlock().

        // An array (pool) of semaphores to synchronize reader wakeups. The writer posts these
        // on publish; readers may also be unblocked locally in an “unordered” fashion.
        sintra_ring_semaphore                dirty_semaphores[max_process_index];

        // A stack of indices into dirty_semaphores[] that are free/ready for use.
        // Initially all semaphores are ready.
        int                                  ready_stack[max_process_index]{};
        int                                  num_ready = max_process_index;

        // A stack of indices allocated to readers that are blocking / about to block /
        // or were blocking and not yet redistributed.
        int                                  sleeping_stack[max_process_index]{};
        int                                  num_sleeping = 0;

        // A stack of indices that were posted “out of order” (e.g., after a local unblock).
        // To avoid O(n) removals from sleeping_stack, unordered posts leave the index in
        // sleeping_stack but flag the semaphore to avoid re-posting; the next ordered post
        // (e.g., on publish) drains unordered items back to ready_stack.
        int                                  unordered_stack[max_process_index]{};
        int                                  num_unordered = 0;

        // Spinlock guarding the ready/sleeping/unordered stacks.
        std::atomic_flag                     spinlock_flag = ATOMIC_FLAG_INIT;
        void lock()   { while (spinlock_flag.test_and_set(std::memory_order_acquire)) {} }
        void unlock() { spinlock_flag.clear(std::memory_order_release); }
#endif


        Control()
        {
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
            for (int i = 0; i < max_process_index; i++) { ready_stack   [i]  =  i; }
            for (int i = 0; i < max_process_index; i++) { sleeping_stack[i]  = -1; }
            for (int i = 0; i < max_process_index; i++) { unordered_stack[i] = -1; }
#endif

            for (int i = 0; i < max_process_index; i++) { reading_sequences[i].data.v = invalid_sequence; }
            for (int i = 0; i < max_process_index; i++) { free_rs_stack[i] = i; }

            writer_pid.store(0, std::memory_order_relaxed);


            // See the 'Note' in N4713 32.5 [Lock-free property], Par. 4.
            // The program is only valid if the conditions below are true.
            
            // On a second read... quoting the aforementioned note from the draft:
            //
            // "Operations that are lock-free should also be address-free. That is, atomic operations on the
            // same memory location via two different addresses will communicate atomically. The implementation
            // should not depend on any per-process state. This restriction enables communication by memory
            // that is mapped into a process more than once and by memory that is shared between two processes."
            //
            // ...so if they are not lock-free, could it be that they are not address-free?
            // My guess is that the author's intention was to stress that atomic operations are address-free
            // either way. Nevertheless, these assertions are just to be on the safe side.

            // Sanity (C++ note: lock-free guarantee is implementation-specific)
            assert(num_attached.is_lock_free());
            assert(leading_sequence.is_lock_free());

            num_free_rs.store(max_process_index, std::memory_order_relaxed);
        }

        static_assert(max_process_index <= 255,
            "max_process_index must be <= 255 because read_access uses 8-bit octile counters.");
    };

    Ring(const std::string& directory,
         const std::string& data_filename,
         size_t             num_elements)
    : Ring_data<T, READ_ONLY_DATA>(directory, data_filename, num_elements)
    {
        m_control_filename = this->m_data_filename + "_control";

        fs::path pc(m_control_filename);
        const bool already_exists = fs::exists(pc) && fs::is_regular_file(pc) && fs::file_size(pc);
        const bool created_or_ok  = already_exists || create();

        if (!created_or_ok || !attach()) {
            throw ring_acquisition_failure_exception();
        }

        if (!already_exists) {
            // Placement-new the shared Control block
            try {
                new (m_control) Control;
            }
            catch (...) {
                throw ring_acquisition_failure_exception();
            }
        }

        m_control->num_attached++;
    }


    ~Ring()
    {
        // The *last* detaching process deletes both control and data files.
        if (m_control->num_attached-- == 1) {
            this->m_remove_files_on_destruction = true;
        }

        delete m_control_region;
        m_control_region = nullptr;

        if (this->m_remove_files_on_destruction) {
            std::error_code ec;
            (void)fs::remove(fs::path(m_control_filename), ec);
        }
    }

    sequence_counter_type get_leading_sequence() const {
        return m_control->leading_sequence.load(std::memory_order_acquire);
    }

    /**
     * Map a sequence number to the in-memory element pointer.
     * Returns nullptr if the sequence is out of the readable historical window.
     */
    T* get_element_from_sequence(sequence_counter_type seq) const
    {
        const auto leading = m_control->leading_sequence.load(std::memory_order_acquire);
        if (leading == 0) return nullptr; // nothing published yet

        const auto last_published = leading - 1;
        const auto ring = this->m_num_elements;

        const auto first_valid = (leading > ring) ? (leading - ring) : 0;
        if (seq < first_valid || seq > last_published) {
            return nullptr;
        }
        return this->m_data + mod_u64(seq, this->m_num_elements);
    }

private:

    // Create the control file (debug-filled in !NDEBUG).
    bool create()
    {
        try {
            ipc::file_handle_t fh_control =
                ipc::ipcdetail::create_new_file(m_control_filename.c_str(), ipc::read_write);
            if (fh_control == ipc::ipcdetail::invalid_file())
                return false;

#ifdef NDEBUG
            if (!ipc::ipcdetail::truncate_file(fh_control, sizeof(Control)))
                return false;
#else
            const char* ustr = "UNINITIALIZED";
            const size_t dv = std::strlen(ustr);
            std::unique_ptr<char[]> tmp(new char[sizeof(Control)]);
            for (size_t i = 0; i < sizeof(Control); ++i) {
                tmp[i] = ustr[i % dv];
            }
            ipc::ipcdetail::write_file(fh_control, tmp.get(), sizeof(Control));
#endif
            return ipc::ipcdetail::close_file(fh_control);
        }
        catch (...) {
        }
        return false;
    }

    // Map the control file read-write.
    bool attach()
    {
        assert(m_control_region == nullptr);

        try {
            if (fs::file_size(m_control_filename) != sizeof(Control)) {
                return false;
            }

            ipc::file_mapping fm_control(m_control_filename.c_str(), ipc::read_write);
            m_control_region = new ipc::mapped_region(fm_control, ipc::read_write, 0, 0);
            m_control = (Control*)m_control_region->get_address();

            return true;
        }
        catch (...) {
            return false;
        }
    }

private:
    ipc::mapped_region*  m_control_region = nullptr;
    std::string          m_control_filename;
protected:
    Control*             m_control        = nullptr;
};



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring /////////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_R /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



//==============================================================================
// Reader API
//==============================================================================

template <typename T>
struct Ring_R : Ring<T, true>
{
    // =========================================================================
    // MODIFIED CONSTRUCTOR: Acquires a reader slot for the object's lifetime.
    // =========================================================================
    Ring_R(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements,
           size_t             max_trailing_elements = 0)
        :
        Ring<T, true>::Ring(directory, data_filename, num_elements),
        m_max_trailing_elements(max_trailing_elements),
        c(*this->m_control)
    {
        assert(num_elements % 8 == 0);
        assert(max_trailing_elements <= 3 * num_elements / 4);

        // Acquire a reader slot from the freelist. This happens ONCE per Ring_R object.
        bool scavenged = false;
        while (true) {
            c.rs_stack_lock();
            if (c.num_free_rs.load(std::memory_order_acquire) > 0) {
                int current_num_free =
                    c.num_free_rs.fetch_sub(1, std::memory_order_relaxed) - 1;
                m_rs_index = c.free_rs_stack[current_num_free];

                // Mark our slot as ACTIVE while rs_stack_lock is still held so the
                // scavenger cannot reclaim it before we publish the ownership.
                auto& slot = c.reading_sequences[m_rs_index].data;
                slot.owner_pid.store(get_current_pid(), std::memory_order_relaxed);
                slot.status.store(
                    Ring<T, true>::READER_STATE_ACTIVE, std::memory_order_release);

                c.rs_stack_unlock();
                break;
            }
            c.rs_stack_unlock();

            if (scavenged || !c.scavenge_orphans()) {
                throw ring_acquisition_failure_exception(); // No slots available.
            }
            scavenged = true;
        }

        m_reading_sequence = &c.reading_sequences[m_rs_index].data.v;
    }


    // =========================================================================
    // MODIFIED DESTRUCTOR: Releases the reader slot.
    // =========================================================================
    ~Ring_R()
    {
        // Ensure any active read guard is released.
        if (m_reading) {
            done_reading();
        }

        // Return the reader slot to the freelist and mark as inactive.
        if (m_rs_index != -1) {
            // Take the freelist lock FIRST to serialize with scavenger and other releasers.
            c.rs_stack_lock();

            // Mark slot as inactive and clear ownership while the freelist is locked,
            // so scavenger cannot race a half-updated slot.
            auto& slot = c.reading_sequences[m_rs_index].data;
            slot.owner_pid.store(0, std::memory_order_relaxed);
            slot.status.store(
                Ring<T, true>::READER_STATE_INACTIVE, std::memory_order_release);

            // Push only if not already in the freelist (defensive: avoid duplicates).
            int nf = c.num_free_rs.load(std::memory_order_relaxed);
            bool already = false;
            for (int i = 0; i < nf; ++i) {
                if (c.free_rs_stack[i] == m_rs_index) { already = true; break; }
            }
            if (!already) {
                c.free_rs_stack[nf] = m_rs_index;
                c.num_free_rs.fetch_add(1, std::memory_order_release);
            }

            c.rs_stack_unlock();
        }
    }

    // Convenience overload
    Range<T> start_reading() { return start_reading(m_max_trailing_elements); }

    /**
     * Begin a snapshot read up to num_trailing_elements behind the current
     * leading sequence. While a snapshot is held, this reader guards exactly one
     * trailing octile to prevent the writer from reclaiming it.
     *
     * IMPORTANT: call done_reading() when you are finished with the snapshot.
     */
    Range<T> start_reading(size_t num_trailing_elements)
    {
        bool f = false;
        while (!m_reading_lock.compare_exchange_strong(f, true)) { f = false; }

        if (m_reading) {
            m_reading_lock = false;
            throw std::logic_error(
                "Sintra Ring: Cannot call start_reading() again before calling done_reading().");
        }

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        if (c.reading_sequences[m_rs_index].data.status.load() == Ring<T, true>::READER_STATE_EVICTED) {
            m_reading_lock = false;
            throw ring_reader_evicted_exception();
        }
#endif
        m_reading = true;

        // NOTE: Readers may only snapshot up to m_max_trailing_elements (typically 3/4 of the ring).
        // If this fires, you probably called start_reading()/try_snapshot_e(reader, N) with N larger than
        // the reader's configured trailing cap. Request a smaller tail (e.g., 1 for "last element") or
        // construct the reader with a larger trailing cap.
        assert(num_trailing_elements <= m_max_trailing_elements);

        // This function NO LONGER acquires m_rs_index. It uses the one from the constructor.

        Range<T> ret;

        while (true) {
            auto leading_sequence = c.leading_sequence.load(std::memory_order_acquire);

            auto range_first_sequence =
                std::max<int64_t>(0, int64_t(leading_sequence) - int64_t(num_trailing_elements));

            size_t trailing_idx = mod_pos_i64(
                int64_t(range_first_sequence) - int64_t(m_max_trailing_elements), this->m_num_elements);

            uint8_t trailing_octile = static_cast<uint8_t>((8 * trailing_idx) / this->m_num_elements);
            uint64_t guard_mask     = uint64_t(1) << (8 * trailing_octile);

            c.read_access.fetch_add(guard_mask, std::memory_order_acq_rel);

            // Publish the octile index to our shared slot for the writer to see.
            c.reading_sequences[m_rs_index].data.trailing_octile.store(
                trailing_octile, std::memory_order_relaxed);
            c.reading_sequences[m_rs_index].data.has_guard.store(1, std::memory_order_release);

            auto confirmed_leading_sequence = c.leading_sequence.load(std::memory_order_acquire);
            auto confirmed_range_first_sequence = std::max<int64_t>(
                0, int64_t(confirmed_leading_sequence) - int64_t(num_trailing_elements));

            size_t confirmed_trailing_idx = mod_pos_i64(
                int64_t(confirmed_range_first_sequence) - int64_t(m_max_trailing_elements), this->m_num_elements);
            uint8_t confirmed_trailing_octile =
                static_cast<uint8_t>((8 * confirmed_trailing_idx) / this->m_num_elements);

            if (confirmed_trailing_octile == trailing_octile) {
                ret.begin = this->m_data +
                            mod_pos_i64(confirmed_range_first_sequence, this->m_num_elements);
                ret.end = ret.begin + (confirmed_leading_sequence - confirmed_range_first_sequence);

                m_trailing_octile = trailing_octile;
                m_reading_sequence->store(confirmed_leading_sequence);
                break;
            }

            // Trailing guard requirement changed between reads; drop and retry.
            c.read_access.fetch_sub(guard_mask, std::memory_order_acq_rel);
            c.reading_sequences[m_rs_index].data.has_guard.store(0, std::memory_order_release);
        }

        m_reading_lock = false;
        return ret;
    }

    /**
     * Release the snapshot. You MUST call this when you are done reading the
     * current view, otherwise you will impede the writer's progress (and
     * potentially other readers).
     *
     * SHUTDOWN MODE: If called with m_reading == false, this signals the reader
     * to stop waiting for new data. This is used during shutdown to unblock
     * reader threads that may be waiting in wait_for_new_data().
     */
    void done_reading()
    {
        // Fast path: if no snapshot is active, there is nothing to release.  This
        // happens frequently when shutdown unblocks a reader that is idle in
        // wait_for_new_data().  In that case we simply propagate the stop signal
        // without touching the local lock, avoiding spurious atomic operations
        // against partially torn down objects during crash recovery.
        if (!m_reading.load(std::memory_order_acquire)) {
            request_stop();
            return;
        }

        bool expected = false;
        while (!m_reading_lock.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
        {
            expected = false;
        }

        if (m_reading) {
            c.read_access.fetch_sub(uint64_t(1) << (8 * m_trailing_octile), std::memory_order_acq_rel);
            c.reading_sequences[m_rs_index].data.has_guard.store(0, std::memory_order_release);
            m_reading.store(false, std::memory_order_release);
        }
        else {
            // done_reading() called without active snapshot => shutdown signal
            request_stop();
        }

        m_reading_lock.store(false, std::memory_order_release);
    }

    sequence_counter_type reading_sequence()     const { return m_reading_sequence->load(); }
    sequence_counter_type get_reading_sequence() const { return m_reading_sequence->load(); }

    /**
     * Block until new data is available (or until unblocked) and return the new
     * readable range. After consuming the returned range, call
     * done_reading_new_data() to update the trailing guard if needed.
     *
     * SHUTDOWN BEHAVIOR: If m_stopping is set, returns an empty range immediately
     * without blocking. This allows reader threads to exit gracefully during shutdown.
     */
    const Range<T> wait_for_new_data()
    {
        // Check for shutdown signal before blocking
        if (m_stopping.load(std::memory_order_acquire)) {
            return Range<T>{};  // Return empty range to signal shutdown
        }

#if SINTRA_RING_READING_POLICY == SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        while (m_reading_sequence->load() == c.leading_sequence.load()) {
            // Check for shutdown during spin
            if (m_stopping.load(std::memory_order_acquire)) {
                return Range<T>{};
            }
        }

#else // HYBRID or ALWAYS_SLEEP
    #if SINTRA_RING_READING_POLICY == SINTRA_RING_READING_POLICY_HYBRID
        double tl = get_wtime() + spin_before_sleep * 0.5;
        while (m_reading_sequence->load() == c.leading_sequence.load() && get_wtime() < tl) {
            // Check for shutdown during spin phase
            if (m_stopping.load(std::memory_order_acquire)) {
                return Range<T>{};
            }
        }
    #endif

        // Transition to sleeping if still no data
        c.lock();
        m_sleepy_index = -1;
        if (m_reading_sequence->load() == c.leading_sequence.load()) {
            // Check for shutdown before registering as sleeping
            if (m_stopping.load(std::memory_order_acquire)) {
                c.unlock();
                return Range<T>{};
            }
            m_sleepy_index = c.ready_stack[--c.num_ready];
            c.sleeping_stack[c.num_sleeping++] = m_sleepy_index;
        }
        c.unlock();

        if (m_sleepy_index >= 0) {
            // Shutdown could have been signaled after we registered but before waiting.
            if (m_stopping.load(std::memory_order_acquire)) {
                c.lock();
                if (m_sleepy_index >= 0) {
                    c.dirty_semaphores[m_sleepy_index].post_unordered();
                }
                c.unlock();
            }

            if (c.dirty_semaphores[m_sleepy_index].wait()) { // unordered wake
                c.lock();
                c.unordered_stack[c.num_unordered++] = m_sleepy_index;
                c.unlock();
            }
            else {
                c.lock();
                c.ready_stack[c.num_ready++] = m_sleepy_index;
                c.unlock();
            }
            m_sleepy_index = -1;

            // Check for shutdown after waking from semaphore
            if (m_stopping.load(std::memory_order_acquire)) {
                return Range<T>{};
            }
        }
#endif

        auto num_range_elements =
            size_t(c.leading_sequence.load(std::memory_order_acquire) - m_reading_sequence->load());

        Range<T> ret;
        if (num_range_elements == 0) {
            // Could happen if we were explicitly unblocked
            return ret;
        }

        ret.begin = this->m_data + mod_u64(m_reading_sequence->load(), this->m_num_elements);
        ret.end   = ret.begin + num_range_elements;
        m_reading_sequence->fetch_add(num_range_elements);  // +=
        return ret;
    }

    /**
     * After wait_for_new_data(), call this to release the trailing guard when
     * crossing to a new octile.
     */
    void done_reading_new_data()
    {
        size_t t_idx = mod_pos_i64(int64_t(m_reading_sequence->load()) - int64_t(m_max_trailing_elements), this->m_num_elements);
        size_t new_trailing_octile = (8 * t_idx) / this->m_num_elements;

        if (new_trailing_octile != m_trailing_octile) {
            auto diff =
                (uint64_t(1) << (8 * new_trailing_octile)) -
                (uint64_t(1) << (8 *   m_trailing_octile));
            c.read_access.fetch_add(diff, std::memory_order_acq_rel);
            c.reading_sequences[m_rs_index].data.trailing_octile.store(
                static_cast<uint8_t>(new_trailing_octile), std::memory_order_relaxed);
            m_trailing_octile = new_trailing_octile;
        }
    }

    /**
     * If another thread is in wait_for_new_data(), force it to return an empty
     * range without publishing any new data. Only affects this reader instance.
     */
    void unblock_local()
    {
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        c.lock();
        if (m_sleepy_index >= 0) {
            c.dirty_semaphores[m_sleepy_index].post_unordered();
        }
        c.unlock();
#endif
    }

    void request_stop()
    {
        m_stopping.store(true, std::memory_order_release);
        unblock_local();
    }

private:
    const size_t                        m_max_trailing_elements;
    std::atomic<sequence_counter_type>* m_reading_sequence      = &s_zero_rs;
    size_t                              m_trailing_octile       = 0;

protected:
    std::atomic<bool>                   m_reading               = false;
    std::atomic<bool>                   m_reading_lock          = false;

private:
    int                                 m_sleepy_index          = -1;
    int                                 m_rs_index              = -1;
    std::atomic<bool>                   m_stopping              = false;

    inline static std::atomic<sequence_counter_type> s_zero_rs{0};

    typename Ring<T, true>::Control& c;
};



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring_R ///////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_W /////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



//==============================================================================
// Writer API
//==============================================================================

template <typename T>
struct Ring_W : Ring<T, false>
{
    Ring_W(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements)
    : Ring<T, false>::Ring(directory, data_filename, num_elements),
      c(*this->m_control)
    {
        ensure_writer_mutex_consistency();

        // Single writer across processes
        if (!c.ownership_mutex.try_lock()) {
            throw ring_acquisition_failure_exception();
        }

        c.writer_pid.store(get_current_pid(), std::memory_order_release);
    }

    ~Ring_W()
    {
        // Wake any sleeping readers to avoid deadlocks during teardown
        unblock_global();
        c.ownership_mutex.unlock();
        c.writer_pid.store(0, std::memory_order_release);
    }

    /**
     * Write a buffer of elements to the ring. Returns the pointer to the first
     * element written (inside the linearized mapping) so the caller can
     * optionally post-process in place before publishing.
     *
     * IMPORTANT: call done_writing() to publish the new leading sequence.
     */
    T* write(const T* src_buffer, size_t num_src_elements)
    {
        T* write_location = prepare_write(num_src_elements);
        try {
            for (size_t i = 0; i < num_src_elements; ++i) {
                write_location[i] = src_buffer[i];
            }
            return write_location;
        }
        catch (...) {
            m_pending_new_sequence -= num_src_elements;
            const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
            m_octile = (8 * head) / this->m_num_elements;
            m_writing_thread = std::thread::id();
            throw;
        }
    }

    /**
     * In-place construction variant for arbitrary payloads composed of T-sized
     * units. sizeof(T2) must be a multiple of sizeof(T). Returns the constructed
     * T2* within the ring (still requires done_writing()).
     */
    template <typename T2, typename... Args>
    T2* write(size_t num_extra_elements, Args&&... args)
    {
        static_assert(sizeof(T2) % sizeof(T) == 0, "T2 must be a multiple of T");
        auto num_elements = sizeof(T2) / sizeof(T) + num_extra_elements;
        T2* write_location = (T2*)prepare_write(num_elements);
        try {
            return new (write_location) T2{std::forward<Args>(args)...};
        }
        catch (...) {
            m_pending_new_sequence -= num_elements;
            const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
            m_octile = (8 * head) / this->m_num_elements;
            m_writing_thread = std::thread::id();
            throw;
        }
    }

    /**
     * Publish the pending write by updating leading_sequence.
     * Returns the new leading sequence value.
     *
     * Publish semantics:
     *  • All element stores must be completed before this atomic store (the
     *    default seq-cst store is sufficient here).
     */
    sequence_counter_type done_writing()
    {
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        c.lock();
        assert(m_writing_thread == std::this_thread::get_id());
        c.leading_sequence.store(m_pending_new_sequence);
        // Wake sleeping readers in a deterministic order
        for (int i = 0; i < c.num_sleeping; i++) {
            c.dirty_semaphores[c.sleeping_stack[i]].post_ordered();
        }
        c.num_sleeping = 0;
        while (c.num_unordered) {
            c.ready_stack[c.num_ready++] = c.unordered_stack[--c.num_unordered];
        }
        c.unlock();
#else
        c.leading_sequence.store(m_pending_new_sequence);
#endif
        m_writing_thread = std::thread::id();
        return m_pending_new_sequence;
    }

    /**
     * Force any reader waiting in wait_for_new_data() to wake up and return
     * an empty range (global unblock).
     */
    void unblock_global()
    {
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        c.lock();
        for (int i = 0; i < c.num_sleeping; i++) {
            c.dirty_semaphores[c.sleeping_stack[i]].post_ordered();
        }
        c.num_sleeping = 0;
        while (c.num_unordered) {
            c.ready_stack[c.num_ready++] = c.unordered_stack[--c.num_unordered];
        }
        c.unlock();
#endif
    }

    /**
     * Return a read-only view of up to the most recent 3/4 of the ring. Useful
     * for diagnostics from the writer side (does not require locks).
     */
    Range<T> get_readable_range()
    {
        const auto leading = c.leading_sequence.load(std::memory_order_acquire);
        const auto ring = this->m_num_elements;
        const auto range_first = (leading > (3*ring/4)) ? (leading - (3*ring/4)) : 0;

        Range<T> ret;
        ret.begin = this->m_data + mod_u64(range_first, ring);
        ret.end   = ret.begin + (leading - range_first);
        return ret;
    }

private:
    void ensure_writer_mutex_consistency()
    {
#ifdef _WIN32
        constexpr uint32_t recovery_flag = 0x80000000u;
        constexpr uint32_t recovery_pid_mask = recovery_flag - 1;
        constexpr uint32_t legacy_recovery_sentinel = std::numeric_limits<uint32_t>::max();
        static_assert(recovery_flag != 0, "Recovery flag must reserve a representable bit");
        const uint32_t self_pid = get_current_pid();

        // Reuse the high bit of writer_pid to encode recovery ownership without
        // changing the shared-memory layout. Windows PIDs are strictly less than
        // 2^31, so masking with recovery_pid_mask preserves the real PID value.
        auto encode_recovery_value = [&](uint32_t pid) {
            return recovery_flag | (pid & recovery_pid_mask);
        };

        auto extract_recovering_pid = [&](uint32_t value) -> uint32_t {
            if ((value & recovery_flag) == 0) {
                return 0;
            }

            uint32_t pid = value & recovery_pid_mask;
            if (value == legacy_recovery_sentinel && pid == recovery_pid_mask) {
                // Old layout used 0xFFFFFFFF as a sentinel without encoding a PID.
                // Treat it as unknown so we can reclaim it.
                return 0;
            }
            return pid;
        };

        auto finalize_recovery = [&]() {
            c.ownership_mutex.~interprocess_mutex();
            new (&c.ownership_mutex) ipc::interprocess_mutex();
            c.writer_pid.store(0, std::memory_order_release);
        };

        for (;;) {
            uint32_t observed = c.writer_pid.load(std::memory_order_acquire);
            if (observed == 0) {
                return;
            }
            if (is_process_alive(observed)) {
                return;
            }
            if ((observed & recovery_flag) != 0) {
                uint32_t recovering_pid = extract_recovering_pid(observed);
                if (recovering_pid == self_pid) {
                    finalize_recovery();
                    return;
                }

                if (recovering_pid != 0 && is_process_alive(recovering_pid)) {
                    std::this_thread::yield();
                    continue;
                }

                uint32_t expected = observed;
                if (!c.writer_pid.compare_exchange_strong(
                        expected,
                        encode_recovery_value(self_pid),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    continue;
                }

                finalize_recovery();
                return;
            }

            uint32_t expected_writer = observed;
            if (c.writer_pid.compare_exchange_strong(
                    expected_writer,
                    encode_recovery_value(self_pid),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                finalize_recovery();
                return;
            }
        }
#else
        (void)c;
#endif
    }

    /**
     * Reserve space for a write of num_elements_to_write elements.
     * Precondition: num_elements_to_write <= ring_size/8 (single octile).
     * Guarantees:
     *  • Returns a pointer to a contiguous range within the linearized mapping.
     *  • Spins only when crossing to a new octile that is currently guarded
     *    by at least one reader.
     */
    T* prepare_write(size_t num_elements_to_write)
    {
        assert(num_elements_to_write <= this->m_num_elements / 8);

        // Enforce exclusive writer (cheap fast-path loop)
        while (m_writing_thread != std::this_thread::get_id()) {
            auto invalid = std::thread::id();
            m_writing_thread.compare_exchange_strong(invalid, std::this_thread::get_id());
        }

        const size_t index = mod_u64(m_pending_new_sequence, this->m_num_elements);
        m_pending_new_sequence += num_elements_to_write;

        const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
        size_t new_octile = (8 * head) / this->m_num_elements;

        // Only check when crossing to a new octile (fast path otherwise)
        if (m_octile != new_octile) {
            auto range_mask = (uint64_t(0xff) << (8 * new_octile));
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
            uint64_t spin_count = 0;
            const uint64_t spin_loop_budget = ring_detail::get_eviction_spin_loop_budget();
#endif
            while (c.read_access.load(std::memory_order_acquire) & range_mask) {
                // Busy-wait until the target octile is unguarded
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
                if (++spin_count > spin_loop_budget) {
                    // Writer is stuck. Time to find and evict the slow reader(s).
                    // This is a slow path, taken only in exceptional circumstances.

                    // The sequence number that marks a reader as "too far behind".
                    // Any reader with a sequence older than this is a candidate for eviction.
                    // A safe threshold is one full ring buffer behind the writer's current head.
                    sequence_counter_type eviction_threshold =
                        (m_pending_new_sequence >
                         sequence_counter_type(SINTRA_EVICTION_LAG_RINGS) * this->m_num_elements)
                            ? (m_pending_new_sequence - sequence_counter_type(SINTRA_EVICTION_LAG_RINGS) *
                                                           this->m_num_elements)
                            : 0;

                    for (int i = 0; i < max_process_index; ++i) {
                        // Check if this reader slot is active
                        if (c.reading_sequences[i].data.status.load(std::memory_order_acquire)
                            == Ring<T, false>::READER_STATE_ACTIVE)
                        {
                            sequence_counter_type reader_seq =
                                c.reading_sequences[i].data.v.load(std::memory_order_acquire);

                            if (reader_seq < eviction_threshold) {
                                // Evict only if the reader currently holds a guard
                                uint8_t expected = 1;
                                if (c.reading_sequences[i].data.has_guard.compare_exchange_strong(
                                        expected, uint8_t{0}, std::memory_order_acq_rel))
                                {
                                    c.reading_sequences[i].data.status.store(
                                        Ring<T, false>::READER_STATE_EVICTED, std::memory_order_release);

                                    size_t evicted_reader_octile =
                                        c.reading_sequences[i].data.trailing_octile.load(std::memory_order_relaxed);
                                    c.read_access.fetch_sub(uint64_t(1) << (8 * evicted_reader_octile), std::memory_order_acq_rel);
                                }
                            }
                        }
                    }
                    spin_count = 0; // Reset spin count after an eviction pass
                    c.scavenge_orphans();
                }
#endif
            }
            m_octile = new_octile;
        }
        return this->m_data + index;
    }

private:
    std::atomic<std::thread::id>    m_writing_thread;
    size_t                          m_octile                 = 0;
    sequence_counter_type           m_pending_new_sequence   = 0;

    typename Ring<T, false>::Control& c;
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Ring_W ///////////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


 //////////////////////////////////////////////////////////////////////////
///// BEGIN Local_Ring_W ///////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//

//==============================================================================
// Local writer alias (kept for API parity / future extension)
//==============================================================================

template <typename T>
struct Local_Ring_W : Ring_W<T>
{
    using Ring_W<T>::Ring_W;
};


  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Local_Ring_W /////////////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////

 //////////////////////////////////////////////////////////////////////////
///// BEGIN Convenience Utilities //////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//



#if __cplusplus >= 202002L
#define SINTRA_NODISCARD [[nodiscard]]
#else
#define SINTRA_NODISCARD
#endif

// RAII snapshot: calls reader.done_reading() iff start_reading() succeeded.
// NOTE: Only one active snapshot per Ring_R<T> instance. Attempting to start a new snapshot before
// done_reading() will throw (use a separate Ring_R<T> instance if you need concurrent snapshots).

template <class Reader>
class SINTRA_NODISCARD Ring_R_snapshot {
public:
    using element_t = typename Reader::element_t;

    Ring_R_snapshot() = default;
    Ring_R_snapshot(Reader& reader, Range<element_t> rg) noexcept
        : m_reader(&reader), m_range(rg), m_active(true) {}

    Ring_R_snapshot(const Ring_R_snapshot&) = delete;
    Ring_R_snapshot& operator=(const Ring_R_snapshot&) = delete;

    Ring_R_snapshot(Ring_R_snapshot&& other) noexcept
        : m_reader(other.m_reader), m_range(other.m_range), m_active(other.m_active) {
        other.m_reader = nullptr;
        other.m_active = false;
    }

    Ring_R_snapshot& operator=(Ring_R_snapshot&& other) noexcept {
        if (this != &other) {
            if (m_active && m_reader) { m_reader->done_reading(); }
            m_reader       = other.m_reader;
            m_range        = other.m_range;
            m_active       = other.m_active;
            other.m_reader = nullptr;
            other.m_active = false;
        }
        return *this;
    }

    ~Ring_R_snapshot() noexcept {
        if (m_active && m_reader) { m_reader->done_reading(); }
    }

    SINTRA_NODISCARD explicit operator bool() const noexcept {
        return m_active && m_range.begin && m_range.end && (m_range.end > m_range.begin);
    }

    SINTRA_NODISCARD Range<element_t> range() const noexcept { return m_range; }
    SINTRA_NODISCARD element_t*       begin() const noexcept { return m_range.begin; }
    SINTRA_NODISCARD element_t*       end()   const noexcept { return m_range.end; }

    // If caller finished early, prevent done_reading() in dtor.
    void dismiss() noexcept { m_active = false; }

private:
    Reader*          m_reader = nullptr;
    Range<element_t> m_range{};
    bool             m_active = false;
};

// Throwing helper: propagates exceptions from start_reading(...)
template <class Reader, class... Args>
SINTRA_NODISCARD inline Ring_R_snapshot<Reader>
make_snapshot(Reader& reader, Args&&... args) {
    auto rg = reader.start_reading(std::forward<Args>(args)...);
    return Ring_R_snapshot<Reader>(reader, rg);
}

// Optional error code for reporting/logging at call sites.
enum class Ring_R_snapshot_error : uint8_t { none, evicted, exception };

// Nothrow helper: returns {snapshot, error}; never logs by itself.
template <class Reader, class... Args>
SINTRA_NODISCARD inline std::pair<Ring_R_snapshot<Reader>, Ring_R_snapshot_error>
try_snapshot_e(Reader& reader, Args&&... args) noexcept
{
    try {
        auto rg = reader.start_reading(std::forward<Args>(args)...);
        return std::pair<Ring_R_snapshot<Reader>, Ring_R_snapshot_error>(
            Ring_R_snapshot<Reader>(reader, rg), Ring_R_snapshot_error::none);
    }
    catch (const ring_reader_evicted_exception&) {
        return std::pair<Ring_R_snapshot<Reader>, Ring_R_snapshot_error>(
            Ring_R_snapshot<Reader>{}, Ring_R_snapshot_error::evicted);
    }
    catch (...) {
        return std::pair<Ring_R_snapshot<Reader>, Ring_R_snapshot_error>(
            Ring_R_snapshot<Reader>{}, Ring_R_snapshot_error::exception);
    }
}



  //\       //\       //\       //\       //\       //\       //\       //
 ////\     ////\     ////\     ////\     ////\     ////\     ////\     ////
//////\   //////\   //////\   //////\   //////\   //////\   //////\   //////
////////////////////////////////////////////////////////////////////////////
///// END Convenience Utilities ////////////////////////////////////////////
 //////////////////////////////////////////////////////////////////////////


} // namespace sintra

