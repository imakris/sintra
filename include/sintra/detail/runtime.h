// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "debug_pause.h"
#include "globals.h"
#include "logging.h"
#include "tls_post_handler.h"
#include "process/coordinator.h"
#include "process/managed_process.h"
#include "transceiver_impl.h"
#include "utility.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include "sintra_windows.h"
#else
#include <unistd.h>
#endif

namespace sintra {
namespace detail {

namespace test_hooks {

inline constexpr const char* k_stage_create_invitation_pre_admission_lock =
    "create_external_process_invitation/pre_admission_lock";
inline constexpr const char* k_stage_spawn_success_before_readiness_wait =
    "spawn_swarm_process/success_before_readiness_wait";

#if defined(SINTRA_ENABLE_TEST_HOOKS)
using Runtime_stage_callback = void (*)(const char*);
inline std::atomic<Runtime_stage_callback> s_runtime_stage{nullptr};

// Observes the completed OS-spawn result after teardown admission has been
// released and before public readiness waiting begins. Tests may rendezvous at
// this boundary; production builds compile the call away.
using Runtime_spawn_success_callback = void (*)(
    instance_id_type process_iid,
    int              os_pid,
    bool             lifeline_enabled,
    bool             lifeline_write_retained);
inline std::atomic<Runtime_spawn_success_callback> s_runtime_spawn_success{nullptr};
#endif

} // namespace test_hooks

#if defined(SINTRA_ENABLE_TEST_HOOKS)
inline void runtime_stage_for_test(const char* stage)
{
    if (auto callback = test_hooks::s_runtime_stage.load(std::memory_order_acquire)) {
        callback(stage);
    }
}
#else
inline void runtime_stage_for_test(const char*) {}
#endif

inline void runtime_spawn_success_for_test(
    instance_id_type process_iid,
    int              os_pid,
    bool             lifeline_enabled,
    bool             lifeline_write_retained)
{
#if defined(SINTRA_ENABLE_TEST_HOOKS)
    if (auto callback =
            test_hooks::s_runtime_spawn_success.load(std::memory_order_acquire))
    {
        callback(
            process_iid,
            os_pid,
            lifeline_enabled,
            lifeline_write_retained);
    }
#else
    (void)process_iid;
    (void)os_pid;
    (void)lifeline_enabled;
    (void)lifeline_write_retained;
#endif
}

inline void append_branch(
    std::vector<Process_descriptor>&   branches,
    const Process_descriptor&          descriptor,
    int                                multiplicity)
{
    if (multiplicity <= 0) {
        return;
    }

    branches.reserve(branches.size() + static_cast<std::size_t>(multiplicity));
    for (int i = 0; i < multiplicity; ++i) {
        branches.push_back(descriptor);
    }
}

inline void collect_branches(std::vector<Process_descriptor>&) {}

template <typename... Args>
void collect_branches(
    std::vector<Process_descriptor>&   branches,
    const Process_descriptor&          descriptor,
    Args&&...                          rest)
{
    append_branch(branches, descriptor, 1);
    collect_branches(branches, std::forward<Args>(rest)...);
}

template <typename... Args>
void collect_branches(
    std::vector<Process_descriptor>&   branches,
    int                                multiplicity,
    const Process_descriptor&          descriptor,
    Args&&...                          rest)
{
    append_branch(branches, descriptor, multiplicity);
    collect_branches(branches, std::forward<Args>(rest)...);
}

inline bool fill_external_attach_random_bytes(unsigned char* data, size_t size)
{
#ifdef _WIN32
    using RtlGenRandom_fn = BOOLEAN (APIENTRY*)(PVOID, ULONG);

    HMODULE module = LoadLibraryA("advapi32.dll");
    if (!module) {
        return false;
    }

    auto fn = reinterpret_cast<RtlGenRandom_fn>(
        GetProcAddress(module, "SystemFunction036"));
    const bool ok = fn && fn(data, static_cast<ULONG>(size));
    FreeLibrary(module);
    return ok;
#else
    std::ifstream random_stream("/dev/urandom", std::ios::binary);
    if (!random_stream) {
        return false;
    }

    random_stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return random_stream.gcount() == static_cast<std::streamsize>(size);
#endif
}

inline std::string make_external_attach_token()
{
    std::array<unsigned char, 32> bytes{};
    if (!fill_external_attach_random_bytes(bytes.data(), bytes.size())) {
        throw std::runtime_error(
            "Sintra could not obtain secure random bytes for an external attach token.");
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : bytes) {
        out << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return out.str();
}

} // namespace detail

inline void disable_debug_pause_for_current_process() noexcept
{
    detail::set_debug_pause_active(false);
}

inline std::vector<Process_descriptor> make_branches()
{
    return {};
}

inline std::vector<Process_descriptor> make_branches(std::vector<Process_descriptor>& branches)
{
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>&   branches,
    const Process_descriptor&          descriptor,
    Args&&...                          rest)
{
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>&   branches,
    int                                multiplicity,
    const Process_descriptor&          descriptor,
    Args&&...                          rest)
{
    detail::collect_branches(branches, multiplicity, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    const Process_descriptor&  descriptor,
    Args&&...                  rest)
{
    std::vector<Process_descriptor> branches;
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    int                        multiplicity,
    const Process_descriptor&  descriptor,
    Args&&...                  rest)
{
    std::vector<Process_descriptor> branches;
    detail::collect_branches(branches, multiplicity, descriptor, std::forward<Args>(rest)...);
    return branches;
}

namespace detail {
inline bool finalize_impl();
inline bool finalize();
} // namespace detail

struct Spawn_options
{
    std::string                binary_path;
    std::vector<std::string>   args;
    std::vector<std::string>   env_overrides;
    instance_id_type           process_instance_id = invalid_instance_id;
    std::string                wait_for_instance_name;
    std::chrono::milliseconds  wait_timeout{0};
    Lifetime_policy            lifetime;
};

struct Managed_child_custody_observation
{
    bool        accepted = false;
    bool        readiness_reached = false;
    bool        release_requested = false;
    bool        release_complete = false;
    std::size_t admitted_occurrences = 0;
    std::size_t created_occurrences = 0;
    std::size_t exited_occurrences = 0;
};

class Managed_child_custody
{
public:
    Managed_child_custody() = default;

    bool accepted() const noexcept { return static_cast<bool>(m_record); }
    explicit operator bool() const noexcept { return accepted(); }

private:
    explicit Managed_child_custody(
        std::shared_ptr<detail::Managed_child_custody_record> record)
        : m_record(std::move(record))
    {}

    std::shared_ptr<detail::Managed_child_custody_record> m_record;

    friend Managed_child_custody spawn_swarm_process(const Spawn_options&);
    friend Managed_child_custody_observation observe_managed_child(
        const Managed_child_custody&);
    friend Managed_child_custody_observation wait_managed_child(
        const Managed_child_custody&, std::chrono::steady_clock::time_point);
    friend Managed_child_custody_observation release_managed_child(
        const Managed_child_custody&, std::chrono::steady_clock::time_point);
    friend Managed_child_custody_observation retry_managed_child_release(
        const Managed_child_custody&, std::chrono::steady_clock::time_point);
};

Managed_child_custody_observation observe_managed_child(
    const Managed_child_custody& custody);
Managed_child_custody_observation wait_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);
Managed_child_custody_observation release_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);
Managed_child_custody_observation retry_managed_child_release(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);

struct External_process_invitation_options
{
    instance_id_type           process_instance_id = invalid_instance_id;
    std::chrono::milliseconds  timeout{std::chrono::seconds(30)};
};

struct External_process_invitation
{
    instance_id_type                          process_instance_id = invalid_instance_id;
    uint64_t                                  swarm_id            = 0;
    instance_id_type                          coordinator_id      = invalid_instance_id;
    uint32_t                                  occurrence          = 0;
    std::chrono::steady_clock::time_point     expires_at{};
    std::string                               token;

