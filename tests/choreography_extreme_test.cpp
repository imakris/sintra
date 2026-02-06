//
// Sintra Extreme Choreography Test
//
// This test deliberately crafts an overly intricate multi-process
// coordination scenario.  It chains together multiple synchronization
// patterns - readiness handshakes, staged production checkpoints,
// chaos probes that demand acknowledgements, and final audit barriers -
// in order to exercise Sintra's message routing and barrier logic in a
// dense, failure-prone arrangement.  While the scenario is contrived, it
// is logically consistent and each step can complete successfully if the
// synchronization primitives behave correctly.
//
// Process layout:
//   * Conductor - orchestrates the phase plan, verifies checkpoints and
//                  audit responses, and emits the global terminate signal.
//   * Aggregator - validates producer payloads, enforces per-round
//                  checkpoints, waits for chaos completion, and emits an
//                  audit outcome for every phase.
//   * Producers (3) - emit deterministic data for each round, wait for
//                     aggregator checkpoints before progressing, and
//                     respond to chaos probes.
//   * Chaos agent - injects probe messages for every phase and waits for
//                   acknowledgements from every producer before allowing
//                   the aggregator to finish.
//
// Every participant relies on multi-way condition variables and barriers
// to finish a phase.  The root process verifies the resulting CSV logs to
// ensure all invariants held: totals match expectations, checkpoint
// sequences are complete and ordered, chaos acknowledgements were
// honoured, and audits declared success.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int k_producer_count = 3;
constexpr int k_phase_count = 4;

struct Phase_plan {
    int rounds;
    int chaos_tokens;
};

constexpr std::array<Phase_plan, k_phase_count> k_phase_plans{{
    {3, 4},   // Phase 0
    {5, 6},   // Phase 1
    {4, 5},   // Phase 2
    {6, 7},   // Phase 3
}};

constexpr int k_participant_slot_aggregator = k_producer_count;
constexpr int k_participant_slot_chaos = k_producer_count + 1;
constexpr int k_participant_slot_count = k_producer_count + 2;

struct Phase_announcement {
    int phase;
    int rounds;
    int chaos_tokens;
};

struct Phase_ready {
    int phase;
    int participant_slot;
};

struct Start_phase {
    int phase;
};

struct Work_result {
    int phase;
    int producer;
    int round;
    int value;
};

struct Round_checkpoint {
    int phase;
    int round;
    int total;
};

struct Audit_outcome {
    int phase;
    bool ok;
    int observed_total;
};

struct Phase_complete {
    int phase;
};

struct Chaos_probe {
    int phase;
    int token;
};

struct Chaos_reply {
    int phase;
    int token;
    int producer;
};

struct Chaos_complete {
    int phase;
};

struct Terminate {};

int compute_value(int phase, int producer, int round)
{
    int base = (phase + 1) * 1000 + (producer + 1) * 100 + (round + 1) * 11;
    if (((phase + producer + round) & 1) == 0) {
        base += 17;
    }
    if ((round % 3) == 0) {
        base += (phase + 1) * 3;
    }
    if ((producer % 2) == 1) {
        base -= (phase + 1) * 5;
    }
    return base;
}

int expected_total_for_phase(int phase)
{
    const auto& plan = k_phase_plans[phase];
    int total = 0;
    for (int round = 0; round < plan.rounds; ++round) {
        for (int producer = 0; producer < k_producer_count; ++producer) {
            total += compute_value(phase, producer, round);
        }
    }
    return total;
}

struct Phase_summary {
    int phase = -1;
    int expected_total = 0;
    int audit_total = 0;
    bool audit_ok = false;
    bool sequential_rounds_ok = true;
    std::vector<int> rounds;
    std::vector<int> partial_totals;
};

struct Phase_observation {
    int phase = -1;
    int expected_total = 0;
    int observed_total = 0;
    bool ok = false;
    bool chaos_complete = false;
    int rounds = 0;
    bool configuration_ok = true;
    bool audit_emitted = false;
};

// -------------------------------------------------------------------------
// Conductor process
// -------------------------------------------------------------------------

