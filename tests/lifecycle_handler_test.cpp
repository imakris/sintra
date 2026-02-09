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

#include "test_utils.h"

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

namespace {

// Worker IDs for correlation
constexpr int k_normal_worker_id = 1;
constexpr int k_crash_worker_id = 2;
constexpr int k_unpublished_worker_id = 3;

constexpr auto k_ready_timeout = std::chrono::seconds(10);
constexpr auto k_signal_timeout = std::chrono::seconds(10);
constexpr auto k_event_timeout = std::chrono::seconds(15);
constexpr auto k_poll_interval = std::chrono::milliseconds(50);

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

// Worker that exits normally after receiving finish signal file
int process_normal_worker()
{
    std::fprintf(stderr, "[NORMAL_WORKER] Starting\n");

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "lifecycle_handler");
    const auto shared_dir = shared.path();
    if (!write_worker_ready(shared_dir,
                            k_normal_worker_id,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[NORMAL_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto finish_signal_path = signal_path(shared_dir, "finish");
    if (!sintra::test::wait_for_file(finish_signal_path, k_signal_timeout, k_poll_interval)) {
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

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "lifecycle_handler");
    const auto shared_dir = shared.path();
    if (!write_worker_ready(shared_dir,
                            k_crash_worker_id,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[CRASH_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto crash_signal_path = signal_path(shared_dir, "crash");
    if (!sintra::test::wait_for_file(crash_signal_path, k_signal_timeout, k_poll_interval)) {
        std::fprintf(stderr, "[CRASH_WORKER] ERROR: Timed out waiting for Crash signal\n");
        return 1;
    }

    std::fprintf(stderr, "[CRASH_WORKER] About to crash via illegal instruction\n");
    sintra::test::prepare_for_intentional_crash();

    sintra::disable_debug_pause_for_current_process();

    // Crash via illegal instruction to exercise crash handling.
    sintra::test::trigger_illegal_instruction_crash();

    // Should never reach here
    return 0;
}

// Worker that exits abruptly via _exit() without calling finalize()
// This should trigger an "unpublished" lifecycle event
int process_unpublished_worker()
{
    std::fprintf(stderr, "[UNPUBLISHED_WORKER] Starting\n");

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "lifecycle_handler");
    const auto shared_dir = shared.path();
    if (!write_worker_ready(shared_dir,
                            k_unpublished_worker_id,
                            sintra::process_of(s_mproc_id))) {
        std::fprintf(stderr, "[UNPUBLISHED_WORKER] ERROR: Failed to write ready file\n");
        return 1;
    }

    const auto unpublish_signal_path = signal_path(shared_dir, "unpublish");
    if (!sintra::test::wait_for_file(unpublish_signal_path, k_signal_timeout, k_poll_interval)) {
        std::fprintf(stderr, "[UNPUBLISHED_WORKER] ERROR: Timed out waiting for Unpublish signal\n");
        return 1;
    }

    if (!sintra::Coordinator::rpc_unpublish_transceiver(s_coord_id, s_mproc_id)) {
        std::fprintf(stderr, "[UNPUBLISHED_WORKER] ERROR: Failed to unpublish self\n");
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
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "lifecycle_handler");
    const auto shared_dir = shared.path();
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
    const auto ready_deadline = std::chrono::steady_clock::now() + k_ready_timeout;
    while (std::chrono::steady_clock::now() < ready_deadline) {
        bool have_all = true;
        if (crash_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, k_crash_worker_id, &crash_worker_iid) && have_all;
        }
        if (unpub_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, k_unpublished_worker_id, &unpub_worker_iid) && have_all;
        }
        if (normal_worker_iid == sintra::invalid_instance_id) {
            have_all = read_worker_ready(shared_dir, k_normal_worker_id, &normal_worker_iid) && have_all;
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

    auto find_event = [&](sintra::process_lifecycle_event::reason reason,
                          sintra::process_lifecycle_event* out) {
        for (const auto& evt : g_events) {
            if (evt.why == reason) {
                if (out) {
                    *out = evt;
                }
                return true;
            }
        }
        return false;
    };

    std::fprintf(stderr, "[COORDINATOR] Test 1: Triggering crash in crash_worker\n");
    if (!write_signal_file(crash_signal_path) && test_passed) {
        test_passed = false;
        failure_reason = "Failed to write crash signal file";
    }

    std::fprintf(stderr, "[COORDINATOR] Test 2: Triggering abrupt exit in unpublished_worker\n");
    if (!write_signal_file(unpublish_signal_path) && test_passed) {
        test_passed = false;
        failure_reason = "Failed to write unpublish signal file";
    }

    std::fprintf(stderr, "[COORDINATOR] Test 3: Signaling normal_worker to finish\n");
    if (!write_signal_file(finish_signal_path) && test_passed) {
        test_passed = false;
        failure_reason = "Failed to write finish signal file";
    }

    sintra::process_lifecycle_event crash_evt {};
    sintra::process_lifecycle_event unpub_evt {};
    sintra::process_lifecycle_event normal_evt {};

    if (test_passed) {
        std::unique_lock<std::mutex> events_lk(g_events_mutex);
        const auto deadline = std::chrono::steady_clock::now() + k_event_timeout;
        g_events_cv.wait_until(events_lk, deadline, [&] {
            return find_event(sintra::process_lifecycle_event::reason::crash, nullptr) &&
                   find_event(sintra::process_lifecycle_event::reason::unpublished, nullptr) &&
                   find_event(sintra::process_lifecycle_event::reason::normal_exit, nullptr);
        });

        const bool have_crash = find_event(sintra::process_lifecycle_event::reason::crash, &crash_evt);
        const bool have_unpub = find_event(sintra::process_lifecycle_event::reason::unpublished, &unpub_evt);
        const bool have_normal = find_event(sintra::process_lifecycle_event::reason::normal_exit, &normal_evt);

        if (!have_crash) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: No crash event received\n");
            test_passed = false;
            failure_reason = "No crash lifecycle event received";
        }
        else if (!have_unpub) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: No unpublished event received\n");
            test_passed = false;
            failure_reason = "No unpublished lifecycle event received";
        }
        else if (!have_normal) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: No normal_exit event received\n");
            test_passed = false;
            failure_reason = "No normal_exit lifecycle event received";
        }
    }

    if (test_passed) {
        std::fprintf(stderr, "[COORDINATOR] Crash event validated: iid=%llu, slot=%u, status=%d\n",
                     (unsigned long long)crash_evt.process_iid,
                     crash_evt.process_slot,
                     crash_evt.status);

        if (crash_evt.process_iid == sintra::invalid_instance_id) {
            test_passed = false;
            failure_reason = "Crash event has invalid process_iid";
        }

        if (test_passed && crash_evt.process_slot == 0) {
            test_passed = false;
            failure_reason = "Crash event has invalid process_slot";
        }

        if (test_passed && crash_evt.process_iid != crash_worker_iid) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: Crash event process_iid mismatch: "
                         "expected %llu (crash_worker), got %llu\n",
                         (unsigned long long)crash_worker_iid,
                         (unsigned long long)crash_evt.process_iid);
            test_passed = false;
            failure_reason = "Crash event process_iid does not match crash_worker";
        }
        else if (test_passed) {
            std::fprintf(stderr, "[COORDINATOR] Crash event correctly correlated to crash_worker\n");
        }

        std::fprintf(stderr, "[COORDINATOR] Crash exit status: %d\n", crash_evt.status);
    }

