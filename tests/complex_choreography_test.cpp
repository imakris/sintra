//
// Sintra Complex Choreography Test
//
// This test orchestrates a multi-process pipeline with several synchronisation
// points and deliberately complicated message exchange patterns.  It attempts to
// surface subtle race conditions by combining:
//   * Multiple barrier phases per round (setup + processing fence completion)
//   * Randomised worker delays while dispatching many per-round micro tasks
//   * Aggregator/validator handshake that must observe every contribution
//   * Failure propagation through explicit RoundAdvance status messages
//
// The scenario involves the following actors:
//   - Conductor: drives the rounds, emits Kickoff messages with per-round seeds,
//                validates RoundAdvance reports, and writes the final result.
//   - Workers (3): respond to Kickoff by sending a burst of MicroTask messages
//                  and announcing completion with WorkerDone.
//   - Aggregator: collects MicroTask traffic, verifies per-worker totals, and
//                  forwards summary Validation results.
//   - Verifier: consumes Validation reports, cross-checks payload integrity, and
//               emits RoundAdvance broadcasts containing per-worker checksums.
//
// Each round requires successful completion of all stages; any discrepancy is
// propagated as a failed RoundAdvance.  The conductor records the first failure
// and terminates the choreography with a Stop broadcast so that processes exit
// without deadlocking on barriers.
//
#include <sintra/sintra.h>

#include "test_environment.h"

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

#ifdef _WIN32
#include <process.h>
#define GETPID() _getpid()
#else
#include <unistd.h>
#define GETPID() getpid()
#endif

// Diagnostic logging macros for failure investigation
#define LOG(role, ...) do { \
    auto now = std::chrono::steady_clock::now().time_since_epoch(); \
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count(); \
    std::fprintf(stderr, "[%s PID:%d T:%lld] ", role, GETPID(), (long long)ms); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); \
    std::fflush(stderr); \
} while(0)

namespace {

constexpr std::size_t kWorkerCount = 3;
constexpr std::size_t kTasksPerWorker = 6;
constexpr int kRounds = 7;
constexpr std::chrono::seconds kWaitTimeout{20};
constexpr std::chrono::milliseconds kMinWorkerDelay{1};
constexpr std::chrono::milliseconds kMaxWorkerDelay{6};
constexpr std::string_view kBarrierGroup = "_sintra_external_processes";
constexpr std::string_view kFinalBarrier = "complex-choreography-finish";
constexpr std::string_view kEnvSharedDir = "SINTRA_COMPLEX_TEST_DIR";

struct Kickoff
{
    int round;
    std::uint64_t seed;
};

struct MicroTask
{
    int round;
    int worker_id;
    int step;
    std::uint64_t payload;
};

struct WorkerDone
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

struct RoundAdvance
{
    int round;
    bool success;
    std::array<std::uint64_t, kWorkerCount> checksums;
};

struct Stop
{
    bool due_to_failure;
};

std::string barrier_round_start_name(int round)
{
    std::ostringstream oss;
    oss << "complex-round-" << round << "-start";
    return oss.str();
}

std::string barrier_round_complete_name(int round)
{
    std::ostringstream oss;
    oss << "complex-round-" << round << "-complete";
    return oss.str();
}

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value)
    {
        throw std::runtime_error("SINTRA_COMPLEX_TEST_DIR is not set");
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

    auto base = sintra::test::scratch_subdirectory("complex_choreography");
    std::filesystem::create_directories(base);

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    const auto unique = counter.fetch_add(1);

    std::ostringstream oss;
    oss << "complex_" << now << '_' << pid << '_' << unique;

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

void write_result(const std::filesystem::path& file, const std::string& status,
                  int completed_rounds, bool failure)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << status << '\n';
    out << completed_rounds << '\n';
    out << (failure ? "failure" : "success") << '\n';
}

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
    for (int step = 0; step < static_cast<int>(kTasksPerWorker); ++step) {
        result ^= compute_payload(round, worker_id, step, seed);
    }
    return result;
}

