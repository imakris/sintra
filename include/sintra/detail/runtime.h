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

#include <algorithm>
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
inline constexpr const char* k_stage_observe_managed_child_exit_selected =
    "observe_managed_child_exit/selected";

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

struct Managed_child_readiness_access
{
    static instance_id_type resolve(
        Coordinator*       coordinator,
        const std::string& assigned_name,
        uint64_t           custody_identity,
        instance_id_type   process_iid,
        uint32_t           occurrence,
        const std::atomic<bool>& cancelled)
    {
        return coordinator
            ? coordinator->resolve_managed_child_instance(
                  assigned_name,
                  custody_identity,
                  process_iid,
                  occurrence,
                  cancelled)
            : invalid_instance_id;
    }

    static instance_id_type wait(
        Coordinator*       coordinator,
        const std::string& assigned_name,
        uint64_t           custody_identity,
        instance_id_type   process_iid,
        uint32_t           occurrence,
        const std::atomic<bool>& cancelled)
    {
        return coordinator
            ? coordinator->wait_for_managed_child_instance(
                  assigned_name,
                  custody_identity,
                  process_iid,
                  occurrence,
                  cancelled)
            : invalid_instance_id;
    }
};

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
    std::string                readiness_instance_name;
    Lifetime_policy            lifetime;
};

enum class Managed_child_readiness_state
{
    not_requested,
    pending,
    reached,
    observation_stopped
};

enum class Managed_child_release_state
{
    open,
    requested,
    complete
};

struct Managed_child_status
{
    Managed_child_readiness_state readiness_state =
        Managed_child_readiness_state::not_requested;
    Managed_child_release_state release_state =
        Managed_child_release_state::open;
    std::size_t admitted_occurrences = 0;
    std::size_t created_occurrences = 0;
    std::size_t exited_occurrences = 0;
    Managed_child_failure last_failure;
};

class Managed_child_custody
{
public:
    Managed_child_custody() = default;

    explicit operator bool() const noexcept { return static_cast<bool>(m_record); }

    Managed_child_status status() const;
    Managed_child_status wait_for_readiness_until(
        std::chrono::steady_clock::time_point deadline) const;

    /// Observes the latest OS-created occurrence retained by this custody.
    ///
    /// Selection and registration are atomic with respect to native exit. The
    /// returned subscription follows only the selected immutable occurrence;
    /// it never follows a later recovery occurrence. Consequently, selecting
    /// the latest occurrence may intentionally skip older exited occurrences.
    /// If no OS-created occurrence exists, the result is empty and the
    /// callback is not retained. Registering after the selected occurrence has
    /// exited still schedules one delivery while the coordinator runtime is
    /// active. Registration during teardown, after shutdown, or through a
    /// custody retained from an earlier runtime returns empty and does not
    /// retain the callback.
    ///
    /// This observation is OS-authoritative. The communication-dependent
    /// `terminated_abnormally` signal, coordinator publication lifecycle
    /// handler, and aggregate custody status do not provide equivalent
    /// exact-occurrence exit evidence.
    ///
    /// Callbacks run on a Sintra-managed lifecycle thread without custody
    /// locks held. Delivery may begin before this function returns, and no
    /// ordering between multiple observers is guaranteed. A callback must not
    /// initiate Sintra runtime teardown; it should post application work to the
    /// caller's executor instead. Teardown attempts throw `std::logic_error`
    /// before teardown admission changes.
    /// Destroying or unsubscribing externally waits for an executing callback
    /// to finish. Self-unsubscription is deferred until that callback returns.
    /// Callback exceptions are logged, are not retried, and do not escape the
    /// worker.
    Managed_child_exit_observation observe_latest_created_exit(
        Managed_child_exit_callback callback) const;

    Managed_child_status release_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_status terminate_until(
        std::chrono::steady_clock::time_point deadline) const;

private:
    explicit Managed_child_custody(
        std::shared_ptr<detail::Managed_child_custody_record> record)
        : m_record(std::move(record))
    {}