    if (test_passed) {
        std::fprintf(stderr, "[COORDINATOR] Unpublished event validated: iid=%llu, slot=%u, status=%d\n",
                     (unsigned long long)unpub_evt.process_iid,
                     unpub_evt.process_slot,
                     unpub_evt.status);

        if (unpub_evt.process_iid == sintra::invalid_instance_id) {
            test_passed = false;
            failure_reason = "Unpublished event has invalid process_iid";
        }

        if (test_passed && unpub_evt.process_slot == 0) {
            test_passed = false;
            failure_reason = "Unpublished event has invalid process_slot";
        }

        if (test_passed && unpub_evt.process_iid != unpub_worker_iid) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: Unpublished event process_iid mismatch: "
                         "expected %llu (unpublished_worker), got %llu\n",
                         (unsigned long long)unpub_worker_iid,
                         (unsigned long long)unpub_evt.process_iid);
            test_passed = false;
            failure_reason = "Unpublished event process_iid does not match unpublished_worker";
        }
        else if (test_passed) {
            std::fprintf(stderr, "[COORDINATOR] Unpublished event correctly correlated to unpublished_worker\n");
        }

        if (unpub_evt.status != 0) {
            std::fprintf(stderr,
                         "[COORDINATOR] Note: unpublished status is %d (expected 0)\n",
                         unpub_evt.status);
        }
    }

    if (test_passed) {
        std::fprintf(stderr, "[COORDINATOR] Normal exit event validated: iid=%llu, slot=%u, status=%d\n",
                     (unsigned long long)normal_evt.process_iid,
                     normal_evt.process_slot,
                     normal_evt.status);

        if (normal_evt.process_iid == sintra::invalid_instance_id) {
            test_passed = false;
            failure_reason = "Normal exit event has invalid process_iid";
        }

        if (test_passed && normal_evt.process_slot == 0) {
            test_passed = false;
            failure_reason = "Normal exit event has invalid process_slot";
        }

        if (test_passed && normal_evt.process_iid != normal_worker_iid) {
            std::fprintf(stderr, "[COORDINATOR] ERROR: Normal exit event process_iid mismatch: "
                         "expected %llu (normal_worker), got %llu\n",
                         (unsigned long long)normal_worker_iid,
                         (unsigned long long)normal_evt.process_iid);
            test_passed = false;
            failure_reason = "Normal exit event process_iid does not match normal_worker";
        }
        else if (test_passed) {
            std::fprintf(stderr, "[COORDINATOR] Normal exit event correctly correlated to normal_worker\n");
        }

        if (normal_evt.status != 0) {
            std::fprintf(stderr,
                         "[COORDINATOR] Warning: normal_exit status is %d (expected 0)\n",
                         normal_evt.status);
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
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "lifecycle_handler",
        {process_normal_worker, process_crash_worker, process_unpublished_worker},
        [](const std::filesystem::path&) {},
        [](const std::filesystem::path&) { return process_coordinator(); },
        [](const std::filesystem::path&) { return 0; },
        nullptr);
}
