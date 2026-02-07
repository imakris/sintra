//
// Sintra Complex Choreography Test
//
// This test intentionally constructs an overly elaborate multi-process
// synchronization scenario.  The goal is to exercise rare synchronization
// paths by combining message passing, staged barriers, and shared file based
// verification.  Every step is deterministic so the test remains feasible, but
// the choreography is intricate enough that even subtle bugs in synchronization
// should manifest.
//
// Overview of the actors:
//   * Conductor: orchestrates phases and verifies summaries from the aggregator.
//   * Aggregator: collects worker status messages, aggregates checksums and
//                 reports progress back to the conductor.
//   * Inspector: passively records message ordering to cross-check the
//                aggregator's view of the world.
//   * Workers (4 processes): participate in multiple phases, perform artificial
//                work, and report structured status messages.
//
// Each phase is split into a "pre" and "post" barrier.  All processes join both
// barriers for every round, creating tight coupling between message delivery
// and global synchronization.  Failure to deliver messages, to respect the
// ordering, or to propagate shutdown signals will leave at least one barrier
// waiting forever, surfacing the bug.
//
// The starter process (the test harness) validates the reports emitted by the
// aggregator and inspector to ensure the choreography completed exactly as
// intended.
//
#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <thread>

namespace {

constexpr const char* k_group_name = "_sintra_external_processes";
constexpr int k_worker_count = 4;
constexpr std::array<int, 4> k_rounds_per_phase = {3, 5, 4, 6};

struct Phase_plan
{
    int phase = 0;
    int rounds = 0;
};

constexpr std::array<Phase_plan, 4> k_plan = {
    Phase_plan{0, k_rounds_per_phase[0]},
    Phase_plan{1, k_rounds_per_phase[1]},
    Phase_plan{2, k_rounds_per_phase[2]},
    Phase_plan{3, k_rounds_per_phase[3]}
};

struct Phase_command
{
    int phase;
    int round;
    std::uint64_t token;
    int expected_workers;
};

struct Worker_status
{
    int worker_id;
    int phase;
    int round;
    std::uint64_t token;
    std::uint32_t sequence_id;
    std::uint64_t checksum;
};

struct Phase_summary
{
    int phase;
    int round;
    std::uint64_t token;
    int worker_count;
    std::uint64_t checksum;
    int aggregator_errors;
};

struct Shutdown {};
struct Shutdown_complete {};

std::uint64_t make_token(int phase, int round)
{
    return (static_cast<std::uint64_t>(phase) << 32) |
           static_cast<std::uint32_t>(round);
}

std::string barrier_name(const char* stage, std::uint64_t token)
{
    const int phase = static_cast<int>(token >> 32);
    const int round = static_cast<int>(token & 0xffffffffu);
    std::ostringstream oss;
    oss << "complex-" << phase << '-' << round << '-' << stage;
    return oss.str();
}

std::uint32_t compute_sequence_id(int worker_id, int phase, int round)
{
    return static_cast<std::uint32_t>((phase << 16) | (round << 8) | worker_id);
}

std::uint64_t compute_worker_checksum(int worker_id, int phase, int round)
{
    // Deterministic but non-trivial mixing.
    std::uint64_t value = 0x9e3779b97f4a7c15ULL;
    value ^= static_cast<std::uint64_t>(worker_id + 1) * 0x6eed0e9da4d94a4fULL;
    value ^= static_cast<std::uint64_t>(phase + 3) * 0x94d049bb133111ebULL;
    value ^= static_cast<std::uint64_t>(round + 7) * 0xd953cc9b85b5a7d3ULL;
    value ^= (static_cast<std::uint64_t>(phase + 11) << 32) ^
             static_cast<std::uint64_t>((round + 13) << 16);
    return value;
}

std::uint64_t expected_round_checksum(int phase, int round)
{
    std::uint64_t total = 0;
    for (int worker = 0; worker < k_worker_count; ++worker) {
        total += compute_worker_checksum(worker, phase, round);
    }
    return total;
}

// -----------------------------------------------------------------------------
// Aggregator
// -----------------------------------------------------------------------------

struct Round_result
{
    int phase = 0;
    int round = 0;
    std::uint64_t token = 0;
    int count = 0;
    std::uint64_t checksum = 0;
};

struct Aggregator_state
{
    std::mutex mutex;
    std::condition_variable cv;

