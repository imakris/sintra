//
// Exact managed-child identities remain unique when a process instance id is
// reused by a later custody in the same runtime.
//

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

using namespace std::chrono_literals;

constexpr const char* k_child_a_flag =
    "--managed_child_occurrence_identity_a";
constexpr const char* k_child_b_flag =
    "--managed_child_occurrence_identity_b";
constexpr const char* k_child_a_file =
    "managed_child_occurrence_identity_a.txt";
constexpr const char* k_child_b_file =
    "managed_child_occurrence_identity_b.txt";
constexpr const char* k_child_b_recovery_file =
    "managed_child_occurrence_identity_b_recovery.txt";
constexpr const char* k_child_b_crash_file =
    "managed_child_occurrence_identity_b_crash.txt";
constexpr std::uint32_t k_terminated_status = 0xc0000005u;
constexpr sintra::instance_id_type k_reused_process_iid =
    sintra::compose_instance(34u, 1ull);

struct Child_marker
{
    std::uint32_t occurrence = 0;
    int           pid = -1;
};

struct Exit_capture
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    sintra::Managed_child_exit event;
    int                        count = 0;
};

bool write_marker(const fs::path& path)
{
    return sintra::test::managed_child::write_complete_file(
        path,
        std::to_string(sintra::s_recovery_occurrence) + ' ' +
            std::to_string(sintra::test::get_pid()) + '\n');
}

Child_marker read_marker(const fs::path& path)
{
    Child_marker marker;
    std::ifstream input(path, std::ios::binary);
    input >> marker.occurrence >> marker.pid;
    return marker;
}

bool wait_for_exit(Exit_capture& capture)
{
    std::unique_lock<std::mutex> lock(capture.mutex);
    return capture.changed.wait_for(
        lock,
        5s,
        [&]() { return capture.count == 1; });
}

bool terminate_child(int pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }
    const bool terminated =
        TerminateProcess(process, k_terminated_status) != 0;
    if (terminated) {
        (void)WaitForSingleObject(process, 5000);
    }
    CloseHandle(process);
    return terminated;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

int run_child_a(int argc, char* argv[], const fs::path& shared_path)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }
    return write_marker(shared_path / k_child_a_file) ? 0 : 2;
}

int run_child_b(int argc, char* argv[], const fs::path& shared_path)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    const auto initial_path = shared_path / k_child_b_file;
    if (!fs::exists(initial_path)) {
        sintra::enable_recovery();
        if (!write_marker(initial_path) ||
            !sintra::test::wait_for_file(
                shared_path / k_child_b_crash_file, 30s, 10ms))
        {
            return 2;
        }
        sintra::disable_debug_pause_for_current_process();
        sintra::test::prepare_for_intentional_crash(
            "managed child occurrence identity reuse");
        std::abort();
    }

    if (!write_marker(shared_path / k_child_b_recovery_file)) {
        return 2;
    }
    std::this_thread::sleep_for(30s);
    return 2;
}

