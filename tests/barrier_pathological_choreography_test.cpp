// Barrier Pathological Choreography Test
// Exercises extremely complex synchronization patterns with many opportunities
// for barrier misuse. The scenario deliberately orchestrates nested barriers,
// delivery- and processing-fence combinations, uneven message fan-out, and
// large numbers of interleaved messages. The goal is to increase the chances of
// exposing race conditions or ordering bugs in the barrier implementation.

#include <sintra/sintra.h>

#include "test_environment.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::string_view k_env_shared_dir = "SINTRA_PATHOLOGICAL_DIR";
constexpr int k_worker_count = 4;
constexpr int k_iterations = 6;
constexpr int k_stage_steps = 5;
constexpr int k_final_steps = 4;

struct Stage_report
{
    int worker;
    int iteration;
    int stage;
    int step;
    int moment;
};

struct Noise_message
{
    int from;
    int to;
    int iteration;
    int step;
    int payload;
};

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_PATHOLOGICAL_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(k_env_shared_dir.data(), dir.string().c_str());
#else
    setenv(k_env_shared_dir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("barrier_pathological");
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1);

    std::ostringstream oss;
    oss << "pathological_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void cleanup_directory(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

std::string make_pre_barrier_name(int iteration)
{
    std::ostringstream oss;
    oss << "pathological-pre-" << iteration;
    return oss.str();
}

std::string make_stage_barrier_name(int iteration, int step)
{
    std::ostringstream oss;
    oss << "pathological-stage-" << iteration << "-step-" << step;
    return oss.str();
}

std::string make_stage_processing_name(int iteration)
{
    std::ostringstream oss;
    oss << "pathological-stage-" << iteration << "-processed";
    return oss.str();
}

std::string make_final_barrier_name(int iteration, int index)
{
    std::ostringstream oss;
    oss << "pathological-final-" << iteration << "-slot-" << index;
    return oss.str();
}

std::string make_final_processing_name(int iteration)
{
    std::ostringstream oss;
    oss << "pathological-final-" << iteration << "-processed";
    return oss.str();
}

std::string make_worker_noise_log(int worker)
{
    std::ostringstream oss;
    oss << "worker_" << worker << "_noise.log";
    return oss.str();
}

std::string make_stage_log_name(int worker)
{
    std::ostringstream oss;
    oss << "worker_" << worker << "_stage.log";
    return oss.str();
}

void append_line(const std::filesystem::path& file, const std::string& line)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << line << '\n';
}

std::array<int, k_worker_count + 1> expected_noise_counts_for_iteration(int iteration)
{
    std::array<int, k_worker_count + 1> counts{};
    counts.fill(0);

    for (int worker = 0; worker < k_worker_count; ++worker) {
        for (int step = 0; step < k_stage_steps; ++step) {
            const int target = (worker + step + iteration) % (k_worker_count + 1);
            counts[static_cast<std::size_t>(target)] += 1;
        }
    }

    return counts;
}

int total_expected_noise_for_worker(int worker_index)
{
    int total = 0;
    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        const auto counts = expected_noise_counts_for_iteration(iteration);
        total += counts[static_cast<std::size_t>(worker_index)];
    }
    return total;
}

int compute_noise_target(int worker, int iteration, int step)
{
    return (worker + step + iteration) % (k_worker_count + 1);
}

std::string stage_report_to_string(const Stage_report& report)
{
    std::ostringstream oss;
    oss << "worker=" << report.worker << ",iter=" << report.iteration
        << ",stage=" << report.stage << ",step=" << report.step
        << ",moment=" << report.moment;
    return oss.str();
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

struct Controller_state
{
    std::mutex mutex;
    std::vector<Stage_report> stage_reports;
    std::array<int, k_iterations> controller_noise_counts{};
    int total_controller_noise = 0;
};

void record_stage_report(Controller_state& state, const Stage_report& report)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.stage_reports.push_back(report);
}

void record_controller_noise(Controller_state& state, const Noise_message& msg)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.controller_noise_counts[static_cast<std::size_t>(msg.iteration)] += 1;
    state.total_controller_noise += 1;
}

