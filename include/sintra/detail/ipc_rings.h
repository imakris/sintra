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
 *
 * READER CAP
 * ----------
 *  • Effective maximum readers = min(255, max_process_index).
 *    - The control block allocates per-reader state arrays sized by
 *      max_process_index; and the octile guard uses an 8×1-byte pattern which
 *      caps actively guarded readers at 255.
 *
 * LIFECYCLE & CLEANUP
 * -------------------
 *  • Two files exist: a data file (double-mapped) and a control file
 *    (atomics, semaphores, per-reader state).
 *  • Any process may create; subsequent processes attach.
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
 *      - Reserve a 2× span with mmap(NULL, 2*size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS,…).
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
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <filesystem>
#include <cstring>       // std::strlen
#include <cstdio>        // std::FILE, std::fprintf
#include <cassert>
#include <vector>
#include <algorithm>     // std::reverse
#include <functional>
#include <limits>
#include <mutex>         // std::once_flag, std::call_once
#include <system_error>

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
#endif

// ─────────────────────────────────────────────────────────────────────────────

namespace sintra {

namespace fs  = std::filesystem;
namespace ipc = boost::interprocess;

using sequence_counter_type = uint64_t;
constexpr auto invalid_sequence = ~sequence_counter_type(0);

//==============================================================================
// Linux-only runtime cache-line helpers (placed AFTER includes, as required)
//==============================================================================
#if defined(__linux__)
/**
 * Attempt to detect the L1 data cache line size at runtime.
 * Order:
 *   1) sysconf(_SC_LEVEL1_DCACHE_LINESIZE) if available.
 *   2) sysfs: /sys/devices/system/cpu/cpu0/cache/index*/coherency_line_size
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
inline bool check_or_create_directory(const std::string& dir_name)
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
 */
inline bool remove_directory(const std::string& dir_name)
{
    std::error_code ec;
    auto removed = fs::remove_all(dir_name.c_str(), ec);
    return !!removed;
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

class ring_acquisition_failure_exception {};

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

            // NOTE: On Windows, Boost’s "page size" here is the allocation granularity.
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
            m_data_region_0 = new ipc::mapped_region(file, data_rights, 0, m_data_region_size, ptr, map_extra_options);
            m_data_region_1 = new ipc::mapped_region(file, data_rights, 0, 0,
                                   ((char*)m_data_region_0->get_address()) + m_data_region_size, map_extra_options);
            m_data = (T*)m_data_region_0->get_address();

            // Basic sanity checks (compile-time asserts are not practical here).
            assert(m_data_region_0->get_address() == ptr);
            assert(m_data_region_1->get_address() == ptr + m_data_region_size);
            assert(m_data_region_0->get_size() == m_data_region_size);
            assert(m_data_region_1->get_size() == m_data_region_size);

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
    // Helper: pad to a cache line to reduce false sharing in Control arrays.
    template <typename U>
    struct cache_line_sized_t {
        U       v;
        uint8_t padding[assumed_cache_line_size - sizeof(U)];
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
    cache_line_sized_t<sequence_counter_type>
                                             reading_sequences[max_process_index];

    // --- Reader Sequence Stack Management ------------------------------------
    // Protects free_rs_stack and num_free_rs during slot acquisition/release.
    std::atomic_flag                     rs_stack_spinlock = ATOMIC_FLAG_INIT;
    void rs_stack_lock()   { while (rs_stack_spinlock.test_and_set(std::memory_order_acquire)) {} }
    void rs_stack_unlock() { rs_stack_spinlock.clear(std::memory_order_release); }

    // Freelist of reader-slot indices into reading_sequences[].
    int                                  free_rs_stack[max_process_index]{};
    std::atomic<int>                     num_free_rs{0};
    // --- End Reader Sequence Stack Management --------------------------------

    // Used to avoid accidentally having multiple writers on the same ring
    // across processes. Only one writer may hold this at a time.
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
            for (int i = 0; i < max_process_index; i++) { ready_stack   [i] =  i; }
            for (int i = 0; i < max_process_index; i++) { sleeping_stack[i] = -1; }
            for (int i = 0; i < max_process_index; i++) { unordered_stack[i] = -1; }
#endif

            for (int i = 0; i < max_process_index; i++) { reading_sequences[i].v = invalid_sequence; }
            for (int i = 0; i < max_process_index; i++) { free_rs_stack[i] = i; }


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
        return m_control->leading_sequence.load();
    }

    /**
     * Map a sequence number to the in-memory element pointer.
     * Returns nullptr if the sequence is out of the readable historical window.
     */
    T* get_element_from_sequence(sequence_counter_type sequence) const
    {
        auto leading_sequence = m_control->leading_sequence.load();
        if (sequence < this->m_num_elements) {
            return this->m_data + sequence;
        }
        auto first_sequence = leading_sequence - this->m_num_elements - 1;
        if (sequence < first_sequence || sequence > leading_sequence) {
            return nullptr;
        }
        return this->m_data + std::max<int64_t>(0, int64_t(sequence)) % this->m_num_elements;
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
    Ring_R(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements,
           size_t             max_trailing_elements = 0)
    :
        Ring<T, true>::Ring(directory, data_filename, num_elements),
        m_max_trailing_elements(max_trailing_elements),
        c(*this->m_control)
    {
        // Enforce octile model (see "CONSTRAINTS & RECOMMENDATIONS" above).
        assert(num_elements % 8 == 0);

        // Reader trailing window must fit within 3/4 of the ring (leaves guard).
        assert(max_trailing_elements <= 3 * num_elements / 4);
    }

    ~Ring_R() { done_reading(); }

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

        m_reading = true;
        assert(num_trailing_elements <= m_max_trailing_elements);

        // Acquire a reader slot from the freelist (bounded by max_process_index).
        c.rs_stack_lock();
        if (c.num_free_rs.load(std::memory_order_acquire) <= 0) {
            c.rs_stack_unlock();
            m_reading = false;
            m_reading_lock = false;
            throw std::runtime_error("Sintra Ring: Maximum number of readers reached.");
        }
        int current_num_free = c.num_free_rs.fetch_sub(1, std::memory_order_relaxed) - 1;
        m_rs_index = c.free_rs_stack[current_num_free];
        c.rs_stack_unlock();

        m_reading_sequence = &c.reading_sequences[m_rs_index].v;

        // Conservatively guard all octiles first (set all 8 bytes to +1)
        uint64_t access_mask = 0x0101010101010101ULL;
        c.read_access += access_mask;

        // Atomic snapshot of the published head
        auto leading_sequence = c.leading_sequence.load();

        auto range_first_sequence =
            std::max<int64_t>(0, int64_t(leading_sequence) - int64_t(num_trailing_elements));

        Range<T> ret;
        ret.begin = this->m_data +
            std::max<int64_t>(0, int64_t(range_first_sequence)) % this->m_num_elements;
        ret.end   = ret.begin + (leading_sequence - range_first_sequence);

        // Determine which single trailing octile we truly need to guard…
        m_trailing_octile =
            (8 * ((range_first_sequence - m_max_trailing_elements) % this->m_num_elements)) /
            this->m_num_elements;

        // …and remove the redundant 7 guards.
        access_mask   -= uint64_t(1) << (8 * m_trailing_octile);
        c.read_access -= access_mask;

        // Advance reader to snapshot head
        *m_reading_sequence = leading_sequence;

        m_reading_lock = false;
        return ret;
    }

    /**
     * Release the snapshot. You MUST call this when you are done reading the
     * current view, otherwise you will impede the writer’s progress (and
     * potentially other readers).
     */
    void done_reading()
    {
        bool f = false;
        while (!m_reading_lock.compare_exchange_strong(f, true)) { f = false; }
        if (m_reading) {
            c.read_access -= uint64_t(1) << (8 * m_trailing_octile);
            *m_reading_sequence = m_trailing_octile = 0;
            m_reading = false;

            // Return the reader slot to the freelist
            assert(m_rs_index >= 0 && m_rs_index < max_process_index);
            c.rs_stack_lock();
#ifndef NDEBUG
            assert(c.num_free_rs.load(std::memory_order_acquire) < max_process_index &&
                "free_rs_stack overflow in done_reading()");
#endif
            int idx = c.num_free_rs.load(std::memory_order_relaxed);
            c.free_rs_stack[idx] = m_rs_index;
            c.num_free_rs.fetch_add(1, std::memory_order_release);
            c.rs_stack_unlock();

            m_rs_index = -1;
            m_reading_sequence = &s_zero_rs;
        }
        m_reading_lock = false;
    }

    sequence_counter_type reading_sequence() const { return *m_reading_sequence; }
    sequence_counter_type get_reading_sequence() const { return *m_reading_sequence; }

    /**
     * Block until new data is available (or until unblocked) and return the new
     * readable range. After consuming the returned range, call
     * done_reading_new_data() to update the trailing guard if needed.
     */
    const Range<T> wait_for_new_data()
    {
#if SINTRA_RING_READING_POLICY == SINTRA_RING_READING_POLICY_ALWAYS_SPIN
        while (*m_reading_sequence == c.leading_sequence.load()) {}

#else // HYBRID or ALWAYS_SLEEP
    #if SINTRA_RING_READING_POLICY == SINTRA_RING_READING_POLICY_HYBRID
        double tl = get_wtime() + spin_before_sleep * 0.5;
        while (*m_reading_sequence == c.leading_sequence.load() && get_wtime() < tl) {}
    #endif

        // Transition to sleeping if still no data
        c.lock();
        m_sleepy_index = -1;
        if (*m_reading_sequence == c.leading_sequence.load()) {
            m_sleepy_index = c.ready_stack[--c.num_ready];
            c.sleeping_stack[c.num_sleeping++] = m_sleepy_index;
        }
        c.unlock();

        if (m_sleepy_index >= 0) {
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
        }
#endif

        Range<T> ret;
        auto num_range_elements = size_t(c.leading_sequence.load() - *m_reading_sequence);

        if (num_range_elements == 0) {
            // Could happen if we were explicitly unblocked
            return ret;
        }

        ret.begin = this->m_data + (*m_reading_sequence % this->m_num_elements);
        ret.end   = ret.begin + num_range_elements;
        *m_reading_sequence += num_range_elements;
        return ret;
    }

    /**
     * After wait_for_new_data(), call this to release the trailing guard when
     * crossing to a new octile.
     */
    void done_reading_new_data()
    {
        size_t new_trailing_octile =
            (8 * ((*m_reading_sequence - m_max_trailing_elements) % this->m_num_elements)) /
            this->m_num_elements;

        if (new_trailing_octile != m_trailing_octile) {
            auto diff =
                (uint64_t(1) << (8 * new_trailing_octile)) -
                (uint64_t(1) << (8 *   m_trailing_octile));
            c.read_access += diff;
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

private:
    const size_t               m_max_trailing_elements;
    sequence_counter_type*     m_reading_sequence       = &s_zero_rs;
    size_t                     m_trailing_octile        = 0;
    std::atomic<bool>          m_reading                = false;
    std::atomic<bool>          m_reading_lock           = false;
    int                        m_sleepy_index           = -1;
    int                        m_rs_index               = -1;

    inline static sequence_counter_type s_zero_rs = 0;

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
        // Single writer across processes
        if (!c.ownership_mutex.try_lock()) {
            throw ring_acquisition_failure_exception();
        }
    }

    ~Ring_W()
    {
        // Wake any sleeping readers to avoid deadlocks during teardown
        unblock_global();
        c.ownership_mutex.unlock();
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
        for (size_t i = 0; i < num_src_elements; ++i) {
            write_location[i] = src_buffer[i];
        }
        return write_location;
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
        return new (write_location) T2{std::forward<Args>(args)...};
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
        auto leading_sequence = c.leading_sequence.load();
        auto range_first_sequence =
            std::max<int64_t>(0, int64_t(leading_sequence) - int64_t(3 * this->m_num_elements / 4));

        Range<T> ret;
        ret.begin = this->m_data +
            std::max<int64_t>(0, int64_t(range_first_sequence)) % this->m_num_elements;
        ret.end = ret.begin + (leading_sequence - range_first_sequence);

        return ret;
    }

private:
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

        size_t index = m_pending_new_sequence % this->m_num_elements;
        m_pending_new_sequence += num_elements_to_write;
        size_t new_octile =
            (8 * (m_pending_new_sequence % this->m_num_elements)) / this->m_num_elements;

        // Only check when crossing to a new octile (fast path otherwise)
        if (m_octile != new_octile) {
            auto range_mask = (uint64_t(0xff) << (8 * new_octile));
            while (c.read_access & range_mask) {
                // Busy-wait until the target octile is unguarded
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

} // namespace sintra