    Phase_command active_command{};
    bool command_active = false;
    bool round_complete = false;
    bool shutdown_received = false;

    std::array<bool, k_worker_count> worker_seen{};
    int current_count = 0;
    std::uint64_t checksum_accumulator = 0;
    std::uint64_t last_completed_token = std::numeric_limits<std::uint64_t>::max();

    int errors = 0;
    std::vector<Round_result> completed_rounds;
};

Aggregator_state& aggregator_state()
{
    static Aggregator_state state;
    return state;
}

void aggregator_phase_command_slot(const Phase_command& cmd)
{
    auto& state = aggregator_state();
    std::lock_guard<std::mutex> lk(state.mutex);

    if (state.command_active && state.active_command.token != cmd.token) {
        // Received a new command before the previous round completed.
        ++state.errors;
    }

    state.active_command = cmd;
    state.command_active = true;
    state.round_complete = false;
    state.worker_seen.fill(false);
    state.current_count = 0;
    state.checksum_accumulator = 0;

    state.cv.notify_all();
}

void aggregator_worker_status_slot(const Worker_status& status)
{
    auto& state = aggregator_state();
    Phase_summary summary{};
    bool should_emit_summary = false;

    {
        std::lock_guard<std::mutex> lk(state.mutex);
        if (!state.command_active ||
            status.token != state.active_command.token) {
            ++state.errors;
            return;
        }

        if (status.phase != state.active_command.phase ||
            status.round != state.active_command.round) {
            ++state.errors;
        }

        if (status.worker_id < 0 || status.worker_id >= k_worker_count) {
            ++state.errors;
            return;
        }

        const auto expected_seq =
            compute_sequence_id(status.worker_id, status.phase, status.round);
        if (status.sequence_id != expected_seq) {
            ++state.errors;
        }

        const auto expected_checksum = compute_worker_checksum(
            status.worker_id, status.phase, status.round);
        if (status.checksum != expected_checksum) {
            ++state.errors;
        }

        if (state.worker_seen[status.worker_id]) {
            ++state.errors;
        }
        else {
            state.worker_seen[status.worker_id] = true;
            ++state.current_count;
            state.checksum_accumulator += status.checksum;
        }

        if (state.current_count == state.active_command.expected_workers) {
            const auto active = state.active_command;

            state.round_complete = true;
            state.last_completed_token = active.token;
            Round_result result;
            result.phase = active.phase;
            result.round = active.round;
            result.token = active.token;
            result.count = state.current_count;
            result.checksum = state.checksum_accumulator;
            state.completed_rounds.push_back(result);

            state.command_active = false;
            state.active_command = Phase_command{};

            summary.phase = active.phase;
            summary.round = active.round;
            summary.token = active.token;
            summary.worker_count = state.current_count;
            summary.checksum = state.checksum_accumulator;
            summary.aggregator_errors = state.errors;
            should_emit_summary = true;
        }
    }

    if (should_emit_summary) {
        sintra::world() << summary;
        state.cv.notify_all();
    }
}

void aggregator_shutdown_slot(const Shutdown&)
{
    auto& state = aggregator_state();
    std::lock_guard<std::mutex> lk(state.mutex);
    state.shutdown_received = true;
    state.cv.notify_all();
}

void wait_for_round_activation(std::uint64_t token)
{
    auto& state = aggregator_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    state.cv.wait(lk, [&] {
        return (state.command_active && state.active_command.token == token) ||
               state.errors > 0;
    });
}

void wait_for_round_completion(std::uint64_t token)
{
    auto& state = aggregator_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    state.cv.wait(lk, [&] {
        return state.last_completed_token == token || state.errors > 0;
    });
}

void wait_for_shutdown()
{
    auto& state = aggregator_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    state.cv.wait(lk, [&] { return state.shutdown_received; });
}

void write_aggregator_report()
{
    sintra::test::Shared_directory shared("SINTRA_COMPLEX_CHOREO_DIR", "complex_choreography_stress");
    const auto dir = shared.path();
    const auto path = dir / "aggregator_report.txt";

    auto& state = aggregator_state();
    std::lock_guard<std::mutex> lk(state.mutex);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open aggregator report");
    }

