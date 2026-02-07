//
// Sintra Barrier Flush Test
//
// Validates that repeated inter-process barrier synchronisation completes
// successfully while messages are flowing between processes. 
// Two worker processes send iteration markers to a coordinator process and
// immediately wait on a barrier.  The coordinator waits until it has received
// the expected number of markers for the current iteration, joins the barrier
// and repeats.  After all iterations complete the coordinator writes the
// result to a shared directory that the parent process verifies.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t k_worker_count         = 2;
constexpr std::size_t k_iterations          = 128;

struct Iteration_marker
{
    std::uint32_t worker;
    std::uint32_t iteration;
};

struct Coordinator_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::uint32_t, k_worker_count> next_expected_iteration{};
    std::size_t messages_in_iteration = 0;
    std::size_t total_messages        = 0;
    bool iteration_failed             = false;
    bool abort_requested              = false;
    std::size_t current_iteration     = 0;
};


void write_result(const std::filesystem::path& dir,
                  bool success,
                  std::size_t iterations_completed,
                  std::size_t total_messages,
                  const std::string& failure_reason)
{
    std::ofstream out(dir / "barrier_flush_result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open barrier_flush_result.txt for writing");
    }

    out << (success ? "ok" : "fail") << '\n';
    out << iterations_completed << '\n';
    out << total_messages << '\n';
    if (!success) {
        out << failure_reason << '\n';
    }
}

int coordinator_process()
{
    using namespace sintra;

    Coordinator_state state;
    bool success = true;
    std::string failure_reason;

    activate_slot([&](const Iteration_marker& marker) {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.abort_requested) {
            state.cv.notify_all();
            return;
        }

        auto mark_failure = [&](const std::string& reason) {
            if (success) {
                success = false;
                failure_reason = reason;
            }
            state.iteration_failed = true;
            state.abort_requested = true;
        };

        if (marker.worker >= k_worker_count) {
            std::ostringstream oss;
            oss << "Invalid worker index " << marker.worker << " (expected < " << k_worker_count
                << ")";
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        auto& expected = state.next_expected_iteration[marker.worker];

        if (marker.iteration == expected) {
            ++expected;
            ++state.messages_in_iteration;
            ++state.total_messages;
            state.cv.notify_all();
            return;
        }

        if (marker.iteration < expected) {
            std::ostringstream oss;
            oss << "Worker " << marker.worker << " sent duplicate marker for iteration "
                << marker.iteration;
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        std::ostringstream oss;
        oss << "Worker " << marker.worker << " skipped from iteration " << expected
            << " to " << marker.iteration;
        mark_failure(oss.str());
        state.cv.notify_all();
    });

    barrier("barrier-flush-ready");

    bool aborted = false;
    constexpr auto iteration_timeout = std::chrono::seconds(10);

    for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {

        if (aborted) {
            barrier("barrier-flush-iteration");
            continue;
        }


        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.current_iteration = iteration;
            state.messages_in_iteration = 0;
        }

        std::unique_lock<std::mutex> lock(state.mutex);
        const bool completed = state.cv.wait_for(lock, iteration_timeout, [&] {
            if (state.abort_requested || state.iteration_failed) {
                return true;
            }
            for (std::size_t worker = 0; worker < k_worker_count; ++worker) {
                if (state.next_expected_iteration[worker] <= state.current_iteration) {
                    return false;
                }
            }
            return true;
        });

        if (!completed && !state.abort_requested && !state.iteration_failed) {
            if (success) {
                success = false;
                std::ostringstream oss;
                oss << "Timed out waiting for iteration " << iteration << " markers. Missing workers:";
                bool any_missing = false;
                for (std::size_t worker = 0; worker < k_worker_count; ++worker) {
                    if (state.next_expected_iteration[worker] <= state.current_iteration) {
                        oss << (any_missing ? " " : " ") << worker;
                        any_missing = true;
                    }
                }
                if (!any_missing) {
                    oss << " (none)";
                }
                failure_reason = oss.str();
            }
            state.abort_requested = true;
        }

        if (state.iteration_failed && success) {
            if (failure_reason.empty()) {
                failure_reason = "coordinator observed invalid marker sequence";
            }
            success = false;
        }

        const bool should_abort = state.abort_requested || state.iteration_failed;

        state.messages_in_iteration = 0;
        lock.unlock();

        barrier("barrier-flush-iteration");

        if (should_abort) {
            aborted = true;
            continue;
        }

    }

    barrier("barrier-flush-done", "_sintra_all_processes");
    deactivate_all_slots();

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "barrier_flush");
    const auto shared_dir = shared.path();
    write_result(shared_dir, success, k_iterations, state.total_messages, failure_reason);
    return success ? 0 : 1;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    barrier("barrier-flush-ready");

    for (std::uint32_t iteration = 0; iteration < k_iterations; ++iteration) {

        world() << Iteration_marker{worker_index, iteration};

        barrier("barrier-flush-iteration");

    }

    barrier("barrier-flush-done", "_sintra_all_processes");
    return 0;
}

int worker0_process()
{
    return worker_process(0);
}

int worker1_process()
{
    return worker_process(1);
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "barrier_flush");
    const auto shared_dir = shared.path();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("barrier-flush-done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "barrier_flush_result.txt";
        if (!std::filesystem::exists(result_path)) {
            std::fprintf(stderr, "Error: result file not found at %s\n", result_path.string().c_str());
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Error: failed to open result file %s\n", result_path.string().c_str());
            return 1;
        }

        std::string status;
        std::size_t iterations_completed = 0;
        std::size_t total_messages = 0;
        std::string reason;

        std::getline(in, status);
        in >> iterations_completed;
        in >> total_messages;
        std::getline(in >> std::ws, reason);
        in.close();

        if (status != "ok") {
            std::fprintf(stderr, "Barrier flush test reported failure: %s\n", reason.c_str());
            return 1;
        }
        if (iterations_completed != k_iterations) {
            std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                         k_iterations, iterations_completed);
            return 1;
        }
        const std::size_t expected_messages = k_worker_count * k_iterations;
        if (total_messages != expected_messages) {
            std::fprintf(stderr, "Expected %zu total messages, got %zu\n",
                         expected_messages, total_messages);
            return 1;
        }
    }

    return 0;
}