std::uint64_t expected_worker_sum(int round, int worker_id, std::uint64_t seed)
{
    std::uint64_t result = 0;
    for (int step = 0; step < static_cast<int>(kTasksPerWorker); ++step) {
        result += compute_payload(round, worker_id, step, seed);
    }
    return result;
}

struct AggregatorWorkerState
{
    int contributions = 0;
    std::uint64_t xor_checksum = 0;
    std::uint64_t sum_checksum = 0;
    bool done = false;
};

struct AggregatorState
{
    int round = -1;
    bool seed_ready = false;
    std::uint64_t seed = 0;
    bool ready_to_validate = false;
    std::array<AggregatorWorkerState, kWorkerCount> workers{};
};

struct VerifierWorkerState
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
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(getpid());
#endif
    return now ^ (pid << 32U) ^ extra ^ static_cast<std::uint64_t>((round + 1) * 0xD1B54A32D192ED03ULL);
}

// ---------------------------------------------------------------------------
// Conductor process
// ---------------------------------------------------------------------------

int conductor_process()
{
    LOG("CONDUCTOR", "===== STARTING =====");

    const auto shared_dir = get_shared_directory();
    const auto result_path = shared_dir / "result.txt";

    std::mutex advance_mutex;
    std::condition_variable advance_cv;
    int last_completed_round = -1;
    std::array<std::uint64_t, kWorkerCount> last_checksums{};
    bool failure_observed = false;
    bool stop_requested = false;

    auto advance_slot = [&](const RoundAdvance& advance) {
        if (advance.round < 0 || advance.round >= kRounds) {
            LOG("CONDUCTOR", "RoundAdvance received with invalid round=%d", advance.round);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(advance_mutex);
            LOG("CONDUCTOR", "RoundAdvance received: round=%d success=%d (last_completed=%d)",
                advance.round, advance.success, last_completed_round);
            if (advance.round > last_completed_round) {
                last_completed_round = advance.round;
                last_checksums = advance.checksums;
            }
            if (!advance.success) {
                failure_observed = true;
                LOG("CONDUCTOR", "FAILURE observed in RoundAdvance for round=%d", advance.round);
            }
        }
        advance_cv.notify_all();
    };

    auto stop_slot = [&](const Stop& stop) {
        std::lock_guard<std::mutex> lk(advance_mutex);
        LOG("CONDUCTOR", "Stop received: due_to_failure=%d", stop.due_to_failure);
        stop_requested = true;
        failure_observed = failure_observed || stop.due_to_failure;
        advance_cv.notify_all();
    };

    sintra::activate_slot(advance_slot);
    sintra::activate_slot(stop_slot);

    const std::string group(kBarrierGroup);
    bool local_failure = false;
    bool stop_broadcasted = false;

    for (int round = 0; round < kRounds && !stop_requested; ++round) {
        const std::uint64_t seed = make_round_seed(round);
        LOG("CONDUCTOR", "==== ROUND %d START ==== seed=%llu", round, (unsigned long long)seed);

        LOG("CONDUCTOR", "Entering start barrier for round %d", round);
        sintra::barrier(barrier_round_start_name(round), group);
        LOG("CONDUCTOR", "Exited start barrier for round %d", round);

        LOG("CONDUCTOR", "Broadcasting Kickoff for round %d", round);
        sintra::world() << Kickoff{round, seed};

        LOG("CONDUCTOR", "Waiting for RoundAdvance (timeout=%ds)...", (int)kWaitTimeout.count());
        std::unique_lock<std::mutex> lk(advance_mutex);
        const bool completed = advance_cv.wait_for(
            lk, kWaitTimeout, [&] { return stop_requested || last_completed_round >= round; });
        if (!completed) {
            LOG("CONDUCTOR", "TIMEOUT waiting for RoundAdvance! round=%d last_completed=%d stop_requested=%d failure_observed=%d",
                round, last_completed_round, stop_requested, failure_observed);
            local_failure = true;
        }
        else {
            LOG("CONDUCTOR", "RoundAdvance received successfully for round %d", round);
        }

        if (last_completed_round >= round) {
            for (std::size_t worker = 0; worker < kWorkerCount; ++worker) {
                const auto expected = expected_worker_xor(round, static_cast<int>(worker), seed);
                if (last_checksums[worker] != expected) {
                    LOG("CONDUCTOR", "CHECKSUM MISMATCH: round=%d worker=%zu expected=%llx got=%llx",
                        round, worker, (unsigned long long)expected, (unsigned long long)last_checksums[worker]);
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
            LOG("CONDUCTOR", "Broadcasting Stop (due to failure) before fence barrier, round=%d", round);
            sintra::world() << Stop{true};
            stop_broadcasted = true;
        }

        LOG("CONDUCTOR", "Entering complete barrier (processing fence) for round %d", round);
        sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round), group);
        LOG("CONDUCTOR", "Exited complete barrier for round %d", round);

        if (local_failure) {
            LOG("CONDUCTOR", "Breaking out of round loop due to local_failure, round=%d", round);
            break;
        }
    }

    const bool due_to_failure = local_failure || failure_observed;
    LOG("CONDUCTOR", "Round loop complete. due_to_failure=%d last_completed_round=%d", due_to_failure, last_completed_round);

    // Broadcast Stop again if not already sent (for the success case) or to ensure
    // all processes have received it
    if (!stop_broadcasted) {
        LOG("CONDUCTOR", "Broadcasting final Stop, due_to_failure=%d", due_to_failure);
        sintra::world() << Stop{due_to_failure};
    }

    write_result(result_path, due_to_failure ? "fail" : "ok",
                 std::max(last_completed_round, -1), due_to_failure);

    LOG("CONDUCTOR", "Entering final barrier");
    sintra::barrier(std::string(kFinalBarrier), "_sintra_all_processes");
    LOG("CONDUCTOR", "Exiting with code %d", due_to_failure ? 1 : 0);
    return due_to_failure ? 1 : 0;
}
// ---------------------------------------------------------------------------
// Worker processes
// ---------------------------------------------------------------------------

