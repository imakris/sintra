//
// Sintra Delivery Fence Regression Reproducer
//
// Exercises the delivery-fence barrier semantics under heavy backlog. Workers
// emit bursts of markers and immediately enter a delivery-fence barrier while
// the coordinator handles the markers very slowly. The coordinator trusts the
// barrier to flush all pre-barrier messages and checks after the barrier returns
// whether every reader observed the complete burst. Any gap indicates that the
// delivery fence released too early.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

constexpr std::size_t k_worker_count  = 2;
constexpr std::size_t k_iterations   = 64;
constexpr std::size_t k_burst_count   = 8;
constexpr auto k_handler_delay        = std::chrono::milliseconds(12);

struct Iteration_marker
{
    std::uint32_t worker;
    std::uint32_t iteration;
    std::uint32_t sequence;
};

struct Coordinator_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::uint32_t, k_worker_count> next_expected_sequence{};
    std::array<std::uint32_t, k_worker_count> next_expected_iteration{};
    std::array<std::uint32_t, k_worker_count> messages_seen_in_iteration{};
    std::size_t messages_in_iteration = 0;
    std::size_t total_messages        = 0;
    bool first_message_arrived        = false;
    bool iteration_failed             = false;
    std::string failure_detail;
};

int coordinator_process()
{
    using namespace sintra;

    Coordinator_state state;
    bool success = true;
    std::string failure_reason;
    std::size_t iterations_completed = 0;
    bool aborted = false;

    activate_slot([&](const Iteration_marker& marker) {
        std::unique_lock<std::mutex> lock(state.mutex);

        auto mark_failure = [&](const std::string& reason) {
            if (!state.iteration_failed) {
                state.iteration_failed = true;
                state.failure_detail = reason;
            }
        };

        if (marker.worker >= k_worker_count) {
            std::ostringstream oss;
            oss << "Invalid worker index " << marker.worker << " (expected < " << k_worker_count << ')';
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        auto& expected_iteration = state.next_expected_iteration[marker.worker];
        auto& expected_sequence = state.next_expected_sequence[marker.worker];
        auto& messages_seen = state.messages_seen_in_iteration[marker.worker];

        if (marker.iteration != expected_iteration) {
            std::ostringstream oss;
            oss << "Worker " << marker.worker << " expected iteration " << expected_iteration
                << " but sent " << marker.iteration;
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        if (marker.sequence != expected_sequence) {
            std::ostringstream oss;
            oss << "Worker " << marker.worker << " expected sequence " << expected_sequence
                << " but sent " << marker.sequence;
            mark_failure(oss.str());
            state.cv.notify_all();
            return;
        }

        const bool first_message = (state.messages_in_iteration == 0);

        lock.unlock();
        std::this_thread::sleep_for(k_handler_delay);
        lock.lock();

        ++expected_sequence;
        ++messages_seen;
        ++state.messages_in_iteration;
        ++state.total_messages;

        if (messages_seen == k_burst_count) {
            messages_seen = 0;
            ++expected_iteration;
        }

        if (first_message) {
            state.first_message_arrived = true;
        }

        state.cv.notify_all();
    });

    barrier("delivery-fence-repro-ready");

    constexpr auto first_marker_timeout = std::chrono::seconds(5);
    constexpr auto drain_timeout        = std::chrono::seconds(10);

    for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
        bool barrier_called = false;

        {
            std::lock_guard<std::mutex> guard(state.mutex);
            state.messages_in_iteration = 0;
            state.first_message_arrived = false;
            state.iteration_failed = false;
            state.failure_detail.clear();
        }

        if (!aborted) {
            std::unique_lock<std::mutex> lock(state.mutex);
            const bool first_arrived = state.cv.wait_for(lock, first_marker_timeout, [&] {
                return state.first_message_arrived || state.iteration_failed;
            });

            if (!first_arrived && !state.iteration_failed) {
                if (success) {
                    std::ostringstream oss;
                    oss << "Coordinator did not observe first marker for iteration " << iteration;
                    failure_reason = oss.str();
                }
                success = false;
                aborted = true;
            }
            else
            if (state.iteration_failed) {
                if (success) {
                    failure_reason = state.failure_detail.empty()
                        ? "Coordinator observed invalid marker sequence"
                        : state.failure_detail;
                }
                success = false;
                aborted = true;
            }
            lock.unlock();

            if (!aborted) {
                const bool barrier_ok = sintra::barrier("delivery-fence-repro-iteration");
                barrier_called = true;
                if (!barrier_ok) {
                    if (success) {
                        failure_reason = "Coordinator barrier rendezvous failed";
                    }
                    success = false;
                    aborted = true;
                }
            }

            lock.lock();
            if (!aborted && !state.iteration_failed) {
                std::vector<std::size_t> missing_workers;
                for (std::size_t worker = 0; worker < k_worker_count; ++worker) {
                    if (state.next_expected_iteration[worker] <= iteration) {
                        missing_workers.push_back(worker);
                    }
                }

                if (!missing_workers.empty()) {
                    if (success) {
                        std::ostringstream oss;
                        oss << "Barrier released before delivery completed for iteration " << iteration
                            << ". Missing workers:";
                        for (std::size_t idx = 0; idx < missing_workers.size(); ++idx) {
                            oss << (idx == 0 ? " " : " ") << missing_workers[idx];
                        }
                        failure_reason = oss.str();
                    }
                    success = false;
                    aborted = true;
                }
            }

            if (!aborted && !state.iteration_failed) {
                const bool drained = state.cv.wait_for(lock, drain_timeout, [&] {
                    for (std::size_t worker = 0; worker < k_worker_count; ++worker) {
                        if (state.next_expected_iteration[worker] <= iteration) {
                            return false;
                        }
                    }
                    return true;
                });

                if (!drained) {
                    if (success) {
                        std::ostringstream oss;
                        oss << "Iteration " << iteration << " did not drain after barrier";
                        failure_reason = oss.str();
                    }
                    success = false;
                    aborted = true;
                }
            }

            lock.unlock();

            if (!aborted) {
                ++iterations_completed;
            }
        }

        if (!barrier_called) {
            sintra::barrier("delivery-fence-repro-iteration");
        }
    }

    barrier("delivery-fence-repro-done", "_sintra_all_processes");
    deactivate_all_slots();

    sintra::test::Shared_directory shared("SINTRA_DELIVERY_FENCE_DIR", "barrier_delivery_fence");
    const auto shared_dir = shared.path();
    std::vector<std::string> lines;
    lines.reserve(success ? 3 : 4);
    lines.push_back(success ? "ok" : "fail");
    lines.push_back(std::to_string(iterations_completed));
    lines.push_back(std::to_string(state.total_messages));
    if (!success) {
        lines.push_back(failure_reason);
    }
    sintra::test::write_lines(shared_dir / "delivery_fence_repro_result.txt", lines);
    return success ? 0 : 1;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    barrier("delivery-fence-repro-ready");

    std::uint32_t sequence = 0;

    for (std::uint32_t iteration = 0; iteration < k_iterations; ++iteration) {

        for (std::uint32_t burst = 0; burst < k_burst_count; ++burst) {
            world() << Iteration_marker{worker_index, iteration, sequence};
            ++sequence;

            if ((burst + worker_index + iteration) % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }

        barrier("delivery-fence-repro-iteration");
    }

    barrier("delivery-fence-repro-done", "_sintra_all_processes");
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
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_DELIVERY_FENCE_DIR",
        "barrier_delivery_fence",
        {coordinator_process, worker0_process, worker1_process},
        [](const std::filesystem::path& shared_dir) {
            const auto result_path = shared_dir / "delivery_fence_repro_result.txt";
            if (!std::filesystem::exists(result_path)) {
                std::fprintf(stderr,
                             "Error: result file not found at %s\n",
                             result_path.string().c_str());
                return 1;
            }

            std::ifstream in(result_path, std::ios::binary);
            if (!in) {
                std::fprintf(stderr,
                             "Error: failed to open result file %s\n",
                             result_path.string().c_str());
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

            if (status != "ok") {
                std::fprintf(stderr,
                             "Delivery fence regression repro reported failure: %s\n",
                             reason.c_str());
                return 1;
            }
            if (iterations_completed != k_iterations) {
                std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                             k_iterations, iterations_completed);
                return 1;
            }
            const std::size_t expected_messages = k_worker_count * k_iterations * k_burst_count;
            if (total_messages != expected_messages) {
                std::fprintf(stderr, "Expected %zu total messages, got %zu\n",
                             expected_messages, total_messages);
                return 1;
            }
            return 0;
        },
        "delivery-fence-repro-done");
}
