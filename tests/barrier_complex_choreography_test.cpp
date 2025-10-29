// Barrier Complex Choreography Test
// Exercises elaborate cross-process choreography with multiple barrier phases
// and message-driven release to increase the chances of exposing subtle
// synchronisation bugs. The scenario uses two stages of workers that must
// proceed through staggered phases coordinated by a central process. Each
// iteration involves dynamically selected additional barrier rounds and relies
// on explicit release messages to advance, creating a dense schedule of
// synchronisation points that stresses ordering guarantees.

#include <sintra/sintra.h>

#include "test_environment.h"

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

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kStageAWorkers = 3;
constexpr std::size_t kStageBWorkers = 3;
constexpr std::size_t kIterations     = 64;
constexpr std::array<std::size_t, 2> kExpectedPerStage = {kStageAWorkers, kStageBWorkers};
constexpr std::uint32_t kMaxExtraRounds = 4;

struct StageReport
{
    std::uint32_t stage;
    std::uint32_t worker;
    std::uint32_t iteration;
    std::uint32_t payload;
};

struct StageDirective
{
    std::uint32_t stage;
    std::uint32_t iteration;
    std::uint32_t extra_rounds;
    std::uint32_t token;
};

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

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

    auto base = sintra::test::scratch_subdirectory("barrier_complex_choreography");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "barrier_complex_choreography_" << unique_suffix;
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
            stage_counts[stage].assign(kIterations, 0);
            worker_seen[stage].assign(kIterations, std::vector<bool>(kExpectedPerStage[stage], false));
        }
        stage_b_release_sent.assign(kIterations, false);
    }
};

void write_result(const std::filesystem::path& dir,
                  bool success,
                  std::size_t iterations_completed,
                  std::size_t stage_a_total,
                  std::size_t stage_b_total,
                  const std::string& failure_reason)
{
    std::ofstream out(dir / "complex_choreography_result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open complex_choreography_result.txt for writing");
    }

    out << (success ? "ok" : "fail") << '\n';
    out << iterations_completed << '\n';
    out << stage_a_total << '\n';
    out << stage_b_total << '\n';
    if (!success) {
        out << failure_reason << '\n';
    }
}

void cleanup_directory_with_retries(const std::filesystem::path& dir)
{
    bool cleanup_succeeded = false;
    for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
        try {
            std::filesystem::remove_all(dir);
            cleanup_succeeded = true;
        } catch (const std::exception& e) {
            if (retry < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                std::fprintf(stderr,
                             "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                             dir.string().c_str(), e.what());
            }
        }
    }
}

