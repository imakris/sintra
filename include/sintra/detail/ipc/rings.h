// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

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
#include "../config.h"      // configuration constants for adaptive waiting, cache sizes, etc.
#include "../get_wtime.h"   // high-res wall clock (used by adaptive reader policy)
#include "../id_types.h"    // ID and type aliases as used by the project

// ─── STL / stdlib ────────────────────────────────────────────────────────────
#include <algorithm>     // std::reverse
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>       // std::byte
#include <cstring>       // std::strlen
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>         // std::once_flag, std::call_once
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <new>
#include <utility>
#include <vector>

// ─── Interprocess Primitives ─────────────────────────────────────────────────
#include "../ipc/file_mapping.h"
#include "../ipc/mutex.h"
#include "../ipc/semaphore.h"

#include "../ipc/platform_utils.h"
#include "../ipc/time_utils.h"

// Enables the writer to forcefully evict readers that are too slow.
#ifndef SINTRA_ENABLE_SLOW_READER_EVICTION
#define SINTRA_ENABLE_SLOW_READER_EVICTION
#endif

// If eviction is enabled, this is the approximate stall budget (in microseconds)
// the writer will tolerate before triggering an eviction scan. Override with a
// compile-time value tailored to your deployment if desired.
#ifndef SINTRA_EVICTION_SPIN_BUDGET_US
// Give readers a wider scheduling window (5ms by default) before the writer
// initiates an eviction pass. The previous 500µs budget was occasionally too
// tight on heavily loaded macOS runners, where short deschedules allowed the
// writer to evict still-progressing readers and triggered data overflows.
#define SINTRA_EVICTION_SPIN_BUDGET_US 5000u
#endif

#ifndef SINTRA_EVICTION_LAG_RINGS
#define SINTRA_EVICTION_LAG_RINGS 1u  // reader is "slow" if > N rings behind
#endif

// Maximum amount of time (in microseconds) the writer will honor the reader's
// busy guard before forcefully clearing it to recover from crashed or stalled
// consumers. This is intentionally much longer than the normal eviction budget
// so healthy readers finish copying long before the fallback engages.
#ifndef SINTRA_READER_GUARD_BUSY_TIMEOUT_US
#define SINTRA_READER_GUARD_BUSY_TIMEOUT_US 200000u  // 200ms default safety net
#endif

// ─────────────────────────────────────────────────────────────────────────────

namespace sintra {

namespace fs  = std::filesystem;
namespace ipc = detail::ipc;

using sequence_counter_type = uint64_t;

struct Ring_diagnostics
{
    sequence_counter_type max_reader_lag             = 0;
    sequence_counter_type worst_overflow_lag         = 0;
    uint64_t              reader_lag_overflow_count  = 0;
    uint64_t              reader_sequence_regressions = 0;
    uint64_t              reader_eviction_count      = 0;
    uint32_t              last_evicted_reader_index  = std::numeric_limits<uint32_t>::max();
    sequence_counter_type last_evicted_reader_sequence = 0;
    sequence_counter_type last_evicted_writer_sequence = 0;
    uint32_t              last_evicted_reader_octile = std::numeric_limits<uint32_t>::max();
    uint32_t              last_overflow_reader_index = std::numeric_limits<uint32_t>::max();
    sequence_counter_type last_overflow_reader_sequence = 0;
    sequence_counter_type last_overflow_leading_sequence = 0;
    sequence_counter_type last_overflow_last_consumed = 0;
};
constexpr auto invalid_sequence = ~sequence_counter_type(0);

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

    size_t page_size = system_page_size();
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

// A binary semaphore tailored for the ring's reader wakeup policy.
class sintra_ring_semaphore
{
public:
    sintra_ring_semaphore() = default;
    sintra_ring_semaphore(const sintra_ring_semaphore&) = delete;
    sintra_ring_semaphore& operator=(const sintra_ring_semaphore&) = delete;

    ~sintra_ring_semaphore() { destroy(); }

    // Wakes all readers in an ordered fashion (used by writer after publishing).
    void post_ordered()
    {
        ensure_initialized().post_ordered();
    }

    // Wakes a single reader in an unordered fashion (used by local unblocks).
    void post_unordered()
    {
        ensure_initialized().post_unordered();
    }

    // Wait returns true if the wakeup was unordered and no ordered post happened since.
    bool wait()
    {
        return ensure_initialized().wait();
    }

    void release_local_handle() noexcept
    {
        if (is_initialized()) {
            access().release_local_handle();
        }
    }

private:
    struct impl : detail::interprocess_semaphore
    {
        impl() : detail::interprocess_semaphore(0) {}

        void post_ordered()
        {
            if (unordered.load(std::memory_order_acquire)) {
                unordered.store(false, std::memory_order_release);
            }
            else if (!posted.test_and_set(std::memory_order_acquire)) {
                this->post();
            }
        }

        void post_unordered()
        {
            if (!posted.test_and_set(std::memory_order_acquire)) {
                unordered.store(true, std::memory_order_release);
                this->post();
            }
        }

        bool wait()
        {
            detail::interprocess_semaphore::wait();
            posted.clear(std::memory_order_release);
            return unordered.exchange(false, std::memory_order_acq_rel);
        }

        std::atomic_flag posted = ATOMIC_FLAG_INIT;
        std::atomic<bool> unordered{false};
    };

