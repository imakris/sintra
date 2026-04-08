// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#pragma once

#include "debug_pause.h"
#include "globals.h"
#include "logging.h"
#include "process/coordinator.h"
#include "process/managed_process.h"
#include "utility.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
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

#ifndef SINTRA_TRACE_FINALIZE
#define SINTRA_TRACE_FINALIZE 0
#endif

namespace sintra {
namespace detail {

inline void append_branch(
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    int multiplicity)
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
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    append_branch(branches, descriptor, 1);
    collect_branches(branches, std::forward<Args>(rest)...);
}

template <typename... Args>
void collect_branches(
    std::vector<Process_descriptor>& branches,
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    append_branch(branches, descriptor, multiplicity);
    collect_branches(branches, std::forward<Args>(rest)...);
}

} // namespace detail

inline void disable_debug_pause_for_current_process()
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
    std::vector<Process_descriptor>& branches,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    std::vector<Process_descriptor>& branches,
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    detail::collect_branches(branches, multiplicity, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    const Process_descriptor& descriptor,
    Args&&... rest)
{
    std::vector<Process_descriptor> branches;
    detail::collect_branches(branches, descriptor, std::forward<Args>(rest)...);
    return branches;
}

template <typename... Args>
std::vector<Process_descriptor> make_branches(
    int multiplicity,
    const Process_descriptor& descriptor,
    Args&&... rest)
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
    std::string binary_path;
    std::vector<std::string> args;
    size_t count = 1;
    instance_id_type process_instance_id = invalid_instance_id;
    std::string wait_for_instance_name;
    std::chrono::milliseconds wait_timeout{0};
    Lifetime_policy lifetime;
};

// Tracks whether init() has been called without a corresponding teardown.
// This flag is reset by detail::finalize_impl() to allow init/teardown cycles
// (e.g., in tests that repeatedly initialize and tear down the library).
inline bool s_init_once = false;

inline void init(
    int argc,
    const char* const* argv,
    std::vector<Process_descriptor> branches = std::vector<Process_descriptor>())
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

    s_mproc = new Managed_process;
    s_mproc->init(argc, argv);
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

    const auto prev = s_shutdown_state.exchange(
        shutdown_protocol_state::finalizing, std::memory_order_acq_rel);
    (void)prev;  // Previous state is informational; no assertion needed here.

    const bool trace_finalize = (SINTRA_TRACE_FINALIZE != 0);

    auto trace = [&](const char* stage) {
        if (trace_finalize) {
            Log_stream(log_level::debug)
                << "[sintra_finalize] stage=" << stage
                << " coord=" << (s_coord ? 1 : 0)
                << " mproc_id=" << static_cast<unsigned long long>(s_mproc_id)
                << "\n";
        }
    };

    trace("begin");

    sequence_counter_type flush_seq = invalid_sequence;

    if (s_coord) {
        s_coord->begin_shutdown();
        trace("begin_process_draining_local.start");
        // Coordinator-local finalize: announce draining to local state so that
        // new barriers exclude this process, then wait until all known
        // processes have entered the draining state (or been scavenged) before
        // proceeding with teardown. This ensures that remote processes can
        // still complete their own begin_process_draining() RPCs while the
        // coordinator remains alive.
        flush_seq = s_coord->begin_process_draining(s_mproc_id);
        s_coord->wait_for_all_draining(s_mproc_id);
        trace("begin_process_draining_local.done");
    }
    else {
        trace("begin_process_draining_remote.start");
        try {
            auto handle = Coordinator::rpc_async_begin_process_draining(s_coord_id, s_mproc_id);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (handle.wait_until(deadline) == Rpc_wait_status::completed) {
                flush_seq = handle.get();
            }
            else {
                handle.abandon();
                s_mproc->unblock_rpc(process_of(s_coord_id));
                flush_seq = invalid_sequence;
                Log_stream(log_level::warning)
                    << "finalize(): begin_process_draining timed out after 5 seconds for process "
                    << static_cast<unsigned long long>(s_mproc_id)
                    << " while waiting on coordinator "
                    << static_cast<unsigned long long>(s_coord_id)
                    << ". Proceeding with degraded shutdown semantics.\n";
            }
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
        trace("begin_process_draining_remote.done");
    }

    if (!s_coord && flush_seq != invalid_sequence) {
        s_mproc->flush(process_of(s_coord_id), flush_seq);
    }

    // Transition into service mode before deactivating slots and unpublishing
    // transceivers. This prevents user-level event handlers from running
    // concurrently with teardown while still allowing RPCs and service
    // messages (including unpublish notifications) to flow.
    trace("pause.start");
    s_mproc->pause();
    trace("pause.done");

    trace("unblock_rpc.start");
    s_mproc->unblock_rpc();
    trace("unblock_rpc.done");

    trace("deactivate_all.start");
    s_mproc->deactivate_all();
    trace("deactivate_all.done");

    trace("unpublish_all.start");
    s_mproc->unpublish_all_transceivers();
    trace("unpublish_all.done");

    delete s_mproc;
    s_mproc = nullptr;

    // Reset flags to allow another init()/finalize() cycle.
    s_init_once = false;
    s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);

    trace("end");

    return true;
}