int run_root(int argc, char* argv[], const fs::path& shared_path)
{
    const std::string binary_path =
        sintra::test::get_binary_path(argc, argv);
    if (binary_path.empty()) {
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    std::atomic_int recovery_requests{0};
    sintra::set_recovery_policy([&](const sintra::Crash_info& crash) {
        return crash.process_iid == k_reused_process_iid &&
            recovery_requests.fetch_add(1, std::memory_order_acq_rel) == 0;
    });

    auto spawn = [&](const char* flag) {
        sintra::Spawn_options options;
        options.binary_path = binary_path;
        options.args = {flag};
        options.process_instance_id = k_reused_process_iid;
        options.lifetime.enable_lifeline = false;
        return sintra::spawn_swarm_process(options);
    };
    auto observe = [](const sintra::Managed_child_custody& custody,
                      Exit_capture& capture) {
        return custody.observe_latest_created_exit(
            [&](const sintra::Managed_child_exit& event) {
                std::lock_guard<std::mutex> lock(capture.mutex);
                capture.event = event;
                ++capture.count;
                capture.changed.notify_all();
            });
    };

    auto custody_a = spawn(k_child_a_flag);
    const bool a_marker_seen = sintra::test::wait_for_file(
        shared_path / k_child_a_file, 5s, 10ms);
    const auto marker_a = a_marker_seen
        ? read_marker(shared_path / k_child_a_file)
        : Child_marker{};
    Exit_capture capture_a;
    auto observation_a = observe(custody_a, capture_a);
    const bool a_exit_seen = wait_for_exit(capture_a);
    const auto released_a = custody_a.release_until(
        std::chrono::steady_clock::now() + 5s);

    auto custody_b = spawn(k_child_b_flag);
    const bool b_marker_seen = sintra::test::wait_for_file(
        shared_path / k_child_b_file, 5s, 10ms);
    const auto marker_b = b_marker_seen
        ? read_marker(shared_path / k_child_b_file)
        : Child_marker{};
    Exit_capture capture_b;
    auto observation_b = observe(custody_b, capture_b);
    const bool crash_requested = observation_b &&
        sintra::test::managed_child::write_complete_file(
            shared_path / k_child_b_crash_file, "1\n");
    const bool b_exit_seen = wait_for_exit(capture_b);

    const bool recovery_marker_seen = sintra::test::wait_for_file(
        shared_path / k_child_b_recovery_file, 10s, 10ms);
    const auto recovery_marker = recovery_marker_seen
        ? read_marker(shared_path / k_child_b_recovery_file)
        : Child_marker{};
    Exit_capture recovery_capture;
    auto recovery_observation = observe(custody_b, recovery_capture);
    const bool recovery_terminated = recovery_observation &&
        recovery_marker.pid > 0 && terminate_child(recovery_marker.pid);
    const bool recovery_exit_seen = wait_for_exit(recovery_capture);
    const auto released_b = custody_b.release_until(
        std::chrono::steady_clock::now() + 5s);

    const auto identity_a = observation_a.occurrence;
    const auto identity_b = observation_b.occurrence;
    const auto recovery_identity = recovery_observation.occurrence;
    const bool identities_monotonic = observation_a && observation_b &&
        recovery_observation &&
        identity_a.process_instance_id == k_reused_process_iid &&
        identity_b.process_instance_id == k_reused_process_iid &&
        recovery_identity.process_instance_id == k_reused_process_iid &&
        identity_a.occurrence == 0 &&
        identity_b.occurrence == identity_a.occurrence + 1 &&
        recovery_identity.occurrence == identity_b.occurrence + 1;
    const bool exact_delivery = a_exit_seen && b_exit_seen &&
        recovery_exit_seen && capture_a.count == 1 && capture_b.count == 1 &&
        recovery_capture.count == 1 && capture_a.event.occurrence == identity_a &&
        capture_b.event.occurrence == identity_b &&
        recovery_capture.event.occurrence == recovery_identity;
    const bool marker_identity_matches =
        marker_a.occurrence == identity_a.occurrence &&
        marker_b.occurrence == identity_b.occurrence &&
        recovery_marker.occurrence == recovery_identity.occurrence;

    observation_a.subscription.unsubscribe();
    observation_b.subscription.unsubscribe();
    recovery_observation.subscription.unsubscribe();
    const bool finalized = sintra::shutdown();

    const bool valid = custody_a && custody_b && a_marker_seen && b_marker_seen &&
        recovery_marker_seen && marker_a.pid > 0 && marker_b.pid > 0 &&
        recovery_marker.pid > 0 && marker_a.pid != marker_b.pid &&
        marker_b.pid != recovery_marker.pid && crash_requested &&
        recovery_terminated && identities_monotonic && marker_identity_matches &&
        exact_delivery && released_a.release_state ==
            sintra::Managed_child_release_state::complete &&
        released_b.release_state ==
            sintra::Managed_child_release_state::complete && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "MANAGED_CHILD_OCCURRENCE_IDENTITY_REUSE_INVALID "
            "custody_a=%d custody_b=%d marker_a=%d marker_b=%d recovery=%d "
            "occurrence_a=%u occurrence_b=%u occurrence_recovery=%u "
            "identities_equal=%d marker_match=%d exact=%d "
            "count_a=%d count_b=%d count_recovery=%d released_a=%d "
            "released_b=%d finalized=%d\n",
            custody_a ? 1 : 0,
            custody_b ? 1 : 0,
            a_marker_seen ? 1 : 0,
            b_marker_seen ? 1 : 0,
            recovery_marker_seen ? 1 : 0,
            identity_a.occurrence,
            identity_b.occurrence,
            recovery_identity.occurrence,
            identity_a == identity_b ? 1 : 0,
            marker_identity_matches ? 1 : 0,
            exact_delivery ? 1 : 0,
            capture_a.count,
            capture_b.count,
            recovery_capture.count,
            released_a.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            released_b.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid ? 0 : 1;
}

}

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "managed_child_occurrence_identity_reuse_contract_test");
    if (sintra::test::has_argv_flag(argc, argv, k_child_a_flag)) {
        return run_child_a(argc, argv, shared.path());
    }
    if (sintra::test::has_argv_flag(argc, argv, k_child_b_flag)) {
        return run_child_b(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared.path());
}