int process_conductor()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "choreography_extreme");
    const auto shared_dir = shared.path();

    std::array<Phase_summary, k_phase_count> summaries{};
    for (int phase = 0; phase < k_phase_count; ++phase) {
        summaries[phase].phase = phase;
        summaries[phase].expected_total = expected_total_for_phase(phase);
    }

    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    int ready_phase = -1;
    std::array<bool, k_participant_slot_count> ready_flags{};

    std::mutex audit_mutex;
    std::condition_variable audit_cv;
    int audit_phase = -1;
    bool audit_received = false;
    bool audit_ok = false;
    int audit_total = 0;

    std::mutex summary_mutex;

    sintra::activate_slot([&](const Phase_ready& msg) {
        std::lock_guard<std::mutex> lk(ready_mutex);
        if (msg.phase == ready_phase && msg.participant_slot >= 0 &&
            msg.participant_slot < static_cast<int>(ready_flags.size())) {
            if (!ready_flags[msg.participant_slot]) {
                ready_flags[msg.participant_slot] = true;
                ready_cv.notify_all();
            }
        }
    });

    sintra::activate_slot([&](const Round_checkpoint& msg) {
        if (msg.phase < 0 || msg.phase >= k_phase_count) {
            return;
        }
        std::lock_guard<std::mutex> lk(summary_mutex);
        auto& summary = summaries[msg.phase];
        if (!summary.rounds.empty()) {
            if (msg.round <= summary.rounds.back()) {
                summary.sequential_rounds_ok = false;
            }
            if (msg.round != static_cast<int>(summary.rounds.size())) {
                summary.sequential_rounds_ok = false;
            }
        }
        else
        if (msg.round != 0) {
            summary.sequential_rounds_ok = false;
        }
        summary.rounds.push_back(msg.round);
        summary.partial_totals.push_back(msg.total);
    });

    sintra::activate_slot([&](const Audit_outcome& msg) {
        std::lock_guard<std::mutex> lk1(summary_mutex);
        if (msg.phase >= 0 && msg.phase < k_phase_count) {
            auto& summary = summaries[msg.phase];
            summary.audit_total = msg.observed_total;
            summary.audit_ok = msg.ok;
        }

        std::lock_guard<std::mutex> lk2(audit_mutex);
        if (msg.phase == audit_phase) {
            audit_ok = msg.ok;
            audit_total = msg.observed_total;
            audit_received = true;
            audit_cv.notify_all();
        }
    });

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-slots-ready");

    for (int phase = 0; phase < k_phase_count; ++phase) {
        const auto& plan = k_phase_plans[phase];

        {
            std::lock_guard<std::mutex> lk(summary_mutex);
            if (plan.rounds != static_cast<int>(k_phase_plans[phase].rounds)) {
                summaries[phase].sequential_rounds_ok = false;
            }
        }

        {
            std::lock_guard<std::mutex> lk(ready_mutex);
            ready_phase = phase;
            ready_flags.fill(false);
        }

        sintra::world() << Phase_announcement{phase, plan.rounds, plan.chaos_tokens};

        {
            std::unique_lock<std::mutex> lk(ready_mutex);
            ready_cv.wait(lk, [&] {
                return std::all_of(ready_flags.begin(), ready_flags.end(), [](bool v) { return v; });
            });
        }

        {
            std::lock_guard<std::mutex> lk(audit_mutex);
            audit_phase = phase;
            audit_received = false;
            audit_ok = false;
            audit_total = 0;
        }

        sintra::world() << Start_phase{phase};

        {
            std::unique_lock<std::mutex> lk(audit_mutex);
            audit_cv.wait(lk, [&] { return audit_received && audit_phase == phase; });
        }

        bool phase_ok = audit_ok && (audit_total == summaries[phase].expected_total);
        if (!phase_ok) {
            std::lock_guard<std::mutex> lk(summary_mutex);
            summaries[phase].audit_ok = false;
        }

        sintra::world() << Phase_complete{phase};
    }

    sintra::world() << Terminate{};

    sintra::deactivate_all_slots();

    const auto summary_path = shared_dir / "conductor_summary.csv";
    std::ofstream summary_out(summary_path, std::ios::binary | std::ios::trunc);
    for (const auto& summary : summaries) {
        summary_out << "phase," << summary.phase
                    << ",expected," << summary.expected_total
                    << ",audit," << summary.audit_total
                    << ",ok," << (summary.audit_ok ? 1 : 0)
                    << ",sequential," << (summary.sequential_rounds_ok ? 1 : 0)
                    << ",checkpoints," << summary.rounds.size() << '\n';

        summary_out << "rounds," << summary.phase;
        for (int value : summary.rounds) {
            summary_out << ',' << value;
        }
        summary_out << '\n';

        summary_out << "totals," << summary.phase;
        for (int value : summary.partial_totals) {
            summary_out << ',' << value;
        }
        summary_out << '\n';
    }
    summary_out.flush();

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-finished", "_sintra_all_processes");
    return 0;
}

