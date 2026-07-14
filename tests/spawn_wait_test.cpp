//
// Sintra spawn_swarm_process Readiness Observation Test
//
// This test validates explicit managed-child readiness observation.
//
// The test verifies:
// - spawn_swarm_process returns accepted custody before readiness completes
// - wait_for_readiness_until reports readiness through an absolute caller deadline
// - Deadline expiry returns retained incomplete custody and requests cleanup
// - The deadline case proves child launch and cleanup when the requested name never publishes
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#endif

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using sintra::s_coord_id;

namespace {

constexpr std::string_view k_env_worker_mode           = "SPAWN_WAIT_TEST_WORKER";
constexpr const char*      k_worker_instance_name      = "spawn_wait_dynamic_worker";
constexpr const char*      k_nonexistent_instance_name = "nonexistent_instance_will_timeout";
constexpr const char*      k_timeout_child_pid_file     = "timeout_child.pid";

struct done_signal_t {};

bool is_worker_mode()
{
    const char* value = std::getenv(k_env_worker_mode.data());
    return value && *value && (*value != '0');
}

void record_failure(
    bool&             all_tests_passed,
    std::string&      failure_reason,
    std::string_view  reason)
{
    if (failure_reason.empty()) {
        failure_reason.assign(reason.data(), reason.size());
    }
    all_tests_passed = false;
}

std::filesystem::path timeout_child_pid_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / k_timeout_child_pid_file;
}

bool write_timeout_child_pid(const std::filesystem::path& shared_dir)
{
    std::ofstream out(timeout_child_pid_path(shared_dir), std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr,
            "[TIMEOUT_CHILD] Failed to open PID marker at %s\n",
            timeout_child_pid_path(shared_dir).string().c_str());
        return false;
    }

    out << sintra::test::get_pid() << '\n';
    return static_cast<bool>(out);
}

int read_timeout_child_pid(const std::filesystem::path& shared_dir)
{
    std::ifstream in(timeout_child_pid_path(shared_dir), std::ios::binary);
    int pid = -1;
    in >> pid;
    return pid;
}

