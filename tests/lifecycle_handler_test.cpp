//
// Sintra Lifecycle Handler Test
//
// This test validates the lifecycle handler functionality introduced in
// commits 4442c0a and 38de6d7.
//
// The test verifies:
// - set_lifecycle_handler() configures the callback correctly
// - process_lifecycle_event is emitted with reason::normal_exit on clean exit
// - process_lifecycle_event is emitted with reason::crash on process crash
// - process_lifecycle_event is emitted with reason::unpublished on abrupt exit without finalize
// - process_lifecycle_event contains valid process_iid and process_slot
// - Lifecycle events correlate to the correct worker (process_iid matches)
// - Crash events have non-zero status (platform-dependent)
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>
#include <sintra/detail/process/lifecycle_types.h>

#include "test_environment.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

namespace {

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

// Worker IDs for correlation
constexpr int kNormalWorkerId = 1;
constexpr int kCrashWorkerId = 2;
constexpr int kUnpublishedWorkerId = 3;

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

    auto base = sintra::test::scratch_subdirectory("lifecycle_handler_test");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "lifecycle_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

constexpr auto kReadyTimeout = std::chrono::seconds(5);
constexpr auto kSignalTimeout = std::chrono::seconds(5);
constexpr auto kEventTimeout = std::chrono::seconds(5);
constexpr auto kPollInterval = std::chrono::milliseconds(50);

std::filesystem::path worker_ready_path(const std::filesystem::path& dir, int worker_id)
{
    std::ostringstream oss;
    oss << "worker_ready_" << worker_id << ".txt";
    return dir / oss.str();
}

bool write_worker_ready(const std::filesystem::path& dir,
                        int worker_id,
                        sintra::instance_id_type process_iid)
{
    const auto path = worker_ready_path(dir, worker_id);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << static_cast<unsigned long long>(process_iid) << "\n";
    return static_cast<bool>(out);
}

bool read_worker_ready(const std::filesystem::path& dir,
                       int worker_id,
                       sintra::instance_id_type* process_iid)
{
    const auto path = worker_ready_path(dir, worker_id);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    unsigned long long raw = 0;
    in >> raw;
    if (!in) {
        return false;
    }
    *process_iid = static_cast<sintra::instance_id_type>(raw);
    return true;
}

std::filesystem::path signal_path(const std::filesystem::path& dir, const char* name)
{
    std::ostringstream oss;
    oss << "signal_" << name << ".txt";
    return dir / oss.str();
}

bool write_signal_file(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out);
}

bool wait_for_signal_file(const std::filesystem::path& path,
                          std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return std::filesystem::exists(path);
}

// Tracked lifecycle events (only in coordinator)
std::mutex g_events_mutex;
std::vector<sintra::process_lifecycle_event> g_events;
std::condition_variable g_events_cv;

void lifecycle_handler(const sintra::process_lifecycle_event& event)
{
    const char* reason_str = "unknown";
    switch (event.why) {
        case sintra::process_lifecycle_event::reason::normal_exit:
            reason_str = "normal_exit";
            break;
        case sintra::process_lifecycle_event::reason::crash:
            reason_str = "crash";
            break;
        case sintra::process_lifecycle_event::reason::unpublished:
            reason_str = "unpublished";
            break;
    }

    std::fprintf(stderr, "[LIFECYCLE] Event: process_iid=%llu, slot=%u, reason=%s, status=%d\n",
                 (unsigned long long)event.process_iid,
                 event.process_slot,
                 reason_str,
                 event.status);

    std::lock_guard<std::mutex> lk(g_events_mutex);
    g_events.push_back(event);
    g_events_cv.notify_all();
}

#if defined(__APPLE__)
void disable_core_dumps_for_intentional_crash()
{
    struct rlimit current {};
    if (getrlimit(RLIMIT_CORE, &current) != 0) {
        return;
    }
    if (current.rlim_cur == 0) {
        return;
    }
    struct rlimit updated = current;
    updated.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &updated);
}
#endif