    bool valid() const
    {
        return
            process_instance_id != invalid_instance_id &&
            coordinator_id      != invalid_instance_id &&
            swarm_id            != 0                   &&
            !token.empty();
    }

    explicit operator bool() const { return valid(); }

    std::vector<std::string> sintra_args() const
    {
        if (!valid()) {
            return {};
        }

        return {
            "--swarm_id",                 std::to_string(swarm_id),
            "--instance_id",              std::to_string(process_instance_id),
            "--coordinator_id",           std::to_string(coordinator_id),
            detail::k_external_attach_occurrence_arg,
            std::to_string(occurrence),
            detail::k_external_attach_token_arg,
            token,
        };
    }
};

// Tracks whether init() has been called without a corresponding teardown.
// This flag is reset by detail::finalize_impl() to allow init/teardown cycles
// (e.g., in tests that repeatedly initialize and tear down the library).
inline bool s_init_once = false;

namespace detail {

inline void cleanup_failed_init_noexcept()
{
    auto* failed_mproc = s_mproc;
    if (failed_mproc) {
        try {
            failed_mproc->stop();
        }
        catch (...) {
        }

        try {
            failed_mproc->unblock_rpc();
        }
        catch (...) {
        }

        delete failed_mproc;
        s_mproc = nullptr;
    }

    s_init_once           = false;
    s_mproc_id            = invalid_instance_id;
    s_coord_id            = invalid_instance_id;
    s_branch_index        = -1;
    s_recovery_occurrence = 0;

    {
        std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
        s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);
        s_teardown_admission_closed.store(false, std::memory_order_release);
    }
}

} // namespace detail

inline void init(
    int                                argc,
    const char* const*                 argv,
    std::vector<Process_descriptor>    branches = std::vector<Process_descriptor>())
{
#ifndef _WIN32
    struct sigaction noaction;
    std::memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &noaction, nullptr);
    setsid();
#endif

    // Install debug pause handlers if enabled via SINTRA_DEBUG_PAUSE_ON_EXIT
    detail::install_debug_pause_handlers();

    if (s_init_once || s_mproc) {
        throw std::runtime_error(
            "sintra::init() called twice without a matching teardown "
            "(use sintra::shutdown(), sintra::leave(), or sintra::detail::finalize()).");
    }
    s_init_once = true;

    static Instantiator cleanup_guard(std::function<void()>([]() {
        if (s_mproc) {
            detail::finalize_impl();
        }
    }));

    try {
        s_mproc = new Managed_process;
        s_mproc->init(argc, argv);
    }
    catch (...) {
        detail::cleanup_failed_init_noexcept();
        throw;
    }

    if (!branches.empty()) {
        s_mproc->branch(branches);
    }
    s_mproc->go();
}

template <typename... Args>
void init(int argc, const char* const* argv, Args&&... args)
{
#ifndef NDEBUG
    const auto cache_line_size = get_cache_line_size();
    if (assumed_cache_line_size != cache_line_size) {
        Log_stream(log_level::warning)
            << "WARNING: assumed_cache_line_size is set to "
            << static_cast<size_t>(assumed_cache_line_size)
            << ", but on this system it is actually "
            << static_cast<size_t>(cache_line_size)
            << ".\n";
    }
#endif

    init(argc, argv, make_branches(std::forward<Args>(args)...));
}