#ifdef _WIN32
bool wait_for_process_exit_or_terminate(int pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return false;
    }

    HANDLE handle = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!handle) {
        return true;
    }

    const DWORD result = WaitForSingleObject(handle, static_cast<DWORD>(timeout.count()));
    if (result == WAIT_OBJECT_0) {
        CloseHandle(handle);
        return true;
    }

    if (result == WAIT_TIMEOUT) {
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 2000);
    }

    CloseHandle(handle);
    return false;
}
#else
bool process_is_alive(int pid)
{
    if (pid <= 0) {
        return false;
    }

    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

bool wait_for_process_exit_or_terminate(int pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process_is_alive(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ::kill(static_cast<pid_t>(pid), SIGTERM);
    const auto term_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < term_deadline) {
        if (!process_is_alive(pid)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ::kill(static_cast<pid_t>(pid), SIGKILL);
    return false;
}
#endif

bool run_preinit_spawn_swarm_validation()
{
    bool ok = true;

    {
        sintra::Spawn_options options;
        options.binary_path = "";
        const auto custody = sintra::spawn_swarm_process(options);
        ok &= sintra::test::assert_true(!custody,
            "[PREINIT] ",
            "spawn_swarm_process should reject when binary_path is empty");
    }

    {
        sintra::Spawn_options options;
        options.binary_path = "dummy_binary";
        options.readiness_instance_name = "dummy_instance";
        const auto custody = sintra::spawn_swarm_process(options);
        ok &= sintra::test::assert_true(
            !custody,
            "[PREINIT] ",
            "spawn_swarm_process should reject when wait requires a coordinator before init");
    }

    return ok;
}

// Worker process entry point - registers itself with a name and waits
int run_worker()
{
    std::fprintf(stderr, "[WORKER] Starting dynamic worker\n");

    struct Worker_transceiver : sintra::Derived_transceiver<Worker_transceiver>
    {
        Worker_transceiver() : Derived_transceiver<Worker_transceiver>() {}
    };

    // Wait for done signal
    std::condition_variable done_cv;
    std::mutex done_mutex;
    bool done = false;

    sintra::activate_slot([&](const done_signal_t&) {
        std::fprintf(stderr, "[WORKER] Received Done signal\n");
        std::lock_guard<std::mutex> lk(done_mutex);
        done = true;
        done_cv.notify_one();
    });

    Worker_transceiver worker;
    if (!worker.assign_name(k_worker_instance_name)) {
        std::fprintf(stderr, "[WORKER] Failed to assign name '%s'\n", k_worker_instance_name);
        sintra::deactivate_all_slots();
        return 1;
    }

    std::fprintf(stderr, "[WORKER] Registered as '%s'\n", k_worker_instance_name);

    std::unique_lock<std::mutex> lk(done_mutex);
    const bool signaled = done_cv.wait_for(lk, std::chrono::seconds(30), [&] { return done; });

    sintra::deactivate_all_slots();

    if (!signaled) {
        std::fprintf(stderr, "[WORKER] Timed out waiting for Done signal\n");
        return 1;
    }

    std::fprintf(stderr, "[WORKER] Exiting normally\n");
    return 0;
}

// Timeout child for Test 1: joins the swarm but intentionally publishes no name.
int run_timeout_child()
{
    std::fprintf(stderr, "[TIMEOUT_CHILD] Running without publishing a name\n");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::fprintf(stderr, "[TIMEOUT_CHILD] Timed out waiting for cleanup\n");
    return 1;
}

// Coordinator that observes managed-child readiness through explicit deadlines.
int run_coordinator(const std::string& binary_path)
{
    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "spawn_wait_test");
    const auto result_path = shared.path() / "result.txt";

    std::fprintf(stderr, "[COORDINATOR] Starting spawn_swarm_process wait tests\n");
    std::fprintf(stderr, "[COORDINATOR] Binary path: %s\n", binary_path.c_str());

    bool all_tests_passed = true;
    std::string failure_reason;

    // Test 1: an explicit readiness deadline for a nonexistent instance.
    // The child writes a PID marker but never publishes the requested instance.
    // The returned status is incomplete and open; explicit cleanup leaves no survivor.
    {
        std::fprintf(stderr, "[COORDINATOR] Test 1: Testing incomplete readiness at a short deadline\n");
        const auto start = std::chrono::steady_clock::now();

        sintra::Spawn_options spawn_options;
        spawn_options.binary_path            = binary_path;
        spawn_options.env_overrides.push_back(std::string(k_env_worker_mode) + "=0");
        spawn_options.readiness_instance_name = k_nonexistent_instance_name;

        const auto custody = sintra::spawn_swarm_process(spawn_options);
        const auto accepted = custody.status();
        const auto launch = custody.wait_for_readiness_until(
            start + std::chrono::milliseconds(1500));

        const auto elapsed    = std::chrono::steady_clock::now() - start;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        std::fprintf(stderr, "[COORDINATOR] Test 1: custody ready=%d after %lldms\n",
            launch.readiness_state == sintra::Managed_child_readiness_state::reached
                ? 1 : 0,
            (long long)elapsed_ms);

        if (!custody ||
            accepted.release_state != sintra::Managed_child_release_state::open ||
            launch.created_occurrences != 1 ||
            launch.readiness_state != sintra::Managed_child_readiness_state::pending ||
            launch.release_state != sintra::Managed_child_release_state::open)
        {
            std::fprintf(stderr, "[COORDINATOR] Test 1: Invalid incomplete custody snapshot\n");
            record_failure(
                all_tests_passed,
                failure_reason,
                "Test 1: readiness deadline should return retained incomplete custody");
        }

        // Verify the explicit deadline wait did not return immediately.
        if (all_tests_passed && elapsed_ms < 200) {
            std::fprintf(stderr,
                "[COORDINATOR] Test 1: Readiness deadline returned too quickly: %lldms\n",
                (long long)elapsed_ms);
            record_failure(
                all_tests_passed,
                failure_reason,
                "Test 1: readiness deadline returned too quickly");
        }
        else
        if (elapsed_ms > 5000) {
            std::fprintf(stderr,
                "[COORDINATOR] Test 1: Readiness wait duration unusually long: %lldms\n",
                (long long)elapsed_ms);
        }

        const auto cleanup = custody.terminate_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
        if (!custody || cleanup.release_state !=
            sintra::Managed_child_release_state::complete)
        {
            record_failure(
                all_tests_passed,
                failure_reason,
                "Test 1: explicit cleanup did not complete");
        }

        const bool child_marker_written = sintra::test::wait_for_file(
            timeout_child_pid_path(shared.path()),
            std::chrono::seconds(3),
            std::chrono::milliseconds(20));
        if (!child_marker_written) {
            std::fprintf(stderr,
                "[COORDINATOR] Test 1: deadline child PID marker was not written\n");
            record_failure(
                all_tests_passed,
                failure_reason,
                "Test 1: spawn_swarm_process failed - child never launched");
        }
        else {
            const int timeout_child_pid = read_timeout_child_pid(shared.path());
            std::fprintf(stderr,
                "[COORDINATOR] Test 1: Confirmed child launched with pid %d\n",
                timeout_child_pid);

            if (timeout_child_pid <= 0) {
                record_failure(
                    all_tests_passed,
                    failure_reason,
                    "Test 1: deadline child PID marker was invalid");
            }
            else {
                const bool child_exited = wait_for_process_exit_or_terminate(
                    timeout_child_pid,
                    std::chrono::seconds(5));
                if (!child_exited) {
                    std::fprintf(stderr,
                        "[COORDINATOR] Test 1: deadline child pid %d remained alive after cleanup\n",
                        timeout_child_pid);
                    record_failure(
                        all_tests_passed,
                        failure_reason,
                        "Test 1: readiness-incomplete child was left alive");
                }
            }
        }
    }

    // Test 2: explicit readiness observation that should succeed
    {
        std::fprintf(stderr, "[COORDINATOR] Test 2: Testing successful wait case\n");

        const auto start = std::chrono::steady_clock::now();

        sintra::Spawn_options spawn_options;
        spawn_options.binary_path            = binary_path;
        spawn_options.env_overrides.push_back(std::string(k_env_worker_mode) + "=1");
        spawn_options.readiness_instance_name = k_worker_instance_name;

        const auto custody = sintra::spawn_swarm_process(spawn_options);
        const auto accepted = custody.status();
        const auto launch = custody.wait_for_readiness_until(
            start + std::chrono::milliseconds(10000));

        const auto elapsed    = std::chrono::steady_clock::now() - start;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        std::fprintf(stderr, "[COORDINATOR] Test 2: custody ready=%d after %lldms\n",
            launch.readiness_state == sintra::Managed_child_readiness_state::reached
                ? 1 : 0,
            (long long)elapsed_ms);

        if (!custody ||
            accepted.release_state != sintra::Managed_child_release_state::open ||
            launch.readiness_state != sintra::Managed_child_readiness_state::reached ||
            launch.created_occurrences != 1)
        {
            std::fprintf(stderr, "[COORDINATOR] Test 2: Expected ready child custody\n");
            record_failure(
                all_tests_passed,
                failure_reason,
                "Test 2: wait_for_readiness_until should confirm the requested readiness instance");
        }
        else {
            // Verify the instance is actually resolvable
            const auto resolved = sintra::Coordinator::rpc_resolve_instance(
                s_coord_id,
                k_worker_instance_name);

            if (resolved == sintra::invalid_instance_id) {
                std::fprintf(stderr, "[COORDINATOR] Test 2: Instance not resolvable after readiness was confirmed\n");
                record_failure(
                    all_tests_passed,
                    failure_reason,
                    "Test 2: Instance should be resolvable after readiness confirmation");
            }
            else {
                std::fprintf(stderr, "[COORDINATOR] Test 2: Instance resolved successfully: %llu\n",
                    (unsigned long long)resolved);
            }

        }

        // Best-effort cleanup if the worker started but readiness stayed incomplete.
        sintra::world() << done_signal_t{};
    }

    // Write result
    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    if (all_tests_passed) {
        out << "ok\n";
        std::fprintf(stderr, "[COORDINATOR] All tests passed\n");
    }
    else {
        out << "fail\n" << failure_reason << "\n";
        std::fprintf(stderr, "[COORDINATOR] Tests failed: %s\n", failure_reason.c_str());
    }

    return all_tests_passed ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_argv_flag(argc, argv, "--instance_id");
    const bool is_worker  = is_worker_mode();
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "spawn_wait_test");
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);

    if (!is_spawned) {
        if (!run_preinit_spawn_swarm_validation()) {
            return 1;
        }
    }

    // If spawned by spawn_swarm_process in worker mode, run as worker
    if (is_spawned && is_worker) {
        sintra::init(argc, argv);
        int result = run_worker();
        sintra::detail::finalize();
        return result;
    }

    // If spawned but SPAWN_WAIT_TEST_WORKER is not set, this is the deadline child
    // from Test 1. Write a PID marker before init so the coordinator can prove
    // the OS process launched even if deadline cleanup terminates it promptly.
    if (is_spawned && !is_worker) {
        if (!write_timeout_child_pid(shared.path())) {
            return 1;
        }
        sintra::init(argc, argv);
        int result = run_timeout_child();
        sintra::detail::finalize();
        return result;
    }

    // Main coordinator process
    sintra::init(argc, argv);

    int result = run_coordinator(binary_path);

    // Wait for result file
    const auto result_path = shared.path() / "result.txt";
    for (int i = 0; i < 100; ++i) {
        if (std::filesystem::exists(result_path)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sintra::detail::finalize();

    // Read and verify result
    if (!std::filesystem::exists(result_path)) {
        std::fprintf(stderr, "Result file not found\n");
        return 1;
    }

    std::ifstream in(result_path, std::ios::binary);
    std::string status;
    in >> status;

    // Cleanup
    shared.cleanup();

    return (status == "ok") ? 0 : 1;
}
