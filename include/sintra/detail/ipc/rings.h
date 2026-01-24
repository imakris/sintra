// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

/* ipc_rings.h - SINTRA SPMC IPC Ring
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS IS
 * ------------
 * A single-producer / multiple-consumer (SPMC) inter-process circular ring
 * buffer that uses the "magic ring" trick (double virtual mapping of the same
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
 *  * Elements & octiles:
 *      - The ring is conceptually split into 8 equal "octiles".
 *      - REQUIRED: num_elements % 8 == 0 (we guard per-octile).
 *  * Page / granularity alignment:
 *      - REQUIRED: data region size (num_elements * sizeof(T)) must be a
 *        multiple of the system's mapping granularity:
 *          * Windows: allocation granularity (dwAllocationGranularity)
 *          * POSIX: page size
 *        We assert this at attach() time.
 *  * Power-of-two size:
 *      - RECOMMENDED (not strictly required): choose power-of-two sizes.
 *        Our helper that proposes configurations typically does that.
 *
 * PUBLISH / MEMORY ORDERING SEMANTICS
 * -----------------------------------
 *  * The writer first writes elements into the mapped region (plain stores),
 *    then calls done_writing(), which atomically publishes the new
 *    leading_sequence (last published element is leading_sequence - 1).
 *  * Readers use the published leading_sequence to compute their readable range.
 *
 * WRITE BOUNDS
 * ------------
 *  * Each write must fit within a single octile (<= ring_size/8 elements).
 *    This ensures the writer only needs to check & spin on at most one octile's
 *    read guard (fast path with minimal contention).
 *
 * READER SNAPSHOTS & VALIDITY
 * ---------------------------
 *  * start_reading() returns a snapshot range into the shared memory mapping.
 *  * The snapshot remains valid until overwritten; i.e., until the writer
 *    advances far enough that the reader's trailing guard octile would be
 *    surpassed and reclaimed by the writer.
 *  * start_reading() must be paired with done_reading().
 *    Calling start_reading() again, while a snapshot is active, throws.
 *
 * READER CAP
 * ----------
 *  * Effective maximum readers = min(255, max_process_index).
 *    - The control block allocates per-reader state arrays sized by
 *      max_process_index; and the octile guard uses an 8×1-byte pattern which
 *      caps actively guarded readers at 255.
 *
 * READER EVICTION (WHEN ENABLED)
 * ------------------------------
 *  * If SINTRA_ENABLE_SLOW_READER_EVICTION is defined, the writer can evict
 *    a reader that is blocking its progress.
 *  * A reader is considered "slow" if it holds a guard and its last-read
 *    sequence is more than one full ring buffer's length behind the writer.
 *  * Eviction forcefully clears the reader's guard and sets its status to
 *    EVICTED. The reader's next call to start_reading() will throw a
 *    ring_reader_evicted_exception, forcing it to re-attach.
 *
 * LIFECYCLE & CLEANUP
 * -------------------
 *  * Two files exist: a data file (double-mapped) and a control file
 *    (atomics, semaphores, per-reader state).
 *  * Any process may create; subsequent processes attach.
 *  * Each Ring_R object acquires a unique "reader slot" at construction and
 *    releases it on normal destruction.
 *  * The *last* detaching process removes BOTH files.
 *
 * PLATFORM MAPPING OVERVIEW
 * -------------------------
 *  * Windows:
 *      - Reserve address space (2× region + granularity) via ::VirtualAlloc.
 *      - Apply the historical rounding to preserve layout parity:
 *            char* ptr = (char*)(
 *                (uintptr_t)((char*)mem + granularity) & ~((uintptr_t)granularity - 1)
 *            );
 *      - Release the reservation and immediately map the file twice contiguously,
 *        expecting the OS to reuse the address.
 *  * Linux / POSIX:
 *      - Reserve a 2× span with mmap(NULL, 2*size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, ...).
 *      - Map the file TWICE into that span using MAP_FIXED (by design replaces
 *        the reservation). Do NOT use MAP_FIXED_NOREPLACE for this step.
 *      - Use ptr = mem (mmap returns a page-aligned address).
 *      - On ANY failure after reserving, munmap the 2× span before returning.
 */

#pragma once

// --- Project config & utilities (kept as in original codebase) ---------------
#include "../config.h"      // configuration constants for adaptive waiting, cache sizes, etc.
#include "../get_wtime.h"   // high-res wall clock (used by adaptive reader policy)
#include "../id_types.h"    // ID and type aliases as used by the project
#include "../logging.h"

// --- STL / stdlib ------------------------------------------------------------
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
#include <optional>
#include <utility>
#include <vector>

// --- Interprocess Primitives -------------------------------------------------
#include "../ipc/file_mapping.h"
#include "../ipc/mutex.h"
#include "../ipc/semaphore.h"
#include "../ipc/spinlock.h"

#include "../ipc/platform_utils.h"

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

#ifndef SINTRA_STALE_GUARD_DELAY_MS
#define SINTRA_STALE_GUARD_DELAY_MS 2000u
#endif

// -----------------------------------------------------------------------------

namespace sintra {

namespace fs  = std::filesystem;
namespace ipc = detail::ipc;

using sequence_counter_type = uint64_t;

// Payload traits for in-place ring writes.
// By default, only trivially copyable/destructible payloads are allowed.
// Specific higher-level types (e.g., sintra::Message) can opt-in to allow
// non-trivial semantics via explicit specializations in their own headers.
template <typename T>
struct ring_payload_traits {
    static constexpr bool allow_nontrivial = false;
};

struct Ring_diagnostics
{
    sequence_counter_type max_reader_lag                  = 0;
    sequence_counter_type worst_overflow_lag              = 0;
    uint64_t              reader_lag_overflow_count       = 0;
    uint64_t              reader_sequence_regressions     = 0;
    uint64_t              reader_eviction_count           = 0;
    uint32_t              last_evicted_reader_index       = std::numeric_limits<uint32_t>::max();
    sequence_counter_type last_evicted_reader_sequence    = 0;
    sequence_counter_type last_evicted_writer_sequence    = 0;
    uint32_t              last_evicted_reader_octile      = std::numeric_limits<uint32_t>::max();
    uint32_t              last_overflow_reader_index      = std::numeric_limits<uint32_t>::max();
    sequence_counter_type last_overflow_reader_sequence   = 0;
    sequence_counter_type last_overflow_leading_sequence  = 0;
    sequence_counter_type last_overflow_last_consumed     = 0;
    uint64_t              stale_guard_clear_count         = 0;
    uint64_t              guard_rollback_attempt_count    = 0;
    uint64_t              guard_rollback_success_count    = 0;
    uint64_t              guard_accounting_mismatch_count = 0;
};
constexpr auto invalid_sequence = ~sequence_counter_type(0);

//==============================================================================
// Suggested ring configurations helper
//==============================================================================

/**
 * Compute candidate ring sizes (in element counts) between min_elements and the
 * maximum byte size constraint, with up to max_subdivisions sizes. The algorithm
 * prefers power-of-two-like sizes aligned to the system's page size so that
 * double-mapping constraints are naturally satisfied.
 *
 * Constraints embodied here:
 *  * The base size (bytes) is the LCM(sizeof(T), page_size).
 *  * The resulting region sizes are multiples of the page size.
 *  * The element count MUST be a multiple of 8 (octiles). We ensure the base
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

    // Respect caller's minimum element constraint
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

/**
 * Return the smallest capacity >= requested that satisfies ring alignment
 * requirements for the given element size. Returns 0 if requested is 0 or
 * if the alignment cannot be computed safely.
 */
inline size_t aligned_capacity(size_t requested, size_t element_size)
{
    if (requested == 0 || element_size == 0) {
        return 0;
    }

    auto gcd = [](size_t m, size_t n) {
        size_t tmp;
        while (m) { tmp = m; m = n % m; n = tmp; }
        return n;
    };

    size_t alignment = 8;
    const size_t page_size = system_page_size();
    if (page_size != 0) {
        const size_t page_step = page_size / gcd(page_size, element_size);
        const size_t step_gcd = gcd(page_step, size_t(8));
        const size_t step_div_gcd = page_step / step_gcd;
        if (step_div_gcd > (std::numeric_limits<size_t>::max() / 8)) {
            return 0;
        }
        alignment = step_div_gcd * 8;
    }

    const size_t remainder = requested % alignment;
    if (remainder != 0) {
        const size_t padding = alignment - remainder;
        if (padding > (std::numeric_limits<size_t>::max() - requested)) {
            return 0;
        }
        requested += padding;
    }

    return requested;
}

template <typename T>
inline size_t aligned_capacity(size_t requested)
{
    return aligned_capacity(requested, sizeof(T));
}

//==============================================================================
// Lightweight range view
//==============================================================================

template <typename T>
struct Range {
    T* begin = nullptr;
    T* end   = nullptr;
};

inline uint64_t octile_mask(uint8_t octile) noexcept
{
    return uint64_t(1) << (8u * octile);
}

inline uint8_t octile_of_index(size_t idx, size_t ring) noexcept
{
    return static_cast<uint8_t>((8u * idx) / ring);
}

inline uint8_t octile_of_sequence(sequence_counter_type seq, size_t ring) noexcept
{
    return octile_of_index(mod_u64(seq, ring), ring);
}

//==============================================================================
// Small helper types
//==============================================================================

struct ring_acquisition_failure_exception : public std::runtime_error {
    ring_acquisition_failure_exception() : std::runtime_error("Failed to acquire ring buffer.") {}
};

struct ring_reader_evicted_exception : public std::runtime_error {
    ring_reader_evicted_exception() : std::runtime_error(
              "Ring reader was evicted by the writer due to being too slow.") {}
};

// A binary semaphore tailored for the ring's reader wakeup policy.
class sintra_ring_semaphore
{
public:
    enum class wait_result
    {
        ordered,
        unordered,
        timeout
    };

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