namespace detail {

/// Low-level teardown implementation. This is the internal workhorse used by
/// the standard `shutdown()` path, the public `leave()` path, and the low-level
/// `detail::finalize()` entry point.
/// Ordinary callers should use `shutdown()` instead.
inline bool finalize_impl()
{
    if (!s_mproc) {
        return false;
    }

    {
        std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
        s_teardown_admission_closed.store(true, std::memory_order_release);
        if (s_shutdown_state.load(std::memory_order_acquire) ==
            shutdown_protocol_state::idle)
        {
            s_shutdown_state.store(
                shutdown_protocol_state::finalizing,
                std::memory_order_release);
        }
    }

    // Finalization participates in the same custody contract.  It closes
    // recovery and requests release, but it does not erase runtime state or
    // report success while an exact occurrence is still unresolved.
    s_mproc->request_all_child_custody_releases();
    if (!s_mproc->wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250)))
    {
        return false;
    }

    const auto prev = s_shutdown_state.exchange(
        shutdown_protocol_state::finalizing, std::memory_order_acq_rel);
    (void)prev;  // Previous state is informational; no assertion needed here.

    sequence_counter_type flush_seq = invalid_sequence;

    if (s_coord) {
        s_coord->begin_shutdown();
        // Coordinator-local finalize: announce draining to local state so that
        // new barriers exclude this process, then wait until all known
        // processes have entered the draining state (or been scavenged) before
        // proceeding with teardown. This ensures that remote processes can
        // still complete their own begin_process_draining() RPCs while the
        // coordinator remains alive.
        flush_seq = s_coord->begin_process_draining(s_mproc_id);
        s_coord->wait_for_all_draining(s_mproc_id);
    }
    else {
        if (s_mproc->m_must_stop.load(std::memory_order_acquire)) {
            // The coordinator has already been unpublished or observed as gone.
            // A drain RPC cannot complete in this state, so proceed with local
            // teardown instead of spending the fallback timeout first.
            flush_seq = invalid_sequence;
        }
        else {
            try {
                auto handle = Coordinator::rpc_async_begin_process_draining(
                    s_coord_id,
                    s_mproc_id);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                flush_seq = handle.get_until(deadline);
            }
            catch (const rpc_timeout&) {
                s_mproc->unblock_rpc(process_of(s_coord_id));
                flush_seq = invalid_sequence;
                Log_stream(log_level::warning)
                    << "finalize(): begin_process_draining timed out after 5 seconds for process "
                    << static_cast<unsigned long long>(s_mproc_id)
                    << " while waiting on coordinator "
                    << static_cast<unsigned long long>(s_coord_id)
                    << ". Proceeding with degraded shutdown semantics.\n";
            }
            catch (const std::exception& e) {
                flush_seq = invalid_sequence;
                Log_stream(log_level::warning)
                    << "finalize(): begin_process_draining failed for process "
                    << static_cast<unsigned long long>(s_mproc_id)
                    << " with: " << e.what()
                    << ". Proceeding with degraded shutdown semantics.\n";
            }
            catch (...) {
                flush_seq = invalid_sequence;
                Log_stream(log_level::warning)
                    << "finalize(): begin_process_draining failed for process "
                    << static_cast<unsigned long long>(s_mproc_id)
                    << " with an unknown exception. Proceeding with degraded shutdown semantics.\n";
            }
        }
    }

    if (!s_coord && flush_seq != invalid_sequence) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    // Transition into service mode before deactivating slots and unpublishing
    // transceivers. This prevents user-level event handlers from running
    // concurrently with teardown while still allowing RPCs and service
    // messages (including unpublish notifications) to flow.
    s_mproc->pause();

    s_mproc->unblock_rpc();

    s_mproc->deactivate_all();

    s_mproc->unpublish_all_transceivers();

    delete s_mproc;
    s_mproc = nullptr;

    // Reset flags to allow another init()/finalize() cycle.
    s_init_once = false;
    {
        std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
        s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);
        s_teardown_admission_closed.store(false, std::memory_order_release);
    }

    return true;
}

inline void claim_lifecycle_teardown_state(
    shutdown_protocol_state    desired_state,
    const char*                api_name)
{
    auto expected = shutdown_protocol_state::idle;
    if (!s_shutdown_state.compare_exchange_strong(
            expected, desired_state, std::memory_order_acq_rel))
    {
        throw std::logic_error(
            std::string(api_name) + " called while another lifecycle teardown is already in progress "
            "(state=" + std::to_string(static_cast<int>(expected)) + ").");
    }
}

inline bool lifecycle_teardown_requested()
{
    return
        s_teardown_admission_closed.load(std::memory_order_acquire) ||
        s_shutdown_state.load(std::memory_order_acquire) != shutdown_protocol_state::idle;
}

inline void close_teardown_admission_and_claim_state(
    shutdown_protocol_state    desired_state,
    const char*                api_name)
{
    std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
    if (s_teardown_admission_closed.load(std::memory_order_acquire)) {
        if (s_shutdown_state.load(std::memory_order_acquire) == desired_state) {
            // Retry the same incomplete terminal phase. Admission stays closed.
            return;
        }
        throw std::logic_error(
            std::string(api_name) + " called while another lifecycle teardown is already in progress.");
    }
    s_teardown_admission_closed.store(true, std::memory_order_release);
    claim_lifecycle_teardown_state(desired_state, api_name);
}

