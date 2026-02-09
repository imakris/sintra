//
// Sintra Complex Choreography Test
//
// This test orchestrates a multi-process pipeline with several synchronisation
// points and deliberately complicated message exchange patterns.  It attempts to
// surface subtle race conditions by combining:
//   * Multiple barrier phases per round (setup + processing fence completion)
//   * Randomised worker delays while dispatching many per-round micro tasks
//   * Aggregator/validator handshake that must observe every contribution
//   * Failure propagation through explicit Round_advance status messages
//
// The scenario involves the following actors:
//   - Conductor: drives the rounds, emits Kickoff messages with per-round seeds,
//                validates Round_advance reports, and writes the final result.
//   - Workers (3): respond to Kickoff by sending a burst of Micro_task messages
//                  and announcing completion with Worker_done.
//   - Aggregator: collects Micro_task traffic, verifies per-worker totals, and
//                  forwards summary Validation results.
//   - Verifier: consumes Validation reports, cross-checks payload integrity, and
//               emits Round_advance broadcasts containing per-worker checksums.
//
// Each round requires successful completion of all stages; any discrepancy is
// propagated as a failed Round_advance.  The conductor records the first failure
// and terminates the choreography with a Stop broadcast so that processes exit
// without deadlocking on barriers.
//
#include <sintra/sintra.h>

#include "test_choreography_utils.h"
#include "test_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t k_worker_count = 3;
constexpr std::size_t k_tasks_per_worker = 6;
constexpr int k_rounds = 7;
constexpr std::chrono::seconds k_wait_timeout{20};
constexpr std::chrono::milliseconds k_min_worker_delay{1};
constexpr std::chrono::milliseconds k_max_worker_delay{6};
constexpr std::string_view k_barrier_group = "_sintra_external_processes";
constexpr std::string_view k_final_barrier = "complex-choreography-finish";
struct Kickoff
{
    int round;
    std::uint64_t seed;
};

struct Micro_task
{
    int round;
    int worker_id;
    int step;
    std::uint64_t payload;
};

struct Worker_done
{
    int round;
    int worker_id;
    int contributions;
};

struct Validation
{
    int round;
    int worker_id;
    int contributions;
    std::uint64_t xor_checksum;
    std::uint64_t sum_checksum;
};

struct Round_advance
{
    int round;
    bool success;
    std::array<std::uint64_t, k_worker_count> checksums;
};

struct Stop
{
    bool due_to_failure;
};

std::uint64_t rotl64(std::uint64_t value, unsigned int shift)
{
    shift %= 64U;
    if (shift == 0U) {
        return value;
    }
    return (value << shift) | (value >> (64U - shift));
}

std::uint64_t compute_payload(int round, int worker_id, int step, std::uint64_t seed)
{
    std::uint64_t base = seed ^ (static_cast<std::uint64_t>(round + 1) * 0x9E3779B97F4A7C15ULL);
    base ^= static_cast<std::uint64_t>(worker_id + 3) * 0xD1B54A32D192ED03ULL;
    base += static_cast<std::uint64_t>(step + 1) * 0x94D049BB133111EBULL;
    const unsigned rotate = static_cast<unsigned>((step + worker_id + 1) * 5 % 63);
    base = rotl64(base, rotate + 1U);
    base ^= static_cast<std::uint64_t>((round + 5) * (worker_id + 11)) * 0xA24BAED4963EE407ULL;
    return base;
}

std::uint64_t expected_worker_xor(int round, int worker_id, std::uint64_t seed)
{
    std::uint64_t result = 0;
    for (int step = 0; step < static_cast<int>(k_tasks_per_worker); ++step) {
        result ^= compute_payload(round, worker_id, step, seed);
    }
    return result;
}

std::uint64_t expected_worker_sum(int round, int worker_id, std::uint64_t seed)
{
    std::uint64_t result = 0;
    for (int step = 0; step < static_cast<int>(k_tasks_per_worker); ++step) {
        result += compute_payload(round, worker_id, step, seed);
    }
    return result;
}

struct Aggregator_worker_state
{
    int contributions = 0;
    std::uint64_t xor_checksum = 0;
    std::uint64_t sum_checksum = 0;
    bool done = false;
};