inline void claim_lifecycle_teardown_state(
    shutdown_protocol_state desired_state,
    const char* api_name)
{
    auto expected = shutdown_protocol_state::idle;
    if (!s_shutdown_state.compare_exchange_strong(
            expected,
            desired_state,
            std::memory_order_acq_rel))
    {
        throw std::logic_error(
            std::string(api_name) + " called while another lifecycle teardown is already in progress "
            "(state=" + std::to_string(static_cast<int>(expected)) + ").");
    }
}

inline bool lifecycle_teardown_requested()
{
    return s_shutdown_state.load(std::memory_order_acquire) !=
           shutdown_protocol_state::idle;
}

inline std::unique_lock<std::mutex> lock_coordinator_teardown_admission()
{
    if (!s_coord) {
        return std::unique_lock<std::mutex>{};
    }

    return std::unique_lock<std::mutex>{detail::s_teardown_admission_mutex};
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
    auto admission_lock = lock_coordinator_teardown_admission();
    claim_lifecycle_teardown_state(
        shutdown_protocol_state::finalizing,
        "sintra::detail::finalize()");

    if (!s_mproc) {
        s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);
        return false;
    }

    try {
        return finalize_impl();
    }
    catch (...) {
        s_shutdown_state.store(shutdown_protocol_state::idle, std::memory_order_release);
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
    auto only_self_remains = [&]() {
        std::lock_guard<std::mutex> groups_lock(s_coord->m_groups_mutex);
        auto group_it = s_coord->m_groups.find(group_name);
        if (group_it == s_coord->m_groups.end()) {
            return true;
        }

        auto& group = group_it->second;
        std::lock_guard<std::mutex> group_lock(group.m_call_mutex);
        return (group.m_process_ids.size() == 1) &&
               (group.m_process_ids.find(s_mproc_id) != group.m_process_ids.end());
    };

    bool group_drained = false;
    s_coord->m_waiting_for_all_draining.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> wait_lock(s_coord->m_draining_state_mutex);
        group_drained = s_coord->m_all_draining_cv.wait_until(
            wait_lock,
            deadline,
            only_self_remains);
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

    std::lock_guard<std::mutex> groups_lock(s_coord->m_groups_mutex);
    auto group_it = s_coord->m_groups.find(group_name);
    if (group_it == s_coord->m_groups.end()) {
        return true;
    }

    auto& group = group_it->second;
    std::lock_guard<std::mutex> group_lock(group.m_call_mutex);
    return (group.m_process_ids.size() == 1) &&
           (group.m_process_ids.find(s_mproc_id) != group.m_process_ids.end());
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
    auto admission_lock = detail::lock_coordinator_teardown_admission();

    // Fail fast: reject if another shutdown or finalize is already in progress.
    auto expected = detail::shutdown_protocol_state::idle;
    if (!detail::s_shutdown_state.compare_exchange_strong(
            expected,
            detail::shutdown_protocol_state::collective_shutdown_entered,
            std::memory_order_acq_rel))
    {
        throw std::logic_error(
            "sintra::shutdown() called while another lifecycle teardown is already in progress "
            "(state=" + std::to_string(static_cast<int>(expected)) + ").");
    }

    if (!s_mproc) {
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::idle, std::memory_order_release);
        return false;
    }

    // True single-process runtimes (no coordinator anywhere) have no
    // collective shutdown protocol to participate in.  Workers in a
    // multi-process setup (s_coord_id is valid) must enter the collective
    // barriers so that the coordinator's processing-fence can complete.
    if (!s_coord && s_coord_id == invalid_instance_id) {
        return detail::finalize_impl();
    }

    const std::string group_name = "_sintra_all_processes";
    constexpr const char* shutdown_barrier_name = "_sintra_shutdown";
    constexpr const char* hook_done_barrier_name = "_sintra_shutdown_hook_done";

    if (detail::shutdown_group_is_trivial(group_name)) {
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
    auto admission_lock = detail::lock_coordinator_teardown_admission();

    detail::claim_lifecycle_teardown_state(
        detail::shutdown_protocol_state::local_departure_entered,
        "sintra::leave()");

    if (!s_mproc) {
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::idle, std::memory_order_release);
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
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::idle, std::memory_order_release);
        throw;
    }
}