    out << (state.errors == 0 ? "ok" : "fail") << '\n';
    out << state.errors << '\n';
    out << state.completed_rounds.size() << '\n';

    for (const auto& result : state.completed_rounds) {
        out << result.phase << ' ' << result.round << ' ' << result.count << ' '
            << result.checksum << '\n';
    }
}

int process_aggregator()
{
    using namespace sintra;

    sintra::activate_slot([](const Phase_command& cmd) {
        aggregator_phase_command_slot(cmd);
    });
    sintra::activate_slot([](const Worker_status& status) {
        aggregator_worker_status_slot(status);
    });
    sintra::activate_slot([](const Shutdown& msg) {
        aggregator_shutdown_slot(msg);
    });

    barrier("complex-choreo-setup", k_group_name);

    for (const auto& phase : k_plan) {
        for (int round = 0; round < phase.rounds; ++round) {
            const auto token = make_token(phase.phase, round);
            const auto pre = barrier_name("pre", token);
            const auto post = barrier_name("post", token);

            barrier(pre, k_group_name);
            wait_for_round_activation(token);
            wait_for_round_completion(token);
            barrier(post, k_group_name);
        }
    }

    wait_for_shutdown();
    write_aggregator_report();

    sintra::world() << Shutdown_complete{};

    barrier("complex-choreography-finished", "_sintra_all_processes");

    auto& state = aggregator_state();
    std::lock_guard<std::mutex> lk(state.mutex);
    return state.errors == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

struct Inspector_state
{
    std::mutex mutex;
    std::condition_variable cv;

    std::vector<std::tuple<int, int, int, std::uint32_t>> arrival_order;
    std::map<std::uint64_t, int> counts;
    std::map<std::uint64_t, std::uint64_t> checksums;
    bool shutdown_received = false;
};

Inspector_state& inspector_state()
{
    static Inspector_state state;
    return state;
}

void inspector_worker_status_slot(const Worker_status& status)
{
    auto& state = inspector_state();
    std::lock_guard<std::mutex> lk(state.mutex);

    const auto key = status.token;
    ++state.counts[key];
    state.checksums[key] += status.checksum;
    state.arrival_order.emplace_back(
        status.phase, status.round, status.worker_id, status.sequence_id);
}

void inspector_shutdown_slot(const Shutdown&)
{
    auto& state = inspector_state();
    std::lock_guard<std::mutex> lk(state.mutex);
    state.shutdown_received = true;
    state.cv.notify_all();
}

void wait_for_inspector_shutdown()
{
    auto& state = inspector_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    state.cv.wait(lk, [&] { return state.shutdown_received; });
}

void write_inspector_report()
{
    sintra::test::Shared_directory shared("SINTRA_COMPLEX_CHOREO_DIR", "complex_choreography_stress");
    const auto dir = shared.path();
    const auto path = dir / "inspector_report.txt";

    auto& state = inspector_state();
    std::lock_guard<std::mutex> lk(state.mutex);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open inspector report");
    }

    out << state.arrival_order.size() << '\n';
    for (const auto& entry : state.arrival_order) {
        out << std::get<0>(entry) << ' ' << std::get<1>(entry) << ' '
            << std::get<2>(entry) << ' ' << std::get<3>(entry) << '\n';
    }
}