inline bool close_teardown_admission_for_shutdown(const char* api_name)
{
    std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
    if (s_teardown_admission_closed.load(std::memory_order_acquire)) {
        const auto state = s_shutdown_state.load(std::memory_order_acquire);
        if (state == shutdown_protocol_state::collective_shutdown_entered ||
            state == shutdown_protocol_state::coordinator_hook_completed)
        {
            return true;
        }
        throw std::logic_error(
            std::string(api_name) + " called while another lifecycle teardown is already in progress.");
    }
    const auto state = s_shutdown_state.load(std::memory_order_acquire);
    if (state != shutdown_protocol_state::idle) {
        throw std::logic_error(
            std::string(api_name) + " called while another lifecycle teardown is already in progress "
            "(state=" + std::to_string(static_cast<int>(state)) + ").");
    }
    s_teardown_admission_closed.store(true, std::memory_order_release);
    return false;
}

inline void reset_lifecycle_teardown_to_idle()
{
    std::lock_guard<std::mutex> admission_lock(s_teardown_admission_mutex);
    s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);
    s_teardown_admission_closed.store(false, std::memory_order_release);
}

inline void validate_leave_context()
{
    if (tl_in_handler_dispatch || tl_in_post_handler()) {
        throw std::logic_error(
            "sintra::leave() must not be called from a message handler or post-handler callback.");
    }
}

/// Low-level teardown for single-process programs, exceptional/error
/// paths, or test code that cannot participate in a symmetric shutdown.
/// Ordinary multi-process callers should use `shutdown()` instead.
///
/// Fails fast if any lifecycle teardown protocol is already in progress
/// (state is not idle), because mixing raw finalize with another terminal
/// path is an illegal composition.
inline bool finalize()
{
    close_teardown_admission_and_claim_state(
        shutdown_protocol_state::finalizing,
        "sintra::detail::finalize()");

    if (!s_mproc) {
        reset_lifecycle_teardown_to_idle();
        return false;
    }

    try {
        return finalize_impl();
    }
    catch (...) {
        reset_lifecycle_teardown_to_idle();
        throw;
    }
}

/// Shared coordinator drain-wait logic used by all shutdown paths.
/// After the collective barrier (and optional coordinator hook), the coordinator
/// waits for all peers to leave the group before proceeding to finalize.
inline void shutdown_coordinator_drain_wait(const std::string& group_name)
{
    if (!s_coord) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    auto external_readers_drained = [&]() {
        if (!s_mproc) {
            return true;
        }

        std::lock_guard<std::mutex> readers_lock(s_mproc->m_num_active_readers_mutex);
        // The coordinator always has exactly two local readers of its own.
        // Drain completion must not race ahead while remote-reader threads are
        // still alive, or finalize can immediately block in reader teardown.
        return s_mproc->m_num_active_readers <= 2;
    };
    auto only_self_remains = [&]() {
        return
            !s_coord->group_has_non_external_peer(group_name, s_mproc_id) &&
            external_readers_drained();
    };

    bool group_drained = false;
    std::unique_lock<std::mutex> wait_lock(s_coord->m_draining_state_mutex);
    assert(!s_coord->m_waiting_for_all_draining.load(std::memory_order_acquire));
    s_coord->m_waiting_for_all_draining.store(true, std::memory_order_release);
    while (true) {
        const uint64_t observed_generation = s_coord->m_draining_state_generation;
        wait_lock.unlock();
        const bool drained_now = only_self_remains();
        wait_lock.lock();

        if (drained_now) {
            group_drained = true;
            break;
        }

        const auto generation_advanced = [&]() {
            return s_coord->m_draining_state_generation != observed_generation;
        };

        if (generation_advanced()) {
            continue;
        }

        if (!s_coord->m_all_draining_cv.wait_until(
                wait_lock, deadline, generation_advanced))
        {
            break;
        }
    }
    s_coord->m_waiting_for_all_draining.store(false, std::memory_order_release);

    if (!group_drained) {
        Log_stream(log_level::warning)
            << "shutdown(): coordinator timed out waiting for peers to exit group '"
            << group_name
            << "' before finalizing.\n";
    }
}

inline bool shutdown_group_is_trivial(const std::string& group_name)
{
    if (!s_coord) {
        // Workers cannot inspect group membership locally.  If there is a
        // known remote coordinator, this process participates in a
        // multi-process group and must enter the collective barriers.
        return s_coord_id == invalid_instance_id;
    }

    return !s_coord->group_has_non_external_peer(group_name, s_mproc_id);
}

inline void begin_collective_shutdown_for_barriers()
{
    sequence_counter_type flush_seq = invalid_sequence;
    if (s_coord) {
        flush_seq = s_coord->begin_collective_shutdown(s_mproc_id);
    }
    else {
        try {
            flush_seq = Coordinator::rpc_begin_collective_shutdown(
                s_coord_id,
                s_mproc_id);
        }
        catch (const rpc_cancelled&) {
            if (!s_teardown_admission_closed.load(std::memory_order_acquire)) {
                throw;
            }
            Log_stream(log_level::warning)
                << "shutdown(): collective shutdown entry RPC was cancelled; "
                   "continuing teardown.\n";
            return;
        }
        catch (const rpc_unavailable& e) {
            if (!s_teardown_admission_closed.load(std::memory_order_acquire)) {
                throw;
            }
            Log_stream(log_level::warning)
                << "shutdown(): collective shutdown entry RPC was unavailable ("
                << e.what() << "); continuing teardown.\n";
            return;
        }
        if (flush_seq != invalid_sequence) {
            s_mproc->flush(process_of(s_coord_id), flush_seq);
        }
    }

    if (flush_seq == invalid_sequence) {
        throw std::runtime_error(
            "shutdown(): failed to announce collective shutdown entry");
    }
}

inline bool coordinator_can_leave_now()
{
    return !s_coord || s_coord->is_sole_known_process(s_mproc_id);
}

} // namespace detail

inline bool shutdown()
{
    return shutdown(shutdown_options{});
}