struct Aggregator_state
{
    int round = -1;
    bool seed_ready = false;
    std::uint64_t seed = 0;
    bool ready_to_validate = false;
    std::array<Aggregator_worker_state, k_worker_count> workers{};
};

struct Verifier_worker_state
{
    bool received = false;
    int contributions = 0;
    std::uint64_t xor_checksum = 0;
    std::uint64_t sum_checksum = 0;
};

std::uint64_t make_round_seed(int round)
{
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t extra = counter.fetch_add(0x9E3779B97F4A7C15ULL);
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid = static_cast<std::uint64_t>(sintra::test::get_pid());
    return now ^ (pid << 32U) ^ extra ^ static_cast<std::uint64_t>((round + 1) * 0xD1B54A32D192ED03ULL);
}

// ---------------------------------------------------------------------------
// Conductor process
// ---------------------------------------------------------------------------

int conductor_process()
{
    sintra::test::Shared_directory shared("SINTRA_COMPLEX_TEST_DIR", "complex_choreography");
    const auto shared_dir = shared.path();
    const auto result_path = shared_dir / "result.txt";

    std::mutex advance_mutex;
    std::condition_variable advance_cv;
    int last_completed_round = -1;
    std::array<std::uint64_t, k_worker_count> last_checksums{};
    bool failure_observed = false;
    bool stop_requested = false;

    auto advance_slot = [&](const Round_advance& advance) {
        if (advance.round < 0 || advance.round >= k_rounds) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(advance_mutex);
            if (advance.round > last_completed_round) {
                last_completed_round = advance.round;
                last_checksums = advance.checksums;
            }
            if (!advance.success) {
                failure_observed = true;
            }
        }
        advance_cv.notify_all();
    };

    auto stop_slot = [&](const Stop& stop) {
        std::lock_guard<std::mutex> lk(advance_mutex);
        stop_requested = true;
        failure_observed = failure_observed || stop.due_to_failure;
        advance_cv.notify_all();
    };

    sintra::activate_slot(advance_slot);
    sintra::activate_slot(stop_slot);

    const std::string group(k_barrier_group);
    bool local_failure = false;
    bool stop_broadcasted = false;

    for (int round = 0; round < k_rounds && !stop_requested; ++round) {
        const std::uint64_t seed = make_round_seed(round);
        sintra::barrier(sintra::test::make_barrier_name("complex-round", round, "start"), group);

        sintra::world() << Kickoff{round, seed};

        std::unique_lock<std::mutex> lk(advance_mutex);
        const bool completed = advance_cv.wait_for(
            lk, k_wait_timeout, [&] { return stop_requested || last_completed_round >= round; });
        if (!completed) {
            local_failure = true;
        }

        if (last_completed_round >= round) {
            for (std::size_t worker = 0; worker < k_worker_count; ++worker) {
                const auto expected = expected_worker_xor(round, static_cast<int>(worker), seed);
                if (last_checksums[worker] != expected) {
                    local_failure = true;
                }
            }
        }
        lk.unlock();

        // Broadcast Stop immediately when failure is detected, BEFORE entering
        // the processing fence barrier. This ensures all processes see the stop
        // signal while still in the current round and won't proceed to the next
        // round's start barrier, which would cause a deadlock.
        if (!stop_broadcasted && (local_failure || failure_observed)) {
            sintra::world() << Stop{true};
            stop_broadcasted = true;
        }

        sintra::barrier<sintra::processing_fence_t>(
            sintra::test::make_barrier_name("complex-round", round, "complete"),
            group);

        if (local_failure) {
            break;
        }
    }

    const bool due_to_failure = local_failure || failure_observed;
    // Broadcast Stop again if not already sent (for the success case) or to ensure
    // all processes have received it
    if (!stop_broadcasted) {
        sintra::world() << Stop{due_to_failure};
    }

    std::vector<std::string> lines;
    lines.push_back(due_to_failure ? "fail" : "ok");
    lines.push_back(std::to_string(std::max(last_completed_round, -1)));
    lines.push_back(due_to_failure ? "failure" : "success");
    sintra::test::write_lines(result_path, lines);

    sintra::barrier(std::string(k_final_barrier), "_sintra_all_processes");
    return due_to_failure ? 1 : 0;
}
// ---------------------------------------------------------------------------
// Worker processes
// ---------------------------------------------------------------------------

