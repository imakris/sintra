// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Transport retirement must not terminate a still-live managed child.
#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr const char* k_child_flag = "--graceful_lifeline_child";
constexpr auto k_child_iid = sintra::compose_instance(35u, 1ull);

struct Ready_target : sintra::Derived_transceiver<Ready_target>
{};

int run_child(int argc, char* argv[], const fs::path& directory)
{
    sintra::init(argc, argv);
    {
        Ready_target ready;
        if (!ready.assign_name("graceful_lifeline_ready") ||
            !sintra::test::wait_for_file(directory / "depart", 10s, 10ms))
        {
            std::fprintf(stderr, "child readiness/departure handshake failed\n");
            return 2;
        }
    }
    if (!sintra::shutdown()) {
        std::fprintf(stderr, "child runtime shutdown did not complete\n");
        return 2;
    }
    // Model application/Qt destruction after transport retirement. This pause
    // is deliberately longer than the unchanged default 100 ms lifeline limit.
    std::this_thread::sleep_for(500ms);
    return sintra::test::managed_child::write_complete_file(
        directory / "destructors_completed", "complete\n") ? 0 : 2;
}

int run_owner(int argc, char* argv[], const fs::path& directory)
{
    sintra::init(argc, argv);
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {k_child_flag};
    options.process_instance_id = k_child_iid;
    options.readiness_instance_name = "graceful_lifeline_ready";
    // Use the real default lifeline, not a disabled or extended watchdog.
    auto custody = sintra::spawn_swarm_process(options);
    const bool ready = custody.wait_for_readiness_until(
        std::chrono::steady_clock::now() + 10s).readiness_state ==
        sintra::Managed_child_readiness_state::reached;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<sintra::Managed_child_exit> exit;
    auto observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            const std::lock_guard lock(mutex);
            exit = event;
            changed.notify_all();
        });
    const bool requested = ready && observation &&
        sintra::test::managed_child::write_complete_file(directory / "depart", "depart\n");
    bool observed = false;
    {
        std::unique_lock lock(mutex);
        observed = changed.wait_for(lock, 10s, [&] { return exit.has_value(); });
    }
    // Cleanup is explicit and cannot manufacture a successful observation.
    if (!observed) {
        (void)custody.terminate_until(std::chrono::steady_clock::now() + 5s);
    }
    const auto released = custody.release_until(std::chrono::steady_clock::now() + 5s);
    bool valid = false;
    {
        const std::lock_guard lock(mutex);
        valid = requested && observed && exit &&
            exit->occurrence == observation.occurrence &&
            exit->native_status_available &&
            exit->status_kind == sintra::Managed_child_exit_status_kind::exited &&
            exit->status == 0 && fs::exists(directory / "destructors_completed") &&
            released.release_state == sintra::Managed_child_release_state::complete;
        std::fprintf(valid ? stdout : stderr,
            "graceful_lifeline: observed=%d kind=%d status=%u destructors=%d released=%d\n",
            observed, exit ? static_cast<int>(exit->status_kind) : -1,
            exit ? exit->status : 0, fs::exists(directory / "destructors_completed"),
            released.release_state == sintra::Managed_child_release_state::complete);
    }
    observation.subscription.unsubscribe();
    const bool finalized = sintra::shutdown();
    return valid && finalized ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "graceful_lifeline");
        return sintra::test::has_argv_flag(argc, argv, k_child_flag)
            ? run_child(argc, argv, shared.path()) : run_owner(argc, argv, shared.path());
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "graceful_lifeline: %s\n", error.what());
        return 2;
    }
}
