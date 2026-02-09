// Barrier Complex Choreography Test
// Exercises elaborate cross-process choreography with multiple barrier phases
// and message-driven release to increase the chances of exposing subtle
// synchronisation bugs. The scenario uses two stages of workers that must
// proceed through staggered phases coordinated by a central process. Each
// iteration involves dynamically selected additional barrier rounds and relies
// on explicit release messages to advance, creating a dense schedule of
// synchronisation points that stresses ordering guarantees.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t k_stage_a_workers = 3;
constexpr std::size_t k_stage_b_workers = 3;
constexpr std::size_t k_iterations     = 64;
constexpr std::array<std::size_t, 2> k_expected_per_stage = {k_stage_a_workers, k_stage_b_workers};
constexpr std::uint32_t k_max_extra_rounds = 4;

struct Stage_report
{
    std::uint32_t stage;
    std::uint32_t worker;
    std::uint32_t iteration;
    std::uint32_t payload;
};

struct Stage_directive
{
    std::uint32_t stage;
    std::uint32_t iteration;
    std::uint32_t extra_rounds;
    std::uint32_t token;
};

std::string make_iteration_barrier_name(const std::string& prefix, std::uint32_t iteration)
{
    std::ostringstream oss;
    oss << prefix << '-' << iteration;
    return oss.str();
}

std::string make_extra_barrier_name(std::uint32_t iteration, std::uint32_t extra_index)
{
    std::ostringstream oss;
    oss << "complex-extra-" << iteration << '-' << extra_index;
    return oss.str();
}

std::uint32_t directive_token(std::uint32_t stage, std::uint32_t iteration)
{
    return static_cast<std::uint32_t>(0xAC00u + iteration * 17u + stage * 5u);
}

std::uint32_t expected_payload(std::uint32_t stage, std::uint32_t iteration, std::uint32_t worker)
{
    return static_cast<std::uint32_t>(iteration * 100u + stage * 10u + worker);
}

std::uint32_t extra_rounds_for_iteration(std::uint32_t iteration)
{
    std::uint32_t rounds = iteration % 3u; // 0, 1, 2 pattern
    if ((iteration % 5u) == 0u) {
        rounds += 1u; // add extra cycle every fifth iteration (including iteration 0)
    }
    return rounds;
}

struct Coordinator_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::vector<std::size_t>, 2> stage_counts;
    std::array<std::vector<std::vector<bool>>, 2> worker_seen;
    std::vector<bool> stage_b_release_sent;
    bool failure = false;
    std::string failure_reason;

    Coordinator_state()
    {
        for (std::size_t stage = 0; stage < 2; ++stage) {
            stage_counts[stage].assign(k_iterations, 0);
            worker_seen[stage].assign(k_iterations, std::vector<bool>(k_expected_per_stage[stage], false));
        }
        stage_b_release_sent.assign(k_iterations, false);
    }
};