int worker_process_impl(int worker_index)
{
    std::atomic<bool> stop_requested{false};
    std::atomic<int> active_round{-1};

    std::mutex state_mutex;
    std::condition_variable kickoff_cv;
    std::condition_variable completion_cv;
    bool kickoff_seen = false;
    bool round_completed = false;

    std::mt19937 rng(static_cast<std::uint32_t>(
        make_round_seed(worker_index) ^ static_cast<std::uint64_t>(worker_index * 0x12345)));
    std::uniform_int_distribution<int> delay_dist(k_min_worker_delay.count(), k_max_worker_delay.count());

    auto kickoff_slot = [&](const Kickoff& kickoff) {
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        const int expected = active_round.load(std::memory_order_acquire);
        if (kickoff.round != expected) {
            return;
        }

        std::array<std::uint64_t, k_tasks_per_worker> payloads{};
        for (int step = 0; step < static_cast<int>(k_tasks_per_worker); ++step) {
            const auto payload = compute_payload(kickoff.round, worker_index, step, kickoff.seed);
            payloads[step] = payload;
            sintra::world() << Micro_task{kickoff.round, worker_index, step, payload};
            if ((step + worker_index) % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(rng)));
            }
            else {
                std::this_thread::yield();
            }
        }

        sintra::world() << Worker_done{kickoff.round, worker_index, static_cast<int>(k_tasks_per_worker)};

        {
            std::lock_guard<std::mutex> lk(state_mutex);
            kickoff_seen = true;
        }
        kickoff_cv.notify_all();
    };

    auto advance_slot = [&](const Round_advance& advance) {
        if (advance.round != active_round.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            round_completed = true;
        }
        if (!advance.success) {
            stop_requested.store(true);
        }
        completion_cv.notify_all();
    };

    auto stop_slot = [&](const Stop&) {
        stop_requested.store(true);
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            kickoff_seen = true;
            round_completed = true;
        }
        kickoff_cv.notify_all();
        completion_cv.notify_all();
    };

    sintra::activate_slot(kickoff_slot);
    sintra::activate_slot(advance_slot);
    sintra::activate_slot(stop_slot);

    const std::string group(k_barrier_group);

    for (int round = 0; round < k_rounds && !stop_requested; ++round) {
        active_round.store(round);
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            kickoff_seen = false;
            round_completed = false;
        }

        sintra::barrier(sintra::test::make_barrier_name("complex-round", round, "start"), group);

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            kickoff_cv.wait_for(lk, k_wait_timeout, [&] {
                return kickoff_seen || stop_requested.load(std::memory_order_acquire);
            });
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            // The stop signal means the conductor is winding down, but the worker has not
            // entered the library's draining path. Other participants may already be parked on
            // the processing fence for this round, so the worker still has to rendezvous before
            // exiting to avoid leaving the barrier short-handed.
            sintra::barrier<sintra::processing_fence_t>(
                sintra::test::make_barrier_name("complex-round", round, "complete"),
                group);
            break;
        }

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            completion_cv.wait_for(lk, k_wait_timeout, [&] {
                return round_completed || stop_requested.load(std::memory_order_acquire);
            });
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            // See comment above: we must still participate in the in-flight processing fence
            // before breaking out once a stop is observed.
            sintra::barrier<sintra::processing_fence_t>(
                sintra::test::make_barrier_name("complex-round", round, "complete"),
                group);
            break;
        }

        sintra::barrier<sintra::processing_fence_t>(
            sintra::test::make_barrier_name("complex-round", round, "complete"),
            group);
    }

    sintra::barrier(std::string(k_final_barrier), "_sintra_all_processes");
    return 0;
}

int worker_process0() { return worker_process_impl(0); }
int worker_process1() { return worker_process_impl(1); }
int worker_process2() { return worker_process_impl(2); }

// ---------------------------------------------------------------------------
// Aggregator process
// ---------------------------------------------------------------------------