    wait_result wait_for(std::chrono::nanoseconds timeout)
    {
        return ensure_initialized().wait_for(timeout);
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
            if (unordered) {
                unordered = false;
            }
            else
            if (!posted.test_and_set()) {
                this->post();
            }
        }

        void post_unordered()
        {
            if (!posted.test_and_set()) {
                unordered = true;
                this->post();
            }
        }

        bool wait()
        {
            detail::interprocess_semaphore::wait();
            posted.clear();
            return unordered.exchange(false);
        }

        wait_result wait_for(std::chrono::nanoseconds timeout)
        {
            if (!detail::interprocess_semaphore::try_wait_for(timeout)) {
                return wait_result::timeout;
            }
            posted.clear();
            return unordered.exchange(false) ? wait_result::unordered : wait_result::ordered;
        }

        std::atomic_flag posted = ATOMIC_FLAG_INIT;
        std::atomic<bool> unordered{false};
    };

    static constexpr uint8_t state_uninitialized = 0;
    static constexpr uint8_t state_initializing  = 1;
    static constexpr uint8_t state_initialized   = 2;

    bool is_initialized() const noexcept
    {
        return m_state == state_initialized;
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
            uint8_t current = m_state;
            if (current == state_initialized) {
                return access();
            }
            if (current == state_uninitialized) {
                if (m_state.compare_exchange_strong(current, state_initializing)) {
                    try {
                        new (&m_storage) impl();
                        m_state = state_initialized;
                        return access();
                    }
                    catch (...) {
                        m_state = state_uninitialized;
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
            m_state = state_uninitialized;
        }
    }

    std::aligned_storage_t<sizeof(impl), alignof(impl)> m_storage;
    std::atomic<uint8_t> m_state{state_uninitialized};
};

 //////////////////////////////////////////////////////////////////////////
///// BEGIN Ring_data //////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
//////   \//////   \//////   \//////   \//////   \//////   \//////   \//////
 ////     \////     \////     \////     \////     \////     \////     \////
  //       \//       \//       \//       \//       \//       \//       \//

//==============================================================================
// Ring_data: file-backed data region with "magic" double mapping
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
        m_data = nullptr;

        if (m_remove_files_on_destruction) {
            std::error_code ec;
            (void)fs::remove(fs::path(m_data_filename), ec);
        }
    }

    size_t   get_num_elements() const { return m_num_elements; }
    const T* get_base_address() const { return m_data; }

private:

    // Create the backing data file (filled with a debug pattern in !NDEBUG).
    bool create()
    {
        try {
            if (!check_or_create_directory(m_directory)) {
                return false;
            }

            detail::native_file_handle fh_data = detail::create_new_file(m_data_filename.c_str());
            if (fh_data == detail::invalid_file()) {
                return false;
            }

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
     * Attach the data file with a "double mapping".
     *
     * WINDOWS
     *   * Reserve address space: 2× region + one granularity page.
     *   * Compute ptr by rounding (mem + granularity) down to granularity
     *     (historical layout parity).
     *   * Release the reservation, then map the file twice contiguously starting
     *     at ptr.
     *
     * LINUX / POSIX
     *   * Reserve a 2× span with mmap(PROT_NONE). POSIX guarantees page alignment.
     *   * Map the file twice using MAP_FIXED so the mappings REPLACE the reservation.
     *   * IMPORTANT: Do NOT use MAP_FIXED_NOREPLACE here. The whole point is to
     *     overwrite the reservation.
     *   * On ANY failure after reserving, munmap the 2× span and fail cleanly.
     */
    bool attach()
    {
        assert(!m_data_region_0 && !m_data_region_1 && m_data == nullptr);

        try {
            if (fs::file_size(m_data_filename) != m_data_region_size) {
                return false; // size mismatch => refuse to map
            }

            // NOTE: On Windows, the "page size" for mapping purposes is the allocation granularity.
            size_t page_size = system_page_size();

            // Enforce the "multiple of page/granularity" constraint explicitly.
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
                // -- Windows: VirtualAlloc → round → VirtualFree → map --------------
                void* mem = ::VirtualAlloc(nullptr, m_data_region_size * 2 + page_size,
                                           MEM_RESERVE, PAGE_READWRITE);
                if (!mem) {
                    return false;
                }

                ptr = (char*)((uintptr_t)((char*)mem + page_size) & ~((uintptr_t)page_size - 1));
                ::VirtualFree(mem, 0, MEM_RELEASE);
#else
                // -- POSIX: mmap PROT_NONE reservation ------------------------------
                void* mem = ::mmap(
                    nullptr, m_data_region_size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
                // -- Platform-independent: map twice back-to-back -------------------
                std::unique_ptr<ipc::mapped_region> region0, region1;
                bool mapping_failed = false;

                try {
                    region0.reset(new ipc::mapped_region(file, data_rights, 0,
                        m_data_region_size, ptr, map_extra_options));
                    region1.reset(new ipc::mapped_region(file, data_rights, 0, 0,
                        ((char*)region0->data()) + m_data_region_size, map_extra_options));
                }
                catch (const std::exception&) {
                    mapping_failed = true;
                }
                catch (...) {
                    mapping_failed = true;
                }

                // -- Validate layout -------------------------------------------------
                bool layout_ok = !mapping_failed &&
                    region0 && region1 &&
                    region0->data() == ptr &&
                    region1->data() == ptr + m_data_region_size &&
                    region0->size() == m_data_region_size &&
                    region1->size() == m_data_region_size;

                if (layout_ok) {
                    // Success! MAP_FIXED has replaced the PROT_NONE reservation.
                    m_data_region_0 = std::move(region0);
                    m_data_region_1 = std::move(region1);
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

                // -- Failure: clean up and retry or fail ----------------------------
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
                sintra_warn_if_cacheline_mismatch(assumed_cache_line_size);
            });
#endif
            return true;
        }
        catch (...) {
            return false;
        }
    }

    std::unique_ptr<ipc::mapped_region> m_data_region_0;
    std::unique_ptr<ipc::mapped_region> m_data_region_1;
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
 * Extends Ring_data by adding the shared "control" region which holds all
 * cross-process state: publisher sequence, reader sequences, semaphores, and
 * small stacks used by the hybrid sleeping policy.
 */
template <typename T, bool READ_ONLY_DATA>
struct Ring: Ring_data<T, READ_ONLY_DATA>
{
    static_assert(
        std::is_trivially_copyable_v<T>,
        "sintra::Ring element type T must be trivially copyable."
    );
    static_assert(
        std::is_trivially_destructible_v<T>,
        "sintra::Ring element type T must be trivially destructible."
    );

    enum Reader_status : uint8_t {
        READER_STATE_INACTIVE = 0,
        READER_STATE_ACTIVE   = 1,
        READER_STATE_EVICTED  = 2
    };

    // Union-based reader state: eliminates bit shifts/masks, uses direct field access
    union Reader_state_union {
        struct Fields {
            uint8_t octile;          // Octile number (0-7)
            uint8_t guard_present;   // 0 = no guard, 1 = guard present
            uint8_t status;          // Reader_status enum value
            uint8_t padding;         // Unused, maintains 4-byte alignment
        } fields;
        uint32_t word;

        constexpr Reader_state_union() : word(0) {}
        constexpr Reader_state_union(uint32_t w) : word(w) {}
        constexpr Reader_state_union(Reader_status s, uint8_t o, bool g)
            : fields{o, g ? uint8_t(1) : uint8_t(0), static_cast<uint8_t>(s), 0} {}

        static constexpr Reader_state_union make(Reader_status status, uint8_t octile, bool guard_present)
        {
            return Reader_state_union(status, octile, guard_present);
        }

        uint8_t status()        const { return fields.status; }
        bool    guard_present() const { return fields.guard_present != 0; }
        uint8_t guard_octile()  const { return fields.octile; }

        Reader_state_union with_status(uint8_t status_value) const
        {
            Reader_state_union copy(*this);
            copy.fields.status = status_value;
            return copy;
        }

        Reader_state_union with_guard(uint8_t octile, bool present) const
        {
            Reader_state_union copy(*this);
            copy.fields.octile = octile;
            copy.fields.guard_present = present ? 1 : 0;
            return copy;
        }

        template <typename F>
        static Reader_state_union cas_update(std::atomic<uint32_t>& state, F&& transform)
        {
            Reader_state_union current(state);
            while (true) {
                Reader_state_union desired = transform(current);
                if (state.compare_exchange_strong(current.word, desired.word)) {
                    return current;
                }
            }
        }

        template <typename F>
        static Reader_state_union cas_update_if(std::atomic<uint32_t>& state, F&& transform, bool& updated)
        {
            Reader_state_union current(state);
            while (true) {
                std::optional<Reader_state_union> desired = transform(current);
                if (!desired.has_value()) {
                    updated = false;
                    return current;
                }
                if (state.compare_exchange_strong(current.word, desired->word)) {
                    updated = true;
                    return current;
                }
            }
        }
    };

    // Helper functions for encoded uint8_t guard tokens (used by accessor return values)
    static constexpr bool encoded_guard_present(uint8_t encoded)
    {
        return (encoded & 0x08) != 0;
    }

    static constexpr uint8_t encoded_guard_octile(uint8_t encoded)
    {
        return encoded & 0x07;
    }

    // All atomic operations use seq_cst (default) memory ordering for maximum safety.
    // Relaxed orderings provide minimal performance benefit but introduce subtle race
    // conditions that may only appear after hours or days of testing. The performance
    // cost of seq_cst is negligible compared to the IPC communication overhead, and
    // the correctness guarantee is invaluable for a lock-free concurrent data structure.

    // Helper: pad to a cache line to reduce false sharing in Control arrays.
    struct cache_line_sized_t {
        struct Packed_reader_state {
            std::atomic<uint32_t> word{Reader_state_union::make(READER_STATE_INACTIVE, 0, false).word};

            // Status field access
            uint8_t status() const
            {
                return Reader_state_union(word).status();
            }

            void set_status(uint8_t value)
            {
                Reader_state_union::cas_update(word, [value](Reader_state_union current) {
                    return current.with_status(value);
                });
            }

            // Trailing octile field access
            uint8_t trailing_octile() const
            {
                return Reader_state_union(word).guard_octile();
            }

            void set_trailing_octile(uint8_t value)
            {
                Reader_state_union::cas_update(word, [value](Reader_state_union current) {
                    return current.with_guard(value, current.guard_present());
                });
            }

            // Guard token field access (encoded: bit 0x08 = present, bits 0x07 = octile)
            uint8_t guard_token() const
            {
                Reader_state_union state(word);
                return state.guard_present() ? (0x08 | state.guard_octile()) : 0;
            }

            void set_guard_token(uint8_t value)
            {
                Reader_state_union::cas_update(word, [value](Reader_state_union current) {
                    return current.with_guard(value & 0x07, (value & 0x08) != 0);
                });
            }

            uint8_t exchange_guard_token(uint8_t value)
            {
                Reader_state_union previous = Reader_state_union::cas_update(
                    word, [value](Reader_state_union current) {
                        return current.with_guard(value & 0x07, (value & 0x08) != 0);
                    });
                return previous.guard_present() ? (0x08 | previous.guard_octile()) : 0;
            }

            template <typename F>
            uint8_t fetch_update_guard_token_if(F&& transform, bool& updated)
            {
                Reader_state_union previous = Reader_state_union::cas_update_if(
                    word, std::forward<F>(transform), updated);
                return previous.guard_present() ? (0x08 | previous.guard_octile()) : 0;
            }
        };

        struct Payload : Packed_reader_state {
            std::atomic<sequence_counter_type> v;
            std::atomic<uint32_t> owner_pid{0};
        };

        Payload data;
        std::array<uint8_t, (assumed_cache_line_size - sizeof(Payload))> padding{};

        static_assert(sizeof(Payload) <= assumed_cache_line_size,
                      "The payload of cache_line_sized_t exceeds the assumed cache line size.");
    };

    /**
     * A simple fixed-capacity stack of indices. Eliminates duplicate
     * push/pop/contains logic for ready_stack, sleeping_stack, unordered_stack, etc.
     * This lives in shared memory (control file), so we avoid std::vector or other
     * heap-backed containers that are not trivially relocation-safe across processes.
     */
    template <int N>
    struct Index_stack {
        int arr[N]{};
        int count = 0;

        void clear() { count = 0; }
        bool empty() const { return count == 0; }
        int size() const { return count; }

        void push(int value)
        {
            assert(count < N);
            arr[count++] = value;
        }

        int pop_or(int fallback)
        {
            if (count == 0) {
                return fallback;
            }
            int value = arr[--count];
            arr[count] = -1;
            return value;
        }

        bool contains(int value) const
        {
            for (int i = 0; i < count; ++i) {
                if (arr[i] == value) {
                    return true;
                }
            }
            return false;
        }

        bool remove_value(int value)
        {
            if (count > 0 && arr[count - 1] == value) {
                arr[--count] = -1;
                return true;
            }
            for (int i = 0; i < count; ++i) {
                if (arr[i] == value) {
                    arr[i] = arr[count - 1];
                    arr[--count] = -1;
                    note_non_tail_removal(value);
                    return true;
                }
            }
            return false;
        }

        template <typename F>
        void drain(F&& fn)
        {
            while (!empty()) {
                const int value = pop_or(-1);
                if (value >= 0) {
                    fn(value);
                }
            }
        }

        int& operator[](int index) { return arr[index]; }
        const int& operator[](int index) const { return arr[index]; }

#ifndef NDEBUG
        static inline std::atomic<uint64_t> s_non_tail_removals{0};

        void note_non_tail_removal(int value)
        {
            const auto removal_count = s_non_tail_removals.fetch_add(1) + 1;
            if (removal_count == 1) {
                Log_stream(log_level::debug)
                    << "[sintra][ring] Index_stack non-tail removal; "
                    << "out-of-order wakeups detected (value=" << value << ").\n";
            }
        }
#else
        void note_non_tail_removal(int /*value*/) {}
#endif
    };

    /**
     * Shared control block. All fields that are concurrently modified are atomic.
     * NOTE: Atomics are chosen such that they are address-free and (ideally) lock-free.
     */
    struct Control
    {
        // This struct is always instantiated in a memory region which is shared among processes.
        // The atomics below are used for *cross-process* communication, including in a
        // double-mapped ("magic ring") region. See the detailed note & lock-free checks
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
        //   With max_process_index statically capped at 255, wraparound to 0 is unreachable.
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
        std::atomic<uint64_t>                stale_guard_clear_count{0};
        std::atomic<uint64_t>                guard_rollback_attempt_count{0};
        std::atomic<uint64_t>                guard_rollback_success_count{0};
        std::atomic<uint64_t>                guard_accounting_mismatch_count{0};

        // Per-reader currently visible (snapshot) sequence. Cache-line sized to minimize
        // false sharing between reader slots. A reader sets its slot to the snapshot head
        // and advances it as it consumes ranges.
        cache_line_sized_t                   reading_sequences[max_process_index];

        // --- Reader Sequence Stack Management ------------------------------------
        // Protects free_rs_stack during slot acquisition/release.
        spinlock                             rs_stack_spinlock;

        // Freelist of reader-slot indices into reading_sequences[].
        Index_stack<max_process_index>       free_rs_stack;
        // --- End Reader Sequence Stack Management --------------------------------

        bool scavenge_orphans()
        {
            bool freed = false;

            spinlock::locker lock(rs_stack_spinlock);

            for (int i = 0; i < max_process_index; ++i) {
                auto& slot = reading_sequences[i].data;

                if (slot.status() == READER_STATE_INACTIVE) {
                    continue;
                }

                const uint32_t pid = slot.owner_pid.load();

                bool owner_unknown = (pid == 0);
                bool dead = owner_unknown || !is_process_alive(pid);

                if (dead) {
                    const uint8_t guard_snapshot = slot.exchange_guard_token(0);
                    if ((guard_snapshot & 0x08) != 0) {
                        const uint8_t oct = guard_snapshot & 0x07;
                        read_access.fetch_sub(octile_mask(oct));
                    }

                    slot.set_status(READER_STATE_INACTIVE);
                    slot.owner_pid = 0;

                    if (!free_rs_stack.contains(i)) {
                        free_rs_stack.push(i);
                    }
                    freed = true;
                }
            }

            return freed;
        }

        void release_local_semaphores()
        {
            for (auto& sem : dirty_semaphores) {
                sem.release_local_handle();
            }
        }

        // Used to avoid accidentally having multiple writers on the same ring
        // across processes. Only one writer may hold this at a time.
        std::atomic<uint32_t>                writer_pid{0};
        detail::interprocess_mutex           ownership_mutex;

        // The following synchronization structures may only be accessed between lock()/unlock().

        // An array (pool) of semaphores to synchronize reader wakeups. The writer posts these
        // on publish; readers may also be unblocked locally in an "unordered" fashion.
        sintra_ring_semaphore                dirty_semaphores[max_process_index];

        // A stack of indices into dirty_semaphores[] that are free/ready for use.
        // Initially all semaphores are ready.
        Index_stack<max_process_index>       ready_stack;

        // A stack of indices allocated to readers that are blocking / about to block /
        // or were blocking and not yet redistributed.
        Index_stack<max_process_index>       sleeping_stack;

        // A stack of indices that were posted "out of order" (e.g., after a local unblock).
        // Unordered posts leave the index in sleeping_stack but flag the semaphore to avoid
        // re-posting; the next ordered post (e.g., on publish) drains unordered items back
        // to ready_stack. This keeps the wakeup path simple while preventing double-posts.
        Index_stack<max_process_index>       unordered_stack;

        void flush_wakeups()
        {
            sleeping_stack.drain([&]( int idx) { dirty_semaphores[idx].post_ordered(); });
            unordered_stack.drain([&](int idx) { ready_stack.push(idx); });
        }

        // Spinlock guarding the ready/sleeping/unordered stacks.
        spinlock                             m_spinlock;

        Control()
        {
            // Initialize ready_stack with all indices (full), others empty
            for (int i = 0; i < max_process_index; i++) { ready_stack.push(i); }

            for (int i = 0; i < max_process_index; i++) { reading_sequences[i].data.v = invalid_sequence; }
            for (int i = 0; i < max_process_index; i++) { free_rs_stack.push(i); }

            writer_pid = 0;

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
        }

        static_assert(max_process_index <= 255,
            "max_process_index must be <= 255 because read_access uses 8-bit octile counters.");
    };

    Ring(const std::string& directory,
         const std::string& data_filename,
         size_t             num_elements)
    : 
        Ring_data<T, READ_ONLY_DATA>(directory, data_filename, num_elements)
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
        m_control->num_attached++;
    }

    ~Ring()
    {
        if (m_control) {
            m_control->release_local_semaphores();
            // The *last* detaching process deletes both control and data files. On
            // platforms where Control wraps kernel semaphore handles (Windows/macOS)
            // we must explicitly run the destructor once the final reference drops.
            if (m_control->num_attached.fetch_sub(1) == 1) {
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
        return m_control->leading_sequence;
    }

    /**
     * Map a sequence number to the in-memory element pointer.
     * Returns nullptr if the sequence is out of the readable historical window.
     */
    T* get_element_from_sequence(sequence_counter_type seq) const
    {
        const auto leading = m_control->leading_sequence.load();
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

        diag.max_reader_lag                 = m_control->max_reader_lag;
        diag.worst_overflow_lag             = m_control->worst_overflow_lag;
        diag.reader_lag_overflow_count      = m_control->reader_lag_overflow_count;
        diag.reader_sequence_regressions    = m_control->reader_sequence_regressions;
        diag.reader_eviction_count          = m_control->reader_eviction_count;
        diag.last_evicted_reader_index      = m_control->last_evicted_reader_index;
        diag.last_evicted_reader_sequence   = m_control->last_evicted_reader_sequence;
        diag.last_evicted_writer_sequence   = m_control->last_evicted_writer_sequence;
        diag.last_evicted_reader_octile     = m_control->last_evicted_reader_octile;
        diag.last_overflow_reader_index     = m_control->last_overflow_reader_index;
        diag.last_overflow_reader_sequence  = m_control->last_overflow_reader_sequence;
        diag.last_overflow_leading_sequence = m_control->last_overflow_leading_sequence;
        diag.last_overflow_last_consumed    = m_control->last_overflow_last_consumed;
        diag.stale_guard_clear_count        = m_control->stale_guard_clear_count;
        diag.guard_rollback_attempt_count   = m_control->guard_rollback_attempt_count;
        diag.guard_rollback_success_count   = m_control->guard_rollback_success_count;
        diag.guard_accounting_mismatch_count = m_control->guard_accounting_mismatch_count;
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
            {
                spinlock::locker lock(c.rs_stack_spinlock);
                if (!c.free_rs_stack.empty()) {
                    m_rs_index = c.free_rs_stack.pop_or(-1);

                    // Mark our slot as ACTIVE while the spinlock is still held so the
                    // scavenger cannot reclaim it before we publish the ownership.
                    auto& slot = c.reading_sequences[m_rs_index].data;
                    slot.owner_pid = get_current_pid();
                    slot.set_status(Ring<T, true>::READER_STATE_ACTIVE);

                    break;
                }
            }

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
            spinlock::locker lock(c.rs_stack_spinlock);

            // Mark slot as inactive and clear ownership while the freelist is locked,
            // so scavenger cannot race a half-updated slot.
            auto& slot = c.reading_sequences[m_rs_index].data;
            slot.owner_pid = 0;
            slot.set_status(Ring<T, true>::READER_STATE_INACTIVE);

            // Push only if not already in the freelist (defensive: avoid duplicates).
            if (!c.free_rs_stack.contains(m_rs_index)) {
                c.free_rs_stack.push(m_rs_index);
            }
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

        if (m_reading) {
            m_reading_lock = false;
            throw std::logic_error(
                "Sintra Ring: Cannot call start_reading() again before calling done_reading().");
        }

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        if (c.reading_sequences[m_rs_index].data.status() == Ring<T, true>::READER_STATE_EVICTED) {
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
            auto leading_sequence = c.leading_sequence.load();

            auto range_first_sequence =
                std::max<int64_t>(0, int64_t(leading_sequence) - int64_t(num_trailing_elements));

            size_t trailing_idx = mod_pos_i64(
                int64_t(range_first_sequence) - int64_t(m_max_trailing_elements), this->m_num_elements);

            uint8_t trailing_octile = octile_of_index(trailing_idx, this->m_num_elements);
            uint64_t guard_mask     = octile_mask(trailing_octile);

            c.read_access.fetch_add(guard_mask);

            bool guard_attached = false;
            const uint8_t previous_state = c.reading_sequences[m_rs_index].data.fetch_update_guard_token_if(
                [&](typename Ring<T, true>::Reader_state_union current)
                    -> std::optional<typename Ring<T, true>::Reader_state_union>
                {
                    if (current.status() == Ring<T, true>::READER_STATE_EVICTED) {
                        return std::nullopt;
                    }

                    return current.with_guard(trailing_octile, true);
                },
                guard_attached);

            if (!guard_attached) {
                c.read_access.fetch_sub(guard_mask);
                m_reading = false;
                m_reading_lock = false;
                throw ring_reader_evicted_exception();
            }
            else {
                if (Ring<T, true>::encoded_guard_present(previous_state)) {
                    const uint8_t previous_octile = Ring<T, true>::encoded_guard_octile(previous_state);
                    if (previous_octile != trailing_octile) {
                        const uint64_t prev_mask = octile_mask(previous_octile);
                        c.read_access.fetch_sub(prev_mask);
                    }
                    else {
                        c.read_access.fetch_sub(guard_mask);
                    }
                }
            }

            auto confirmed_leading_sequence = c.leading_sequence.load();
            auto confirmed_range_first_sequence = std::max<int64_t>(
                0, int64_t(confirmed_leading_sequence) - int64_t(num_trailing_elements));

            size_t confirmed_trailing_idx = mod_pos_i64(
                int64_t(confirmed_range_first_sequence) - int64_t(m_max_trailing_elements), this->m_num_elements);
            uint8_t confirmed_trailing_octile =
                octile_of_index(confirmed_trailing_idx, this->m_num_elements);

            if (confirmed_trailing_octile == trailing_octile) {
                if (c.reading_sequences[m_rs_index].data.status() == Ring<T, true>::READER_STATE_EVICTED) {
                    m_reading = false;
                    m_reading_lock = false;
                    throw ring_reader_evicted_exception();
                }

                ret.begin = this->m_data + mod_pos_i64(confirmed_range_first_sequence, this->m_num_elements);
                ret.end   = ret.begin + (confirmed_leading_sequence - confirmed_range_first_sequence);

                m_trailing_octile = trailing_octile;
                m_reading_sequence->store(confirmed_leading_sequence);
                m_last_consumed_sequence = confirmed_leading_sequence;
                break;
            }

            // Trailing guard requirement changed between reads; drop and retry.
            bool guard_cleared = false;
            const uint8_t guard_snapshot = c.reading_sequences[m_rs_index].data.fetch_update_guard_token_if(
                [&](typename Ring<T, true>::Reader_state_union current)
                    -> std::optional<typename Ring<T, true>::Reader_state_union>
                {
                    if (current.status() == Ring<T, true>::READER_STATE_EVICTED) {
                        return std::nullopt;
                    }
                    return current.with_guard(current.guard_octile(), false);
                },
                guard_cleared);

            // Always release our own guard contribution before deciding next steps.
            c.read_access.fetch_sub(guard_mask);

            if (!guard_cleared) {
                m_reading      = false;
                m_reading_lock = false;
                throw ring_reader_evicted_exception();
            }
            if (Ring<T, true>::encoded_guard_present(guard_snapshot)) {
                const uint8_t guarded_octile = Ring<T, true>::encoded_guard_octile(guard_snapshot);
                if (guarded_octile != trailing_octile) {
                    const uint64_t prev_mask = octile_mask(guarded_octile);
                    c.read_access.fetch_sub(prev_mask);
                }
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
        if (!m_reading) {
            request_stop();
            return;
        }

        bool expected = false;
        while (!m_reading_lock.compare_exchange_strong(expected, true)) {
            expected = false;
        }

        if (m_reading) {
            bool   guard_cleared       = false;
            const uint8_t previous_state = c.reading_sequences[m_rs_index].data.fetch_update_guard_token_if(
                [&](typename Ring<T, true>::Reader_state_union current)
                    -> std::optional<typename Ring<T, true>::Reader_state_union>
                {
                    if (current.status() == Ring<T, true>::READER_STATE_EVICTED) {
                        return std::nullopt;
                    }
                    return current.with_guard(current.guard_octile(), false);
                },
                guard_cleared);

            if (guard_cleared && Ring<T, true>::encoded_guard_present(previous_state)) {
                const uint8_t released_octile = Ring<T, true>::encoded_guard_octile(previous_state);
                const uint64_t released_mask = octile_mask(released_octile);
                c.read_access.fetch_sub(released_mask);
            }
            else
            if (!guard_cleared) {
                try_rollback_unpaired_read_access(static_cast<uint8_t>(m_trailing_octile));
                // With strong CAS, failing to clear guard means we were evicted
                m_evicted_since_last_wait = true;
            }
            else {
                const bool had_guard = Ring<T, true>::encoded_guard_present(previous_state);
                if (!had_guard) {
                    try_rollback_unpaired_read_access(
                        static_cast<uint8_t>(m_trailing_octile));
                }
            }
            m_reading = false;
        }
        else {
            // done_reading() called without active snapshot => shutdown signal
            request_stop();
        }

        m_reading_lock = false;
    }

    sequence_counter_type reading_sequence()     const { return m_reading_sequence->load(); }

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
        constexpr auto blocking_wait_watchdog = std::chrono::milliseconds(50);

        auto produce_range = [&]() -> Range<T> {
            if (handle_eviction_if_needed()) {
                return {};
            }

            auto start_sequence   = m_reading_sequence->load();
            auto leading_sequence = c.leading_sequence.load();
            const auto last_consumed_before = m_last_consumed_sequence;

            auto update_max_relaxed = [](auto& target, sequence_counter_type value) {
                sequence_counter_type current = target;
                while (value > current && !target.compare_exchange_strong(current, value))
                {}
            };

            Range<T> ret;

            sequence_counter_type full_lag = 0;
            if (leading_sequence >= start_sequence) {
                full_lag = leading_sequence - start_sequence;
            }
            else {
                c.reader_sequence_regressions++;
            }

            update_max_relaxed(c.max_reader_lag, full_lag);

            sequence_counter_type clamped_lag = full_lag;
            if (clamped_lag > sequence_counter_type(this->m_num_elements)) {
                c.reader_lag_overflow_count++;
                update_max_relaxed(c.worst_overflow_lag, full_lag);
                c.last_overflow_reader_index     = static_cast<uint32_t>(m_rs_index);
                c.last_overflow_reader_sequence  = start_sequence;
                c.last_overflow_leading_sequence = leading_sequence;
                c.last_overflow_last_consumed    = last_consumed_before;
                clamped_lag = sequence_counter_type(this->m_num_elements);
            }

            if (start_sequence < m_last_consumed_sequence) {
                c.reader_sequence_regressions++;
            }

            size_t num_range_elements = static_cast<size_t>(clamped_lag);

            if (num_range_elements == 0) {
                // Could happen if we were explicitly unblocked
                return ret;
            }

            ret.begin = this->m_data + mod_u64(start_sequence, this->m_num_elements);
            ret.end   = ret.begin + num_range_elements;
            m_reading_sequence->fetch_add(num_range_elements);  // +=
            m_last_consumed_sequence = start_sequence + sequence_counter_type(num_range_elements);
            return ret;
        };

        auto sequences_equal = [&]() {
            return m_reading_sequence->load() == c.leading_sequence;
        };

        while (true) {
            bool heartbeat_timeout = false;
            if (m_stopping) {
                return Range<T>{};
            }

            // Phase 1 - fast spin: aggressively poll for a very short window to
            // deliver sub-100µs wakeups when the writer is still active.
            const double fast_spin_end = get_wtime() + fast_spin_duration;
            while (sequences_equal() && get_wtime() < fast_spin_end) {
                if (m_stopping) {
                    return Range<T>{};
                }
            }

            if (sequences_equal()) {
                // Phase 2 - precision sleeps: yield the CPU in 1ms slices while
                // still polling at a high enough cadence to catch bursts quickly.
                Scoped_timer_resolution timer_resolution_guard(1);
                const double precision_sleep_end = get_wtime() + precision_sleep_duration;
                while (sequences_equal() && get_wtime() < precision_sleep_end) {
                    if (m_stopping) {
                        return Range<T>{};
                    }
                    const auto seq_now = c.global_unblock_sequence.load();
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
                // Phase 3 - blocking wait: fully park on the semaphore to avoid
                // burning CPU when the writer is stalled for long periods.
                const auto unblock_sequence_now = c.global_unblock_sequence.load();
                if (unblock_sequence_now != m_seen_unblock_sequence) {
                    m_seen_unblock_sequence = unblock_sequence_now;
                    if (!sequences_equal()) {
                        return produce_range();
                    }
                    return Range<T>{};
                }

                c.m_spinlock.lock();
                m_sleepy_index = -1;
                if (sequences_equal()) {
                    if (m_stopping) {
                        c.m_spinlock.unlock();
                        return Range<T>{};
                    }
                    int sleepy = c.ready_stack.pop_or(-1);
                    m_sleepy_index = sleepy;
                    c.sleeping_stack.push(sleepy);
                }
                const auto unblock_sequence_after = c.global_unblock_sequence.load();
                if (unblock_sequence_after != m_seen_unblock_sequence) {
                    m_seen_unblock_sequence = unblock_sequence_after;
                    const int sleepy = m_sleepy_index;
                    if (sleepy >= 0) {
                        c.sleeping_stack.remove_value(sleepy);
                        c.ready_stack.push(sleepy);
                        m_sleepy_index = -1;
                    }
                    c.m_spinlock.unlock();
                    if (!sequences_equal()) {
                        return produce_range();
                    }
                    return Range<T>{};
                }
                c.m_spinlock.unlock();

                int sleepy_index = m_sleepy_index;
                if (sleepy_index >= 0) {
                    while (true) {
                        if (m_stopping) {
                            spinlock::locker lock(c.m_spinlock);
                            if (m_sleepy_index >= 0) {
                                c.dirty_semaphores[sleepy_index].post_unordered();
                            }
                            return Range<T>{};
                        }

                        auto wait_status = c.dirty_semaphores[sleepy_index].wait_for(blocking_wait_watchdog);
                        if (wait_status == sintra_ring_semaphore::wait_result::timeout) {
                            spinlock::locker lock(c.m_spinlock);
                            const int current = m_sleepy_index;
                            if (current >= 0) {
                                c.sleeping_stack.remove_value(current);
                                c.ready_stack.push(current);
                                m_sleepy_index = -1;
                            }
                            heartbeat_timeout = true;
                            break;
                        }

                        if (wait_status == sintra_ring_semaphore::wait_result::unordered) {
                            spinlock::locker lock(c.m_spinlock);
                            c.unordered_stack.push(sleepy_index);
                        }
                        else {
                            spinlock::locker lock(c.m_spinlock);
                            c.ready_stack.push(sleepy_index);
                        }
                        m_sleepy_index = -1;

                        if (m_stopping) {
                            return Range<T>{};
                        }
                        break;
                    }
                }
            }

            if (!sequences_equal()) {
                return produce_range();
            }
            if (heartbeat_timeout) {
                continue;
            }
            return Range<T>{};
        }
    }

    /**
     * After wait_for_new_data(), call this to release the trailing guard when
     * crossing to a new octile.
     */
    void done_reading_new_data()
    {
        // CRITICAL: We must not return until guard is successfully updated OR we confirm
        // eviction was handled. Returning with stale guard can cause permanent deadlock.
        while (true) {
            // The eviction handler can advance m_reading_sequence and reattach the guard
            // to a new octile. Recompute the trailing position afterwards so we migrate
            // toward the post-eviction state rather than the stale pre-eviction value.
            if (handle_eviction_if_needed()) {
                continue;  // Eviction handled, retry from top
            }

            const size_t t_idx = mod_pos_i64(
                int64_t(m_reading_sequence->load()) - int64_t(m_max_trailing_elements),
                this->m_num_elements);

            const uint8_t new_trailing_octile = octile_of_index(t_idx, this->m_num_elements);

            if (new_trailing_octile == m_trailing_octile) {
                return;  // Nothing to do
            }

            const uint64_t new_mask     = octile_mask(new_trailing_octile);

            c.read_access.fetch_add(new_mask);

            bool guard_updated = false;
            const uint8_t previous_state = c.reading_sequences[m_rs_index].data.fetch_update_guard_token_if(
                [&](typename Ring<T, true>::Reader_state_union current)
                    -> std::optional<typename Ring<T, true>::Reader_state_union>
                {
                    if (current.status() == Ring<T, true>::READER_STATE_EVICTED) {
                        return std::nullopt;
                    }
                    if (!current.guard_present()) {
                        return std::nullopt;
                    }

                    return current.with_guard(static_cast<uint8_t>(new_trailing_octile), true);
                },
                guard_updated);

            if (!guard_updated) {
                c.read_access.fetch_sub(new_mask);

                // Check why CAS failed to determine retry strategy. The guard accessor returns an
                // encoded byte, so inspect the decoded fields directly instead of interpreting it
                // as a Reader_state_union (which would zero the guard-present bit).
                const bool guard_present = Ring<T, true>::encoded_guard_present(previous_state);
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
                const bool was_evicted =
                    c.reading_sequences[m_rs_index].data.status() == Ring<T, true>::READER_STATE_EVICTED;
#else
                const bool was_evicted = false;
#endif

                if (was_evicted || !guard_present) {
                    if (!guard_present) {
                        try_rollback_unpaired_read_access(static_cast<uint8_t>(m_trailing_octile));
                    }
                    handle_eviction_if_needed();
                    continue;  // Retry after handling eviction
                }

                // Spurious CAS failure or concurrent modification - yield and retry
                std::this_thread::yield();
                continue;
            }

            // Success - clean up old guard octile
            const uint8_t previous_octile = Ring<T, true>::encoded_guard_octile(previous_state);
            if (previous_octile != new_trailing_octile) {
                const uint64_t prev_mask = octile_mask(previous_octile);
                c.read_access.fetch_sub(prev_mask);
            }
            else {
                c.read_access.fetch_sub(new_mask);
            }

            m_trailing_octile = static_cast<uint8_t>(new_trailing_octile);
            return;
        }
    }

    bool handle_eviction_if_needed()
    {
        auto& slot = c.reading_sequences[m_rs_index].data;
        const uint8_t guard_snapshot = slot.guard_token();
        if ((guard_snapshot & 0x08) != 0) {
            return false;
        }

        try_rollback_unpaired_read_access(static_cast<uint8_t>(m_trailing_octile));

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        if (slot.status() == Ring<T, false>::READER_STATE_EVICTED) {
            // Reader was evicted by writer for being too slow.
            // Skip all missed data and jump to writer's current position.
            // This is the only safe recovery strategy since old data has been overwritten.
            sequence_counter_type new_seq = c.leading_sequence;
            slot.v = new_seq;
            m_reading_sequence->store(new_seq);
            m_last_consumed_sequence = new_seq;

            // Recalculate trailing octile to match the new jumped-forward position
            const size_t trailing_idx = mod_pos_i64(
                int64_t(new_seq) - int64_t(m_max_trailing_elements), this->m_num_elements);
            m_trailing_octile = octile_of_index(trailing_idx, this->m_num_elements);

            reattach_after_eviction();
            m_evicted_since_last_wait = true;
            return true;
        }
#endif

        const size_t trailing_idx = mod_pos_i64(
            int64_t(m_reading_sequence->load()) - int64_t(m_max_trailing_elements),
            this->m_num_elements);
        m_trailing_octile = octile_of_index(trailing_idx, this->m_num_elements);

        reattach_after_eviction();
        return false;
    }

    void reattach_after_eviction()
    {
        const uint64_t mask = octile_mask(static_cast<uint8_t>(m_trailing_octile));

        while (true) {
            c.read_access.fetch_add(mask);

            bool guard_updated = false;
            const uint8_t previous_state = c.reading_sequences[m_rs_index].data.fetch_update_guard_token_if(
                [&](typename Ring<T, true>::Reader_state_union current)
                    -> std::optional<typename Ring<T, true>::Reader_state_union>
                {
                    if (current.guard_present()) {
                        return std::nullopt;
                    }

                    return current.with_status(Ring<T, true>::READER_STATE_ACTIVE)
                                  .with_guard(static_cast<uint8_t>(m_trailing_octile), true);
                },
                guard_updated);

            if (guard_updated) {
                const uint8_t guard_snapshot = c.reading_sequences[m_rs_index].data.guard_token();
                const bool guard_present = Ring<T, true>::encoded_guard_present(guard_snapshot);
                const uint8_t guard_octile = Ring<T, true>::encoded_guard_octile(guard_snapshot);
                if (!guard_present || guard_octile != static_cast<uint8_t>(m_trailing_octile)) {
                    c.read_access.fetch_sub(mask);
                    continue;
                }
                return;
            }

            c.read_access.fetch_sub(mask);

            // Check if someone else already attached the correct guard
            if (Ring<T, true>::encoded_guard_present(previous_state) &&
                Ring<T, true>::encoded_guard_octile( previous_state) == static_cast<uint8_t>(m_trailing_octile))
            {
                // Guard is present with correct octile - work already done
                return;
            }

            std::this_thread::yield();
        }
    }

public:
    bool consume_eviction_notification()
    {
        return m_evicted_since_last_wait.exchange(false);
    }

    bool eviction_pending() const
    {
        return m_evicted_since_last_wait;
    }

    /**
     * If another thread is in wait_for_new_data(), force it to return an empty
     * range without publishing any new data. Only affects this reader instance.
     */
    void unblock_local()
    {
        spinlock::locker lock(c.m_spinlock);
        int sleepy = m_sleepy_index;
        if (sleepy >= 0) {
            c.dirty_semaphores[sleepy].post_unordered();
        }
    }

    void request_stop()
    {
        m_stopping = true;
        unblock_local();
    }

    bool try_rollback_unpaired_read_access(uint8_t octile)
    {
        if (octile >= 8) {
            return false;
        }

        c.guard_rollback_attempt_count.fetch_add(1, std::memory_order_relaxed);

        auto count_guards_for_octile = [&](uint8_t target) -> uint32_t {
            uint32_t count = 0;
            for (int i = 0; i < max_process_index; ++i) {
                const uint8_t guard_snapshot = c.reading_sequences[i].data.guard_token();
                if ((guard_snapshot & 0x08) == 0) {
                    continue;
                }
                const uint8_t guard_octile = guard_snapshot & 0x07;
                if (guard_octile == target) {
                    ++count;
                }
            }
            return count;
        };

        uint64_t access_snapshot = c.read_access.load();
        uint32_t count = static_cast<uint32_t>((access_snapshot >> (8 * octile)) & 0xffu);
        if (count == 0) {
            return false;
        }

        const uint32_t guard_count = count_guards_for_octile(octile);
        if (guard_count != 0) {
            return false;
        }

        // Diagnostic: read_access shows guards but no guard tokens were found.
        if (count > 0 && guard_count == 0) {
            c.guard_accounting_mismatch_count.fetch_add(1, std::memory_order_relaxed);
        }

        std::this_thread::yield();

        access_snapshot = c.read_access.load();
        count = static_cast<uint32_t>((access_snapshot >> (8 * octile)) & 0xffu);
        if (count == 0) {
            return false;
        }
        if (count_guards_for_octile(octile) != 0) {
            return false;
        }

        const uint64_t decrement = (uint64_t(1) << (8 * octile));
        uint64_t expected = access_snapshot;
        uint64_t desired = expected - decrement;

        // Critical assertion: verify we're not decrementing below zero
        assert(count > 0 && "Attempting to rollback with zero read_access count");

        if (!c.read_access.compare_exchange_strong(expected, desired)) {
            return false;
        }

        c.guard_rollback_success_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool is_stopping() const
    {
        return m_stopping.load();
    }

private:
    const size_t                        m_max_trailing_elements;
    std::atomic<sequence_counter_type>* m_reading_sequence       = &s_zero_rs;
    size_t                              m_trailing_octile        = 0;
    uint64_t                            m_seen_unblock_sequence  = 0;
    sequence_counter_type               m_last_consumed_sequence = 0;
    std::atomic<bool>                   m_evicted_since_last_wait{false};

protected:
    std::atomic<bool>                   m_reading                = false;
    std::atomic<bool>                   m_reading_lock           = false;

private:
    std::atomic<int>                    m_sleepy_index           = -1;
    int                                 m_rs_index               = -1;
    std::atomic<bool>                   m_stopping               = false;

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
    thread_local uint32_t mine = next++;
    return mine;
}

template <typename T>
struct Ring_W : Ring<T, false>
{
    Ring_W(const std::string& directory,
           const std::string& data_filename,
           size_t             num_elements)
    :   Ring<T, false>::Ring(directory, data_filename, num_elements),
        c(*this->m_control)
    {
        ensure_writer_mutex_consistency();

        // Single writer across processes
        if (!c.ownership_mutex.try_lock()) {
            throw ring_acquisition_failure_exception();
        }

        c.writer_pid = get_current_pid();
    }

    ~Ring_W()
    {
        // Wake any sleeping readers to avoid deadlocks during teardown
        unblock_global();
        c.ownership_mutex.unlock();
        c.writer_pid = 0;
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
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(write_location, src_buffer, num_src_elements * sizeof(T));
        }
        else {
            std::copy_n(src_buffer, num_src_elements, write_location);
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
        static_assert(
            std::is_trivially_copyable_v<T2> || ring_payload_traits<T2>::allow_nontrivial,
            "Ring payload T2 must be trivially copyable or explicitly allowed by ring_payload_traits."
        );
        static_assert(
            std::is_trivially_destructible_v<T2> || ring_payload_traits<T2>::allow_nontrivial,
            "Ring payload T2 must be trivially destructible or explicitly allowed by ring_payload_traits."
        );
        auto num_elements = sizeof(T2) / sizeof(T) + num_extra_elements;
        T2* write_location = (T2*)prepare_write(num_elements);
        try {
            return new (write_location) T2{std::forward<Args>(args)...};
        }
        catch (...) {
            m_pending_new_sequence -= num_elements;
            const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
            m_octile = octile_of_index(head, this->m_num_elements);
            m_writing_thread_index = 0;
            throw;
        }
    }

    /**
     * Write and publish in one step (convenience for single or buffered writes).
     */
    sequence_counter_type write_commit(const T& value)
    {
        write(&value, 1);
        return done_writing();
    }

    sequence_counter_type write_commit(const T* src_buffer, size_t num_src_elements)
    {
        write(src_buffer, num_src_elements);
        return done_writing();
    }

    /**
     * Publish the pending write by updating leading_sequence.
     * Returns the new leading sequence value.
     *
     * Publish semantics:
     *  * All element stores must be completed before this atomic store (the
     *    default seq-cst store is sufficient here).
     */
    sequence_counter_type done_writing()
    {
        {
            spinlock::locker lock(c.m_spinlock);
            assert(m_writing_thread_index == thread_index());
            c.leading_sequence = m_pending_new_sequence;
            c.flush_wakeups();
        }
        m_writing_thread_index = 0;
        return m_pending_new_sequence;
    }

    /**
     * Force any reader waiting in wait_for_new_data() to wake up and return
     * an empty range (global unblock).
     */
    void unblock_global()
    {
        c.global_unblock_sequence++;

        spinlock::locker lock(c.m_spinlock);
        c.flush_wakeups();
    }

    /**
     * Return a read-only view of up to the most recent 3/4 of the ring. Useful
     * for diagnostics from the writer side (does not require locks).
     */
    Range<T> get_readable_range()
    {
        const auto leading = c.leading_sequence.load();
        const auto ring = this->m_num_elements;
        const auto range_first = (leading > (3*ring/4)) ? (leading - (3*ring/4)) : 0;

        Range<T> ret;
        ret.begin = this->m_data + mod_u64(range_first, ring);
        ret.end   = ret.begin + (leading - range_first);
        return ret;
    }


    void advance_writer_octile_if_needed(size_t head_index)
    {
        const uint8_t new_octile = octile_of_index(head_index, this->m_num_elements);
        if (m_octile == new_octile) {
            return;
        }

#ifndef NDEBUG
        {
            const uint64_t ra = c.read_access;
            for (int b = 0; b < 8; ++b) {
                assert(((ra >> (8 * b)) & 0xffu) != 0xffu && "read_access octile underflowed to 255");
            }
        }
#endif

        auto range_mask = (uint64_t(0xff) << (8 * new_octile));
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
        auto run_eviction_pass = [&]() {
            sequence_counter_type eviction_threshold =
                (m_pending_new_sequence > sequence_counter_type(SINTRA_EVICTION_LAG_RINGS) * this->m_num_elements)
                    ? (m_pending_new_sequence - sequence_counter_type(SINTRA_EVICTION_LAG_RINGS) * this->m_num_elements)
                    : 0;

            for (int i = 0; i < max_process_index; ++i) {
                if (c.reading_sequences[i].data.status() != Ring<T, false>::READER_STATE_ACTIVE) {
                    continue;
                }

                sequence_counter_type reader_seq = c.reading_sequences[i].data.v;
                uint8_t guard_snapshot           = c.reading_sequences[i].data.guard_token();
                bool    reader_has_guard         = (guard_snapshot & 0x08) != 0;
                uint8_t reader_octile            = guard_snapshot & 0x07;
                bool blocking_current_octile     = reader_has_guard && (reader_octile == new_octile);

                if (reader_seq >= eviction_threshold && !blocking_current_octile) {
                    continue;
                }

                bool guard_evicted = false;
                const uint8_t previous_state = c.reading_sequences[i].data.fetch_update_guard_token_if(
                    [&](typename Ring<T, false>::Reader_state_union current)
                        -> std::optional<typename Ring<T, false>::Reader_state_union>
                    {
                        if (!current.guard_present()) {
                            return std::nullopt;
                        }
                        auto cleared = current.with_guard(current.guard_octile(), false);
                        return cleared.with_status(Ring<T, false>::READER_STATE_EVICTED);
                    },
                    guard_evicted);

                if (!guard_evicted) {
                    continue;
                }

                const size_t evicted_reader_octile = Ring<T, false>::encoded_guard_octile(previous_state);

                const uint64_t evict_mask = octile_mask(static_cast<uint8_t>(evicted_reader_octile));
                c.read_access.fetch_sub(evict_mask);
                c.reader_eviction_count++;
                c.last_evicted_reader_index    = static_cast<uint32_t>(i);
                c.last_evicted_reader_sequence = reader_seq;
                c.last_evicted_writer_sequence = m_pending_new_sequence;
                c.last_evicted_reader_octile   = static_cast<uint32_t>(evicted_reader_octile);
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

        constexpr auto k_stale_guard_delay =
            std::chrono::milliseconds(SINTRA_STALE_GUARD_DELAY_MS);
        auto blocked_start = std::chrono::steady_clock::time_point{};
        uint64_t last_access_snapshot = 0;

        auto scan_blocking_readers = [&]() -> bool {
            for (int i = 0; i < max_process_index; ++i) {
                uint8_t guard_snapshot = c.reading_sequences[i].data.guard_token();
                if ((guard_snapshot & 0x08) != 0 && (guard_snapshot & 0x07) == new_octile) {
                    return true;
                }
            }
            return false;
        };

        auto count_guards_for_octile = [&](uint8_t target_octile) -> uint32_t {
            uint32_t count = 0;
            for (int i = 0; i < max_process_index; ++i) {
                uint8_t guard_snapshot = c.reading_sequences[i].data.guard_token();
                if ((guard_snapshot & 0x08) == 0) {
                    continue;
                }
                if ((guard_snapshot & 0x07) == target_octile) {
                    ++count;
                }
            }
            return count;
        };

        while (c.read_access & range_mask) {
            bool has_blocking_reader = scan_blocking_readers();

            if (has_blocking_reader) {
                blocked_start = std::chrono::steady_clock::time_point{};
            }

            if (!has_blocking_reader) {
                uint64_t access_snapshot = c.read_access.load();
                if (blocked_start == std::chrono::steady_clock::time_point{} ||
                    access_snapshot != last_access_snapshot) {
                    blocked_start = std::chrono::steady_clock::now();
                    last_access_snapshot = access_snapshot;
                }

                std::this_thread::yield();
                bool confirmed_blocking_reader = scan_blocking_readers();
                has_blocking_reader = confirmed_blocking_reader;

                if (!confirmed_blocking_reader) {
                    const auto now = std::chrono::steady_clock::now();
                    if ((access_snapshot & range_mask) != 0 &&
                        count_guards_for_octile(new_octile) == 0) {
                        std::this_thread::yield();
                        uint64_t confirm_snapshot = c.read_access.load();
                        if ((confirm_snapshot & range_mask) != 0 &&
                            count_guards_for_octile(new_octile) == 0) {
                            uint64_t expected = confirm_snapshot;
                            uint64_t desired = expected & ~range_mask;
                            if (c.read_access.compare_exchange_strong(expected, desired)) {
                                c.guard_accounting_mismatch_count.fetch_add(1, std::memory_order_relaxed);
                                blocked_start = now;
                                last_access_snapshot = desired;
                                continue;
                            }
                        }
                    }
                    if ((access_snapshot & range_mask) != 0 &&
                        (now - blocked_start) >= k_stale_guard_delay)
                    {
                        uint64_t expected_access = access_snapshot;
                        uint64_t desired_access = expected_access & ~range_mask;
                        if (c.read_access.compare_exchange_strong(expected_access, desired_access)) {
                            // Track stale guard forcible clear
                            c.stale_guard_clear_count.fetch_add(1, std::memory_order_relaxed);
#ifndef NDEBUG
                            // Assertion: this should be rare - log in debug builds
                            const uint32_t cleared_count = static_cast<uint32_t>(
                                (expected_access >> (8 * new_octile)) & 0xffu);
                            assert(cleared_count > 0 && cleared_count < 255 &&
                                   "Stale guard clear with suspicious count");
#endif
                            blocked_start = now;
                            last_access_snapshot = desired_access;
                        }
                        else {
                            last_access_snapshot = expected_access;
                        }
                    }
                }
                else {
                    blocked_start = std::chrono::steady_clock::time_point{};
                }
            }

            if (!has_blocking_reader) {
                if (!(c.read_access & range_mask)) {
                    break;
                }
#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
                spin_count = 0;
#else
                eviction_deadline_armed = false;
#endif
#endif
                continue;
            }

#ifdef SINTRA_ENABLE_SLOW_READER_EVICTION
#if defined(SINTRA_EVICTION_SPIN_THRESHOLD) && SINTRA_EVICTION_SPIN_THRESHOLD > 0
            if (++spin_count > spin_loop_budget) {
                run_eviction_pass();
                spin_count = 0;
            }
#else
            auto now = eviction_clock::now();
            if (eviction_budget.count() == 0) {
                run_eviction_pass();
            }
            else
            if (!eviction_deadline_armed) {
                eviction_deadline = now + eviction_budget;
                eviction_deadline_armed = true;
            }
            else
            if (now >= eviction_deadline) {
                run_eviction_pass();
                eviction_deadline = now + eviction_budget;
            }
#endif
#endif
        }

        m_octile = new_octile;
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
            c.writer_pid = 0;
        };

        for (;;) {
            uint32_t observed = c.writer_pid;
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
                if (!c.writer_pid.compare_exchange_strong(expected, encode_recovery_value(self_pid))) {
                    continue;
                }

                finalize_recovery();
                return;
            }

            uint32_t expected_writer = observed;
            if (c.writer_pid.compare_exchange_strong(expected_writer, encode_recovery_value(self_pid))) {
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
     *  * Returns a pointer to a contiguous range within the linearized mapping.
     *  * Spins only when crossing to a new octile that is currently guarded
     *    by at least one reader.
     */
    T* prepare_write(size_t num_elements_to_write)
    {
        assert(num_elements_to_write <= this->m_num_elements / 8);
        if (num_elements_to_write > this->m_num_elements / 8) {
            throw std::invalid_argument("Ring write size exceeds single-octile capacity (ring_size/8).");
        }

        // Enforce exclusive writer (cheap fast-path loop)
        // Use thread-local index (trivial type) for reliable atomic operations across all platforms
        const uint32_t my_thread_idx = thread_index();
        while (m_writing_thread_index != my_thread_idx) {
            uint32_t expected = 0;
            m_writing_thread_index.compare_exchange_strong(expected, my_thread_idx);
        }

        const size_t index = mod_u64(m_pending_new_sequence, this->m_num_elements);
        m_pending_new_sequence += num_elements_to_write;

        const size_t head = mod_u64(m_pending_new_sequence, this->m_num_elements);
        advance_writer_octile_if_needed(head);
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