    static constexpr uint8_t state_uninitialized = 0;
    static constexpr uint8_t state_initializing  = 1;
    static constexpr uint8_t state_initialized   = 2;

    bool is_initialized() const noexcept
    {
        return m_state.load(std::memory_order_acquire) == state_initialized;
    }

    impl& access() noexcept
    {
        return *std::launder(reinterpret_cast<impl*>(&m_storage));
    }

    const impl& access() const noexcept
    {
        return *std::launder(reinterpret_cast<const impl*>(&m_storage));
    }

    impl& ensure_initialized()
    {
        while (true) {
            uint8_t current = m_state.load(std::memory_order_acquire);
            if (current == state_initialized) {
                return access();
            }
            if (current == state_uninitialized) {
                if (m_state.compare_exchange_strong(
                        current,
                        state_initializing,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    try {
                        new (&m_storage) impl();
                        m_state.store(state_initialized, std::memory_order_release);
                        return access();
                    }
                    catch (...) {
                        m_state.store(state_uninitialized, std::memory_order_release);
                        throw;
                    }
                }
            }
            std::this_thread::yield();
        }
    }

    void destroy() noexcept
    {
        if (is_initialized()) {
            access().~impl();
            m_state.store(state_uninitialized, std::memory_order_release);
        }
    }

    typename std::aligned_storage<sizeof(impl), alignof(impl)>::type m_storage;
    std::atomic<uint8_t> m_state{state_uninitialized};
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

            detail::native_file_handle fh_data =
                detail::create_new_file(m_data_filename.c_str());
            if (fh_data == detail::invalid_file())
                return false;

#ifdef NDEBUG
            if (!detail::truncate_file(fh_data, m_data_region_size)) {
                detail::close_file(fh_data);
                return false;
            }
#else
            // Fill with a recognizable pattern to aid debugging
            const char* ustr = "UNINITIALIZED";
            const size_t dv = std::strlen(ustr); // std::strlen: see header notes
            std::unique_ptr<char[]> tmp(new char[m_data_region_size]);
            for (size_t i = 0; i < m_data_region_size; ++i) {
                tmp[i] = ustr[i % dv];
            }
            if (!detail::write_file(fh_data, tmp.get(), m_data_region_size)) {
                detail::close_file(fh_data);
                return false;
            }
#endif
            return detail::close_file(fh_data);
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
     *     at ptr.
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

            // NOTE: On Windows, the "page size" for mapping purposes is the allocation granularity.
            size_t page_size = system_page_size();

            // Enforce the “multiple of page/granularity” constraint explicitly.
            assert((m_data_region_size % page_size) == 0 &&
                   "Ring size (bytes) must be multiple of mapping granularity");

            auto data_rights = READ_ONLY_DATA ? ipc::read_only : ipc::read_write;
            ipc::file_mapping file(m_data_filename.c_str(), data_rights);

            // Unified retry logic for all platforms.
            // When multiple threads in the same process try to map the same file simultaneously,
            // the OS can fail due to concurrent access to the virtual memory manager.
            // Retrying with a CPU yield allows the OS to complete pending operations.
            constexpr int max_attach_attempts = 8;