void custom_terminate_handler()
{
    std::fprintf(stderr, "std::terminate called!\n");
    std::fprintf(stderr, "Uncaught exceptions: %d\n", std::uncaught_exceptions());

    try {
        auto eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        } else {
            std::fprintf(stderr, "terminate called without an active exception\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Uncaught exception of unknown type\n");
    }

    std::abort();
}

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

    activate_slot([&](const StageReport& report) {
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
        if (report.iteration >= kIterations) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.failure) {
                state.failure = true;
                state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                    oss << "iteration index out of bounds (allowed range 0.." << (kIterations - 1)
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
        if (report.worker >= kExpectedPerStage[stage]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "worker index out of bounds for stage " << stage << " (worker="
                    << report.worker << ", expected < " << kExpectedPerStage[stage] << ')';
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
        if (count > kExpectedPerStage[stage]) {
            state.failure = true;
            state.failure_reason = make_failure_message(report.iteration, [&](std::ostringstream& oss) {
                oss << "too many reports for stage " << stage << " (received=" << count
                    << ", expected=" << kExpectedPerStage[stage] << ')';
            });
        }
        state.cv.notify_all();
    });

    std::size_t iterations_completed = 0;

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        const auto start_barrier = make_iteration_barrier_name("complex-start", iteration);
        barrier(start_barrier);

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock, [&] {
                return state.stage_counts[0][iteration] >= kExpectedPerStage[0] || state.failure;
            });
        }

        const std::uint32_t extra_rounds = extra_rounds_for_iteration(iteration);
        const auto stage0_token = directive_token(0, iteration);
        world() << StageDirective{0u, iteration, extra_rounds, stage0_token};

        const auto phase_a_barrier = make_iteration_barrier_name("complex-phase-a", iteration);
        barrier(phase_a_barrier);

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stage_b_release_sent[iteration] = true;
        }

        const auto stage1_token = directive_token(1, iteration);
        world() << StageDirective{1u, iteration, extra_rounds, stage1_token};

        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.cv.wait(lock, [&] {
                return state.stage_counts[1][iteration] >= kExpectedPerStage[1] || state.failure;
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
        for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
            stage_a_total += state.stage_counts[0][iteration];
            stage_b_total += state.stage_counts[1][iteration];
        }
        failure_reason = state.failure_reason;
        success = !state.failure;
    }

    try {
        const auto shared_dir = get_shared_directory();
        write_result(shared_dir, success, iterations_completed, stage_a_total, stage_b_total, failure_reason);
    } catch (const std::exception& e) {
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
    std::vector<bool> release_received(kIterations, false);
    std::vector<std::uint32_t> extra_rounds_cache(kIterations, 0);
    std::vector<std::uint32_t> release_tokens(kIterations, 0);

    activate_slot([&](const StageDirective& directive) {
        if (directive.stage != stage) {
            return;
        }
        if (directive.iteration >= kIterations) {
            failure.store(true, std::memory_order_relaxed);
            return;
        }
        if (directive.extra_rounds > kMaxExtraRounds) {
            failure.store(true, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(release_mutex);
            if (release_received[directive.iteration]) {
                failure.store(true, std::memory_order_relaxed);
            } else {
                release_received[directive.iteration] = true;
            }
            extra_rounds_cache[directive.iteration] = directive.extra_rounds;
            release_tokens[directive.iteration]     = directive.token;
        }
        release_cv.notify_all();
    });

    const auto now_seed = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid_seed = static_cast<unsigned>(_getpid());
#else
    const auto pid_seed = static_cast<unsigned>(getpid());
#endif
    std::seed_seq seed{now_seed, pid_seed, static_cast<unsigned>(stage), static_cast<unsigned>(worker_index)};
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> delay_dist(0, 40);

    auto wait_for_release = [&](std::uint32_t iteration) {
        std::unique_lock<std::mutex> lock(release_mutex);
        release_cv.wait(lock, [&] { return release_received[iteration]; });
        return std::make_pair(extra_rounds_cache[iteration], release_tokens[iteration]);
    };

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
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
            world() << StageReport{stage,
                                   worker_index,
                                   iteration,
                                   expected_payload(stage, iteration, worker_index)};

            auto [extra_rounds, token] = wait_for_release(iteration);
            if (token != directive_token(stage, iteration)) {
                failure.store(true, std::memory_order_relaxed);
            }

            barrier(phase_a_barrier);
            barrier(phase_b_barrier);

            if (extra_rounds > kMaxExtraRounds) {
                failure.store(true, std::memory_order_relaxed);
                extra_rounds = kMaxExtraRounds;
            }
            for (std::uint32_t extra = 0; extra < extra_rounds; ++extra) {
                const auto extra_barrier = make_extra_barrier_name(iteration, extra);
                barrier(extra_barrier);
            }

            barrier(done_barrier);
        } else {
            barrier(phase_a_barrier);

            auto [extra_rounds, token] = wait_for_release(iteration);
            if (token != directive_token(stage, iteration)) {
                failure.store(true, std::memory_order_relaxed);
            }

            const int delay = delay_dist(gen);
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
            }
            world() << StageReport{stage,
                                   worker_index,
                                   iteration,
                                   expected_payload(stage, iteration, worker_index)};

            barrier(phase_b_barrier);

            if (extra_rounds > kMaxExtraRounds) {
                failure.store(true, std::memory_order_relaxed);
                extra_rounds = kMaxExtraRounds;
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

    return failure.load(std::memory_order_relaxed) ? 1 : 0;
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
#ifdef _WIN32
    // Disable Windows error reporting dialogs and CRT abort popups
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::set_terminate(custom_terminate_handler);

    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(stage_a0_process);
    processes.emplace_back(stage_a1_process);
    processes.emplace_back(stage_a2_process);
    processes.emplace_back(stage_b0_process);
    processes.emplace_back(stage_b1_process);
    processes.emplace_back(stage_b2_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("complex-choreography-test-done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "complex_choreography_result.txt";
        if (!std::filesystem::exists(result_path)) {
            std::fprintf(stderr, "Error: result file not found at %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Error: failed to open result file %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
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

        cleanup_directory_with_retries(shared_dir);

        const std::size_t expected_stage_a = kStageAWorkers * kIterations;
        const std::size_t expected_stage_b = kStageBWorkers * kIterations;

        if (status != "ok") {
            std::fprintf(stderr, "Complex choreography test reported failure: %s\n", reason.c_str());
            return 1;
        }
        if (iterations_completed != kIterations) {
            std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                         kIterations, iterations_completed);
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
    }

    return 0;
}