inline bool shutdown(const shutdown_options& options)
{
    const bool resume_finalization =
        detail::close_teardown_admission_for_shutdown("sintra::shutdown()");

    if (!s_mproc) {
        detail::reset_lifecycle_teardown_to_idle();
        return false;
    }
    if (resume_finalization) {
        return detail::finalize_impl();
    }

    // True single-process runtimes (no coordinator anywhere) have no
    // collective shutdown protocol to participate in.  Workers in a
    // multi-process setup (s_coord_id is valid) must enter the collective
    // barriers so that the coordinator's processing-fence can complete.
    if (!s_coord && s_coord_id == invalid_instance_id) {
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::collective_shutdown_entered,
            std::memory_order_release);
        return detail::finalize_impl();
    }

    const std::string     group_name             = "_sintra_all_processes";
    constexpr const char* shutdown_barrier_name  = "_sintra_shutdown";
    constexpr const char* hook_done_barrier_name = "_sintra_shutdown_hook_done";

    if (detail::shutdown_group_is_trivial(group_name)) {
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::collective_shutdown_entered,
            std::memory_order_release);
        if (s_coord && options.coordinator_shutdown_hook) {
            detail::s_shutdown_state.store(
                detail::shutdown_protocol_state::coordinator_hook_running,
                std::memory_order_release);
            try {
                options.coordinator_shutdown_hook();
            }
            catch (...) {
                detail::s_shutdown_state.store(
                    detail::shutdown_protocol_state::coordinator_hook_completed,
                    std::memory_order_release);
                detail::finalize_impl();
                throw;
            }
            detail::s_shutdown_state.store(
                detail::shutdown_protocol_state::coordinator_hook_completed,
                std::memory_order_release);
        }

        return detail::finalize_impl();
    }

    // Wrap the collective protocol in a catch-all so that an unexpected
    // exception from a barrier or hook always leaves the runtime torn down
    // and s_shutdown_state reset to idle.  finalize_impl() is safe to call
    // more than once (returns false if already finalized).
    try {
        detail::begin_collective_shutdown_for_barriers();
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::collective_shutdown_entered,
            std::memory_order_release);

        // Step 1: Collective processing fence — all participants synchronize here.
        detail::internal_processing_fence_barrier(shutdown_barrier_name, group_name);

        // Step 2: The hook-done rendezvous is always part of the shutdown protocol.
        // Local hook presence only affects coordinator-local work inside the phase;
        // it must not change collective barrier cardinality.
        if (s_coord && options.coordinator_shutdown_hook) {
            detail::s_shutdown_state.store(
                detail::shutdown_protocol_state::coordinator_hook_running,
                std::memory_order_release);
            try {
                options.coordinator_shutdown_hook();
            }
            catch (...) {
                // The coordinator enters the hook-done rendezvous exactly once.
                // On the throwing path, this catch block performs that entry before
                // draining/finalize and rethrowing.
                detail::s_shutdown_state.store(
                    detail::shutdown_protocol_state::coordinator_hook_completed,
                    std::memory_order_release);
                detail::rendezvous_barrier(
                    hook_done_barrier_name, group_name,
                    detail::k_barrier_mode_rendezvous);
                detail::shutdown_coordinator_drain_wait(group_name);
                detail::finalize_impl();
                throw;
            }
            detail::s_shutdown_state.store(
                detail::shutdown_protocol_state::coordinator_hook_completed,
                std::memory_order_release);
        }

        // All participants always enter the same hook-done rendezvous, regardless
        // of local hook presence or which shutdown overload they used.
        detail::rendezvous_barrier(
            hook_done_barrier_name, group_name,
            detail::k_barrier_mode_rendezvous);

        // Step 3: Coordinator drain-wait and finalize.
        detail::shutdown_coordinator_drain_wait(group_name);

        return detail::finalize_impl();
    }
    catch (...) {
        detail::finalize_impl();
        throw;
    }
}

inline bool leave()
{
    detail::validate_leave_context();
    detail::close_teardown_admission_and_claim_state(
        detail::shutdown_protocol_state::local_departure_entered,
        "sintra::leave()");

    if (!s_mproc) {
        detail::reset_lifecycle_teardown_to_idle();
        return false;
    }

    try {
        if (s_coord && !detail::coordinator_can_leave_now()) {
            throw std::logic_error(
                "sintra::leave() is not supported on a coordinator while other known "
                "processes are still alive. Use shutdown() for collective termination.");
        }

        return detail::finalize_impl();
    }
    catch (...) {
        detail::reset_lifecycle_teardown_to_idle();
        throw;
    }
}

