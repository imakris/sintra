//
// Sintra spawn_swarm_process Wait Options Test
//
// This test validates the wait_for_instance_name and wait_timeout options
// added to spawn_swarm_process in commit 7481284.
//
// The test verifies:
// - spawn_swarm_process with wait_for_instance_name blocks until the instance appears
// - spawn_swarm_process returns the spawned count on success
// - spawn_swarm_process with wait_timeout returns 0 on timeout
// - The exponential backoff polling path is exercised (via short timeout)
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_environment.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";
constexpr std::string_view kEnvWorkerMode = "SPAWN_WAIT_TEST_WORKER";
constexpr const char* kWorkerInstanceName = "spawn_wait_dynamic_worker";
constexpr const char* kNonexistentInstanceName = "nonexistent_instance_will_timeout";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("spawn_wait_test");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "spawn_wait_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

struct Done_signal {};

bool has_instance_id_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--instance_id") {
            return true;
        }
    }
    return false;
}

bool is_worker_mode()
{
    const char* value = std::getenv(kEnvWorkerMode.data());
    return value && *value && (*value != '0');
}

std::string get_binary_path(int argc, char* argv[])
{
    if (argc > 0 && argv[0]) {
        return argv[0];
    }
    return "";
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

    sintra::activate_slot([&](const Done_signal&) {
        std::fprintf(stderr, "[WORKER] Received Done signal\n");
        std::lock_guard<std::mutex> lk(done_mutex);
        done = true;
        done_cv.notify_one();
    });

    Worker_transceiver worker;
    if (!worker.assign_name(kWorkerInstanceName)) {
        std::fprintf(stderr, "[WORKER] Failed to assign name '%s'\n", kWorkerInstanceName);
        sintra::deactivate_all_slots();
        return 1;
    }

    std::fprintf(stderr, "[WORKER] Registered as '%s'\n", kWorkerInstanceName);

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

// Coordinator that uses spawn_swarm_process with wait options
int run_coordinator(const std::string& binary_path)
{
    const auto shared_dir = get_shared_directory();
    const auto result_path = shared_dir / "result.txt";

    std::fprintf(stderr, "[COORDINATOR] Starting spawn_swarm_process wait tests\n");
    std::fprintf(stderr, "[COORDINATOR] Binary path: %s\n", binary_path.c_str());

    bool all_tests_passed = true;
    std::string failure_reason;

    // Ensure worker mode is disabled before the timeout test.
#ifdef _WIN32
    _putenv_s(kEnvWorkerMode.data(), "");
#else
    unsetenv(kEnvWorkerMode.data());
#endif

    // Test 1: spawn_swarm_process with timeout that should fail (nonexistent instance)
    // This exercises the exponential backoff polling path.
    // The spawned child won't register any name because SPAWN_WAIT_TEST_WORKER env var
    // is not set, causing it to take the "dummy" path that just sleeps and exits.
    {
        std::fprintf(stderr, "[COORDINATOR] Test 1: Testing timeout case with short wait\n");
        const auto start = std::chrono::steady_clock::now();

        const size_t spawned = sintra::spawn_swarm_process(
            binary_path,
            {},  // No extra args; child behavior controlled by env var
            1,
            sintra::invalid_instance_id,
            kNonexistentInstanceName,  // Wait for a name that won't exist
            std::chrono::milliseconds(500)  // Short timeout to exercise backoff
        );

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        std::fprintf(stderr, "[COORDINATOR] Test 1: spawn returned %zu after %lldms\n",
                     spawned, (long long)elapsed_ms);

        // The spawn itself may succeed (process started) but wait should fail and return 0
        // because the instance name never appears.
        if (spawned != 0) {
            // The function returns 0 when wait fails, even if spawn succeeded.
            std::fprintf(stderr, "[COORDINATOR] Test 1: Expected return 0 on timeout, got %zu\n", spawned);
            all_tests_passed = false;
            failure_reason = "Test 1: spawn_swarm_process should return 0 on wait timeout";
        }

        // Verify the timeout was respected (should not return immediately).
        if (elapsed_ms < 200) {
            std::fprintf(stderr,
                         "[COORDINATOR] Test 1: Timeout returned too quickly: %lldms\n",
                         (long long)elapsed_ms);
            all_tests_passed = false;
            failure_reason = "Test 1: wait_timeout returned too quickly";
        }
        else if (elapsed_ms > 2000) {
            std::fprintf(stderr,
                         "[COORDINATOR] Test 1: Timeout duration unusually long: %lldms\n",
                         (long long)elapsed_ms);
        }
    }

    // Test 2: spawn_swarm_process with wait_for_instance_name that should succeed
    {
        std::fprintf(stderr, "[COORDINATOR] Test 2: Testing successful wait case\n");

        // Set environment to tell the spawned process to run in worker mode
#ifdef _WIN32
        _putenv_s(kEnvWorkerMode.data(), "1");
#else
        setenv(kEnvWorkerMode.data(), "1", 1);
#endif

        const auto start = std::chrono::steady_clock::now();

        const size_t spawned = sintra::spawn_swarm_process(
            binary_path,
            {},  // No extra args
            1,
            sintra::invalid_instance_id,
            kWorkerInstanceName,  // Wait for worker to register this name
            std::chrono::milliseconds(10000)  // 10s timeout should be enough
        );

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        std::fprintf(stderr, "[COORDINATOR] Test 2: spawn returned %zu after %lldms\n",
                     spawned, (long long)elapsed_ms);

        if (spawned != 1) {
            std::fprintf(stderr, "[COORDINATOR] Test 2: Expected return 1 on success, got %zu\n", spawned);
            all_tests_passed = false;
            failure_reason = "Test 2: spawn_swarm_process should return 1 on successful wait";
        }
        else {
            // Verify the instance is actually resolvable
            const auto resolved = sintra::Coordinator::rpc_resolve_instance(
                s_coord_id,
                kWorkerInstanceName);

            if (resolved == sintra::invalid_instance_id) {
                std::fprintf(stderr, "[COORDINATOR] Test 2: Instance not resolvable after spawn returned\n");
                all_tests_passed = false;
                failure_reason = "Test 2: Instance should be resolvable after spawn_swarm_process returns";
            }
            else {
                std::fprintf(stderr, "[COORDINATOR] Test 2: Instance resolved successfully: %llu\n",
                             (unsigned long long)resolved);
            }

            // Signal the worker to finish
            sintra::world() << Done_signal{};
        }

        // Clear worker mode env
#ifdef _WIN32
        _putenv_s(kEnvWorkerMode.data(), "");
#else
        unsetenv(kEnvWorkerMode.data());
#endif
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
    const bool is_spawned = has_instance_id_flag(argc, argv);
    const bool is_worker = is_worker_mode();
    const auto shared_dir = ensure_shared_directory();
    const std::string binary_path = get_binary_path(argc, argv);

    // If spawned by spawn_swarm_process in worker mode, run as worker
    if (is_spawned && is_worker) {
        sintra::init(argc, argv);
        int result = run_worker();
        sintra::finalize();
        return result;
    }

    // If spawned but SPAWN_WAIT_TEST_WORKER env var is not set, this is the "dummy" child
    // from Test 1. Don't register any name - just wait long enough for the coordinator's
    // timeout to trigger, then clean up and exit.
    if (is_spawned && !is_worker) {
        sintra::init(argc, argv);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        sintra::finalize();
        return 0;
    }

    // Main coordinator process
    sintra::init(argc, argv);

    int result = run_coordinator(binary_path);

    // Wait for result file
    const auto result_path = shared_dir / "result.txt";
    for (int i = 0; i < 100; ++i) {
        if (std::filesystem::exists(result_path)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sintra::finalize();

    // Read and verify result
    if (!std::filesystem::exists(result_path)) {
        std::fprintf(stderr, "Result file not found\n");
        return 1;
    }

    std::ifstream in(result_path, std::ios::binary);
    std::string status;
    in >> status;

    // Cleanup
    std::error_code ec;
    std::filesystem::remove_all(shared_dir, ec);

    return (status == "ok") ? 0 : 1;
}