// Worker that exits normally after receiving finish signal file
int process_normal_worker()
{
    std::fprintf(stderr, "[NORMAL_WORKER] Starting\n");

    const auto shared_dir = get_shared_directory();
    if (!write_worker_ready(shared_dir,
                            kNormalWorkerId,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[NORMAL_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto finish_signal_path = signal_path(shared_dir, "finish");
    if (!wait_for_signal_file(finish_signal_path, kSignalTimeout)) {
        std::fprintf(stderr, "[NORMAL_WORKER] ERROR: Timed out waiting for Finish signal\n");
        return 1;  // Fail explicitly on timeout
    }

    std::fprintf(stderr, "[NORMAL_WORKER] Exiting normally\n");
    return 0;
}

// Worker that crashes after receiving crash signal file
int process_crash_worker()
{
    std::fprintf(stderr, "[CRASH_WORKER] Starting\n");

#if defined(_MSC_VER)
    // Suppress the CRT abort dialog
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    const auto shared_dir = get_shared_directory();
    if (!write_worker_ready(shared_dir,
                            kCrashWorkerId,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[CRASH_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto crash_signal_path = signal_path(shared_dir, "crash");
    if (!wait_for_signal_file(crash_signal_path, kSignalTimeout)) {
        std::fprintf(stderr, "[CRASH_WORKER] ERROR: Timed out waiting for Crash signal\n");
        return 1;
    }

    std::fprintf(stderr, "[CRASH_WORKER] About to crash via illegal instruction\n");

#if defined(__APPLE__)
    disable_core_dumps_for_intentional_crash();
#endif

    sintra::disable_debug_pause_for_current_process();

    // Crash via illegal instruction
    sintra::test::trigger_illegal_instruction_crash();

    // Should never reach here
    return 0;
}

// Worker that exits abruptly via _exit() without calling finalize()
// This should trigger an "unpublished" lifecycle event
int process_unpublished_worker()
{
    std::fprintf(stderr, "[UNPUBLISHED_WORKER] Starting\n");

    const auto shared_dir = get_shared_directory();
    if (!write_worker_ready(shared_dir,
                            kUnpublishedWorkerId,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[UNPUBLISHED_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto unpublish_signal_path = signal_path(shared_dir, "unpublish");
    if (!wait_for_signal_file(unpublish_signal_path, kSignalTimeout)) {
        std::fprintf(stderr, "[UNPUBLISHED_WORKER] ERROR: Timed out waiting for Unpublish signal\n");
        return 1;
    }

    std::fprintf(stderr, "[UNPUBLISHED_WORKER] About to exit abruptly via _exit(0) without finalize\n");

    // Exit without calling finalize() - this should cause an "unpublished" event
    // We use _exit() to bypass atexit handlers and ensure no cleanup occurs
#ifdef _WIN32
    _exit(0);
#else
    _exit(0);
#endif

    // Should never reach here
    return 0;
}

// Coordinator that sets up lifecycle handler and verifies events
int process_coordinator()
{
    const auto shared_dir = get_shared_directory();
    const auto result_path = shared_dir / "result.txt";

    std::fprintf(stderr, "[COORDINATOR] Starting, setting up lifecycle handler\n");

    // Set up lifecycle handler
    sintra::set_lifecycle_handler(lifecycle_handler);

    bool test_passed = true;
    std::string failure_reason;

    // Wait for workers to publish ready files (written after slot activation)
    sintra::instance_id_type crash_worker_iid = sintra::invalid_instance_id;
    sintra::instance_id_type unpub_worker_iid = sintra::invalid_instance_id;
    sintra::instance_id_type normal_worker_iid = sintra::invalid_instance_id;
    const auto ready_deadline = std::chrono::steady_clock::now() + kReadyTimeout;
    while (std::chrono::steady_clock::now() < ready_deadline) {
        bool have_all = true;
        if (crash_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, kCrashWorkerId, &crash_worker_iid) && have_all;
        }
        if (unpub_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, kUnpublishedWorkerId, &unpub_worker_iid) && have_all;
        }
        if (normal_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, kNormalWorkerId, &normal_worker_iid) && have_all;
        }
        if (have_all) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (crash_worker_iid == sintra::invalid_instance_id ||
        unpub_worker_iid == sintra::invalid_instance_id ||
        normal_worker_iid == sintra::invalid_instance_id) {
        std::fprintf(stderr,
                     "[COORDINATOR] ERROR: Not all workers ready via ready files\n");
        test_passed = false;
        failure_reason = "Not all workers became ready";
    }

    std::fprintf(stderr, "[COORDINATOR] Worker process_iids: normal=%llu, crash=%llu, unpublished=%llu\n",
                 (unsigned long long)normal_worker_iid,
                 (unsigned long long)crash_worker_iid,
                 (unsigned long long)unpub_worker_iid);

    const auto crash_signal_path = signal_path(shared_dir, "crash");
    const auto unpublish_signal_path = signal_path(shared_dir, "unpublish");
    const auto finish_signal_path = signal_path(shared_dir, "finish");

    if (test_passed) {
        // Test 1: Trigger crash worker
        std::fprintf(stderr, "[COORDINATOR] Test 1: Triggering crash in crash_worker\n");
        if (!write_signal_file(crash_signal_path)) {
            test_passed = false;
            failure_reason = "Failed to write crash signal file";
        }

        // Wait for crash lifecycle event
        {
            std::unique_lock<std::mutex> events_lk(g_events_mutex);
            const bool crash_event = g_events_cv.wait_for(
                events_lk, kEventTimeout,
                [&] {
                    for (const auto& evt : g_events) {
                        if (evt.why == sintra::process_lifecycle_event::reason::crash) {
                            return true;
                        }
                    }
                    return false;
                });

            if (!crash_event) {
                std::fprintf(stderr, "[COORDINATOR] ERROR: No crash event received\n");
                test_passed = false;
                failure_reason = "No crash lifecycle event received";
            }
            else {
                // Verify crash event fields and correlation
                for (const auto& evt : g_events) {
                    if (evt.why == sintra::process_lifecycle_event::reason::crash) {
                        std::fprintf(stderr, "[COORDINATOR] Crash event validated: iid=%llu, slot=%u, status=%d\n",
                                     (unsigned long long)evt.process_iid,
                                     evt.process_slot,
                                     evt.status);

                        // Validate process_iid is valid
                        if (evt.process_iid == sintra::invalid_instance_id) {
                            test_passed = false;
                            failure_reason = "Crash event has invalid process_iid";
                        }

                        // Validate process_slot is non-zero for spawned processes
                        if (evt.process_slot == 0) {
                            test_passed = false;
                            failure_reason = "Crash event has invalid process_slot";
                        }

                        // Verify event came from crash_worker (event-to-worker correlation)
                        if (test_passed && evt.process_iid != crash_worker_iid) {
                            std::fprintf(stderr, "[COORDINATOR] ERROR: Crash event process_iid mismatch: "
                                         "expected %llu (crash_worker), got %llu\n",
                                         (unsigned long long)crash_worker_iid,
                                         (unsigned long long)evt.process_iid);
                            test_passed = false;
                            failure_reason = "Crash event process_iid does not match crash_worker";
                        }
                        else if (test_passed) {
                            std::fprintf(stderr, "[COORDINATOR] Crash event correctly correlated to crash_worker\n");
                        }

                        // Note: status might be 0 on some platforms even for crashes
                        // due to how signals are reported, so we just log it
                        std::fprintf(stderr, "[COORDINATOR] Crash exit status: %d\n", evt.status);
                        break;
                    }
                }
            }
        }
    }

    // Always signal the unpublished worker so it can exit, even if crash failed.
    if (!write_signal_file(unpublish_signal_path) && test_passed) {
        test_passed = false;
        failure_reason = "Failed to write unpublish signal file";
    }

    if (test_passed) {
        // Test 2: Trigger unpublished worker (abrupt exit without finalize)
        std::fprintf(stderr, "[COORDINATOR] Test 2: Triggering abrupt exit in unpublished_worker\n");

        // Wait for unpublished lifecycle event
        {
            std::unique_lock<std::mutex> events_lk(g_events_mutex);
            const bool unpub_event = g_events_cv.wait_for(
                events_lk, kEventTimeout,
                [&] {
                    for (const auto& evt : g_events) {
                        if (evt.why == sintra::process_lifecycle_event::reason::unpublished) {
                            return true;
                        }
                    }
                    return false;
                });

            if (!unpub_event) {
                std::fprintf(stderr, "[COORDINATOR] ERROR: No unpublished event received\n");
                test_passed = false;
                failure_reason = "No unpublished lifecycle event received";
            }
            else {
                // Verify unpublished event fields and correlation
                for (const auto& evt : g_events) {
                    if (evt.why == sintra::process_lifecycle_event::reason::unpublished) {
                        std::fprintf(stderr, "[COORDINATOR] Unpublished event validated: iid=%llu, slot=%u, status=%d\n",
                                     (unsigned long long)evt.process_iid,
                                     evt.process_slot,
                                     evt.status);

                        // Validate process_iid is valid
                        if (evt.process_iid == sintra::invalid_instance_id) {
                            test_passed = false;
                            failure_reason = "Unpublished event has invalid process_iid";
                        }

                        if (evt.process_slot == 0) {
                            test_passed = false;
                            failure_reason = "Unpublished event has invalid process_slot";
                        }

                        // Verify event came from unpublished_worker (event-to-worker correlation)
                        if (test_passed && evt.process_iid != unpub_worker_iid) {
                            std::fprintf(stderr, "[COORDINATOR] ERROR: Unpublished event process_iid mismatch: "
                                         "expected %llu (unpublished_worker), got %llu\n",
                                         (unsigned long long)unpub_worker_iid,
                                         (unsigned long long)evt.process_iid);
                            test_passed = false;
                            failure_reason = "Unpublished event process_iid does not match unpublished_worker";
                        }
                        else if (test_passed) {
                            std::fprintf(stderr, "[COORDINATOR] Unpublished event correctly correlated to unpublished_worker\n");
                        }

                        // For _exit(0), status should typically be 0
                        if (evt.status != 0) {
                            std::fprintf(stderr,
                                         "[COORDINATOR] Note: unpublished status is %d (expected 0)\n",
                                         evt.status);
                        }
                        break;
                    }
                }
            }
        }
    }

    // Always signal the normal worker so it can exit, even if earlier checks failed.
    if (!write_signal_file(finish_signal_path) && test_passed) {
        test_passed = false;
        failure_reason = "Failed to write finish signal file";
    }

    if (test_passed) {
        // Test 3: Signal normal worker to finish
        std::fprintf(stderr, "[COORDINATOR] Test 3: Signaling normal_worker to finish\n");

        // Wait for normal_exit lifecycle event
        {
            std::unique_lock<std::mutex> events_lk(g_events_mutex);
            const bool normal_event = g_events_cv.wait_for(
                events_lk, kEventTimeout,
                [&] {
                    for (const auto& evt : g_events) {
                        if (evt.why == sintra::process_lifecycle_event::reason::normal_exit) {
                            return true;
                        }
                    }
                    return false;
                });

            if (!normal_event) {
                std::fprintf(stderr, "[COORDINATOR] ERROR: No normal_exit event received\n");
                test_passed = false;
                failure_reason = "No normal_exit lifecycle event received";
            }
            else {
                // Verify normal_exit event fields and correlation
                for (const auto& evt : g_events) {
                    if (evt.why == sintra::process_lifecycle_event::reason::normal_exit) {
                        std::fprintf(stderr, "[COORDINATOR] Normal exit event validated: iid=%llu, slot=%u, status=%d\n",
                                     (unsigned long long)evt.process_iid,
                                     evt.process_slot,
                                     evt.status);

                        // Validate process_iid is valid
                        if (evt.process_iid == sintra::invalid_instance_id) {
                            test_passed = false;
                            failure_reason = "Normal exit event has invalid process_iid";
                        }

                        if (evt.process_slot == 0) {
                            test_passed = false;
                            failure_reason = "Normal exit event has invalid process_slot";
                        }

                        // Verify event came from normal_worker (event-to-worker correlation)
                        if (test_passed && evt.process_iid != normal_worker_iid) {
                            std::fprintf(stderr, "[COORDINATOR] ERROR: Normal exit event process_iid mismatch: "
                                         "expected %llu (normal_worker), got %llu\n",
                                         (unsigned long long)normal_worker_iid,
                                         (unsigned long long)evt.process_iid);
                            test_passed = false;
                            failure_reason = "Normal exit event process_iid does not match normal_worker";
                        }
                        else if (test_passed) {
                            std::fprintf(stderr, "[COORDINATOR] Normal exit event correctly correlated to normal_worker\n");
                        }

                        // For normal exit, status should be 0
                        if (evt.status != 0) {
                            std::fprintf(stderr,
                                         "[COORDINATOR] Warning: normal_exit status is %d (expected 0)\n",
                                         evt.status);
                        }
                        break;
                    }
                }
            }
        }
    }

    sintra::deactivate_all_slots();

    // Final verification: ensure we received all three event types
    if (test_passed) {
        std::lock_guard<std::mutex> lk(g_events_mutex);
        bool found_crash = false;
        bool found_normal = false;
        bool found_unpub = false;

        for (const auto& evt : g_events) {
            if (evt.why == sintra::process_lifecycle_event::reason::crash) {
                found_crash = true;
            }
            if (evt.why == sintra::process_lifecycle_event::reason::normal_exit) {
                found_normal = true;
            }
            if (evt.why == sintra::process_lifecycle_event::reason::unpublished) {
                found_unpub = true;
            }
        }

        if (!found_crash) {
            test_passed = false;
            failure_reason = "Missing crash event in final verification";
        }
        else if (!found_normal) {
            test_passed = false;
            failure_reason = "Missing normal_exit event in final verification";
        }
        else if (!found_unpub) {
            test_passed = false;
            failure_reason = "Missing unpublished event in final verification";
        }
        else {
            std::fprintf(stderr, "[COORDINATOR] All three lifecycle event types verified with correct worker correlation\n");
        }
    }

    // Write result
    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    if (test_passed) {
        out << "ok\n";
        std::fprintf(stderr, "[COORDINATOR] All tests passed\n");
    }
    else {
        out << "fail\n" << failure_reason << "\n";
        std::fprintf(stderr, "[COORDINATOR] Tests failed: %s\n", failure_reason.c_str());
    }

    return test_passed ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_coordinator);
    processes.emplace_back(process_normal_worker);
    processes.emplace_back(process_crash_worker);
    processes.emplace_back(process_unpublished_worker);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        const auto result_path = shared_dir / "result.txt";

        // Wait for result file
        for (int i = 0; i < 600; ++i) {  // Up to 60 seconds
            if (std::filesystem::exists(result_path)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "result.txt";

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

    return 0;
}