int worker_process_impl(int worker_index)
{
    char role_name[32];
    std::snprintf(role_name, sizeof(role_name), "WORKER%d", worker_index);
    LOG(role_name, "===== STARTING =====");

    std::atomic<bool> stop_requested{false};
    std::atomic<int> active_round{-1};

    std::mutex state_mutex;
    std::condition_variable kickoff_cv;
    std::condition_variable completion_cv;
    bool kickoff_seen = false;
    bool round_completed = false;

    std::mt19937 rng(static_cast<std::uint32_t>(
        make_round_seed(worker_index) ^ static_cast<std::uint64_t>(worker_index * 0x12345)));
    std::uniform_int_distribution<int> delay_dist(kMinWorkerDelay.count(), kMaxWorkerDelay.count());

    auto kickoff_slot = [&, role_name](const Kickoff& kickoff) {
        if (stop_requested.load(std::memory_order_acquire)) {
            return;
        }
        const int expected = active_round.load(std::memory_order_acquire);
        if (kickoff.round != expected) {
            LOG(role_name, "Kickoff received but round mismatch: got %d expected %d", kickoff.round, expected);
            return;
        }

        LOG(role_name, "Kickoff received for round %d, sending %d MicroTasks", kickoff.round, (int)kTasksPerWorker);
        std::array<std::uint64_t, kTasksPerWorker> payloads{};
        for (int step = 0; step < static_cast<int>(kTasksPerWorker); ++step) {
            const auto payload = compute_payload(kickoff.round, worker_index, step, kickoff.seed);
            payloads[step] = payload;
            sintra::world() << MicroTask{kickoff.round, worker_index, step, payload};
            if ((step + worker_index) % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(rng)));
            }
            else {
                std::this_thread::yield();
            }
        }

        sintra::world() << WorkerDone{kickoff.round, worker_index, static_cast<int>(kTasksPerWorker)};
        LOG(role_name, "Sent WorkerDone for round %d", kickoff.round);

        {
            std::lock_guard<std::mutex> lk(state_mutex);
            kickoff_seen = true;
        }
        kickoff_cv.notify_all();
    };

    auto advance_slot = [&, role_name](const RoundAdvance& advance) {
        if (advance.round != active_round.load(std::memory_order_acquire)) {
            return;
        }
        LOG(role_name, "RoundAdvance received for round %d, success=%d", advance.round, advance.success);
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            round_completed = true;
        }
        if (!advance.success) {
            stop_requested = true, std::memory_order_release;
        }
        completion_cv.notify_all();
    };

    auto stop_slot = [&, role_name](const Stop& stop) {
        LOG(role_name, "Stop received, due_to_failure=%d", stop.due_to_failure);
        stop_requested = true, std::memory_order_release;
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

    const std::string group(kBarrierGroup);

    for (int round = 0; round < kRounds && !stop_requested; ++round) {
        active_round = round, std::memory_order_release;
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            kickoff_seen = false;
            round_completed = false;
        }

        LOG(role_name, "==== ROUND %d START ==== Entering start barrier", round);
        sintra::barrier(barrier_round_start_name(round), group);
        LOG(role_name, "Exited start barrier for round %d, waiting for Kickoff...", round);

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            bool got_kickoff = kickoff_cv.wait_for(lk, kWaitTimeout, [&] {
                return kickoff_seen || stop_requested.load(std::memory_order_acquire);
            });
            if (!got_kickoff) {
                LOG(role_name, "TIMEOUT waiting for Kickoff! round=%d kickoff_seen=%d stop_requested=%d",
                    round, kickoff_seen, stop_requested.load(std::memory_order_acquire));
            }
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            LOG(role_name, "Stop requested, entering fence barrier for round %d before breaking", round);
            // The stop signal means the conductor is winding down, but the worker has not
            // entered the library's draining path. Other participants may already be parked on
            // the processing fence for this round, so the worker still has to rendezvous before
            // exiting to avoid leaving the barrier short-handed.
            sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round),
                                                        group);
            break;
        }

        LOG(role_name, "Waiting for RoundAdvance...");
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            bool got_advance = completion_cv.wait_for(lk, kWaitTimeout, [&] {
                return round_completed || stop_requested.load(std::memory_order_acquire);
            });
            if (!got_advance) {
                LOG(role_name, "TIMEOUT waiting for RoundAdvance! round=%d round_completed=%d stop_requested=%d",
                    round, round_completed, stop_requested.load(std::memory_order_acquire));
            }
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            LOG(role_name, "Stop requested after RoundAdvance wait, entering fence barrier for round %d", round);
            // See comment above: we must still participate in the in-flight processing fence
            // before breaking out once a stop is observed.
            sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round),
                                                        group);
            break;
        }

        LOG(role_name, "Entering complete barrier (processing fence) for round %d", round);
        sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round), group);
        LOG(role_name, "Exited complete barrier for round %d", round);
    }

    LOG(role_name, "Entering final barrier");
    sintra::barrier(std::string(kFinalBarrier), "_sintra_all_processes");
    LOG(role_name, "Exiting with code 0");
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
    LOG("AGGREGATOR", "===== STARTING =====");

    AggregatorState state;
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

    auto micro_slot = [&](const MicroTask& task) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (task.round != state.round || task.worker_id < 0 ||
            task.worker_id >= static_cast<int>(kWorkerCount))
        {
            failure = true, std::memory_order_release;
            return;
        }
        auto& worker = state.workers[static_cast<std::size_t>(task.worker_id)];
        worker.contributions += 1;
        worker.xor_checksum ^= task.payload;
        worker.sum_checksum += task.payload;
    };

    auto done_slot = [&](const WorkerDone& done) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (done.round != state.round || done.worker_id < 0 ||
            done.worker_id >= static_cast<int>(kWorkerCount))
        {
            failure = true, std::memory_order_release;
            return;
        }
        auto& worker = state.workers[static_cast<std::size_t>(done.worker_id)];
        worker.done = true;
        worker.contributions = std::max(worker.contributions, done.contributions);
        const bool all_done = std::all_of(state.workers.begin(), state.workers.end(),
            [](const AggregatorWorkerState& w) { return w.done; });
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

    const std::string group(kBarrierGroup);

    for (int round = 0; round < kRounds && !stop_requested; ++round) {
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            state.round = round;
            state.seed_ready = false;
            state.ready_to_validate = false;
            for (auto& worker : state.workers) {
                worker = AggregatorWorkerState{};
            }
        }

        LOG("AGGREGATOR", "==== ROUND %d START ==== Entering start barrier", round);
        sintra::barrier(barrier_round_start_name(round), group);
        LOG("AGGREGATOR", "Exited start barrier, waiting for all workers to be done...");

        std::array<AggregatorWorkerState, kWorkerCount> snapshot{};
        bool local_failure = false;
        std::uint64_t seed = 0;

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool ready = state_cv.wait_for(lk, kWaitTimeout, [&] {
                return state.ready_to_validate || stop_requested;
            });
            if (!ready) {
                int done_count = 0;
                for (size_t i = 0; i < kWorkerCount; ++i) {
                    if (state.workers[i].done) done_count++;
                }
                LOG("AGGREGATOR", "TIMEOUT waiting for workers! round=%d done_count=%d/%d seed_ready=%d",
                    round, done_count, (int)kWorkerCount, state.seed_ready);
                local_failure = true;
            }
            seed = state.seed;
            snapshot = state.workers;
            if (!state.seed_ready) {
                LOG("AGGREGATOR", "Seed not ready for round %d", round);
                local_failure = true;
            }
        }

        for (std::size_t idx = 0; idx < kWorkerCount; ++idx) {
            auto& worker = snapshot[idx];
            if (worker.contributions != static_cast<int>(kTasksPerWorker)) {
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

        for (std::size_t idx = 0; idx < kWorkerCount; ++idx) {
            const auto& worker = snapshot[idx];
            sintra::world() << Validation{round, static_cast<int>(idx), worker.contributions,
                                          worker.xor_checksum, worker.sum_checksum};
        }

        LOG("AGGREGATOR", "Entering complete barrier (processing fence) for round %d", round);
        sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round), group);
        LOG("AGGREGATOR", "Exited complete barrier for round %d", round);
    }

    LOG("AGGREGATOR", "Entering final barrier, failure=%d", failure.load());
    sintra::barrier(std::string(kFinalBarrier), "_sintra_all_processes");
    LOG("AGGREGATOR", "Exiting with code %d", failure ? 1 : 0);
    return failure ? 1 : 0;
}
// ---------------------------------------------------------------------------
// Verifier process
// ---------------------------------------------------------------------------

