// Slice 5 regression: a process already inside collective shutdown must be
// ignored by user processing-fence barriers, including their
// `_sintra_processing_phase/<user>` second phase, while still participating in
// the internal `_sintra_processing_phase/_sintra_shutdown` shutdown fence.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr char k_shared_dir_env[]    = "SINTRA_SHUTDOWN_USER_BARRIER_DIR";
constexpr char k_user_processing_fence_name[] = "after-early-shutdown";
constexpr auto k_marker_timeout      = 10s;
constexpr auto k_user_processing_fence_timeout = 8s;
constexpr auto k_overall_timeout     = 30s;

std::filesystem::path shared_dir()
{
    const char* value = std::getenv(k_shared_dir_env);
    if (!value || !*value) {
        throw std::runtime_error("shared directory environment variable is not set");
    }
    return std::filesystem::path(value);
}

std::filesystem::path early_shutdown_marker()
{
    return shared_dir() / "early-worker-entered-shutdown.txt";
}

std::filesystem::path early_shutdown_done_marker()
{
    return shared_dir() / "early-worker-shutdown-done.txt";
}

std::filesystem::path worker_entered_marker(int worker_id)
{
    return shared_dir() / ("barrier-worker-" + std::to_string(worker_id) + ".entered");
}

std::filesystem::path worker_done_marker(int worker_id)
{
    return shared_dir() / ("barrier-worker-" + std::to_string(worker_id) + ".done");
}

std::filesystem::path worker_shutdown_started_marker(int worker_id)
{
    return shared_dir() / ("shutdown-worker-" + std::to_string(worker_id) + ".started");
}

std::filesystem::path worker_shutdown_done_marker(int worker_id)
{
    return shared_dir() / ("shutdown-worker-" + std::to_string(worker_id) + ".done");
}

std::filesystem::path failure_log()
{
    return shared_dir() / "failures.txt";
}

void write_marker(const std::filesystem::path& path, const std::string& value)
{
    sintra::test::write_lines(path, {value});
}

std::string read_first_line(const std::filesystem::path& path)
{
    const auto lines = sintra::test::read_lines(path);
    return lines.empty() ? std::string() : lines.front();
}

void note_failure(const std::string& message)
{
    sintra::test::append_line_best_effort(failure_log(), message);
}

void verify_admission_closed_blocks_user_barrier()
{
    const auto prior_state =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    const auto prior_admission_closed =
        sintra::detail::s_teardown_admission_closed.load(std::memory_order_acquire);

    sintra::detail::s_shutdown_state.store(
        sintra::detail::shutdown_protocol_state::idle,
        std::memory_order_release);
    sintra::detail::s_teardown_admission_closed.store(true, std::memory_order_release);

    bool rejected = false;
    try {
        sintra::detail::validate_no_barrier_during_shutdown("_sintra_all_processes");
    }
    catch (const std::logic_error&) {
        rejected = true;
    }

    sintra::detail::s_teardown_admission_closed.store(
        prior_admission_closed,
        std::memory_order_release);
    sintra::detail::s_shutdown_state.store(prior_state, std::memory_order_release);

    if (!rejected) {
        throw std::runtime_error(
            "admission-closed lifecycle teardown did not reject all-processes user barrier");
    }
}