// -------------------------------------------------------------------------
// Aggregator process
// -------------------------------------------------------------------------

int process_aggregator()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "choreography_extreme");
    const auto shared_dir = shared.path();

    std::array<Phase_observation, k_phase_count> observations{};
    for (int phase = 0; phase < k_phase_count; ++phase) {
        observations[phase].phase = phase;
        observations[phase].expected_total = expected_total_for_phase(phase);
        observations[phase].ok = false;
        observations[phase].rounds = 0;
        observations[phase].configuration_ok = true;
        observations[phase].chaos_complete = false;
        observations[phase].observed_total = 0;
        observations[phase].audit_emitted = false;
    }

    std::mutex state_mutex;
    std::condition_variable exit_cv;
    bool terminate_requested = false;

    int current_phase = -1;
    int current_rounds = 0;
    bool active = false;

    struct Round_data {
        std::array<bool, k_producer_count> received{};
        bool checkpoint_sent = false;
    };

    std::vector<Round_data> round_state;
    int contributions_count = 0;
    int total_sum = 0;
    bool chaos_done = false;
    bool audit_sent = false;
    bool phase_ok = true;

    auto prepare_audit_locked = [&]() -> std::optional<Audit_outcome> {
        if (!active) {
            return std::nullopt;
        }
        if (current_phase < 0 || current_phase >= k_phase_count) {
            return std::nullopt;
        }
        const int expected_contributions = current_rounds * k_producer_count;
        if (contributions_count == expected_contributions && chaos_done && !audit_sent) {
            bool ok = phase_ok && (total_sum == observations[current_phase].expected_total) && observations[current_phase].configuration_ok;
            observations[current_phase].observed_total = total_sum;
            observations[current_phase].chaos_complete = true;
            observations[current_phase].ok = ok;
            observations[current_phase].audit_emitted = true;
            audit_sent = true;
            return Audit_outcome{current_phase, ok, total_sum};
        }
        return std::nullopt;
    };

    sintra::activate_slot([&](const Phase_announcement& msg) {
        bool send_ready = false;
        if (msg.phase < 0 || msg.phase >= k_phase_count) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            current_phase = msg.phase;
            current_rounds = msg.rounds;
            active = false;
            round_state.assign(current_rounds, {});
            contributions_count = 0;
            total_sum = 0;
            chaos_done = false;
            audit_sent = false;
            phase_ok = true;
            observations[msg.phase].rounds = msg.rounds;
            observations[msg.phase].configuration_ok = (msg.rounds == k_phase_plans[msg.phase].rounds);
            observations[msg.phase].chaos_complete = false;
            observations[msg.phase].ok = true;
            observations[msg.phase].observed_total = 0;
            observations[msg.phase].audit_emitted = false;
            send_ready = true;
        }
        if (send_ready) {
            sintra::world() << Phase_ready{msg.phase, k_participant_slot_aggregator};
        }
    });

    sintra::activate_slot([&](const Start_phase& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase) {
            active = true;
        }
    });

    sintra::activate_slot([&](const Work_result& msg) {
        bool emit_checkpoint = false;
        Round_checkpoint checkpoint{};
        std::optional<Audit_outcome> audit;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            if (msg.phase != current_phase) {
                return;
            }
            if (!active) {
                phase_ok = false;
            }
            if (msg.producer < 0 || msg.producer >= k_producer_count) {
                phase_ok = false;
                return;
            }
            if (msg.round < 0 || msg.round >= current_rounds) {
                phase_ok = false;
                return;
            }
            if (msg.value != compute_value(msg.phase, msg.producer, msg.round)) {
                phase_ok = false;
            }
            Round_data& data = round_state[msg.round];
            if (data.received[msg.producer]) {
                phase_ok = false;
                return;
            }
            data.received[msg.producer] = true;
            ++contributions_count;
            total_sum += msg.value;

            bool round_complete = std::all_of(data.received.begin(), data.received.end(), [](bool v) { return v; });
            if (round_complete && !data.checkpoint_sent) {
                data.checkpoint_sent = true;
                emit_checkpoint = true;
                checkpoint = Round_checkpoint{current_phase, msg.round, total_sum};
            }

            audit = prepare_audit_locked();
        }
        if (emit_checkpoint) {
            sintra::world() << checkpoint;
        }
        if (audit.has_value()) {
            sintra::world() << *audit;
        }
    });

    sintra::activate_slot([&](const Chaos_complete& msg) {
        std::optional<Audit_outcome> audit;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            if (msg.phase == current_phase) {
                chaos_done = true;
                observations[current_phase].chaos_complete = true;
                audit = prepare_audit_locked();
            }
        }
        if (audit.has_value()) {
            sintra::world() << *audit;
        }
    });

    sintra::activate_slot([&](const Phase_complete& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase) {
            active = false;
        }
    });

    sintra::activate_slot([&](const Terminate&) {
        std::lock_guard<std::mutex> lk(state_mutex);
        terminate_requested = true;
        exit_cv.notify_all();
    });

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-slots-ready");

    {
        std::unique_lock<std::mutex> lk(state_mutex);
        exit_cv.wait(lk, [&] { return terminate_requested; });
    }

    sintra::deactivate_all_slots();

    const auto aggregator_path = shared_dir / "aggregator_results.csv";
    std::ofstream aggregator_out(aggregator_path, std::ios::binary | std::ios::trunc);
    for (const auto& observation : observations) {
        const bool ok = observation.ok && observation.audit_emitted && observation.chaos_complete && observation.configuration_ok && observation.observed_total == observation.expected_total;
        aggregator_out << "phase," << observation.phase
                       << ",expected," << observation.expected_total
                       << ",observed," << observation.observed_total
                       << ",ok," << (ok ? 1 : 0)
                       << ",chaos," << (observation.chaos_complete ? 1 : 0)
                       << ",rounds," << observation.rounds << '\n';
    }
    aggregator_out.flush();

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-finished", "_sintra_all_processes");
    return 0;
}