int verifier_process()
{
    LOG("VERIFIER", "===== STARTING =====");

    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> failure{false};

    int current_round = -1;
    std::array<std::uint64_t, kRounds> seeds{};
    std::array<bool, kRounds> seed_ready{};
    std::array<VerifierWorkerState, kWorkerCount> worker_state{};
    int validations_received = 0;
    bool ready_to_advance = false;

    auto kickoff_slot = [&](const Kickoff& kickoff) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (kickoff.round >= 0 && kickoff.round < kRounds) {
            seeds[static_cast<std::size_t>(kickoff.round)] = kickoff.seed;
            seed_ready[static_cast<std::size_t>(kickoff.round)] = true;
            state_cv.notify_all();
        }
    };

    auto validation_slot = [&](const Validation& validation) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (validation.round != current_round || validation.worker_id < 0 ||
            validation.worker_id >= static_cast<int>(kWorkerCount))
        {
            failure = true, std::memory_order_release;
            return;
        }
        auto& worker = worker_state[static_cast<std::size_t>(validation.worker_id)];
        worker.received = true;
        worker.contributions = validation.contributions;
        worker.xor_checksum = validation.xor_checksum;
        worker.sum_checksum = validation.sum_checksum;
        ++validations_received;
        if (validations_received >= static_cast<int>(kWorkerCount)) {
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

    const std::string group(kBarrierGroup);

    for (int round = 0; round < kRounds && !stop_requested; ++round) {
        {
            std::lock_guard<std::mutex> lk(state_mutex);
            current_round = round;
            validations_received = 0;
            ready_to_advance = false;
            for (auto& worker : worker_state) {
                worker = VerifierWorkerState{};
            }
        }

        LOG("VERIFIER", "==== ROUND %d START ==== Entering start barrier", round);
        sintra::barrier(barrier_round_start_name(round), group);
        LOG("VERIFIER", "Exited start barrier, waiting for seed...");

        std::uint64_t seed = 0;
        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool seed_ok = state_cv.wait_for(lk, kWaitTimeout, [&] {
                return seed_ready[static_cast<std::size_t>(round)] || stop_requested;
            });
            if (!seed_ok) {
                LOG("VERIFIER", "TIMEOUT waiting for seed! round=%d seed_ready=%d",
                    round, seed_ready[static_cast<std::size_t>(round)]);
                failure = true, std::memory_order_release;
            }
            if (seed_ready[static_cast<std::size_t>(round)]) {
                seed = seeds[static_cast<std::size_t>(round)];
            }
        }

        LOG("VERIFIER", "Waiting for validations from all workers...");
        std::array<std::uint64_t, kWorkerCount> checksums{};
        bool success = true;

        {
            std::unique_lock<std::mutex> lk(state_mutex);
            const bool ready = state_cv.wait_for(lk, kWaitTimeout, [&] {
                return ready_to_advance || stop_requested;
            });
            if (!ready) {
                LOG("VERIFIER", "TIMEOUT waiting for validations! round=%d received=%d/%d",
                    round, validations_received, (int)kWorkerCount);
                success = false;
            }
            for (std::size_t idx = 0; idx < kWorkerCount; ++idx) {
                const auto& worker = worker_state[idx];
                if (!worker.received || worker.contributions != static_cast<int>(kTasksPerWorker)) {
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

        LOG("VERIFIER", "Broadcasting RoundAdvance: round=%d success=%d", round, success);
        sintra::world() << RoundAdvance{round, success, checksums};
        if (!success) {
            LOG("VERIFIER", "Failure detected, setting stop_requested");
            stop_requested = true, std::memory_order_release;
        }

        LOG("VERIFIER", "Entering complete barrier (processing fence) for round %d", round);
        sintra::barrier<sintra::processing_fence_t>(barrier_round_complete_name(round), group);
        LOG("VERIFIER", "Exited complete barrier for round %d", round);
    }

    LOG("VERIFIER", "Entering final barrier, failure=%d", failure.load());
    sintra::barrier(std::string(kFinalBarrier), "_sintra_all_processes");
    LOG("VERIFIER", "Exiting with code %d", failure ? 1 : 0);
    return failure ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();
    const auto result_path = shared_dir / "result.txt";

    if (!is_spawned)
    {
        std::filesystem::remove(result_path);
    }

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(conductor_process);
    processes.emplace_back(aggregator_process);
    processes.emplace_back(verifier_process);
    processes.emplace_back(worker_process0);
    processes.emplace_back(worker_process1);
    processes.emplace_back(worker_process2);

    sintra::init(argc, argv, processes);
    if (!is_spawned) {
        sintra::barrier(std::string(kFinalBarrier), "_sintra_all_processes");
    }
    sintra::finalize();

    if (!is_spawned) {
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

        const bool ok = (status == "ok") && (completed_rounds >= kRounds - 1) &&
                        (failure_state == "success");
        std::error_code ec;
        std::filesystem::remove_all(shared_dir, ec);
        return ok ? 0 : 1;
    }

    return 0;
}