void write_summary(const Controller_state& state, const std::filesystem::path& dir)
{
    std::array<std::array<int, 2>, k_iterations> pre_counts{};
    std::array<std::array<std::array<int, 2>, k_stage_steps>, k_iterations> stage_counts{};
    std::array<std::array<int, 2>, k_iterations> process_counts{};
    std::array<std::array<std::array<int, 2>, k_final_steps>, k_iterations> final_counts{};
    std::array<std::array<int, 2>, k_iterations> final_process_counts{};

    for (const auto& report : state.stage_reports) {
        const auto iter = static_cast<std::size_t>(report.iteration);
        if (report.stage == 0) {
            pre_counts[iter][static_cast<std::size_t>(report.moment)] += 1;
        }
        else
        if (report.stage == 1) {
            const auto step = static_cast<std::size_t>(report.step);
            stage_counts[iter][step][static_cast<std::size_t>(report.moment)] += 1;
        }
        else
        if (report.stage == 2) {
            process_counts[iter][static_cast<std::size_t>(report.moment)] += 1;
        }
        else
        if (report.stage == 3) {
            const auto step = static_cast<std::size_t>(report.step);
            final_counts[iter][step][static_cast<std::size_t>(report.moment)] += 1;
        }
        else
        if (report.stage == 4) {
            final_process_counts[iter][static_cast<std::size_t>(report.moment)] += 1;
        }
    }

    bool ok = true;

    auto check_pair = [&](const auto& pair_counts, const std::string& label) {
        for (std::size_t iter = 0; iter < pair_counts.size(); ++iter) {
            for (std::size_t moment = 0; moment < pair_counts[iter].size(); ++moment) {
                if (pair_counts[iter][moment] != k_worker_count) {
                    ok = false;
                }
            }
        }
    };

    auto check_stage = [&](const auto& stage_counts_array, const std::string& label) {
        for (std::size_t iter = 0; iter < stage_counts_array.size(); ++iter) {
            for (std::size_t step = 0; step < stage_counts_array[iter].size(); ++step) {
                for (std::size_t moment = 0; moment < stage_counts_array[iter][step].size(); ++moment) {
                    if (stage_counts_array[iter][step][moment] != k_worker_count) {
                        ok = false;
                    }
                }
            }
        }
    };

    check_pair(pre_counts, "pre");
    check_stage(stage_counts, "stage");
    check_pair(process_counts, "process");
    check_stage(final_counts, "final");
    check_pair(final_process_counts, "final-process");

    std::ofstream out(dir / "summary.txt", std::ios::binary | std::ios::trunc);
    out << (ok ? "ok" : "fail") << '\n';
    out << "reports=" << state.stage_reports.size() << '\n';
    out << "controller_noise_total=" << state.total_controller_noise << '\n';
    for (int iter = 0; iter < k_iterations; ++iter) {
        out << "controller_noise_iter_" << iter << '='
            << state.controller_noise_counts[static_cast<std::size_t>(iter)] << '\n';
    }
}

int controller_process()
{
    using namespace sintra;

    Controller_state state;
    const auto shared_dir = get_shared_directory();
    const auto controller_log = shared_dir / "controller_stage.log";

    auto stage_slot = [&state, controller_log](const Stage_report& report) {
        record_stage_report(state, report);
        append_line(controller_log, stage_report_to_string(report));
    };

    auto noise_slot = [&state](const Noise_message& msg) {
        if (msg.to == k_worker_count) {
            record_controller_noise(state, msg);
        }
    };

    sintra::activate_slot(stage_slot);
    sintra::activate_slot(noise_slot);

    sintra::barrier("pathological-setup");

    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        const auto pre_name = make_pre_barrier_name(iteration);
        sintra::barrier(pre_name);

        for (int step = 0; step < k_stage_steps; ++step) {
            const auto stage_name = make_stage_barrier_name(iteration, step);
            sintra::barrier(stage_name);
        }

        const auto processing_name = make_stage_processing_name(iteration);
        sintra::barrier<sintra::processing_fence_t>(processing_name);

        std::vector<int> final_order(k_final_steps);
        std::iota(final_order.begin(), final_order.end(), 0);
        if (iteration % 2 == 1) {
            std::reverse(final_order.begin(), final_order.end());
        }

        for (int index : final_order) {
            const auto final_name = make_final_barrier_name(iteration, index);
            sintra::barrier(final_name);
        }

        const auto final_processing = make_final_processing_name(iteration);
        sintra::barrier<sintra::processing_fence_t>(final_processing);
    }

    sintra::barrier("pathological-done", "_sintra_all_processes");

    write_summary(state, shared_dir);
    return 0;
}

void log_stage_event(const std::filesystem::path& log_path,
                     const Stage_report& report,
                     std::string_view phase)
{
    std::ostringstream oss;
    oss << phase << ':' << stage_report_to_string(report);
    append_line(log_path, oss.str());
}

void log_noise_event(const std::filesystem::path& log_path, const Noise_message& msg)
{
    std::ostringstream oss;
    oss << "from=" << msg.from << ",iter=" << msg.iteration << ",step=" << msg.step
        << ",payload=" << msg.payload;
    append_line(log_path, oss.str());
}