// -------------------------------------------------------------------------
// Producer processes
// -------------------------------------------------------------------------

int run_producer(int producer_index)
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "choreography_extreme");
    (void)shared;

    std::mutex state_mutex;
    std::condition_variable start_cv;
    std::condition_variable checkpoint_cv;
    std::condition_variable complete_cv;

    int current_phase = -1;
    bool start_ready = false;
    bool terminate_requested = false;
    int last_confirmed_round = -1;
    int announced_rounds = 0;
    int completed_phase = -1;

    sintra::activate_slot([&](const Phase_announcement& msg) {
        if (msg.phase < 0 || msg.phase >= k_phase_count) {
            return;
        }
        bool send_ready = false;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            current_phase = msg.phase;
            start_ready = false;
            terminate_requested = false;
            announced_rounds = msg.rounds;
            send_ready = true;
        }
        if (send_ready) {
            sintra::world() << Phase_ready{msg.phase, producer_index};
        }
    });

    sintra::activate_slot([&](const Start_phase& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase) {
            start_ready = true;
            start_cv.notify_all();
        }
    });

    sintra::activate_slot([&](const Round_checkpoint& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase && msg.round > last_confirmed_round) {
            last_confirmed_round = msg.round;
            checkpoint_cv.notify_all();
        }
    });

    sintra::activate_slot([&](const Phase_complete& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        completed_phase = std::max(completed_phase, msg.phase);
        complete_cv.notify_all();
    });

    sintra::activate_slot([&](const Chaos_probe& msg) {
        bool reply = false;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            reply = (!terminate_requested && msg.phase >= 0 && msg.phase <= current_phase);
        }
        if (reply) {
            sintra::world() << Chaos_reply{msg.phase, msg.token, producer_index};
        }
    });

    sintra::activate_slot([&](const Terminate&) {
        std::lock_guard<std::mutex> lk(state_mutex);
        terminate_requested = true;
        start_cv.notify_all();
        checkpoint_cv.notify_all();
        complete_cv.notify_all();
    });

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-slots-ready");

    for (int phase = 0; phase < k_phase_count; ++phase) {
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            start_cv.wait(lk, [&] { return terminate_requested || (start_ready && current_phase == phase); });
            if (terminate_requested) {
                break;
            }
            last_confirmed_round = -1;
        }

        const int rounds = k_phase_plans[phase].rounds;
        for (int round = 0; round < rounds; ++round) {
            sintra::world() << Work_result{phase, producer_index, round, compute_value(phase, producer_index, round)};

            std::unique_lock<std::mutex> lk(state_mutex);
            checkpoint_cv.wait(lk, [&] {
                return terminate_requested || last_confirmed_round >= round;
            });
            if (terminate_requested) {
                break;
            }
        }

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            complete_cv.wait(lk, [&] {
                return terminate_requested || completed_phase >= phase;
            });
            if (terminate_requested) {
                break;
            }
        }
    }

    sintra::deactivate_all_slots();
    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-finished", "_sintra_all_processes");
    return 0;
}