inline External_process_invitation create_external_process_invitation(
    const External_process_invitation_options& options = {})
{
    if (!s_mproc || !s_coord) {
        Log_stream(log_level::error)
            << "create_external_process_invitation: an active coordinator is required\n";
        return {};
    }

    if (options.timeout.count() <= 0) {
        Log_stream(log_level::error)
            << "create_external_process_invitation: timeout must be greater than zero\n";
        return {};
    }

    detail::runtime_stage_for_test(
        detail::test_hooks::k_stage_create_invitation_pre_admission_lock);

    std::lock_guard<std::mutex> admission_lock(detail::s_teardown_admission_mutex);
    if (!s_mproc || !s_coord) {
        Log_stream(log_level::error)
            << "create_external_process_invitation: an active coordinator is required\n";
        return {};
    }

    if (detail::s_teardown_admission_closed.load(std::memory_order_acquire)) {
        Log_stream(log_level::warning)
            << "create_external_process_invitation: rejected because lifecycle "
            << "teardown is in progress\n";
        return {};
    }

    instance_id_type process_iid = options.process_instance_id;
    if (process_iid == invalid_instance_id) {
        try {
            process_iid = make_process_instance_id();
        }
        catch (const std::exception& e) {
            Log_stream(log_level::error)
                << "create_external_process_invitation: " << e.what() << "\n";
            return {};
        }
    }

    if (!detail::is_valid_process_instance_id(process_iid)) {
        Log_stream(log_level::error)
            << "create_external_process_invitation: process_instance_id must identify a process\n";
        return {};
    }

    std::string token;
    try {
        token = detail::make_external_attach_token();
    }
    catch (const std::exception& e) {
        Log_stream(log_level::error)
            << "create_external_process_invitation: " << e.what() << "\n";
        return {};
    }

    const auto expires_at = std::chrono::steady_clock::now() + options.timeout;
    uint32_t   occurrence = 0;
    if (!s_coord->reserve_external_process_invitation(process_iid, token, expires_at, occurrence)) {
        Log_stream(log_level::warning)
            << "create_external_process_invitation: process instance id "
            << static_cast<unsigned long long>(process_iid)
            << " is already in use or cannot be reserved\n";
        return {};
    }

    External_process_invitation invitation;
    invitation.process_instance_id = process_iid;
    invitation.swarm_id            = s_mproc->m_swarm_id;
    invitation.coordinator_id      = s_coord_id;
    invitation.occurrence          = occurrence;
    invitation.expires_at          = expires_at;
    invitation.token               = std::move(token);
    return invitation;
}

inline bool cancel_external_process_invitation(instance_id_type process_instance_id)
{
    if (!s_coord || process_instance_id == invalid_instance_id) {
        return false;
    }

    return s_coord->cancel_external_process_invitation(process_instance_id);
}

inline bool cancel_external_process_invitation(const External_process_invitation& invitation)
{
    if (!s_coord || !invitation) {
        return false;
    }

    return s_coord->cancel_external_process_invitation(
        invitation.process_instance_id,
        invitation.token);
}