[[noreturn]] void watchdog_exit(const std::string& message)
{
    note_failure(message);
    std::fprintf(stderr, "shutdown_user_barrier_departure_test: %s\n", message.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

int early_shutdown_worker()
{
    return 0;
}

int barrier_worker(int worker_id)
{
    if (!sintra::test::wait_for_file(early_shutdown_marker(), k_marker_timeout, 10ms)) {
        note_failure(
            "worker " + std::to_string(worker_id) +
            " timed out waiting for the early shutdown-entry marker");
        return 1;
    }
    if (read_first_line(early_shutdown_marker()) != "entered") {
        note_failure(
            "worker " + std::to_string(worker_id) +
            " saw an invalid early shutdown-entry marker");
        return 1;
    }

    write_marker(worker_entered_marker(worker_id), "entered");

    std::atomic<bool> barrier_done{false};
    std::thread watchdog([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + k_user_processing_fence_timeout;
        while (!barrier_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit(
                    "worker " + std::to_string(worker_id) +
                    " blocked in user processing fence after the early worker entered shutdown");
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    try {
        // This creates `_sintra_processing_phase/after-early-shutdown`; the
        // classifier must strip that wrapper before deciding user-vs-internal.
        const auto sequence =
            sintra::barrier<sintra::processing_fence_t>(k_user_processing_fence_name);
        barrier_done.store(true, std::memory_order_release);
        watchdog.join();

        if (sequence == sintra::invalid_sequence || sequence == 0) {
            note_failure(
                "worker " + std::to_string(worker_id) +
                " got an invalid user processing-fence sequence");
            return 1;
        }

        write_marker(worker_done_marker(worker_id), "done");
        return 0;
    }
    catch (const std::exception& e) {
        barrier_done.store(true, std::memory_order_release);
        watchdog.join();
        note_failure(
            "worker " + std::to_string(worker_id) +
            " user processing fence threw: " + e.what());
        return 1;
    }
    catch (...) {
        barrier_done.store(true, std::memory_order_release);
        watchdog.join();
        note_failure(
            "worker " + std::to_string(worker_id) +
            " user processing fence threw an unknown exception");
        return 1;
    }
}

int barrier_worker_1()
{
    return barrier_worker(1);
}

int barrier_worker_2()
{
    return barrier_worker(2);
}

bool wait_for_barrier_observation()
{
    bool ok = true;
    if (!sintra::test::wait_for_file(early_shutdown_marker(), k_marker_timeout, 10ms)) {
        note_failure("coordinator did not observe the early shutdown-entry marker");
        ok = false;
    }
    else
    if (read_first_line(early_shutdown_marker()) != "entered") {
        note_failure("coordinator observed an invalid early shutdown-entry marker");
        ok = false;
    }
    if (!sintra::test::wait_for_file(worker_entered_marker(1), k_marker_timeout, 10ms)) {
        note_failure("coordinator did not observe worker 1 entering the user processing fence");
        ok = false;
    }
    if (!sintra::test::wait_for_file(worker_entered_marker(2), k_marker_timeout, 10ms)) {
        note_failure("coordinator did not observe worker 2 entering the user processing fence");
        ok = false;
    }
    if (!ok) {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + k_user_processing_fence_timeout + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!sintra::test::read_lines(failure_log()).empty()) {
            return true;
        }
        if (std::filesystem::exists(worker_done_marker(1)) &&
            std::filesystem::exists(worker_done_marker(2)))
        {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }

    note_failure(
        "both workers entered the user processing fence after the early shutdown-entry marker, "
        "but neither completion nor watchdog failure was observed");
    return true;
}

int verify_results(bool shutdown_ok)
{
    bool ok = shutdown_ok;

    const auto failures = sintra::test::read_lines(failure_log());
    for (const auto& failure : failures) {
        std::fprintf(stderr, "shutdown_user_barrier_departure_test: %s\n", failure.c_str());
    }
    if (!failures.empty()) {
        ok = false;
    }

    if (!std::filesystem::exists(worker_done_marker(1))) {
        std::fprintf(stderr,
            "shutdown_user_barrier_departure_test: worker 1 did not complete the user processing fence\n");
        ok = false;
    }
    if (!std::filesystem::exists(worker_done_marker(2))) {
        std::fprintf(stderr,
            "shutdown_user_barrier_departure_test: worker 2 did not complete the user processing fence\n");
        ok = false;
    }

    for (int worker_id : {1, 2}) {
        if (!std::filesystem::exists(worker_shutdown_started_marker(worker_id))) {
            std::fprintf(stderr,
                "shutdown_user_barrier_departure_test: worker %d did not enter shutdown after the user processing fence\n",
                worker_id);
            ok = false;
        }

        const auto worker_done = worker_shutdown_done_marker(worker_id);
        if (shutdown_ok) {
            (void)sintra::test::wait_for_file(worker_done, k_marker_timeout, 10ms);
        }
        if (!std::filesystem::exists(worker_done)) {
            std::fprintf(stderr,
                "shutdown_user_barrier_departure_test: worker %d did not complete shutdown\n",
                worker_id);
            ok = false;
        }
        else
        if (read_first_line(worker_done) != "done") {
            std::fprintf(stderr,
                "shutdown_user_barrier_departure_test: worker %d shutdown failed\n",
                worker_id);
            ok = false;
        }
    }

    const auto early_done = early_shutdown_done_marker();
    if (shutdown_ok) {
        (void)sintra::test::wait_for_file(early_done, k_marker_timeout, 10ms);
    }
    if (!std::filesystem::exists(early_done)) {
        std::fprintf(stderr,
            "shutdown_user_barrier_departure_test: early worker did not complete shutdown\n");
        ok = false;
    }
    else {
        const auto result = read_first_line(early_done);
        if (result != "done") {
            std::fprintf(stderr,
                "shutdown_user_barrier_departure_test: early worker shutdown result was '%s'\n",
                result.c_str());
            ok = false;
        }
    }

    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    verify_admission_closed_blocks_user_barrier();

    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    const std::string branch_index =
        sintra::test::get_argv_value(argc, argv, "--branch_index");
    sintra::test::Shared_directory shared(k_shared_dir_env, "shutdown_user_barrier_departure");

    std::atomic<bool> finished{false};
    std::thread overall_watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + k_overall_timeout;
        while (!finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit("overall timeout");
            }
            std::this_thread::sleep_for(50ms);
        }
    });

    auto finish = [&](int result) {
        finished.store(true, std::memory_order_release);
        overall_watchdog.join();
        return result;
    };

    sintra::init(argc, argv, {early_shutdown_worker, barrier_worker_1, barrier_worker_2});

    if (is_spawned) {
        if (branch_index == "1") {
            std::atomic<bool> entered_shutdown{false};
            std::thread marker_thread([&] {
                const auto deadline = std::chrono::steady_clock::now() + k_marker_timeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    // Test-only observation of existing lifecycle state; this
                    // keeps the oracle out of production hooks or synthetic RPCs.
                    const auto state =
                        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
                    if (state ==
                        sintra::detail::shutdown_protocol_state::collective_shutdown_entered)
                    {
                        write_marker(early_shutdown_marker(), "entered");
                        entered_shutdown.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::sleep_for(5ms);
                }

                note_failure("early worker did not enter collective shutdown state");
            });

            const bool shutdown_ok = sintra::shutdown();
            marker_thread.join();
            if (!entered_shutdown.load(std::memory_order_acquire)) {
                return finish(1);
            }
            write_marker(early_shutdown_done_marker(), shutdown_ok ? "done" : "failed");
            return finish(shutdown_ok ? 0 : 1);
        }

        if (branch_index == "2" || branch_index == "3") {
            const int worker_id = (branch_index == "2") ? 1 : 2;
            write_marker(worker_shutdown_started_marker(worker_id), "started");
            const bool shutdown_ok = sintra::shutdown();
            write_marker(worker_shutdown_done_marker(worker_id), shutdown_ok ? "done" : "failed");
            return finish(shutdown_ok ? 0 : 1);
        }

        note_failure("unexpected branch_index: " + branch_index);
        return finish(1);
    }

    const bool observed = wait_for_barrier_observation();
    if (!observed || !sintra::test::read_lines(failure_log()).empty()) {
        (void)verify_results(false);
        finished.store(true, std::memory_order_release);
        overall_watchdog.join();
        std::fflush(stderr);
        std::_Exit(1);
    }

    const bool shutdown_ok = sintra::shutdown();
    return finish(verify_results(shutdown_ok));
}