int process_producer0() { return run_producer(0); }
int process_producer1() { return run_producer(1); }
int process_producer2() { return run_producer(2); }

// -------------------------------------------------------------------------
// Chaos process
// -------------------------------------------------------------------------

int process_chaos()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "choreography_extreme");
    (void)shared;

    std::mutex state_mutex;
    std::condition_variable ready_cv;
    std::condition_variable ack_cv;
    std::condition_variable complete_cv;

    int current_phase = -1;
    int expected_tokens = 0;
    int expected_acks = 0;
    int received_acks = 0;
    bool phase_ready = false;
    bool terminate_requested = false;
    int completed_phase = -1;

    sintra::activate_slot([&](const Phase_announcement& msg) {
        if (msg.phase < 0 || msg.phase >= k_phase_count) {
            return;
        }
        bool send_ready = false;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            current_phase = msg.phase;
            expected_tokens = msg.chaos_tokens;
            expected_acks = msg.chaos_tokens * k_producer_count;
            received_acks = 0;
            phase_ready = false;
            terminate_requested = false;
            send_ready = true;
        }
        if (send_ready) {
            sintra::world() << Phase_ready{msg.phase, k_participant_slot_chaos};
        }
    });

    sintra::activate_slot([&](const Start_phase& msg) {
        std::unique_lock<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase) {
            phase_ready = true;
            ready_cv.notify_all();
        }
    });

    sintra::activate_slot([&](const Chaos_reply& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (msg.phase == current_phase && !terminate_requested) {
            ++received_acks;
            ack_cv.notify_all();
        }
    });

    sintra::activate_slot([&](const Phase_complete& msg) {
        std::lock_guard<std::mutex> lk(state_mutex);
        completed_phase = std::max(completed_phase, msg.phase);
        complete_cv.notify_all();
    });

    sintra::activate_slot([&](const Terminate&) {
        std::lock_guard<std::mutex> lk(state_mutex);
        terminate_requested = true;
        ready_cv.notify_all();
        ack_cv.notify_all();
        complete_cv.notify_all();
    });

    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-slots-ready");

    for (int phase = 0; phase < k_phase_count; ++phase) {
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            ready_cv.wait(lk, [&] { return terminate_requested || (phase_ready && current_phase == phase); });
            if (terminate_requested) {
                break;
            }
        }

        for (int token = 0; token < k_phase_plans[phase].chaos_tokens; ++token) {
            sintra::world() << Chaos_probe{phase, token};
            std::this_thread::sleep_for(std::chrono::microseconds(50 + (phase + token) % 7));
        }

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            ack_cv.wait(lk, [&] {
                return terminate_requested || received_acks >= expected_acks;
            });
            if (terminate_requested) {
                break;
            }
        }

        sintra::world() << Chaos_complete{phase};

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            complete_cv.wait(lk, [&] {
                return terminate_requested || completed_phase >= phase;
            });
            if (terminate_requested) {
                break;
            }
        }
    }

    sintra::deactivate_all_slots();
    sintra::barrier<sintra::rendezvous_t>("extreme-choreo-finished", "_sintra_all_processes");
    return 0;
}

// -------------------------------------------------------------------------
// Root verification helpers
// -------------------------------------------------------------------------