int coordinator_process()
{
    using namespace sintra;

    Coordinator_state state;

    const auto make_failure_message = [](std::uint32_t iteration, auto&& formatter) {
        std::ostringstream oss;
        oss << "iteration " << iteration << ": ";
        std::forward<decltype(formatter)>(formatter)(oss);
        return oss.str();
    };

    activate_slot([&](const Stage_report& report) {
        if (report.stage >= state.stage_counts.size()) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.failure) {
                state.failure = true;
                state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                    oss << "stage index out of bounds (stage=" << report.stage << ')';
                });
            }
            state.cv.notify_all();
            return;
        }
        if (report.iteration >= k_iterations) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.failure) {
                state.failure = true;
                state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                    oss << "iteration index out of bounds (allowed range 0.." << (k_iterations - 1)
                        << ')';
                });
            }
            state.cv.notify_all();
            return;
        }

        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.failure) {
            state.cv.notify_all();
            return;
        }

        const std::size_t stage = report.stage;
        if (report.worker >= k_expected_per_stage[stage]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "worker index out of bounds for stage " << stage << " (worker="
                    << report.worker << ", expected < " << k_expected_per_stage[stage] << ')';
            });
            state.cv.notify_all();
            return;
        }

        if (state.worker_seen[stage][report.iteration][report.worker]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "duplicate report from stage " << stage << ", worker " << report.worker;
            });
            state.cv.notify_all();
            return;
        }

        const auto expected = expected_payload(stage, report.iteration, report.worker);
        if (expected != report.payload) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "payload mismatch for stage " << stage << ", worker " << report.worker
                    << " (expected=" << expected << ", actual=" << report.payload << ')';
            });
            state.cv.notify_all();
            return;
        }

        if (stage == 1 && !state.stage_b_release_sent[report.iteration]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "stage B report received before release";
            });
            state.cv.notify_all();
            return;
        }

        state.worker_seen[stage][report.iteration][report.worker] = true;
        const auto count = ++state.stage_counts[stage][report.iteration];
        if (count > k_expected_per_stage[stage]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "too many reports for stage " << stage << " (received=" << count
                    << ", expected=" << k_expected_per_stage[stage] << ')';
            });
        }
        state.cv.notify_all();
    });

    std::size_t iterations_completed = 0;

    for (std::uint32_t iteration = 0; iteration < k_iterations; ++iteration) {
        const auto start_barrier = make_iteration_barrier_name("complex-start", iteration);
        barrier(start_barrier);

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock, [&] {
                return state.stage_counts[0][iteration] >= k_expected_per_stage[0] || state.failure;
            });
        }

        const std::uint32_t extra_rounds = extra_rounds_for_iteration(iteration);
        const auto stage0_token = directive_token(0, iteration);
        world() << Stage_directive{0u, iteration, extra_rounds, stage0_token};

        const auto phase_a_barrier = make_iteration_barrier_name("complex-phase-a", iteration);
        barrier(phase_a_barrier);

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stage_b_release_sent[iteration] = true;
        }

        const auto stage1_token = directive_token(1, iteration);
        world() << Stage_directive{1u, iteration, extra_rounds, stage1_token};

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock, [&] {
                return state.stage_counts[1][iteration] >= k_expected_per_stage[1] || state.failure;
            });
        }

        const auto phase_b_barrier = make_iteration_barrier_name("complex-phase-b", iteration);
        barrier(phase_b_barrier);

        for (std::uint32_t extra = 0; extra < extra_rounds; ++extra) {
            const auto extra_barrier = make_extra_barrier_name(iteration, extra);
            barrier(extra_barrier);
        }

        const auto done_barrier = make_iteration_barrier_name("complex-done", iteration);
        barrier(done_barrier);

        ++iterations_completed;
    }

    barrier("complex-choreography-test-done", "_sintra_all_processes");
    deactivate_all_slots();

    std::size_t stage_a_total = 0;
    std::size_t stage_b_total = 0;
    std::string failure_reason;
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (std::uint32_t iteration = 0; iteration < k_iterations; ++iteration) {
            stage_a_total += state.stage_counts[0][iteration];
            stage_b_total += state.stage_counts[1][iteration];
        }
        failure_reason = state.failure_reason;
        success = !state.failure;
    }

    try {
        sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "barrier_complex_choreography");
        const auto shared_dir = shared.path();
        std::vector<std::string> lines;
        lines.reserve(success ? 4 : 5);
        lines.push_back(success ? "ok" : "fail");
        lines.push_back(std::to_string(iterations_completed));
        lines.push_back(std::to_string(stage_a_total));
        lines.push_back(std::to_string(stage_b_total));
        if (!success) {
            lines.push_back(failure_reason);
        }
        sintra::test::write_lines(shared_dir / "complex_choreography_result.txt", lines);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to write result: %s\n", e.what());
        return 1;
    }

    return success ? 0 : 1;
}