// Accepts durable logical custody before authorizing OS creation.  Readiness is
// an observation on the returned opaque custody and never controls ownership.
inline Managed_child_custody spawn_swarm_process(const Spawn_options& options)
{
    if (options.binary_path.empty()) {
        Log_stream(log_level::error)
            << "spawn_swarm_process: binary path is empty\n";
        return {};
    }

    const bool       wait_requested = !options.wait_for_instance_name.empty();
    const auto       wait_timeout   = options.wait_timeout;
    const bool       async_deadline_setup = wait_requested && wait_timeout.count() > 0;
    const auto       deadline = wait_timeout.count() > 0
        ? std::chrono::steady_clock::now() + wait_timeout
        : std::chrono::steady_clock::time_point::max();
    instance_id_type coord_id       = invalid_instance_id;

    const auto piid = (options.process_instance_id != invalid_instance_id)
        ? options.process_instance_id
        : make_process_instance_id();
    std::shared_ptr<detail::Managed_child_custody_record> custody_record;
    Managed_process::Spawn_result spawn_result;
    bool os_process_created = false;
    auto readiness_observer = [](
        Managed_process* owner,
        const std::shared_ptr<detail::Managed_child_custody_record>& record,
        instance_id_type coordinator_id,
        const std::string& target,
        std::chrono::steady_clock::time_point readiness_deadline,
        bool child_created)
    {
        bool readiness_reached = false;
        bool release_requested = false;
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            release_requested = record->release_requested;
        }
        try {
            if (child_created && !release_requested &&
                readiness_deadline == std::chrono::steady_clock::time_point::max())
            {
                readiness_reached = Coordinator::rpc_wait_for_instance(
                    coordinator_id, target) != invalid_instance_id;
            }
            else if (child_created && !release_requested) {
                auto poll_delay = std::chrono::milliseconds(5);
                while (std::chrono::steady_clock::now() < readiness_deadline) {
                    {
                        std::lock_guard<std::mutex> lock(record->mutex);
                        if (record->release_requested) {
                            break;
                        }
                    }
                    if (Coordinator::rpc_resolve_instance(coordinator_id, target) !=
                        invalid_instance_id)
                    {
                        readiness_reached = true;
                        break;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= readiness_deadline) {
                        break;
                    }
                    std::this_thread::sleep_for(std::min(
                        poll_delay,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            readiness_deadline - now)));
                    poll_delay = std::min(
                        poll_delay * 2, std::chrono::milliseconds(200));
                }
            }
        }
        catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->readiness_reached =
                record->readiness_reached || readiness_reached;
            record->readiness_observer_complete = true;
        }
        record->changed.notify_all();
        if (owner) {
            owner->retire_child_custody_if_complete(record);
        }
    };

    // Ensure argv[0] is the program name (required on Windows); avoid duplicates.
    {
        std::lock_guard<std::mutex> admission_lock(detail::s_teardown_admission_mutex);
        if (detail::s_teardown_admission_closed.load(std::memory_order_acquire) || !s_mproc) {
            Log_stream(log_level::warning)
                << "spawn_swarm_process: rejected because lifecycle teardown is in progress\n";
            return {};
        }

        coord_id = s_coord_id;
        if (wait_requested && coord_id == invalid_instance_id) {
            Log_stream(log_level::error)
                << "spawn_swarm_process: wait requires a valid coordinator\n";
            return {};
        }

        if (options.process_instance_id != invalid_instance_id &&
            s_coord &&
            s_coord->external_process_invitation_exists(piid))
        {
            Log_stream(log_level::warning)
                << "spawn_swarm_process: process instance id "
                << static_cast<unsigned long long>(piid)
                << " is reserved by a pending external-process invitation\n";
            return {};
        }

        if (!s_mproc->can_accept_child_custody(piid)) {
            Log_stream(log_level::warning)
                << "spawn_swarm_process: rejected because process instance id "
                << static_cast<unsigned long long>(piid)
                << " already has unresolved custody\n";
            return {};
        }

        auto args = options.args;
        auto argv0_matches = [&]() {
            if (args.empty()) {
                return false;
            }
            if (args.front() == options.binary_path) {
                return true;
            }
            const auto front_name = std::filesystem::path(args.front()).filename().string();
            const auto bin_name   = std::filesystem::path(options.binary_path).filename().string();
#ifdef _WIN32
            return _stricmp(front_name.c_str(), bin_name.c_str()) == 0;
#else
            return front_name == bin_name;
#endif
        };
        if (!argv0_matches()) {
            args.insert(args.begin(), options.binary_path);
        }

        args.insert(args.end(), {
            "--swarm_id",       std::to_string(s_mproc->m_swarm_id),
            "--instance_id",    std::to_string(piid),
            "--coordinator_id", std::to_string(coord_id)
        });

        Managed_process::Spawn_swarm_process_args spawn_args;
        spawn_args.binary_name   = options.binary_path;
        spawn_args.args          = args;
        spawn_args.env_overrides = options.env_overrides;
        spawn_args.lifetime      = options.lifetime;
        spawn_args.piid = piid;
        custody_record = s_mproc->accept_child_custody(options.wait_for_instance_name);
        spawn_args.custody = custody_record;
        if (async_deadline_setup) {
            if (!s_mproc->admit_child_custody_occurrence(
                    custody_record, piid, spawn_args.occurrence))
            {
                return {};
            }
            spawn_args.custody_occurrence_admitted = true;
            {
                std::lock_guard<std::mutex> lock(custody_record->mutex);
                custody_record->readiness_observer_complete = false;
            }
            auto* custody_owner = s_mproc;
            auto async_setup =
                [custody_owner, custody_record, spawn_args, coord_id,
                 target = options.wait_for_instance_name, deadline,
                readiness_observer]() mutable
                {
                    bool created = false;
                    bool setup_threw = false;
                    try {
                        auto result = custody_owner->spawn_swarm_process(spawn_args);
                        created = result.success && result.os_process_created;
                        if (created) {
                            detail::runtime_spawn_success_for_test(
                                result.instance_id,
                                result.os_pid,
                                result.lifeline_enabled,
                                result.lifeline_write_retained);
                            detail::runtime_stage_for_test(
                                detail::test_hooks::k_stage_spawn_success_before_readiness_wait);
                        }
                    }
                    catch (...) {
                        setup_threw = true;
                        // The internal setup guard resolves the exact admitted
                        // occurrence before unwinding. Recover the confirmed
                        // native fact so cleanup retains ownership when the OS
                        // child was already created.
                        std::lock_guard<std::mutex> lock(custody_record->mutex);
                        for (auto& occurrence : custody_record->occurrences) {
                            if (occurrence.process_instance_id == spawn_args.piid &&
                                occurrence.occurrence == spawn_args.occurrence)
                            {
                                created = occurrence.os_process_created;
                                if (occurrence.setup ==
                                    detail::Managed_child_occurrence_record::setup_state::pending)
                                {
                                    occurrence.setup = created
                                        ? detail::Managed_child_occurrence_record::setup_state::ownership_ready
                                        : detail::Managed_child_occurrence_record::setup_state::no_child;
                                }
                                break;
                            }
                        }
                        custody_record->changed.notify_all();
                    }

                    if (!created) {
                        custody_owner->request_child_custody_release(custody_record);
                    }
                    else if (setup_threw) {
                        custody_owner->request_child_custody_release(custody_record, true);
                    }
                    readiness_observer(
                        custody_owner, custody_record, coord_id, target, deadline, created);
                };
            try {
                custody_owner->start_child_custody_worker(std::move(async_setup));
            }
            catch (...) {
                {
                    std::lock_guard<std::mutex> lock(custody_record->mutex);
                    for (auto& occurrence : custody_record->occurrences) {
                        if (occurrence.process_instance_id == piid &&
                            occurrence.occurrence == spawn_args.occurrence &&
                            occurrence.setup == detail::Managed_child_occurrence_record::setup_state::pending)
                        {
                            occurrence.setup =
                                detail::Managed_child_occurrence_record::setup_state::no_child;
                            break;
                        }
                    }
                }
                custody_record->changed.notify_all();
                custody_owner->request_child_custody_release(custody_record);
                readiness_observer(
                    custody_owner, custody_record, coord_id,
                    options.wait_for_instance_name, deadline, false);
            }
        }
        else {
            spawn_result = s_mproc->spawn_swarm_process(spawn_args);
            os_process_created = spawn_result.success && spawn_result.os_process_created;
            if (!os_process_created) {
                s_mproc->request_child_custody_release(custody_record);
            }
        }
    }
    if (!async_deadline_setup && os_process_created) {
        detail::runtime_spawn_success_for_test(
            spawn_result.instance_id,
            spawn_result.os_pid,
            spawn_result.lifeline_enabled,
            spawn_result.lifeline_write_retained);
        detail::runtime_stage_for_test(
            detail::test_hooks::k_stage_spawn_success_before_readiness_wait);
    }

    Managed_child_custody custody(custody_record);
    if (!async_deadline_setup && (!os_process_created || !wait_requested)) {
        return custody;
    }
    auto* custody_owner = s_mproc;
    if (!async_deadline_setup) {
        {
            std::lock_guard<std::mutex> lock(custody_record->mutex);
            custody_record->readiness_observer_complete = false;
        }
        custody_owner->start_child_custody_worker(
            [custody_owner, custody_record, coord_id,
             target = options.wait_for_instance_name, deadline, readiness_observer]()
            {
                readiness_observer(
                    custody_owner, custody_record, coord_id, target, deadline, true);
            });
    }

    {
        std::unique_lock<std::mutex> lock(custody_record->mutex);
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            custody_record->changed.wait(lock, [&]() {
                return custody_record->readiness_reached ||
                    custody_record->release_requested;
            });
        }
        else {
            custody_record->changed.wait_until(lock, deadline, [&]() {
                return custody_record->readiness_reached ||
                    custody_record->release_requested;
            });
        }
        if (custody_record->readiness_reached) {
            return custody;
        }
    }

    Log_stream(log_level::warning)
        << "spawn_swarm_process: readiness incomplete at deadline for instance '"
        << options.wait_for_instance_name << "'; custody retained\n";
    custody_owner->request_child_custody_release(custody_record, true);
    return custody;
}

