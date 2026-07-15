//
// Exact-occurrence managed-child native-exit observation contract evidence.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

using namespace std::chrono_literals;

constexpr const char* k_child_flag =
    "--managed_child_exact_exit_observation_child";
constexpr const char* k_pid_file =
    "managed_child_exact_exit_observation_pid.txt";
constexpr int k_exit_code = 77;
constexpr sintra::instance_id_type k_child_process_iid =
    sintra::compose_instance(31u, 1ull);
constexpr sintra::instance_id_type k_missing_process_iid =
    sintra::compose_instance(32u, 1ull);

struct Callback_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    started = false;
    bool                    release = false;
};

bool write_pid(const fs::path& path, int pid)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << pid << '\n';
    return static_cast<bool>(out);
}

int read_pid(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    int pid = -1;
    in >> pid;
    return pid;
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
    const bool terminated = TerminateProcess(process, k_exit_code) != 0;
    if (terminated) {
        (void)WaitForSingleObject(process, 5000);
    }
    CloseHandle(process);
    return terminated;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

bool expected_exit(const sintra::Managed_child_exit& event)
{
    if (!event.native_status_available) {
        return false;
    }
#ifdef _WIN32
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::exited &&
        event.status == k_exit_code &&
        event.native_status == k_exit_code;
#else
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::signaled &&
        event.status == SIGKILL &&
        WIFSIGNALED(event.native_status) &&
        WTERMSIG(event.native_status) == SIGKILL;
#endif
}

int run_child(
    int argc,
    char* argv[],
    const fs::path& pid_path)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    if (!write_pid(pid_path, sintra::test::get_pid())) {
        return 2;
    }
    std::this_thread::sleep_for(30s);
    return 2;
}