            for (int attempt = 0; attempt < max_attach_attempts; ++attempt) {
                char* ptr = nullptr;
                ipc::map_options_t map_extra_options = 0;

#ifdef _WIN32
                // ── Windows: VirtualAlloc → round → VirtualFree → map ──────────────
                void* mem = ::VirtualAlloc(nullptr, m_data_region_size * 2 + page_size,
                                           MEM_RESERVE, PAGE_READWRITE);
                if (!mem) return false;

                ptr = (char*)((uintptr_t)((char*)mem + page_size) & ~((uintptr_t)page_size - 1));
                ::VirtualFree(mem, 0, MEM_RELEASE);
#else
                // ── POSIX: mmap PROT_NONE reservation ──────────────────────────────
                void* mem = ::mmap(nullptr, m_data_region_size * 2,
                                   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (mem == MAP_FAILED) {
                    return false;
                }

                ptr = static_cast<char*>(mem);
                assert((reinterpret_cast<uintptr_t>(ptr) % page_size) == 0 &&
                       "mmap(PROT_NONE) base not page-aligned?");

                #ifdef MAP_FIXED
                map_extra_options |= MAP_FIXED;
                #endif
                #ifdef MAP_NOSYNC
                map_extra_options |= MAP_NOSYNC;
                #endif
#endif
                // ── Platform-independent: map twice back-to-back ───────────────────
                std::unique_ptr<ipc::mapped_region> region0, region1;
                bool mapping_failed = false;

                try {
                    region0.reset(new ipc::mapped_region(file, data_rights, 0,
                        m_data_region_size, ptr, map_extra_options));
                    region1.reset(new ipc::mapped_region(file, data_rights, 0, 0,
                        ((char*)region0->data()) + m_data_region_size, map_extra_options));
                }
                catch (const std::exception& ex) {
                    mapping_failed = true;
                }
                catch (...) {
                    mapping_failed = true;
                }

                // ── Validate layout ─────────────────────────────────────────────────
                bool layout_ok = !mapping_failed &&
                    region0 && region1 &&
                    region0->data() == ptr &&
                    region1->data() == ptr + m_data_region_size &&
                    region0->size() == m_data_region_size &&
                    region1->size() == m_data_region_size;

                if (layout_ok) {
                    // Success! MAP_FIXED has replaced the PROT_NONE reservation.
                    m_data_region_0 = region0.release();
                    m_data_region_1 = region1.release();
                    m_data = (T*)m_data_region_0->data();

#if defined(MADV_DONTDUMP)
                    // Keep crash dumps compact (especially on macOS where Mach
                    // cores include every reserved span) and avoid leaking
                    // transient payloads. Failing to apply MADV_DONTDUMP is
                    // harmless, so we deliberately ignore the return value.
                    ::madvise(m_data_region_0->data(), m_data_region_size, MADV_DONTDUMP);
                    ::madvise(m_data_region_1->data(), m_data_region_size, MADV_DONTDUMP);
#endif
                    break;
                }

                // ── Failure: clean up and retry or fail ────────────────────────────
#ifndef _WIN32
                // CRITICAL: Release the PROT_NONE reservation on POSIX.
                // On macOS especially, leaked reservations accumulate and cause
                // subsequent mapping attempts to fail deterministically.
                ::munmap(mem, m_data_region_size * 2);
#endif

                if (attempt + 1 < max_attach_attempts) {
#ifdef _WIN32
                    ::Sleep(0);
#else
                    std::this_thread::yield();
#endif
                    continue;
                }

                // Final attempt failed
                return false;
            }

            if (!m_data_region_0) {
                return false;
            }

#if defined(SINTRA_RUNTIME_CACHELINE_CHECK) && (defined(__linux__) || defined(__APPLE__))
            // Warn ONCE per process if we detect a cache-line mismatch.
            static std::once_flag once;
            std::call_once(once, []{
                #if defined(__linux__)
                    sintra_warn_if_cacheline_mismatch_linux(assumed_cache_line_size);
                #elif defined(__APPLE__)
                    sintra_warn_if_cacheline_mismatch_macos(assumed_cache_line_size);
                #endif
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

    static constexpr uint8_t READER_GUARD_HELD = 0x1;
    static constexpr uint8_t READER_GUARD_BUSY = 0x2;

    // Helper: pad to a cache line to reduce false sharing in Control arrays.
    struct cache_line_sized_t {
        struct Payload {
            std::atomic<sequence_counter_type> v;
            std::atomic<uint8_t> status{READER_STATE_INACTIVE};
            std::atomic<uint8_t> trailing_octile{0};
            std::atomic<uint8_t> has_guard{0};
            std::atomic<uint64_t> guard_busy_since{0};
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

        // Monotonic counter tracking global unblock events so readers can detect
        // wake signals even if they were not yet sleeping when the writer
        // issued the unblock.
        std::atomic<uint64_t>                global_unblock_sequence{0};

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

        // Diagnostic counters (best-effort, relaxed atomics).
        std::atomic<sequence_counter_type>   max_reader_lag{0};
        std::atomic<sequence_counter_type>   worst_overflow_lag{0};
        std::atomic<uint64_t>                reader_lag_overflow_count{0};
        std::atomic<uint64_t>                reader_sequence_regressions{0};
        std::atomic<uint64_t>                reader_eviction_count{0};
        std::atomic<uint32_t>                last_evicted_reader_index{std::numeric_limits<uint32_t>::max()};
        std::atomic<sequence_counter_type>   last_evicted_reader_sequence{0};
        std::atomic<sequence_counter_type>   last_evicted_writer_sequence{0};
        std::atomic<uint32_t>                last_evicted_reader_octile{std::numeric_limits<uint32_t>::max()};
        std::atomic<uint32_t>                last_overflow_reader_index{std::numeric_limits<uint32_t>::max()};
        std::atomic<sequence_counter_type>   last_overflow_reader_sequence{0};
        std::atomic<sequence_counter_type>   last_overflow_leading_sequence{0};
        std::atomic<sequence_counter_type>   last_overflow_last_consumed{0};

        // Per-reader currently visible (snapshot) sequence. Cache-line sized to minimize
        // false sharing between reader slots. A reader sets its slot to the snapshot head
        // and advances it as it consumes ranges.
        cache_line_sized_t                   reading_sequences[max_process_index];

        // --- Reader Sequence Stack Management ------------------------------------
        // Protects free_rs_stack and num_free_rs during slot acquisition/release.
        std::atomic_flag                     rs_stack_spinlock = ATOMIC_FLAG_INIT;
        void rs_stack_lock()
        {
            while (rs_stack_spinlock.test_and_set(std::memory_order_acquire)) {
            }
        }
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
                    auto& guard = slot.has_guard;
                    uint8_t observed = guard.load(std::memory_order_acquire);
                    while (observed & READER_GUARD_HELD) {
                        if (guard.compare_exchange_strong(
                                observed, uint8_t{0}, std::memory_order_acq_rel, std::memory_order_acquire))
                        {
                            uint8_t oct = slot.trailing_octile.load(std::memory_order_acquire);
                            read_access.fetch_sub(
                                uint64_t(1) << (8 * oct), std::memory_order_acq_rel);
                            slot.guard_busy_since.store(0, std::memory_order_release);
                            break;
                        }
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

        void release_local_semaphores()
        {
            for (int i = 0; i < max_process_index; ++i) {
                dirty_semaphores[i].release_local_handle();
            }
        }

        // Used to avoid accidentally having multiple writers on the same ring
        // across processes. Only one writer may hold this at a time.
        std::atomic<uint32_t>                writer_pid{0};
        detail::interprocess_mutex           ownership_mutex;

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
        void lock()
        {
            while (spinlock_flag.test_and_set(std::memory_order_acquire)) {
            }
        }
        void unlock() { spinlock_flag.clear(std::memory_order_release); }

        Control()
        {
            for (int i = 0; i < max_process_index; i++) { ready_stack   [i]  =  i; }
            for (int i = 0; i < max_process_index; i++) { sleeping_stack[i]  = -1; }
            for (int i = 0; i < max_process_index; i++) { unordered_stack[i] = -1; }

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

        // Multiple writers/readers may attach concurrently (e.g. stress tests spawn
        // readers in parallel). num_attached is an interprocess atomic so we must use
        // a fetch_add here; a plain ++ loses updates under contention and the final
        // detacher would see an incorrect count and tear down the shared state while
        // other processes still use it.
        m_control->num_attached.fetch_add(1, std::memory_order_acq_rel);
    }

    ~Ring()
    {
        if (m_control) {
            m_control->release_local_semaphores();
            // The *last* detaching process deletes both control and data files. On
            // platforms where Control wraps kernel semaphore handles (Windows/macOS)
            // we must explicitly run the destructor once the final reference drops.
            if (m_control->num_attached.fetch_sub(1, std::memory_order_acq_rel) == 1) {
#if defined(_WIN32) || defined(__APPLE__)
                m_control->~Control();
#endif
                this->m_remove_files_on_destruction = true;
            }
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
            detail::native_file_handle fh_control =
                detail::create_new_file(m_control_filename.c_str());
            if (fh_control == detail::invalid_file())
                return false;

#ifdef NDEBUG
            if (!detail::truncate_file(fh_control, sizeof(Control))) {
                detail::close_file(fh_control);
                return false;
            }
#else
            const char* ustr = "UNINITIALIZED";
            const size_t dv = std::strlen(ustr);
            std::unique_ptr<char[]> tmp(new char[sizeof(Control)]);
            for (size_t i = 0; i < sizeof(Control); ++i) {
                tmp[i] = ustr[i % dv];
            }
            if (!detail::write_file(fh_control, tmp.get(), sizeof(Control))) {
                detail::close_file(fh_control);
                return false;
            }
#endif
            return detail::close_file(fh_control);
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
            m_control = (Control*)m_control_region->data();

#if defined(MADV_DONTDUMP)
            ::madvise(m_control_region->data(), m_control_region->size(), MADV_DONTDUMP);
#endif

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

public:
    Ring_diagnostics get_diagnostics() const noexcept
    {
        Ring_diagnostics diag;
        if (!m_control) {
            return diag;
        }

        diag.max_reader_lag = m_control->max_reader_lag.load(std::memory_order_acquire);
        diag.worst_overflow_lag = m_control->worst_overflow_lag.load(std::memory_order_acquire);
        diag.reader_lag_overflow_count = m_control->reader_lag_overflow_count.load(std::memory_order_acquire);
        diag.reader_sequence_regressions = m_control->reader_sequence_regressions.load(std::memory_order_acquire);
        diag.reader_eviction_count = m_control->reader_eviction_count.load(std::memory_order_acquire);
        diag.last_evicted_reader_index = m_control->last_evicted_reader_index.load(std::memory_order_acquire);
        diag.last_evicted_reader_sequence = m_control->last_evicted_reader_sequence.load(std::memory_order_acquire);
        diag.last_evicted_writer_sequence = m_control->last_evicted_writer_sequence.load(std::memory_order_acquire);
        diag.last_evicted_reader_octile = m_control->last_evicted_reader_octile.load(std::memory_order_acquire);
        diag.last_overflow_reader_index = m_control->last_overflow_reader_index.load(std::memory_order_acquire);
        diag.last_overflow_reader_sequence = m_control->last_overflow_reader_sequence.load(std::memory_order_acquire);
        diag.last_overflow_leading_sequence = m_control->last_overflow_leading_sequence.load(std::memory_order_acquire);
        diag.last_overflow_last_consumed = m_control->last_overflow_last_consumed.load(std::memory_order_acquire);
        return diag;
    }
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
        if (m_reading.load(std::memory_order_acquire)) {
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
        while (!m_reading_lock.compare_exchange_strong(f, true)) {
            f = false;
        }

        if (m_reading.load(std::memory_order_acquire)) {
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
        m_reading.store(true, std::memory_order_release);

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
                trailing_octile, std::memory_order_release);
            c.reading_sequences[m_rs_index].data.has_guard.store(
                Ring<T, true>::READER_GUARD_HELD, std::memory_order_release);
            c.reading_sequences[m_rs_index].data.guard_busy_since.store(
                0, std::memory_order_release);

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
                m_last_consumed_sequence = confirmed_leading_sequence;
                break;
            }

            // Trailing guard requirement changed between reads; drop and retry.
            uint8_t had_guard = c.reading_sequences[m_rs_index].data.has_guard.exchange(
                0, std::memory_order_acq_rel);
            if (had_guard != 0) {
                c.read_access.fetch_sub(guard_mask, std::memory_order_acq_rel);
            }
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

        if (m_reading.load(std::memory_order_acquire)) {
            auto& guard = c.reading_sequences[m_rs_index].data.has_guard;
            guard.fetch_and(static_cast<uint8_t>(~Ring<T, true>::READER_GUARD_BUSY),
                            std::memory_order_acq_rel);
            c.reading_sequences[m_rs_index].data.guard_busy_since.store(
                0, std::memory_order_release);
            uint8_t expected = Ring<T, true>::READER_GUARD_HELD;
            if (guard.compare_exchange_strong(
                    expected, uint8_t{0}, std::memory_order_acq_rel))
            {
                c.read_access.fetch_sub(
                    uint64_t(1) << (8 * m_trailing_octile), std::memory_order_acq_rel);
            }
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
        auto produce_range = [&]() -> Range<T> {
            auto& slot = c.reading_sequences[m_rs_index].data;
            if (handle_eviction_if_needed()) {
                return {};
            }

            auto start_sequence = m_reading_sequence->load(std::memory_order_acquire);
            auto leading_sequence = c.leading_sequence.load(std::memory_order_acquire);
            const auto last_consumed_before = m_last_consumed_sequence;

            auto update_max_relaxed = [](auto& target, sequence_counter_type value) {
                auto current = target.load(std::memory_order_relaxed);
                while (value > current &&
                       !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
                }
            };

            Range<T> ret;

            sequence_counter_type full_lag = 0;
            if (leading_sequence >= start_sequence) {
                full_lag = leading_sequence - start_sequence;
            }
            else {
                c.reader_sequence_regressions.fetch_add(1, std::memory_order_relaxed);
            }

            update_max_relaxed(c.max_reader_lag, full_lag);

            sequence_counter_type clamped_lag = full_lag;
            if (clamped_lag > sequence_counter_type(this->m_num_elements)) {
                c.reader_lag_overflow_count.fetch_add(1, std::memory_order_relaxed);
                update_max_relaxed(c.worst_overflow_lag, full_lag);
                c.last_overflow_reader_index.store(static_cast<uint32_t>(m_rs_index), std::memory_order_relaxed);
                c.last_overflow_reader_sequence.store(start_sequence, std::memory_order_relaxed);
                c.last_overflow_leading_sequence.store(leading_sequence, std::memory_order_relaxed);
                c.last_overflow_last_consumed.store(last_consumed_before, std::memory_order_relaxed);
                clamped_lag = sequence_counter_type(this->m_num_elements);
            }

            if (start_sequence < m_last_consumed_sequence) {
                c.reader_sequence_regressions.fetch_add(1, std::memory_order_relaxed);
            }

            size_t num_range_elements = static_cast<size_t>(clamped_lag);

            if (num_range_elements == 0) {
                // Could happen if we were explicitly unblocked
                return ret;
            }

            auto mark_guard_busy = [&]() -> bool {
                uint8_t observed = slot.has_guard.load(std::memory_order_acquire);
                while (true) {
                    if ((observed & Ring<T, false>::READER_GUARD_HELD) == 0) {
                        return false;
                    }

                    const uint64_t now_us = detail::monotonic_now_us();

                    if (observed & Ring<T, false>::READER_GUARD_BUSY) {
                        slot.guard_busy_since.store(now_us, std::memory_order_release);
                        return true;
                    }

                    slot.guard_busy_since.store(now_us, std::memory_order_release);
                    std::atomic_thread_fence(std::memory_order_release);
                    uint8_t desired = static_cast<uint8_t>(observed | Ring<T, false>::READER_GUARD_BUSY);
                    if (slot.has_guard.compare_exchange_weak(
                            observed, desired, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        return true;
                    }
                }
            };

            if (!mark_guard_busy()) {
                handle_eviction_if_needed();
                return {};
            }

            ret.begin = this->m_data + mod_u64(start_sequence, this->m_num_elements);
            ret.end   = ret.begin + num_range_elements;
            m_reading_sequence->fetch_add(num_range_elements);  // +=
            m_last_consumed_sequence = start_sequence + sequence_counter_type(num_range_elements);
            return ret;
        };

        auto sequences_equal = [&]() {
            return m_reading_sequence->load(std::memory_order_acquire) ==
                   c.leading_sequence.load(std::memory_order_acquire);
        };

        auto should_shutdown = [&]() {
            return m_stopping.load(std::memory_order_acquire);
        };

        if (should_shutdown()) {
            return Range<T>{};
        }

        // Phase 1 — fast spin: aggressively poll for a very short window to
        // deliver sub-100µs wakeups when the writer is still active.
        const double fast_spin_end = get_wtime() + fast_spin_duration;
        while (sequences_equal() && get_wtime() < fast_spin_end) {
            if (should_shutdown()) {
                return Range<T>{};
            }
        }

        if (sequences_equal()) {
            // Phase 2 — precision sleeps: yield the CPU in 1ms slices while
            // still polling at a high enough cadence to catch bursts quickly.
            Scoped_timer_resolution timer_resolution_guard(1);
            const double precision_sleep_end = get_wtime() + precision_sleep_duration;
            while (sequences_equal() && get_wtime() < precision_sleep_end) {
                if (should_shutdown()) {
                    return Range<T>{};
                }
                const auto seq_now = c.global_unblock_sequence.load(std::memory_order_acquire);
                if (seq_now != m_seen_unblock_sequence) {
                    m_seen_unblock_sequence = seq_now;
                    if (!sequences_equal()) {
                        return produce_range();
                    }
                    return Range<T>{};
                }
                precision_sleep_for(std::chrono::duration<double>(precision_sleep_cycle));
            }
        }

        if (sequences_equal()) {
            // Phase 3 — blocking wait: fully park on the semaphore to avoid
            // burning CPU when the writer is stalled for long periods.
            const auto unblock_sequence_now =
                c.global_unblock_sequence.load(std::memory_order_acquire);
            if (unblock_sequence_now != m_seen_unblock_sequence) {
                m_seen_unblock_sequence = unblock_sequence_now;
                if (!sequences_equal()) {
                    return produce_range();
                }
                return Range<T>{};
            }

            c.lock();
            m_sleepy_index.store(-1, std::memory_order_relaxed);
            if (sequences_equal()) {
                if (should_shutdown()) {
                    c.unlock();
                    return Range<T>{};
                }
                int sleepy = c.ready_stack[--c.num_ready];
                m_sleepy_index.store(sleepy, std::memory_order_release);
                c.sleeping_stack[c.num_sleeping++] = sleepy;
            }
            const auto unblock_sequence_after =
                c.global_unblock_sequence.load(std::memory_order_acquire);
            if (unblock_sequence_after != m_seen_unblock_sequence) {
                m_seen_unblock_sequence = unblock_sequence_after;
                const int sleepy = m_sleepy_index.load(std::memory_order_relaxed);
                if (sleepy >= 0) {
                    if (c.num_sleeping > 0 && c.sleeping_stack[c.num_sleeping - 1] == sleepy) {
                        c.sleeping_stack[--c.num_sleeping] = -1;
                    }
                    c.ready_stack[c.num_ready++] = sleepy;
                    m_sleepy_index.store(-1, std::memory_order_release);
                }
                c.unlock();
                if (!sequences_equal()) {
                    return produce_range();
                }
                return Range<T>{};
            }
            c.unlock();

            int sleepy_index = m_sleepy_index.load(std::memory_order_acquire);
            if (sleepy_index >= 0) {
                if (should_shutdown()) {
                    c.lock();
                    if (m_sleepy_index.load(std::memory_order_acquire) >= 0) {
                        c.dirty_semaphores[sleepy_index].post_unordered();
                    }
                    c.unlock();
                }

                if (c.dirty_semaphores[sleepy_index].wait()) { // unordered wake
                    c.lock();
                    c.unordered_stack[c.num_unordered++] = sleepy_index;
                    c.unlock();
                }
                else {
                    c.lock();
                    c.ready_stack[c.num_ready++] = sleepy_index;
                    c.unlock();
                }
                m_sleepy_index.store(-1, std::memory_order_release);

                if (should_shutdown()) {
                    return Range<T>{};
                }
            }
        }

        return produce_range();
    }

    /**
     * After wait_for_new_data(), call this to release the trailing guard when
     * crossing to a new octile.
     */
    void done_reading_new_data()
    {
        const size_t t_idx = mod_pos_i64(
            int64_t(m_reading_sequence->load(std::memory_order_acquire)) -
            int64_t(m_max_trailing_elements),
            this->m_num_elements);

        const size_t new_trailing_octile = (8 * t_idx) / this->m_num_elements;

        handle_eviction_if_needed();

        if (new_trailing_octile != m_trailing_octile) {
            const uint64_t new_mask = uint64_t(1) << (8 * new_trailing_octile);
            const uint64_t old_mask = uint64_t(1) << (8 * m_trailing_octile);
            c.read_access.fetch_add(new_mask, std::memory_order_acq_rel);
            c.read_access.fetch_sub(old_mask, std::memory_order_acq_rel);
            c.reading_sequences[m_rs_index].data.trailing_octile.store(
                static_cast<uint8_t>(new_trailing_octile), std::memory_order_release);

            m_trailing_octile = new_trailing_octile;
        }

        c.reading_sequences[m_rs_index].data.has_guard.fetch_and(
            static_cast<uint8_t>(~Ring<T, false>::READER_GUARD_BUSY), std::memory_order_acq_rel);
        c.reading_sequences[m_rs_index].data.guard_busy_since.store(
            0, std::memory_order_release);
    }

    bool handle_eviction_if_needed()
    {
        auto& slot = c.reading_sequences[m_rs_index].data;
        if (slot.has_guard.load(std::memory_order_acquire) & Ring<T, false>::READER_GUARD_HELD) {
            return false;
        }

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        if (slot.status.load(std::memory_order_acquire) == Ring<T, false>::READER_STATE_EVICTED) {
            // Reader was evicted by writer for being too slow.
            // Skip all missed data and jump to writer's current position.
            // This is the only safe recovery strategy since old data has been overwritten.
            sequence_counter_type new_seq = c.leading_sequence.load(std::memory_order_acquire);
            slot.v.store(new_seq, std::memory_order_release);
            m_reading_sequence->store(new_seq, std::memory_order_release);
            m_last_consumed_sequence = new_seq;
            reattach_after_eviction();
            m_evicted_since_last_wait.store(true, std::memory_order_release);
            return true;
        }
#endif

        reattach_after_eviction();
        return false;
    }

    void reattach_after_eviction()
    {
        const uint64_t mask = uint64_t(1) << (8 * m_trailing_octile);
        c.read_access.fetch_add(mask, std::memory_order_acq_rel);
        c.reading_sequences[m_rs_index].data.trailing_octile.store(
            static_cast<uint8_t>(m_trailing_octile), std::memory_order_release);
        c.reading_sequences[m_rs_index].data.has_guard.store(
            Ring<T, false>::READER_GUARD_HELD, std::memory_order_release);
        c.reading_sequences[m_rs_index].data.guard_busy_since.store(
            0, std::memory_order_release);
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        c.reading_sequences[m_rs_index].data.status.store(
            Ring<T, false>::READER_STATE_ACTIVE, std::memory_order_release);
#endif
    }

public:
    bool consume_eviction_notification()
    {
        return m_evicted_since_last_wait.exchange(false, std::memory_order_acq_rel);
    }

    bool eviction_pending() const
    {
        return m_evicted_since_last_wait.load(std::memory_order_acquire);
    }

    /**
     * If another thread is in wait_for_new_data(), force it to return an empty
     * range without publishing any new data. Only affects this reader instance.
     */
    void unblock_local()
    {
        c.lock();
        int sleepy = m_sleepy_index.load(std::memory_order_acquire);
        if (sleepy >= 0) {
            c.dirty_semaphores[sleepy].post_unordered();
        }
        c.unlock();
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
    uint64_t                            m_seen_unblock_sequence = 0;
    sequence_counter_type               m_last_consumed_sequence = 0;
    std::atomic<bool>                   m_evicted_since_last_wait{false};

protected:
    std::atomic<bool>                   m_reading               = false;
    std::atomic<bool>                   m_reading_lock          = false;

private:
    std::atomic<int>                    m_sleepy_index          = -1;
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

// Helper to get a unique thread index (trivial type) for atomic operations
// std::atomic<std::thread::id> has issues on some platforms (macOS)
inline uint32_t thread_index()
{
    static std::atomic<uint32_t> next{1};
    thread_local uint32_t mine = next.fetch_add(1, std::memory_order_relaxed);
    return mine;
}

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
            m_writing_thread_index.store(0, std::memory_order_release);
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
            m_writing_thread_index.store(0, std::memory_order_release);
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
        c.lock();
        assert(m_writing_thread_index.load(std::memory_order_relaxed) == thread_index());
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
        m_writing_thread_index.store(0, std::memory_order_release);
        return m_pending_new_sequence;
    }

    /**
     * Force any reader waiting in wait_for_new_data() to wake up and return
     * an empty range (global unblock).
     */
    void unblock_global()
    {
        c.global_unblock_sequence.fetch_add(1, std::memory_order_acq_rel);

        c.lock();
        for (int i = 0; i < c.num_sleeping; i++) {
            c.dirty_semaphores[c.sleeping_stack[i]].post_ordered();
        }
        c.num_sleeping = 0;
        while (c.num_unordered) {
            c.ready_stack[c.num_ready++] = c.unordered_stack[--c.num_unordered];
        }
        c.unlock();
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
            new (&c.ownership_mutex) detail::interprocess_mutex();
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
        // Use thread-local index (trivial type) for reliable atomic operations across all platforms
        const uint32_t my_thread_idx = thread_index();
        while (m_writing_thread_index.load(std::memory_order_relaxed) != my_thread_idx) {
            uint32_t expected = 0;
            m_writing_thread_index.compare_exchange_strong(
                expected,
                my_thread_idx,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }

        const size_t index = mod_u64(m_pending_new_sequence, this->m_num_elements);
        m_pending_new_sequence += num_elements_to_write;

        const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
        size_t new_octile = (8 * head) / this->m_num_elements;

        // Only check when crossing to a new octile (fast path otherwise)
        if (m_octile != new_octile) {
#ifndef NDEBUG
            {
                uint64_t ra = c.read_access.load(std::memory_order_acquire);
                for (int b = 0; b < 8; ++b) {
                    assert(((ra >> (8*b)) & 0xffu) != 0xffu && "read_access octile underflowed to 255");
                }
            }
#endif
            auto range_mask = (uint64_t(0xff) << (8 * new_octile));
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
            auto run_eviction_pass = [&]() {
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
                        uint8_t guard_flag =
                            c.reading_sequences[i].data.has_guard.load(std::memory_order_acquire);
                        uint8_t reader_octile =
                            c.reading_sequences[i].data.trailing_octile.load(std::memory_order_acquire);

                        bool guard_held =
                            (guard_flag & Ring<T, false>::READER_GUARD_HELD) != 0;
                        bool reader_busy =
                            (guard_flag & Ring<T, false>::READER_GUARD_BUSY) != 0;

                        if (guard_held && reader_busy) {
                            auto clear_busy_flag = [&]() {
                                uint8_t busy_expected = static_cast<uint8_t>(
                                    Ring<T, false>::READER_GUARD_HELD | Ring<T, false>::READER_GUARD_BUSY);
                                if (c.reading_sequences[i].data.has_guard.compare_exchange_strong(
                                        busy_expected,
                                        Ring<T, false>::READER_GUARD_HELD,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
                                {
                                    c.reading_sequences[i].data.guard_busy_since.store(
                                        0, std::memory_order_release);
                                    guard_flag = Ring<T, false>::READER_GUARD_HELD;
                                    guard_held = true;
                                    reader_busy = false;
                                    return true;
                                }

                                guard_flag = c.reading_sequences[i].data.has_guard.load(std::memory_order_acquire);
                                guard_held = (guard_flag & Ring<T, false>::READER_GUARD_HELD) != 0;
                                reader_busy = (guard_flag & Ring<T, false>::READER_GUARD_BUSY) != 0;
                                return false;
                            };

                            const uint64_t busy_since =
                                c.reading_sequences[i].data.guard_busy_since.load(std::memory_order_acquire);

                            if (busy_since == 0) {
                                clear_busy_flag();
                            }
                            else {
                                const uint64_t now_us = detail::monotonic_now_us();
                                if (now_us >= busy_since &&
                                    now_us - busy_since >= SINTRA_READER_GUARD_BUSY_TIMEOUT_US)
                                {
                                    clear_busy_flag();
                                }
                            }
                        }

                        bool blocking_current_octile =
                            guard_held && (reader_octile == new_octile);

                        if ((reader_seq < eviction_threshold || blocking_current_octile) && guard_held &&
                            !reader_busy)
                        {
                            // Evict only if the reader currently holds an idle guard
                            uint8_t expected = Ring<T, false>::READER_GUARD_HELD;
                            if (c.reading_sequences[i].data.has_guard.compare_exchange_strong(
                                    expected,
                                    uint8_t{0},
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire))
                            {
                                c.reading_sequences[i].data.guard_busy_since.store(
                                    0, std::memory_order_release);
                                c.reading_sequences[i].data.status.store(
                                    Ring<T, false>::READER_STATE_EVICTED, std::memory_order_release);

                                size_t evicted_reader_octile =
                                    c.reading_sequences[i].data.trailing_octile.load(std::memory_order_acquire);
                                c.read_access.fetch_sub(uint64_t(1) << (8 * evicted_reader_octile), std::memory_order_acq_rel);
                                c.reader_eviction_count.fetch_add(1, std::memory_order_relaxed);
                                c.last_evicted_reader_index.store(static_cast<uint32_t>(i), std::memory_order_relaxed);
                                c.last_evicted_reader_sequence.store(reader_seq, std::memory_order_relaxed);
                                c.last_evicted_writer_sequence.store(m_pending_new_sequence, std::memory_order_relaxed);
                                c.last_evicted_reader_octile.store(static_cast<uint32_t>(evicted_reader_octile), std::memory_order_relaxed);
                            }
                        }
                    }
                }

                c.scavenge_orphans();
            };

#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
            uint64_t spin_count = 0;
            constexpr uint64_t spin_loop_budget = SINTRA_EVICTION_SPIN_THRESHOLD;
#else
            using eviction_clock = std::chrono::steady_clock;
            const auto eviction_budget = std::chrono::microseconds{SINTRA_EVICTION_SPIN_BUDGET_US};
            auto eviction_deadline = eviction_clock::time_point{};
            bool eviction_deadline_armed = false;
#endif
#endif
            while (c.read_access.load(std::memory_order_acquire) & range_mask) {
                bool has_blocking_reader = false;
                for (int i = 0; i < max_process_index; ++i) {
                    if ((c.reading_sequences[i].data.has_guard.load(std::memory_order_acquire) &
                         Ring<T, false>::READER_GUARD_HELD) != 0 &&
                        c.reading_sequences[i].data.trailing_octile.load(std::memory_order_acquire) == new_octile)
                    {
                        has_blocking_reader = true;
                        break;
                    }
                }

                if (!has_blocking_reader) {
                    if (!(c.read_access.load(std::memory_order_acquire) & range_mask)) {
                        break;
                    }

                    // A reader is in the process of moving its guard: the guard flag was observed but the
                    // trailing octile has not yet been published. Keep spinning until the guard count drops
                    // (or an eviction occurs) so that the writer never writes into an octile that is still
                    // protected by a reader.
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
                    spin_count = 0;
#else
                    eviction_deadline_armed = false;
#endif
#endif
                    continue;
                }

                // Busy-wait until the target octile is unguarded
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
                if (++spin_count > spin_loop_budget) {
                    // Writer is stuck. Time to find and evict the slow reader(s).
                    // This is a slow path, taken only in exceptional circumstances.
                    run_eviction_pass();
                    spin_count = 0; // Reset spin count after an eviction pass
                }
#else
                auto now = eviction_clock::now();
                if (eviction_budget.count() == 0) {
                    run_eviction_pass();
                }
                else if (!eviction_deadline_armed) {
                    eviction_deadline = now + eviction_budget;
                    eviction_deadline_armed = true;
                }
                else if (now >= eviction_deadline) {
                    run_eviction_pass();
                    eviction_deadline = now + eviction_budget;
                }
#endif
#endif
            }
            m_octile = new_octile;
        }
        return this->m_data + index;
    }

private:
    // Use uint32_t (trivial type) instead of std::thread::id for reliable atomic operations
    // std::atomic<std::thread::id> has platform-specific issues (broken on macOS)
    std::atomic<uint32_t>           m_writing_thread_index   = {0};
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