bool verify_aggregator_results(const std::filesystem::path& shared_dir)
{
    const auto path = shared_dir / "aggregator_results.csv";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::array<bool, k_phase_count> seen{};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }
        if (tokens.size() != 12) {
            return false;
        }
        if (tokens[0] != "phase" || tokens[2] != "expected" || tokens[4] != "observed" ||
            tokens[6] != "ok" || tokens[8] != "chaos" || tokens[10] != "rounds") {
            return false;
        }
        int phase = std::stoi(tokens[1]);
        if (phase < 0 || phase >= k_phase_count) {
            return false;
        }
        int expected = std::stoi(tokens[3]);
        int observed = std::stoi(tokens[5]);
        int ok = std::stoi(tokens[7]);
        int chaos = std::stoi(tokens[9]);
        int rounds = std::stoi(tokens[11]);
        if (expected != expected_total_for_phase(phase)) {
            return false;
        }
        if (observed != expected) {
            return false;
        }
        if (ok != 1 || chaos != 1) {
            return false;
        }
        if (rounds != k_phase_plans[phase].rounds) {
            return false;
        }
        seen[phase] = true;
    }

    return std::all_of(seen.begin(), seen.end(), [](bool v) { return v; });
}

bool verify_conductor_summary(const std::filesystem::path& shared_dir)
{
    const auto path = shared_dir / "conductor_summary.csv";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    std::array<bool, k_phase_count> seen{};
    std::string phase_line;
    std::string rounds_line;
    std::string totals_line;

    while (std::getline(in, phase_line)) {
        if (!std::getline(in, rounds_line)) {
            return false;
        }
        if (!std::getline(in, totals_line)) {
            return false;
        }

        auto split = [](const std::string& text) {
            std::vector<std::string> tokens;
            std::stringstream ss(text);
            std::string token;
            while (std::getline(ss, token, ',')) {
                tokens.push_back(token);
            }
            return tokens;
        };

        const auto tokens_phase = split(phase_line);
        const auto tokens_rounds = split(rounds_line);
        const auto tokens_totals = split(totals_line);

        if (tokens_phase.size() != 12 || tokens_rounds.size() < 2 || tokens_totals.size() < 2) {
            return false;
        }

        if (tokens_phase[0] != "phase" || tokens_phase[2] != "expected" ||
            tokens_phase[4] != "audit" || tokens_phase[6] != "ok" ||
            tokens_phase[8] != "sequential" || tokens_phase[10] != "checkpoints") {
            return false;
        }

        if (tokens_rounds[0] != "rounds" || tokens_totals[0] != "totals") {
            return false;
        }

        int phase = std::stoi(tokens_phase[1]);
        if (phase < 0 || phase >= k_phase_count) {
            return false;
        }
        int expected = std::stoi(tokens_phase[3]);
        int audit_total = std::stoi(tokens_phase[5]);
        int ok = std::stoi(tokens_phase[7]);
        int sequential = std::stoi(tokens_phase[9]);
        int checkpoints = std::stoi(tokens_phase[11]);

        if (expected != expected_total_for_phase(phase)) {
            return false;
        }
        if (audit_total != expected || ok != 1 || sequential != 1) {
            return false;
        }

        const int expected_rounds = k_phase_plans[phase].rounds;
        if (checkpoints != expected_rounds) {
            return false;
        }

        if (static_cast<int>(tokens_rounds.size()) != expected_rounds + 2) {
            return false;
        }
        if (static_cast<int>(tokens_totals.size()) != expected_rounds + 2) {
            return false;
        }

        for (int i = 0; i < expected_rounds; ++i) {
            int round_value = std::stoi(tokens_rounds[i + 2]);
            if (round_value != i) {
                return false;
            }
        }

        int last_total = -1;
        for (int i = 0; i < expected_rounds; ++i) {
            int total_value = std::stoi(tokens_totals[i + 2]);
            if (total_value <= last_total) {
                return false;
            }
            last_total = total_value;
        }
        if (last_total != expected) {
            return false;
        }

        seen[phase] = true;
    }

    return std::all_of(seen.begin(), seen.end(), [](bool v) { return v; });
}

// -------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "choreography_extreme");
    const auto shared_dir = shared.path();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_conductor);
    processes.emplace_back(process_aggregator);
    processes.emplace_back(process_producer0);
    processes.emplace_back(process_producer1);
    processes.emplace_back(process_producer2);
    processes.emplace_back(process_chaos);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier<sintra::rendezvous_t>("extreme-choreo-finished", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        bool ok = verify_aggregator_results(shared_dir) && verify_conductor_summary(shared_dir);
        shared.cleanup();
        return ok ? 0 : 1;
    }

    return 0;
}