// Returns the number of processes spawned. When wait_for_instance_name is set in
// options, this waits for the instance to appear and returns 0 on wait failure
// even if spawning succeeded (the process may still be running). process_instance_id
// is not validated for uniqueness; callers must ensure it is unused. Timeout
// waits use polling with backoff to provide bounded wait durations.
inline size_t spawn_swarm_process(const Spawn_options& options)
{
    if (options.binary_path.empty()) {
        Log_stream(log_level::error)
            << "spawn_swarm_process: binary path is empty\n";
        return 0;
    }

    if (options.count == 0) {
        Log_stream(log_level::error)
            << "spawn_swarm_process: count must be greater than zero\n";
        return 0;
    }

    if ((options.process_instance_id != invalid_instance_id ||
         !options.wait_for_instance_name.empty()) &&
        options.count != 1)
    {
        Log_stream(log_level::error)
            << "spawn_swarm_process: explicit instance or wait requires count == 1\n";
        return 0;
    }

    const bool wait_requested = !options.wait_for_instance_name.empty();
    const auto wait_timeout = options.wait_timeout;
    const auto coord_id = s_coord_id;
    if (wait_requested && coord_id == invalid_instance_id) {
        Log_stream(log_level::error)
            << "spawn_swarm_process: wait requires a valid coordinator\n";
        return 0;
    }

    size_t spawned = 0;
    const auto piid = (options.process_instance_id != invalid_instance_id)
        ? options.process_instance_id
        : make_process_instance_id();

    // Ensure argv[0] is the program name (required on Windows); avoid duplicates.
    auto args = options.args;
    auto argv0_matches = [&]() {
        if (args.empty()) {
            return false;
        }
        if (args.front() == options.binary_path) {
            return true;
        }
        const auto front_name = std::filesystem::path(args.front()).filename().string();
        const auto bin_name = std::filesystem::path(options.binary_path).filename().string();
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
    spawn_args.binary_name = options.binary_path;
    spawn_args.args = args;
    spawn_args.lifetime = options.lifetime;

    for (size_t i = 0; i < options.count; ++i) {
        spawn_args.piid = piid;
        auto result = s_mproc->spawn_swarm_process(spawn_args);
        if (result.success) {
            ++spawned;
        }
    }

    if (spawned == 0 || !wait_requested) {
        return spawned;
    }

    bool wait_succeeded = false;
    const char* wait_failure_reason = nullptr;
    try {
        if (wait_timeout.count() <= 0) {
            const auto waited = Coordinator::rpc_wait_for_instance(
                coord_id,
                options.wait_for_instance_name);
            wait_succeeded = waited != invalid_instance_id;
            if (!wait_succeeded) {
                wait_failure_reason = "wait returned invalid instance id";
            }
        }
        else {
            const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
            auto poll_delay = std::chrono::milliseconds(5);
            const auto max_poll_delay = std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < deadline) {
                const auto resolved = Coordinator::rpc_resolve_instance(
                    coord_id,
                    options.wait_for_instance_name);
                if (resolved != invalid_instance_id) {
                    wait_succeeded = true;
                    break;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    break;
                }

                auto sleep_for = poll_delay;
                const auto remaining = deadline - now;
                if (sleep_for > remaining) {
                    sleep_for = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
                }
                std::this_thread::sleep_for(sleep_for);

                if (poll_delay < max_poll_delay) {
                    auto next_delay = poll_delay * 2;
                    poll_delay = (next_delay < max_poll_delay) ? next_delay : max_poll_delay;
                }
            }

            if (!wait_succeeded) {
                wait_failure_reason = "timeout";
            }
        }
    }
    catch (const std::exception& ex) {
        Log_stream(log_level::warning)
            << "spawn_swarm_process: wait failed (" << ex.what() << ")\n";
        return 0;
    }
    catch (...) {
        Log_stream(log_level::warning)
            << "spawn_swarm_process: wait failed (unknown exception)\n";
        return 0;
    }

    if (!wait_succeeded) {
        Log_stream(log_level::warning)
            << "spawn_swarm_process: wait failed (" << wait_failure_reason
            << ") for instance '" << options.wait_for_instance_name
            << "' (spawned=" << spawned << ")\n";
        return 0;
    }

    return spawned;
}

inline instance_id_type join_swarm(
    int branch_index,
    std::string binary_name = std::string())
{
    if (!s_mproc || s_coord_id == invalid_instance_id || branch_index < 1) {
        return invalid_instance_id;
    }

    if (detail::lifecycle_teardown_requested()) {
        return invalid_instance_id;
    }

    if (binary_name.empty()) {
        binary_name = s_mproc->m_binary_name;
    }

    try {
        return Coordinator::rpc_join_swarm(
            s_coord_id,
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
    return s_mproc->activate(slot_function, sender_id);
}

inline void deactivate_all_slots()
{
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