int process_inspector()
{
    using namespace sintra;

    sintra::activate_slot([](const Worker_status& status) {
        inspector_worker_status_slot(status);
    });
    sintra::activate_slot([](const Shutdown& msg) {
        inspector_shutdown_slot(msg);
    });

    barrier("complex-choreo-setup", k_group_name);

    for (const auto& phase : k_plan) {
        for (int round = 0; round < phase.rounds; ++round) {
            const auto token = make_token(phase.phase, round);
            const auto pre = barrier_name("pre", token);
            const auto post = barrier_name("post", token);
            (void)token;
            barrier(pre, k_group_name);
            barrier(post, k_group_name);
        }
    }

    wait_for_inspector_shutdown();
    write_inspector_report();

    barrier("complex-choreography-finished", "_sintra_all_processes");
    return 0;
}

// -----------------------------------------------------------------------------
// Worker implementation
// -----------------------------------------------------------------------------

struct Worker_local_state
{
    std::mutex mutex;
    std::condition_variable cv;
    Phase_command pending_command{};
    bool has_command = false;
    bool shutdown_requested = false;
    std::uint64_t last_token = 0;
    int errors = 0;
};

void worker_command_slot(Worker_local_state& state, const Phase_command& cmd)
{
    std::lock_guard<std::mutex> lk(state.mutex);
    state.pending_command = cmd;
    state.has_command = true;
    state.cv.notify_all();
}

void worker_shutdown_slot(Worker_local_state& state, const Shutdown&)
{
    std::lock_guard<std::mutex> lk(state.mutex);
    state.shutdown_requested = true;
    state.cv.notify_all();
}

int worker_process_impl(int worker_id)
{
    using namespace sintra;

    Worker_local_state state;

    auto command_slot = [&state](const Phase_command& cmd) {
        worker_command_slot(state, cmd);
    };
    auto shutdown_slot = [&state](const Shutdown& msg) {
        worker_shutdown_slot(state, msg);
    };

    sintra::activate_slot(command_slot);
    sintra::activate_slot(shutdown_slot);

    barrier("complex-choreo-setup", k_group_name);

    bool fatal_error = false;

    for (const auto& phase : k_plan) {
        for (int round = 0; round < phase.rounds; ++round) {
            if (fatal_error) {
                break;
            }

            const auto token = make_token(phase.phase, round);
            const auto pre = barrier_name("pre", token);
            const auto post = barrier_name("post", token);

            barrier(pre, k_group_name);

            Phase_command cmd;
            {
                std::unique_lock<std::mutex> lk(state.mutex);
                const bool got_command = state.cv.wait_for(
                    lk, std::chrono::seconds(5), [&] {
                        return state.has_command &&
                               state.pending_command.token == token;
                    });
                if (!got_command) {
                    ++state.errors;
                    fatal_error = true;
                    break;
                }
                cmd = state.pending_command;
                state.has_command = false;
            }

            if (cmd.phase != phase.phase || cmd.round != round) {
                ++state.errors;
            }

            if (state.last_token != 0 && token <= state.last_token) {
                ++state.errors;
            }
            state.last_token = token;

            const auto sequence_id =
                compute_sequence_id(worker_id, cmd.phase, cmd.round);
            const auto checksum =
                compute_worker_checksum(worker_id, cmd.phase, cmd.round);

            // Deterministic delay pattern amplifies the chance of
            // interleavings without making the test flaky.
            const auto delay_ms =
                (worker_id * 3 + cmd.phase * 5 + cmd.round * 7) % 11;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5 + delay_ms));

            Worker_status status;
            status.worker_id = worker_id;
            status.phase = cmd.phase;
            status.round = cmd.round;
            status.token = cmd.token;
            status.sequence_id = sequence_id;
            status.checksum = checksum;
            sintra::world() << status;

            barrier(post, k_group_name);
        }
        if (fatal_error) {
            break;
        }
    }

    {
        std::unique_lock<std::mutex> lk(state.mutex);
        const bool shutdown_ok = state.cv.wait_for(
            lk, std::chrono::seconds(5), [&] { return state.shutdown_requested; });
        if (!shutdown_ok) {
            ++state.errors;
        }
    }

    barrier("complex-choreography-finished", "_sintra_all_processes");

    return state.errors == 0 ? 0 : 1;
}