int worker_process(int worker_index)
{
    using namespace sintra;

    const auto shared_dir = get_shared_directory();
    const auto stage_log = shared_dir / make_stage_log_name(worker_index);
    const auto noise_log = shared_dir / make_worker_noise_log(worker_index);

    auto stage_slot = [stage_log, worker_index](const Stage_report& report) {
        // All workers observe every report but only write detailed entries for
        // their own messages to avoid pathological log sizes.
        if (report.worker == worker_index) {
            log_stage_event(stage_log, report, "observe");
        }
    };

    auto noise_slot = [noise_log, worker_index](const Noise_message& msg) {
        if (msg.to != worker_index) {
            return;
        }

        const auto delay_us = static_cast<int>((msg.payload ^ (worker_index * 17)) % 7);
        if (delay_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
        log_noise_event(noise_log, msg);
    };

    sintra::activate_slot(stage_slot);
    sintra::activate_slot(noise_slot);

    sintra::barrier("pathological-setup");

    std::mt19937 rng(static_cast<unsigned>(worker_index * 7919 + 101));
    std::uniform_int_distribution<int> jitter(0, 5);

    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        const auto pre_name = make_pre_barrier_name(iteration);

        Stage_report pre_before{worker_index, iteration, 0, -1, 0};
        sintra::world() << pre_before;
        log_stage_event(stage_log, pre_before, "emit");
        if (jitter(rng) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        sintra::barrier(pre_name);

        Stage_report pre_after{worker_index, iteration, 0, -1, 1};
        sintra::world() << pre_after;
        log_stage_event(stage_log, pre_after, "emit");

        for (int step = 0; step < k_stage_steps; ++step) {
            Stage_report step_before{worker_index, iteration, 1, step, 0};
            sintra::world() << step_before;
            log_stage_event(stage_log, step_before, "emit");

            const int noise_target = compute_noise_target(worker_index, iteration, step);
            Noise_message noise{worker_index, noise_target, iteration, step,
                               worker_index * 100 + iteration * 10 + step};
            sintra::world() << noise;

            const auto stage_name = make_stage_barrier_name(iteration, step);
            if ((iteration + worker_index + step) % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
            sintra::barrier(stage_name);

            Stage_report step_after{worker_index, iteration, 1, step, 1};
            sintra::world() << step_after;
            log_stage_event(stage_log, step_after, "emit");
        }

        Stage_report process_pending{worker_index, iteration, 2, -1, 0};
        sintra::world() << process_pending;
        log_stage_event(stage_log, process_pending, "emit");

        const auto processing_name = make_stage_processing_name(iteration);
        sintra::barrier<processing_fence_t>(processing_name);

        Stage_report process_done{worker_index, iteration, 2, -1, 1};
        sintra::world() << process_done;
        log_stage_event(stage_log, process_done, "emit");

        std::vector<int> final_order(k_final_steps);
        std::iota(final_order.begin(), final_order.end(), 0);
        if (iteration % 2 == 1) {
            std::reverse(final_order.begin(), final_order.end());
        }

        for (int index : final_order) {
            Stage_report final_before{worker_index, iteration, 3, index, 0};
            sintra::world() << final_before;
            log_stage_event(stage_log, final_before, "emit");

            const auto final_name = make_final_barrier_name(iteration, index);
            if ((index + worker_index) % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(3));
            }
            sintra::barrier(final_name);

            Stage_report final_after{worker_index, iteration, 3, index, 1};
            sintra::world() << final_after;
            log_stage_event(stage_log, final_after, "emit");
        }

        Stage_report final_pending{worker_index, iteration, 4, -1, 0};
        sintra::world() << final_pending;
        log_stage_event(stage_log, final_pending, "emit");

        const auto final_processing = make_final_processing_name(iteration);
        sintra::barrier<processing_fence_t>(final_processing);

        Stage_report final_done{worker_index, iteration, 4, -1, 1};
        sintra::world() << final_done;
        log_stage_event(stage_log, final_done, "emit");
    }

    sintra::barrier("pathological-done", "_sintra_all_processes");
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }
int worker3_process() { return worker_process(3); }

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    if (!is_spawned) {
        for (int worker = 0; worker < k_worker_count; ++worker) {
            const auto stage_log = shared_dir / make_stage_log_name(worker);
            const auto noise_log = shared_dir / make_worker_noise_log(worker);
            std::filesystem::remove(stage_log);
            std::filesystem::remove(noise_log);
        }
        std::filesystem::remove(shared_dir / "controller_stage.log");
        std::filesystem::remove(shared_dir / "summary.txt");
    }

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(controller_process);
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);
    processes.emplace_back(worker2_process);
    processes.emplace_back(worker3_process);

    sintra::init(argc, argv, processes);
    if (!is_spawned) {
        sintra::barrier("pathological-done", "_sintra_all_processes");
    }
    sintra::finalize();

    int exit_code = 0;

    if (!is_spawned) {
        const auto summary_path = shared_dir / "summary.txt";
        std::ifstream summary_in(summary_path, std::ios::binary);
        std::string status;
        if (summary_in) {
            std::getline(summary_in, status);
        }

        if (status != "ok") {
            exit_code = 1;
        }
        else {
            for (int worker = 0; worker < k_worker_count; ++worker) {
                const auto noise_log = shared_dir / make_worker_noise_log(worker);
                std::ifstream in(noise_log, std::ios::binary);
                int lines = 0;
                std::string tmp;
                while (std::getline(in, tmp)) {
                    ++lines;
                }
                const int expected = total_expected_noise_for_worker(worker);
                if (lines != expected) {
                    exit_code = 1;
                    break;
                }
            }
        }

        cleanup_directory(shared_dir);
    }

    return exit_code;
}