int stage_process(std::uint32_t stage, std::uint32_t worker_index)
{
    using namespace sintra;

    std::atomic<bool> failure{false};
    std::mutex release_mutex;
    std::condition_variable release_cv;
    std::vector<bool> release_received(k_iterations, false);
    std::vector<std::uint32_t> extra_rounds_cache(k_iterations, 0);
    std::vector<std::uint32_t> release_tokens(k_iterations, 0);

    activate_slot([&](const Stage_directive& directive) {
        if (directive.stage != stage) {
            return;
        }
        if (directive.iteration >= k_iterations) {
            failure.store(true);
            return;
        }
        if (directive.extra_rounds > k_max_extra_rounds) {
            failure.store(true);
        }

        {
            std::lock_guard<std::mutex> lock(release_mutex);
            if (release_received[directive.iteration]) {
                failure.store(true);
            }
            else {
                release_received[directive.iteration] = true;
            }
            extra_rounds_cache[directive.iteration] = directive.extra_rounds;
            release_tokens[directive.iteration]     = directive.token;
        }
        release_cv.notify_all();
    });

    const auto now_seed = static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid_seed = static_cast<unsigned>(sintra::test::get_pid());
    std::seed_seq seed{now_seed, pid_seed, static_cast<unsigned>(stage), static_cast<unsigned>(worker_index)};
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> delay_dist(0, 40);

    auto wait_for_release = [&](std::uint32_t iteration) {
        std::unique_lock<std::mutex> lock(release_mutex);
        release_cv.wait(lock, [&] { return release_received[iteration]; });
        return std::make_pair(extra_rounds_cache[iteration], release_tokens[iteration]);
    };

    for (std::uint32_t iteration = 0; iteration < k_iterations; ++iteration) {
        const auto start_barrier = make_iteration_barrier_name("complex-start", iteration);
        barrier(start_barrier);

        const auto phase_a_barrier = make_iteration_barrier_name("complex-phase-a", iteration);
        const auto phase_b_barrier = make_iteration_barrier_name("complex-phase-b", iteration);
        const auto done_barrier    = make_iteration_barrier_name("complex-done", iteration);

        if (stage == 0) {
            const int delay = delay_dist(gen);
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
            }
            world() << Stage_report{stage,
                                   worker_index,
                                   iteration,
                                   expected_payload(stage, iteration, worker_index)};

            auto [extra_rounds, token] = wait_for_release(iteration);
            if (token != directive_token(stage, iteration)) {
                failure.store(true);
            }

            barrier(phase_a_barrier);
            barrier(phase_b_barrier);

            if (extra_rounds > k_max_extra_rounds) {
                failure.store(true);
                extra_rounds = k_max_extra_rounds;
            }
            for (std::uint32_t extra = 0; extra < extra_rounds; ++extra) {
                const auto extra_barrier = make_extra_barrier_name(iteration, extra);
                barrier(extra_barrier);
            }

            barrier(done_barrier);
        }
        else {
            barrier(phase_a_barrier);

            auto [extra_rounds, token] = wait_for_release(iteration);
            if (token != directive_token(stage, iteration)) {
                failure.store(true);
            }

            const int delay = delay_dist(gen);
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
            }
            world() << Stage_report{stage,
                                   worker_index,
                                   iteration,
                                   expected_payload(stage, iteration, worker_index)};

            barrier(phase_b_barrier);

            if (extra_rounds > k_max_extra_rounds) {
                failure.store(true);
                extra_rounds = k_max_extra_rounds;
            }
            for (std::uint32_t extra = 0; extra < extra_rounds; ++extra) {
                const auto extra_barrier = make_extra_barrier_name(iteration, extra);
                barrier(extra_barrier);
            }

            barrier(done_barrier);
        }
    }

    deactivate_all_slots();
    barrier("complex-choreography-test-done", "_sintra_all_processes");

    return failure.load() ? 1 : 0;
}

int stage_a0_process() { return stage_process(0, 0); }
int stage_a1_process() { return stage_process(0, 1); }
int stage_a2_process() { return stage_process(0, 2); }
int stage_b0_process() { return stage_process(1, 0); }
int stage_b1_process() { return stage_process(1, 1); }
int stage_b2_process() { return stage_process(1, 2); }

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "barrier_complex_choreography",
        {coordinator_process,
         stage_a0_process,
         stage_a1_process,
         stage_a2_process,
         stage_b0_process,
         stage_b1_process,
         stage_b2_process},
        [](const std::filesystem::path& shared_dir) {
            const auto result_path = shared_dir / "complex_choreography_result.txt";
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
            std::size_t stage_a_total = 0;
            std::size_t stage_b_total = 0;
            std::string reason;

            std::getline(in, status);
            in >> iterations_completed;
            in >> stage_a_total;
            in >> stage_b_total;
            std::getline(in >> std::ws, reason);

            const std::size_t expected_stage_a = k_stage_a_workers * k_iterations;
            const std::size_t expected_stage_b = k_stage_b_workers * k_iterations;

            if (status != "ok") {
                std::fprintf(stderr,
                             "Complex choreography test reported failure: %s\n",
                             reason.c_str());
                return 1;
            }
            if (iterations_completed != k_iterations) {
                std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                             k_iterations, iterations_completed);
                return 1;
            }
            if (stage_a_total != expected_stage_a) {
                std::fprintf(stderr, "Expected %zu stage A reports, got %zu\n",
                             expected_stage_a, stage_a_total);
                return 1;
            }
            if (stage_b_total != expected_stage_b) {
                std::fprintf(stderr, "Expected %zu stage B reports, got %zu\n",
                             expected_stage_b, stage_b_total);
                return 1;
            }
            return 0;
        },
        "complex-choreography-test-done");
}