int process_worker0() { return worker_process_impl(0); }
int process_worker1() { return worker_process_impl(1); }
int process_worker2() { return worker_process_impl(2); }
int process_worker3() { return worker_process_impl(3); }

// -----------------------------------------------------------------------------
// Conductor
// -----------------------------------------------------------------------------

struct Conductor_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t expected_token = 0;
    bool awaiting_summary = false;
    bool shutdown_confirmed = false;
    int errors = 0;
    std::vector<std::uint64_t> summaries_received;
};

Conductor_state& conductor_state()
{
    static Conductor_state state;
    return state;
}

void conductor_summary_slot(const Phase_summary& summary)
{
    auto& state = conductor_state();
    std::unique_lock<std::mutex> lk(state.mutex);

    if (!state.awaiting_summary || summary.token != state.expected_token) {
        ++state.errors;
    }
    else {
        if (summary.worker_count != k_worker_count) {
            ++state.errors;
        }
        const auto expected_checksum = expected_round_checksum(
            summary.phase, summary.round);
        if (summary.checksum != expected_checksum) {
            ++state.errors;
        }
        if (summary.aggregator_errors != 0) {
            state.errors += summary.aggregator_errors;
        }

        state.awaiting_summary = false;
        state.summaries_received.push_back(summary.token);
    }

    lk.unlock();
    state.cv.notify_all();
}

void conductor_shutdown_complete_slot(const Shutdown_complete&)
{
    auto& state = conductor_state();
    std::lock_guard<std::mutex> lk(state.mutex);
    state.shutdown_confirmed = true;
    state.cv.notify_all();
}

bool wait_for_summary(std::uint64_t token)
{
    auto& state = conductor_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    const bool ok = state.cv.wait_for(
        lk, std::chrono::seconds(5), [&] { return !state.awaiting_summary; });
    if (!ok) {
        ++state.errors;
    }
    if (!state.awaiting_summary && state.expected_token != token) {
        ++state.errors;
    }
    return ok;
}

bool wait_for_shutdown_confirmation()
{
    auto& state = conductor_state();
    std::unique_lock<std::mutex> lk(state.mutex);
    const bool ok = state.cv.wait_for(
        lk, std::chrono::seconds(5), [&] { return state.shutdown_confirmed; });
    if (!ok) {
        ++state.errors;
    }
    return ok;
}

int process_conductor()
{
    using namespace sintra;

    sintra::activate_slot([](const Phase_summary& summary) {
        conductor_summary_slot(summary);
    });
    sintra::activate_slot([](const Shutdown_complete& msg) {
        conductor_shutdown_complete_slot(msg);
    });

    barrier("complex-choreo-setup", k_group_name);

    for (const auto& phase : k_plan) {
        for (int round = 0; round < phase.rounds; ++round) {
            const auto token = make_token(phase.phase, round);
            const auto pre = barrier_name("pre", token);
            const auto post = barrier_name("post", token);

            barrier(pre, k_group_name);

            {
                std::lock_guard<std::mutex> lk(conductor_state().mutex);
                conductor_state().expected_token = token;
                conductor_state().awaiting_summary = true;
            }

            Phase_command cmd;
            cmd.phase = phase.phase;
            cmd.round = round;
            cmd.token = token;
            cmd.expected_workers = k_worker_count;
            sintra::world() << cmd;

            wait_for_summary(token);
            barrier(post, k_group_name);
        }
    }

    sintra::world() << Shutdown{};
    wait_for_shutdown_confirmation();

    barrier("complex-choreography-finished", "_sintra_all_processes");

    auto& state = conductor_state();
    std::lock_guard<std::mutex> lk(state.mutex);
    return state.errors == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// Starter (main) process utilities
// -----------------------------------------------------------------------------

std::vector<Round_result> read_aggregator_results(const std::filesystem::path& path,
                                                 bool& ok,
                                                 int& errors)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open aggregator report for reading");
    }

    std::string status;
    in >> status;
    ok = (status == "ok");
    in >> errors;
    std::size_t rounds = 0;
    in >> rounds;

    std::vector<Round_result> results;
    results.reserve(rounds);

    for (std::size_t i = 0; i < rounds; ++i) {
        Round_result r;
        in >> r.phase >> r.round >> r.count >> r.checksum;
        r.token = make_token(r.phase, r.round);
        results.push_back(r);
    }

    return results;
}