int run_root(
    int argc,
    char* argv[],
    const fs::path& pid_path,
    const fs::path& shared_path)
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

    std::atomic_int no_occurrence_callback_count{0};
    sintra::Spawn_options missing_options;
    missing_options.binary_path =
        (shared_path / "missing-managed-child.exe").string();
    missing_options.process_instance_id = k_missing_process_iid;
    missing_options.lifetime.enable_lifeline = false;
    auto missing_custody = sintra::spawn_swarm_process(missing_options);
    auto missing_observation = missing_custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            no_occurrence_callback_count.fetch_add(1, std::memory_order_release);
        });
    const auto missing_status = missing_custody.status();
    const auto missing_released = missing_custody.release_until(
        std::chrono::steady_clock::now() + 5s);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_child_flag};
    options.process_instance_id = k_child_process_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const std::thread::id registering_thread = std::this_thread::get_id();
    std::mutex event_mutex;
    std::condition_variable event_changed;
    sintra::Managed_child_exit observed_event;
    std::thread::id observed_thread;
    int observed_count = 0;
    auto observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            std::lock_guard<std::mutex> lock(event_mutex);
            observed_event = event;
            observed_thread = std::this_thread::get_id();
            ++observed_count;
            event_changed.notify_all();
        });

    std::atomic_int cancelled_count{0};
    auto cancelled_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            cancelled_count.fetch_add(1, std::memory_order_release);
        });
    cancelled_observation.subscription.unsubscribe();

    std::atomic_int self_unsubscribe_count{0};
    sintra::Managed_child_exit_observation self_observation;
    self_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            self_unsubscribe_count.fetch_add(1, std::memory_order_release);
            self_observation.subscription.unsubscribe();
        });

    std::atomic_int throwing_count{0};
    auto throwing_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            throwing_count.fetch_add(1, std::memory_order_release);
            throw std::runtime_error("intentional exit callback failure");
        });

    Callback_gate callback_gate;
    auto blocking_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            std::unique_lock<std::mutex> lock(callback_gate.mutex);
            callback_gate.started = true;
            callback_gate.changed.notify_all();
            callback_gate.changed.wait(lock, [&]() {
                return callback_gate.release;
            });
            lock.unlock();
            std::this_thread::sleep_for(100ms);
        });

    const bool observation_registered = static_cast<bool>(observation);
    const bool self_observation_registered = static_cast<bool>(self_observation);
    const bool throwing_observation_registered =
        static_cast<bool>(throwing_observation);

    const bool pid_seen = sintra::test::wait_for_file(
        pid_path,
        5s,
        10ms);
    const int pid = pid_seen ? read_pid(pid_path) : -1;
    const bool terminated = pid > 0 && terminate_child(pid);

    bool callback_started = false;
    {
        std::unique_lock<std::mutex> lock(callback_gate.mutex);
        callback_started = callback_gate.changed.wait_for(
            lock,
            5s,
            [&]() { return callback_gate.started; });
    }

    std::atomic_bool unsubscribe_returned{false};
    std::chrono::steady_clock::duration unsubscribe_duration{};
    std::thread unsubscribe_thread([&]() {
        const auto begin = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(callback_gate.mutex);
            callback_gate.release = true;
        }
        callback_gate.changed.notify_all();
        blocking_observation.subscription.unsubscribe();
        unsubscribe_duration = std::chrono::steady_clock::now() - begin;
        unsubscribe_returned.store(true, std::memory_order_release);
    });
    unsubscribe_thread.join();
    const bool unsubscribe_waited = unsubscribe_duration >= 50ms;

    bool callback_observed = false;
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        callback_observed = event_changed.wait_for(
            lock,
            5s,
            [&]() { return observed_count == 1; });
    }

    std::mutex replay_mutex;
    std::condition_variable replay_changed;
    sintra::Managed_child_exit replay_event;
    std::thread::id replay_thread;
    int replay_count = 0;
    auto replay_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            std::lock_guard<std::mutex> lock(replay_mutex);
            replay_event = event;
            replay_thread = std::this_thread::get_id();
            ++replay_count;
            replay_changed.notify_all();
        });
    const bool replay_observation_registered =
        static_cast<bool>(replay_observation);
    bool replay_observed = false;
    {
        std::unique_lock<std::mutex> lock(replay_mutex);
        replay_observed = replay_changed.wait_for(
            lock,
            5s,
            [&]() { return replay_count == 1; });
    }

    observation.subscription.unsubscribe();
    throwing_observation.subscription.unsubscribe();
    replay_observation.subscription.unsubscribe();

    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    bool finalized = false;
    try {
        finalized = sintra::shutdown();
    }
    catch (...) {
    }

    const bool identity_valid = observation_registered &&
        observation.occurrence.process_instance_id == k_child_process_iid &&
        observed_event.occurrence == observation.occurrence &&
        replay_observation.occurrence == observation.occurrence &&
        replay_event.occurrence == observation.occurrence;
    const bool valid = missing_custody && !missing_observation &&
        missing_status.created_occurrences == 0 &&
        missing_released.release_state ==
            sintra::Managed_child_release_state::complete &&
        no_occurrence_callback_count.load(std::memory_order_acquire) == 0 &&
        custody && observation_registered && self_observation_registered &&
        throwing_observation_registered &&
        callback_started && unsubscribe_waited &&
        unsubscribe_returned.load(std::memory_order_acquire) &&
        pid_seen && terminated && callback_observed && observed_count == 1 &&
        expected_exit(observed_event) && identity_valid &&
        observed_thread != registering_thread &&
        cancelled_count.load(std::memory_order_acquire) == 0 &&
        self_unsubscribe_count.load(std::memory_order_acquire) == 1 &&
        throwing_count.load(std::memory_order_acquire) == 1 &&
        replay_observation_registered && replay_observed && replay_count == 1 &&
        expected_exit(replay_event) && replay_thread != registering_thread &&
        released.release_state ==
            sintra::Managed_child_release_state::complete &&
        finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "MANAGED_CHILD_EXACT_EXIT_OBSERVATION_INVALID missing=%d "
            "missing_observation=%d missing_created=%zu missing_released=%d "
            "no_occurrence_count=%d custody=%d observation=%d identity=%d "
            "pid_seen=%d terminated=%d callback_started=%d waited=%d "
            "callback=%d observed_count=%d expected_exit=%d off_thread=%d "
            "cancelled=%d self=%d throwing=%d replay=%d replay_count=%d "
            "replay_exit=%d replay_off_thread=%d released=%d finalized=%d\n",
            missing_custody ? 1 : 0,
            missing_observation ? 1 : 0,
            missing_status.created_occurrences,
            missing_released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            no_occurrence_callback_count.load(std::memory_order_acquire),
            custody ? 1 : 0,
            observation_registered ? 1 : 0,
            identity_valid ? 1 : 0,
            pid_seen ? 1 : 0,
            terminated ? 1 : 0,
            callback_started ? 1 : 0,
            unsubscribe_waited ? 1 : 0,
            callback_observed ? 1 : 0,
            observed_count,
            expected_exit(observed_event) ? 1 : 0,
            observed_thread != registering_thread ? 1 : 0,
            cancelled_count.load(std::memory_order_acquire),
            self_unsubscribe_count.load(std::memory_order_acquire),
            throwing_count.load(std::memory_order_acquire),
            replay_observed ? 1 : 0,
            replay_count,
            expected_exit(replay_event) ? 1 : 0,
            replay_thread != registering_thread ? 1 : 0,
            released.release_state ==
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
        "managed_child_exact_exit_observation_contract_test");
    const fs::path pid_path = shared.path() / k_pid_file;
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, pid_path);
    }
    return run_root(argc, argv, pid_path, shared.path());
}