    std::shared_ptr<detail::Managed_child_custody_record> m_record;

    Managed_child_status wait_until_released(
        std::chrono::steady_clock::time_point deadline) const;

    friend Managed_child_custody spawn_swarm_process(const Spawn_options&);
};

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

// Finalization may return bounded-incomplete while managed-child custody is
// still resolving. Keep the authoritative drain admission as retained runtime
// state so a retry resumes after it instead of replaying shutdown/barrier work.
inline std::mutex s_finalization_drain_mutex;
inline bool       s_finalization_drain_started = false;

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
    }

    s_init_once           = false;
    s_mproc_id            = invalid_instance_id;
    s_coord_id            = invalid_instance_id;
    s_branch_index        = -1;
    s_recovery_occurrence = 0;
    {
        std::lock_guard<std::mutex> lock(s_finalization_drain_mutex);
        s_finalization_drain_started = false;
    }

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
        auto* initialized_mproc = new Managed_process;
        assert(s_mproc == initialized_mproc);
        initialized_mproc->init(argc, argv);
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

    // Finalization participates in the same custody contract. It first closes
    // recovery and requests release without manufacturing per-child
    // retirement. The system-level drain announcement must precede the
    // bounded custody wait so an in-flight _sintra_all_processes barrier can
    // complete and children can retire naturally. This phase is retained
    // across an incomplete retry and is never replayed.
    s_mproc->request_all_child_custody_releases();

    {
        std::lock_guard<std::mutex> drain_lock(s_finalization_drain_mutex);
        if (!s_finalization_drain_started) {
            sequence_counter_type flush_seq = invalid_sequence;
            if (s_coord) {
                s_coord->begin_shutdown();
                flush_seq = s_coord->begin_process_draining(s_mproc_id);
            }
            else if (!s_mproc->m_must_stop.load(std::memory_order_acquire)) {
                try {
                    auto handle = Coordinator::rpc_async_begin_process_draining(
                        s_coord_id,
                        s_mproc_id);
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    flush_seq = handle.get_until(deadline);
                }
                catch (const rpc_timeout&) {
                    s_mproc->unblock_rpc(process_of(s_coord_id));
                    Log_stream(log_level::warning)
                        << "finalize(): begin_process_draining timed out after 5 seconds for process "
                        << static_cast<unsigned long long>(s_mproc_id)
                        << " while waiting on coordinator "
                        << static_cast<unsigned long long>(s_coord_id)
                        << ". Proceeding with degraded shutdown semantics.\n";
                }
                catch (const std::exception& e) {
                    Log_stream(log_level::warning)
                        << "finalize(): begin_process_draining failed for process "
                        << static_cast<unsigned long long>(s_mproc_id)
                        << " with: " << e.what()
                        << ". Proceeding with degraded shutdown semantics.\n";
                }
                catch (...) {
                    Log_stream(log_level::warning)
                        << "finalize(): begin_process_draining failed for process "
                        << static_cast<unsigned long long>(s_mproc_id)
                        << " with an unknown exception. Proceeding with degraded shutdown semantics.\n";
                }
            }

            if (!s_coord && flush_seq != invalid_sequence) {
                s_mproc->flush(process_of(s_coord_id), flush_seq);
            }
            s_finalization_drain_started = true;
        }
    }

    if (!s_mproc->wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250)))
    {
        return false;
    }

    const auto prev = s_shutdown_state.exchange(
        shutdown_protocol_state::finalizing, std::memory_order_acq_rel);
    (void)prev;  // Previous state is informational; no assertion needed here.

    if (s_coord) {
        // The coordinator drain announcement was made before the custody wait.
        // Once every child occurrence is terminal, wait for remaining known
        // peers to drain before deleting runtime state.
        s_coord->wait_for_all_draining(s_mproc_id);
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

    // Reset flags to allow another init()/finalize() cycle.
    s_init_once = false;
    {
        std::lock_guard<std::mutex> lock(s_finalization_drain_mutex);
        s_finalization_drain_started = false;
    }
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

inline void validate_managed_child_exit_callback_teardown(
    const char* api_name)
{
    if (tl_in_managed_child_exit_callback) {
        throw std::logic_error(
            std::string(api_name) +
            " must not be called from a managed-child exit callback.");
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
    validate_managed_child_exit_callback_teardown(
        "sintra::detail::finalize()");
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
    detail::validate_managed_child_exit_callback_teardown(
        "sintra::shutdown()");
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
    detail::validate_managed_child_exit_callback_teardown(
        "sintra::leave()");
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

inline Managed_child_exit_subscription::Managed_child_exit_subscription(
    std::shared_ptr<detail::Managed_child_exit_subscription_state> state)
:
    m_state(std::move(state))
{}

inline Managed_child_exit_subscription::~Managed_child_exit_subscription()
{
    unsubscribe();
}

inline Managed_child_exit_subscription::Managed_child_exit_subscription(
    Managed_child_exit_subscription&& other) noexcept
:
    m_state(std::move(other.m_state))
{}

inline Managed_child_exit_subscription&
Managed_child_exit_subscription::operator=(
    Managed_child_exit_subscription&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    unsubscribe();
    m_state = std::move(other.m_state);
    return *this;
}

inline Managed_child_exit_subscription::operator bool() const noexcept
{
    return static_cast<bool>(m_state);
}

inline void Managed_child_exit_subscription::unsubscribe() noexcept
{
    auto state = std::move(m_state);
    if (state) {
        state->unsubscribe();
    }
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

    const bool       readiness_requested = !options.readiness_instance_name.empty();
    instance_id_type coord_id       = invalid_instance_id;
    Coordinator*     readiness_coordinator = nullptr;
    uint64_t         custody_identity = 0;

    const auto piid = (options.process_instance_id != invalid_instance_id)
        ? options.process_instance_id
        : make_process_instance_id();
    if (!detail::is_valid_process_instance_id(piid)) {
        Log_stream(log_level::error)
            << "spawn_swarm_process: process_instance_id must identify a process\n";
        return {};
    }
    std::shared_ptr<detail::Managed_child_custody_record> custody_record;
    detail::Managed_child_launch_attempt launch_attempt;
    Managed_process::Spawn_swarm_process_args spawn_args;
    Managed_process* custody_owner = nullptr;
    Managed_process::Spawn_result spawn_result;
    bool os_process_created = false;
    auto exception_message = [](
        std::exception_ptr exception,
        const char* fallback)
    {
        try {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& error) {
            return std::string(error.what());
        }
        catch (...) {
            return std::string(fallback);
        }
    };
    auto readiness_observer = [exception_message](
        Managed_process* owner,
        Coordinator* coordinator,
        const std::shared_ptr<detail::Managed_child_custody_record>& record,
        const std::string& target,
        bool child_created,
        uint64_t custody_identity,
        instance_id_type process_iid,
        uint32_t occurrence)
    {
        bool readiness_reached = false;
        bool release_requested = false;
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            release_requested = !record->release_state.open();
        }
        try {
            if (child_created && !release_requested) {
                readiness_reached = detail::Managed_child_readiness_access::wait(
                    coordinator,
                    target,
                    custody_identity,
                    process_iid,
                    occurrence,
                    record->readiness_cancelled) != invalid_instance_id;
            }
        }
        catch (...) {
            if (owner) {
                owner->note_child_custody_failure(
                    record,
                    {Managed_child_failure_kind::readiness_observation,
                     occurrence,
                     0,
                     exception_message(
                         std::current_exception(),
                         "Managed child readiness observation failed")});
            }
        }
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            record->readiness = readiness_reached
                ? detail::Readiness_phase::reached
                : detail::Readiness_phase::stopped;
        }
        record->changed.notify_all();
        if (owner) {
            owner->retire_child_custody_if_complete(record);
        }
    };
    auto handle_setup_exception = [](
        Managed_process* owner,
        const std::shared_ptr<detail::Managed_child_custody_record>& record,
        instance_id_type process_id,
        uint32_t occurrence_number)
    {
        bool child_created = false;
        {
            std::lock_guard<std::mutex> lock(record->mutex);
            const auto* occurrence = record->find_occurrence_locked(
                process_id, occurrence_number);
            if (occurrence) {
                child_created = occurrence->native.created();
            }
        }
        owner->request_child_custody_release(
            record,
            child_created
                ? detail::Release_mode::cleanup
                : detail::Release_mode::passive);
        return child_created;
    };

    // Ensure argv[0] is the program name (required on Windows); avoid duplicates.
    {
        std::lock_guard<std::mutex> admission_lock(detail::s_teardown_admission_mutex);
        if (detail::s_teardown_admission_closed.load(std::memory_order_acquire) || !s_mproc) {
            Log_stream(log_level::warning)
                << "spawn_swarm_process: rejected because lifecycle teardown is in progress\n";
            return {};
        }
        if (!s_coord) {
            Log_stream(log_level::warning)
                << "spawn_swarm_process: rejected because only the coordinator process "
                   "may launch a managed child\n";
            return {};
        }

        coord_id = s_coord_id;
        if (readiness_requested && coord_id == invalid_instance_id) {
            Log_stream(log_level::error)
                << "spawn_swarm_process: wait requires a valid coordinator\n";
            return {};
        }

        if (options.process_instance_id != invalid_instance_id &&
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

        spawn_args.binary_name   = options.binary_path;
        spawn_args.args          = args;
        spawn_args.env_overrides = options.env_overrides;
        spawn_args.lifetime      = options.lifetime;
        spawn_args.piid = piid;
        custody_owner = s_mproc;
        custody_record = custody_owner->accept_child_custody();
        spawn_args.custody = custody_record;
        readiness_coordinator = s_coord;
        custody_identity = custody_record->identity;
        if (readiness_requested) {
            std::lock_guard<std::mutex> lock(custody_record->mutex);
            custody_record->readiness = detail::Readiness_phase::pending;
        }

        try {
            launch_attempt = custody_owner->admit_child_custody_occurrence(
                custody_record, piid, spawn_args.occurrence);
        }
        catch (...) {
            uint32_t admitted_failure_occurrence = 0;
            {
                std::lock_guard<std::mutex> lock(custody_record->mutex);
                if (custody_record->find_occurrence_locked(
                        piid, spawn_args.occurrence))
                {
                    admitted_failure_occurrence = spawn_args.occurrence;
                }
            }
            custody_owner->note_child_custody_failure(
                custody_record,
                {readiness_requested
                     ? Managed_child_failure_kind::occurrence_admission
                     : Managed_child_failure_kind::setup_exception,
                 admitted_failure_occurrence,
                 0,
                 exception_message(
                     std::current_exception(),
                     "Managed child occurrence admission failed")});
            handle_setup_exception(
                custody_owner, custody_record, piid, spawn_args.occurrence);
            if (readiness_requested) {
                readiness_observer(
                    custody_owner,
                    readiness_coordinator,
                    custody_record,
                    options.readiness_instance_name,
                    false,
                    custody_identity,
                    piid,
                    spawn_args.occurrence);
            }
            return Managed_child_custody(custody_record);
        }
        if (!launch_attempt) {
            custody_owner->note_child_custody_failure(
                custody_record,
                {Managed_child_failure_kind::custody_closed,
                 0,
                 0,
                 "Managed child custody closed during occurrence admission"});
            custody_owner->request_child_custody_release(custody_record);
            if (readiness_requested) {
                readiness_observer(
                    custody_owner,
                    readiness_coordinator,
                    custody_record,
                    options.readiness_instance_name,
                    false,
                    custody_identity,
                    piid,
                    spawn_args.occurrence);
            }
            return Managed_child_custody(custody_record);
        }
    }

    if (readiness_requested) {
        auto shared_launch_attempt =
            std::make_shared<detail::Managed_child_launch_attempt>(
                std::move(launch_attempt));
        auto async_setup =
            [custody_owner, readiness_coordinator, custody_record, spawn_args,
             shared_launch_attempt, custody_identity,
             target = options.readiness_instance_name,
             readiness_observer, handle_setup_exception]() mutable
            {
                bool created = false;
                bool setup_threw = false;
                try {
                    auto result = custody_owner->spawn_swarm_process(
                        spawn_args, *shared_launch_attempt);
                    created = result.success && result.os_process_created;
                    if (created) {
                        detail::runtime_spawn_success_for_test(
                            result.instance_id,
                            result.os_pid,
                            result.lifeline_enabled,
                            result.lifeline_write_retained);
                    }
                }
                catch (...) {
                    setup_threw = true;
                    created = handle_setup_exception(
                        custody_owner,
                        custody_record,
                        spawn_args.piid,
                        spawn_args.occurrence);
                }

                if (!created && !setup_threw) {
                    custody_owner->request_child_custody_release(custody_record);
                }
                readiness_observer(
                    custody_owner,
                    readiness_coordinator,
                    custody_record,
                    target,
                    created,
                    custody_identity,
                    spawn_args.piid,
                    spawn_args.occurrence);
            };
        try {
            custody_owner->start_child_custody_worker(std::move(async_setup));
        }
        catch (...) {
            custody_owner->note_child_custody_failure(
                custody_record,
                {Managed_child_failure_kind::setup_worker_start,
                 spawn_args.occurrence,
                 0,
                 exception_message(
                     std::current_exception(),
                     "Managed child setup worker failed to start")});
            custody_owner->request_child_custody_release(custody_record);
            readiness_observer(
                custody_owner,
                readiness_coordinator,
                custody_record,
                options.readiness_instance_name,
                false,
                custody_identity,
                spawn_args.piid,
                spawn_args.occurrence);
        }
    }
    else {
        try {
            spawn_result = custody_owner->spawn_swarm_process(
                spawn_args, launch_attempt);
        }
        catch (...) {
            os_process_created = handle_setup_exception(
                custody_owner, custody_record, piid, spawn_args.occurrence);
            return Managed_child_custody(custody_record);
        }
        os_process_created = spawn_result.success && spawn_result.os_process_created;
        if (!os_process_created) {
            detail::managed_child_post_spawn_for_test(
                detail::Managed_child_post_spawn_stage::public_sync_no_child,
                spawn_args.piid,
                spawn_args.occurrence);
            custody_owner->request_child_custody_release(custody_record);
        }
    }
    if (!readiness_requested && os_process_created) {
        detail::runtime_spawn_success_for_test(
            spawn_result.instance_id,
            spawn_result.os_pid,
            spawn_result.lifeline_enabled,
            spawn_result.lifeline_write_retained);
    }

    Managed_child_custody custody(custody_record);
    return custody;
}

inline Managed_child_status Managed_child_custody::status() const
{
    Managed_child_status result;
    if (!m_record) {
        return result;
    }
    std::lock_guard<std::mutex> lock(m_record->mutex);
    switch (m_record->readiness) {
    case detail::Readiness_phase::not_requested:
        result.readiness_state = Managed_child_readiness_state::not_requested;
        break;
    case detail::Readiness_phase::pending:
        result.readiness_state = Managed_child_readiness_state::pending;
        break;
    case detail::Readiness_phase::reached:
        result.readiness_state = Managed_child_readiness_state::reached;
        break;
    case detail::Readiness_phase::stopped:
        result.readiness_state =
            Managed_child_readiness_state::observation_stopped;
        break;
    }
    result.release_state = m_record->release_state.released()
        ? Managed_child_release_state::complete
        : m_record->release_state.open()
            ? Managed_child_release_state::open
            : Managed_child_release_state::requested;
    result.last_failure = m_record->last_failure;
    result.admitted_occurrences = m_record->occurrences.size();
    for (const auto& occurrence : m_record->occurrences) {
        result.created_occurrences += occurrence.native.created() ? 1u : 0u;
        result.exited_occurrences += occurrence.native.exited() ? 1u : 0u;
    }
    return result;
}

inline Managed_child_status Managed_child_custody::wait_for_readiness_until(
    std::chrono::steady_clock::time_point deadline) const
{
    if (!m_record) {
        return {};
    }

    {
        std::unique_lock<std::mutex> lock(m_record->mutex);
        if (m_record->readiness == detail::Readiness_phase::pending) {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                m_record->changed.wait(lock, [&]() {
                    return m_record->readiness != detail::Readiness_phase::pending;
                });
            }
            else {
                m_record->changed.wait_until(
                    lock,
                    deadline,
                    [&]() {
                        return m_record->readiness != detail::Readiness_phase::pending;
                    });
            }
        }
    }
    return status();
}

inline Managed_child_exit_observation
Managed_child_custody::observe_latest_created_exit(
    Managed_child_exit_callback callback) const
{
    if (!m_record || !callback) {
        return {};
    }

    std::lock_guard<std::mutex> admission_lock(
        detail::s_teardown_admission_mutex);
    if (detail::s_teardown_admission_closed.load(std::memory_order_acquire) ||
        !s_mproc)
    {
        return {};
    }
    const auto runtime_lifetime = m_record->runtime_lifetime.lock();
    if (!runtime_lifetime || runtime_lifetime != s_mproc->m_runtime_lifetime) {
        return {};
    }

    Managed_child_occurrence_identity identity;
    std::shared_ptr<detail::Managed_child_exit_subscription_state> state;
    std::optional<Managed_child_exit> replay;
    {
        std::lock_guard<std::mutex> lock(m_record->mutex);
        const auto selected = std::find_if(
            m_record->occurrences.rbegin(),
            m_record->occurrences.rend(),
            [](const detail::Managed_child_occurrence_record& candidate) {
                return candidate.native.created();
            });
        if (selected == m_record->occurrences.rend()) {
            return {};
        }
        // Establish delivery custody before the callback becomes observable.
        // A thread-start failure rejects registration instead of losing a
        // callback after the exit transition has claimed it.
        if (!s_mproc->ensure_child_exit_dispatcher()) {
            return {};
        }

        identity = detail::make_managed_child_occurrence_identity(
            m_record->identity, *selected);
        state = std::make_shared<
            detail::Managed_child_exit_subscription_state>(
                m_record,
                identity,
                std::move(callback));
        detail::runtime_stage_for_test(
            detail::test_hooks::k_stage_observe_managed_child_exit_selected);
        if (selected->native.exited()) {
            replay = detail::make_managed_child_exit(
                identity,
                selected->native.wait_status(),
                selected->native.wait_status_available());
        }
        else {
            selected->exit_subscriptions.push_back(state);
        }
    }

    Managed_child_exit_observation observation;
    observation.occurrence = identity;
    observation.subscription = Managed_child_exit_subscription(state);
    if (replay.has_value()) {
        try {
            s_mproc->dispatch_child_exit_subscription(state, *replay);
        }
        catch (...) {
            observation.subscription.unsubscribe();
            return {};
        }
    }
    return observation;
}

inline Managed_child_status Managed_child_custody::wait_until_released(
    std::chrono::steady_clock::time_point deadline) const
{
    if (!m_record) {
        return {};
    }
    std::unique_lock<std::mutex> lock(m_record->mutex);
    m_record->changed.wait_until(lock, deadline, [&]() {
        return m_record->release_state.released();
    });
    lock.unlock();
    return status();
}

inline Managed_child_status Managed_child_custody::release_until(
    std::chrono::steady_clock::time_point deadline) const
{
    if (m_record && s_mproc) {
        s_mproc->request_child_custody_release(m_record);
    }
    return wait_until_released(deadline);
}

inline Managed_child_status Managed_child_custody::terminate_until(
    std::chrono::steady_clock::time_point deadline) const
{
    if (m_record && s_mproc) {
        s_mproc->request_child_custody_release(
            m_record, detail::Release_mode::cleanup);
    }
    return wait_until_released(deadline);
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