struct Inspector_entry
{
    int phase = 0;
    int round = 0;
    int worker = 0;
    std::uint32_t sequence = 0;
};

std::vector<Inspector_entry> read_inspector_entries(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open inspector report for reading");
    }

    std::size_t entries = 0;
    in >> entries;
    std::vector<Inspector_entry> result;
    result.reserve(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        Inspector_entry entry;
        in >> entry.phase >> entry.round >> entry.worker >> entry.sequence;
        result.push_back(entry);
    }
    return result;
}

bool validate_reports(const std::filesystem::path& dir)
{
    const auto aggregator_path = dir / "aggregator_report.txt";
    const auto inspector_path = dir / "inspector_report.txt";

    bool aggregator_ok = false;
    int aggregator_errors = 0;
    const auto aggregator_results =
        read_aggregator_results(aggregator_path, aggregator_ok, aggregator_errors);

    if (!aggregator_ok || aggregator_errors != 0) {
        return false;
    }

    std::vector<Inspector_entry> inspector_entries =
        read_inspector_entries(inspector_path);

    const std::size_t expected_rounds = [] {
        std::size_t total = 0;
        for (const auto& phase : k_plan) {
            total += static_cast<std::size_t>(phase.rounds);
        }
        return total;
    }();

    if (aggregator_results.size() != expected_rounds) {
        return false;
    }

    const std::size_t expected_messages = expected_rounds * k_worker_count;
    if (inspector_entries.size() != expected_messages) {
        return false;
    }

    std::size_t inspector_index = 0;
    std::size_t aggregator_index = 0;
    for (const auto& phase : k_plan) {
        for (int round = 0; round < phase.rounds; ++round) {
            const auto expected_checksum = expected_round_checksum(phase.phase, round);

            const auto& aggregator_round = aggregator_results[aggregator_index++];
            if (aggregator_round.phase != phase.phase ||
                aggregator_round.round != round ||
                aggregator_round.count != k_worker_count ||
                aggregator_round.checksum != expected_checksum) {
                return false;
            }

            std::array<bool, k_worker_count> worker_seen{};
            for (int worker = 0; worker < k_worker_count; ++worker) {
                const auto& entry = inspector_entries[inspector_index++];
                if (entry.phase != phase.phase || entry.round != round) {
                    return false;
                }
                if (entry.worker < 0 || entry.worker >= k_worker_count) {
                    return false;
                }
                if (worker_seen[entry.worker]) {
                    return false;
                }
                worker_seen[entry.worker] = true;
                const auto expected_seq = compute_sequence_id(
                    entry.worker, entry.phase, entry.round);
                if (entry.sequence != expected_seq) {
                    return false;
                }
            }
        }
    }

    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_COMPLEX_CHOREO_DIR", "complex_choreography_stress");
    const auto shared_dir = shared.path();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_conductor);
    processes.emplace_back(process_aggregator);
    processes.emplace_back(process_inspector);
    processes.emplace_back(process_worker0);
    processes.emplace_back(process_worker1);
    processes.emplace_back(process_worker2);
    processes.emplace_back(process_worker3);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("complex-choreography-finished", "_sintra_all_processes");
    }

    sintra::finalize();

    int exit_code = 0;

    if (!is_spawned) {
        bool success = false;
        try {
            success = validate_reports(shared_dir);
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "validation failed: %s\n", e.what());
            success = false;
        }

        shared.cleanup();
        exit_code = success ? 0 : 1;
    }

    return exit_code;
}