inline Managed_child_custody_observation observe_managed_child(
    const Managed_child_custody& custody)
{
    Managed_child_custody_observation observation;
    if (!custody.m_record) {
        return observation;
    }
    std::lock_guard<std::mutex> lock(custody.m_record->mutex);
    observation.accepted = custody.m_record->accepted;
    observation.readiness_reached = custody.m_record->readiness_reached;
    observation.release_requested = custody.m_record->release_requested;
    observation.release_complete = custody.m_record->release_complete;
    observation.admitted_occurrences = custody.m_record->occurrences.size();
    for (const auto& occurrence : custody.m_record->occurrences) {
        observation.created_occurrences += occurrence.os_process_created ? 1u : 0u;
        observation.exited_occurrences += occurrence.os_exit_confirmed ? 1u : 0u;
    }
    return observation;
}

inline Managed_child_custody_observation wait_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline)
{
    if (!custody.m_record) {
        return {};
    }
    std::unique_lock<std::mutex> lock(custody.m_record->mutex);
    custody.m_record->changed.wait_until(lock, deadline, [&]() {
        return custody.m_record->release_complete;
    });
    lock.unlock();
    return observe_managed_child(custody);
}

inline Managed_child_custody_observation release_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline)
{
    if (custody.m_record && s_mproc) {
        s_mproc->request_child_custody_release(custody.m_record);
    }
    return wait_managed_child(custody, deadline);
}

inline Managed_child_custody_observation retry_managed_child_release(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline)
{
    return release_managed_child(custody, deadline);
}

inline instance_id_type join_swarm(
    int            branch_index,
    std::string    binary_name = std::string())
{
    if (branch_index < 1) {
        return invalid_instance_id;
    }

    instance_id_type coord_id = invalid_instance_id;
    {
        std::lock_guard<std::mutex> admission_lock(detail::s_teardown_admission_mutex);
        if (detail::s_teardown_admission_closed.load(std::memory_order_acquire) ||
            !s_mproc ||
            s_coord_id == invalid_instance_id)
        {
            return invalid_instance_id;
        }

        coord_id = s_coord_id;
        if (binary_name.empty()) {
            binary_name = s_mproc->m_binary_name;
        }
    }

    try {
        return Coordinator::rpc_join_swarm(
            coord_id,
            binary_name,
            branch_index);
    }
    catch (...) {
        return invalid_instance_id;
    }
}

inline int process_index()
{
    return s_branch_index;
}

template <typename FT, typename SENDER_T>
auto activate_slot(const FT& slot_function, Typed_instance_id<SENDER_T> sender_id)
{
    if (!s_mproc) {
        throw std::runtime_error("activate_slot() requires an active Sintra runtime.");
    }

    return s_mproc->activate(slot_function, sender_id);
}

inline void deactivate_all_slots()
{
    if (!s_mproc) {
        throw std::runtime_error("deactivate_all_slots() requires an active Sintra runtime.");
    }

    s_mproc->deactivate_all();
}

template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T receive(Typed_instance_id<SENDER_T> sender_id)
{
#ifndef NDEBUG
    if (tl_is_req_thread) {
        Log_stream(log_level::error)
            << "receive<T>() called from a message handler thread. "
            << "This will deadlock. Use a control thread or defer work.\n";
        detail::debug_aware_abort();
    }
#endif

    std::condition_variable cv;
    std::mutex mtx;
    std::optional<MESSAGE_T> result;

    auto deactivator = activate_slot(
        [&](MESSAGE_T msg) {
            std::lock_guard<std::mutex> lock(mtx);
            result.emplace(std::move(msg));
            cv.notify_one();
        },
        sender_id
    );

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return result.has_value(); });

    auto value = std::move(*result);
    lock.unlock();
    deactivator();
    return value;
}

template <typename MESSAGE_T>
MESSAGE_T receive()
{
    return receive<MESSAGE_T, void>(Typed_instance_id<void>(any_local_or_remote));
}

inline void enable_recovery()
{
    if (!s_mproc) {
        throw std::runtime_error("enable_recovery() requires an active Sintra runtime.");
    }
    s_mproc->enable_recovery();
}

inline void set_recovery_policy(Recovery_policy policy)
{
    if (!s_coord) {
        return;
    }
    s_coord->set_recovery_policy(std::move(policy));
}

inline void set_recovery_runner(Recovery_runner runner)
{
    if (!s_coord) {
        return;
    }
    s_coord->set_recovery_runner(std::move(runner));
}

inline void set_lifecycle_handler(Lifecycle_handler handler)
{
    if (!s_coord) {
        return;
    }
    s_coord->set_lifecycle_handler(std::move(handler));
}

} // namespace sintra