int aggregator_process()
{
    Aggregator_state state;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> failure{false};

    auto kickoff_slot = [&](const Kickoff& kickoff) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (kickoff.round == state.round) {
            state.seed = kickoff.seed;
            state.seed_ready = true;
        }
    };

    auto micro_slot = [&](const Micro_task& task) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (task.round != state.round || task.worker_id < 0 ||
            task.worker_id >= static_cast<int>(k_worker_count))
        {
            failure.store(true);
            return;
        }
        auto& worker = state.workers[static_cast<std::size_t>(task.worker_id)];
        worker.contributions += 1;
        worker.xor_checksum ^= task.payload;
        worker.sum_checksum += task.payload;
    };

    auto done_slot = [&](const Worker_done& done) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (done.round != state.round || done.worker_id < 0 ||
            done.worker_id >= static_cast<int>(k_worker_count))
        {
            failure.store(true);
            return;
        }
        auto& worker = state.workers[static_cast<std::size_t>(done.worker_id)];
        worker.done = true;
        worker.contributions = std::max(worker.contributions, done.contributions);
        const bool all_done = std::all_of(state.workers.begin(), state.workers.end(),
            [](const Aggregator_worker_state& w) { return w.done; });
        if (all_done) {
            state.ready_to_validate = true;
            state_cv.notify_one();
        }
    };

    auto stop_slot = [&](const Stop& stop) {
        stop_requested.store(true);
        failure.store(failure.load() || stop.due_to_failure);
        state_cv.notify_all();
    };

    sintra::activate_slot(kickoff_slot);
    sintra::activate_slot(micro_slot);
    sintra::activate_slot(done_slot);
    sintra::activate_slot(stop_slot);

    const std::string group(k_barrier_group);

    for (int round = 0; round < k_rounds && !stop_requested; ++round) {
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            state.round = round;
            state.seed_ready = false;
            state.ready_to_validate = false;
            for (auto& worker : state.workers) {
                worker = Aggregator_worker_state{};
            }
        }

        sintra::barrier(sintra::test::make_barrier_name("complex-round", round, "start"), group);

        std::array<Aggregator_worker_state, k_worker_count> snapshot{};
        bool local_failure = false;
        std::uint64_t seed = 0;

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool ready = state_cv.wait_for(lk, k_wait_timeout, [&] {
                return state.ready_to_validate || stop_requested;
            });
            if (!ready) {
                local_failure = true;
            }
            seed = state.seed;
            snapshot = state.workers;
            if (!state.seed_ready) {
                local_failure = true;
            }
        }

        for (std::size_t idx = 0; idx < k_worker_count; ++idx) {
            auto& worker = snapshot[idx];
            if (worker.contributions != static_cast<int>(k_tasks_per_worker)) {
                local_failure = true;
            }
            if (worker.done == false) {
                local_failure = true;
            }
            const auto expected_xor_val = expected_worker_xor(round, static_cast<int>(idx), seed);
            const auto expected_sum_val = expected_worker_sum(round, static_cast<int>(idx), seed);
            if (worker.xor_checksum != expected_xor_val || worker.sum_checksum != expected_sum_val) {
                local_failure = true;
            }
        }

        failure.store(failure.load() || local_failure);

        for (std::size_t idx = 0; idx < k_worker_count; ++idx) {
            const auto& worker = snapshot[idx];
            sintra::world() << Validation{round, static_cast<int>(idx), worker.contributions,
                                          worker.xor_checksum, worker.sum_checksum};
        }

        sintra::barrier<sintra::processing_fence_t>(
            sintra::test::make_barrier_name("complex-round", round, "complete"),
            group);
    }

    sintra::barrier(std::string(k_final_barrier), "_sintra_all_processes");
    return failure ? 1 : 0;
}
// ---------------------------------------------------------------------------
// Verifier process
// ---------------------------------------------------------------------------

