// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr const char* k_child_flag = "--orderly-lifeline-child";
constexpr auto k_child_iid = sintra::compose_instance(47u, 1ull);

class Ready : public sintra::Derived_transceiver<Ready> {};

bool check(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return value;
}

bool wait_for_file(const std::filesystem::path& path, Clock::time_point deadline)
{
    while (Clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

int run_child(int argc, char* argv[], const std::filesystem::path& directory)
{
    sintra::init(argc, argv);
    {
        Ready ready;
        if (!ready.assign_name("orderly_lifeline_ready")) {
            return 2;
        }
        if (!wait_for_file(directory / "leave", Clock::now() + 10s)) {
            return 3;
        }
    }
    if (!sintra::leave()) {
        return 4;
    }
    // Native process teardown may legitimately outlive IPC publication. This
    // is a test hold, not a production watchdog or a larger shutdown allowance.
    std::ofstream(directory / "left") << "IPC retired\n";
    if (!wait_for_file(directory / "return", Clock::now() + 10s)) {
        return 5;
    }
    return 0;
}

int run_root(int argc, char* argv[], const std::filesystem::path& directory)
{
    sintra::init(argc, argv);
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {k_child_flag};
    options.process_instance_id = k_child_iid;
    options.readiness_instance_name = "orderly_lifeline_ready";
    // Keep the production lifeline enabled, with its unchanged 100 ms timeout.
    auto custody = sintra::spawn_swarm_process(options);
    bool valid = check(custody.wait_for_readiness_until(Clock::now() + 8s).readiness_state ==
        sintra::Managed_child_readiness_state::reached, "child readiness");
    const auto native = custody.native_snapshot();
    if (!check(native.size() == 1, "one exact native occurrence")) {
        custody.terminate_until(Clock::now() + 5s);
        sintra::shutdown();
        return 1;
    }
    const auto identity = native.front().occurrence;
    sintra::test::managed_child::Managed_child_exit_capture exited;
    auto observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exited.record(event); });
    valid &= check(static_cast<bool>(observation), "exact exit observer registration");
    std::ofstream(directory / "leave") << "leave\n";
    valid &= check(wait_for_file(directory / "left", Clock::now() + 8s),
        "child returns from local IPC leave");
    // A baseline that drops the lifeline on unpublish exits 99 during this
    // hold. Parent remains alive; no cleanup/termination request is issued.
    std::this_thread::sleep_for(400ms);
    valid &= check(exited.snapshot().deliveries == 0,
        "publication retirement must not terminate a live native child");
    std::ofstream(directory / "return") << "return\n";
    valid &= check(exited.wait_for_one_until(Clock::now() + 8s), "native exit observed");
    const auto event = exited.snapshot().event;
    if (event) {
        std::fprintf(stderr, "NATIVE_EXIT iid=%llu custody=%llu occurrence=%u kind=%d status=%u native=%u\n",
            (unsigned long long)event->occurrence.process_instance_id,
            (unsigned long long)event->occurrence.custody_identity,
            event->occurrence.occurrence, (int)event->status_kind,
            event->status, event->native_status);
    }
    valid &= check(exited.exact(identity), "original occurrence delivers exactly once");
    valid &= check(exited.normal_zero(), "orderly child returns native zero");
    valid &= check(custody.release_until(Clock::now() + 5s).release_state ==
        sintra::Managed_child_release_state::complete, "passive custody settlement");
    sintra::shutdown();
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared("SINTRA_ORDERLY_LIFELINE_DIR", "orderly_lifeline");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared.path());
}
