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

class Cleanup_guard {
public:
    explicit Cleanup_guard(std::function<void()> callback)
        : m_callback(std::move(callback))
    {}

    Cleanup_guard(const Cleanup_guard&) = delete;
    Cleanup_guard& operator=(const Cleanup_guard&) = delete;

    ~Cleanup_guard()
    {
        m_callback();
    }

private:
    std::function<void()> m_callback;
};

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

inline bool finalize();

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

// Tracks whether init() has been called without a corresponding finalize().
// This flag is reset by finalize() to allow init()/finalize() cycles (e.g.,
// in tests that repeatedly initialize and tear down the library).
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

    assert(!s_init_once); // init() may only be called once before finalize().
    s_init_once = true;

    static detail::Cleanup_guard cleanup_guard([]() {
        if (s_mproc) {
            finalize();
        }
    });

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

inline bool finalize()
{
    if (!s_mproc) {
        return false;
    }

    const bool trace_finalize = [] {
        const char* env = std::getenv("SINTRA_TRACE_FINALIZE");
        return env && *env && (*env != '0');
    }();

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
        std::atomic<bool> done{false};
        std::thread watchdog([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!done.load()) {
                s_mproc->unblock_rpc(process_of(s_coord_id));
            }
        });

        trace("begin_process_draining_remote.start");
        try {
            flush_seq = Coordinator::rpc_begin_process_draining(s_coord_id, s_mproc_id);
        }
        catch (...) {
            flush_seq = invalid_sequence;
        }
        trace("begin_process_draining_remote.done");

        done = true;
        watchdog.join();
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

    trace("deactivate_all.start");
    s_mproc->deactivate_all();
    trace("deactivate_all.done");

    trace("unpublish_all.start");
    s_mproc->unpublish_all_transceivers();
    trace("unpublish_all.done");

    delete s_mproc;
    s_mproc = nullptr;

    // Reset the init flag to allow another init()/finalize() cycle.
    s_init_once = false;

    trace("end");

    return true;
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

    deactivator();
    return std::move(*result);
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