int verifier_process()
{
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> failure{false};

    int current_round = -1;
    std::array<std::uint64_t, k_rounds> seeds{};
    std::array<bool, k_rounds> seed_ready{};
    std::array<Verifier_worker_state, k_worker_count> worker_state{};
    int validations_received = 0;
    bool ready_to_advance = false;

    auto kickoff_slot = [&](const Kickoff& kickoff) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (kickoff.round >= 0 && kickoff.round < k_rounds) {
            seeds[static_cast<std::size_t>(kickoff.round)] = kickoff.seed;
            seed_ready[static_cast<std::size_t>(kickoff.round)] = true;
            state_cv.notify_all();
        }
    };

    auto validation_slot = [&](const Validation& validation) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (validation.round != current_round || validation.worker_id < 0 ||
            validation.worker_id >= static_cast<int>(k_worker_count))
        {
            failure.store(true);
            return;
        }
        auto& worker = worker_state[static_cast<std::size_t>(validation.worker_id)];
        worker.received = true;
        worker.contributions = validation.contributions;
        worker.xor_checksum = validation.xor_checksum;
        worker.sum_checksum = validation.sum_checksum;
        ++validations_received;
        if (validations_received >= static_cast<int>(k_worker_count)) {
            ready_to_advance = true;
            state_cv.notify_all();
        }
    };

    auto stop_slot = [&](const Stop& stop) {
        stop_requested.store(true);
        failure.store(failure.load() || stop.due_to_failure);
        state_cv.notify_all();
    };

    sintra::activate_slot(kickoff_slot);
    sintra::activate_slot(validation_slot);
    sintra::activate_slot(stop_slot);

    const std::string group(k_barrier_group);

    for (int round = 0; round < k_rounds && !stop_requested; ++round) {
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            current_round = round;
            validations_received = 0;
            ready_to_advance = false;
            for (auto& worker : worker_state) {
                worker = Verifier_worker_state{};
            }
        }

        sintra::barrier(sintra::test::make_barrier_name("complex-round", round, "start"), group);

        std::uint64_t seed = 0;
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool seed_ok = state_cv.wait_for(lk, k_wait_timeout, [&] {
                return seed_ready[static_cast<std::size_t>(round)] || stop_requested;
            });
            if (!seed_ok) {
                failure.store(true);
            }
            if (seed_ready[static_cast<std::size_t>(round)]) {
                seed = seeds[static_cast<std::size_t>(round)];
            }
        }

        std::array<std::uint64_t, k_worker_count> checksums{};
        bool success = true;

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool ready = state_cv.wait_for(lk, k_wait_timeout, [&] {
                return ready_to_advance || stop_requested;
            });
            if (!ready) {
                success = false;
            }
            for (std::size_t idx = 0; idx < k_worker_count; ++idx) {
                const auto& worker = worker_state[idx];
                if (!worker.received || worker.contributions != static_cast<int>(k_tasks_per_worker)) {
                    success = false;
                }
                const auto expected_xor = expected_worker_xor(round, static_cast<int>(idx), seed);
                const auto expected_sum_val = expected_worker_sum(round, static_cast<int>(idx), seed);
                if (worker.xor_checksum != expected_xor || worker.sum_checksum != expected_sum_val) {
                    success = false;
                }
                checksums[idx] = worker.xor_checksum;
            }
        }

        success = success && !failure.load(std::memory_order_acquire);

        sintra::world() << Round_advance{round, success, checksums};
        if (!success) {
            stop_requested.store(true);
        }

        sintra::barrier<sintra::processing_fence_t>(
            sintra::test::make_barrier_name("complex-round", round, "complete"),
            group);
    }

    sintra::barrier(std::string(k_final_barrier), "_sintra_all_processes");
    return failure ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_COMPLEX_TEST_DIR",
        "complex_choreography",
        {conductor_process,
         aggregator_process,
         verifier_process,
         worker_process0,
         worker_process1,
         worker_process2},
        [](const std::filesystem::path& shared_dir) {
            std::filesystem::remove(shared_dir / "result.txt");
        },
        [](const std::filesystem::path&) { return 0; },
        [](const std::filesystem::path& shared_dir) {
            const auto result_path = shared_dir / "result.txt";
            std::ifstream in(result_path, std::ios::binary);
            if (!in) {
                return 1;
            }
            std::string status;
            int completed_rounds = -1;
            std::string failure_state;
            std::getline(in, status);
            in >> completed_rounds;
            std::getline(in >> std::ws, failure_state);

            const bool ok = (status == "ok") && (completed_rounds >= k_rounds - 1) &&
                            (failure_state == "success");
            return ok ? 0 : 1;
        },
        k_final_barrier.data());
}
