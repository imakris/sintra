//
// Sintra Extreme Choreography Synchronisation Test
//
// This test pushes Sintra's coordination mechanisms through an intentionally
// elaborate multi-process dance.  Seven processes collaborate (and occasionally
// compete) across four phases while exchanging multiple message types.  The
// scenario aims to exercise:
//
//   * High fan-out message delivery with mixed timing jitter
//   * Strict ordering guarantees enforced through processing-fence barriers
//   * Cross-process validation of acknowledgements and heartbeats
//   * Failure propagation when any participant detects an unexpected event
//
// Every phase involves both producers emitting payloads, consumers filtering the
// payloads based on sequence parity, an aggregator counting acknowledgements and
// orchestrating a processing-fence rendezvous, and a monitor validating that
// every heartbeat arrived in-order.  Each participant reports anomalies via a
// FailureNotice message which the aggregator records before the root process
// inspects the final summary written to disk.

#include <sintra/sintra.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <sstream>
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

constexpr std::size_t kProducerCount = 2;
constexpr std::size_t kConsumerCount = 2;
constexpr std::size_t kPhaseCount = 4;

// Payload choreography plan: each producer emits a specific number of payloads
// per phase.  The sequence indices start at zero for every phase and producer.
constexpr std::array<std::array<int, kPhaseCount>, kProducerCount> kProducerPayloadPlan{{
    {9, 6, 11, 8},
    {7, 13, 5, 10},
}};

// Compute aggregate payload counts per phase.
constexpr std::array<int, kPhaseCount> compute_phase_totals()
{
    std::array<int, kPhaseCount> totals{};
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        int sum = 0;
        for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
            sum += kProducerPayloadPlan[producer][phase];
        }
        totals[phase] = sum;
    }
    return totals;
}

constexpr std::array<int, kPhaseCount> kPhaseTotals = compute_phase_totals();

// Consumer 0 processes even sequence numbers, consumer 1 processes odd ones.
constexpr std::array<std::array<int, kPhaseCount>, kConsumerCount> compute_consumer_expectations()
{
    std::array<std::array<int, kPhaseCount>, kConsumerCount> expectations{};
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
            const int count = kProducerPayloadPlan[producer][phase];
            expectations[0][phase] += (count + 1) / 2; // even sequences
            expectations[1][phase] += count / 2;        // odd sequences
        }
    }
    return expectations;
}

constexpr std::array<std::array<int, kPhaseCount>, kConsumerCount> kConsumerExpectations =
    compute_consumer_expectations();

enum class Actor : int {
    Coordinator = 0,
    Producer0,
    Producer1,
    Consumer0,
    Consumer1,
    Aggregator,
    Monitor,
};

enum class FailureCode : int {
    UnexpectedPhaseStart = 1,
    SequenceOutOfRange,
    DuplicatePayload,
    AckBeforeStart,
    AckOverflow,
    FenceResent,
    HeartbeatOutOfOrder,
    HeartbeatMissing,
    CompletionMismatch,
    CoordinatorTimeout,
};

struct PhaseStart
{
    int phase{};
};

struct Payload
{
    int phase{};
    int producer{};
    int sequence{};
};

struct Heartbeat
{
    int phase{};
    int producer{};
    int tick{};
};

struct Ack
{
    int phase{};
    int producer{};
    int sequence{};
    int consumer{};
};

struct PhaseFence
{
    int phase{};
};

struct PhaseComplete
{
    int phase{};
    int ack_count{};
};

struct FailureNotice
{
    int actor{};
    int phase{};
    int code{};
    char message[96]{};
};

struct Stop
{
};

constexpr std::string_view kEnvSharedDir = "SINTRA_EXTREME_CHOREOGRAPHY_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("extreme choreography shared directory is not set");
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
    if (value) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_extreme_choreography";
    std::filesystem::create_directories(base);

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();

#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    const auto unique = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "run_" << now << '_' << pid << '_' << unique;

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

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

const std::array<std::string, kPhaseCount>& fence_names()
{
    static const std::array<std::string, kPhaseCount> names = [] {
        std::array<std::string, kPhaseCount> values{};
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            std::ostringstream oss;
            oss << "extreme-choreography-fence-phase-" << phase;
            values[phase] = oss.str();
        }
        return values;
    }();
    return names;
}

using SequenceTracker = std::array<std::array<std::vector<bool>, kProducerCount>, kPhaseCount>;

SequenceTracker make_sequence_tracker()
{
    SequenceTracker tracker;
    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
            tracker[phase][producer].assign(kProducerPayloadPlan[producer][phase], false);
        }
    }
    return tracker;
}

void send_failure_notice(Actor actor, int phase, FailureCode code, const std::string& text)
{
    FailureNotice notice{};
    notice.actor = static_cast<int>(actor);
    notice.phase = phase;
    notice.code = static_cast<int>(code);
    std::snprintf(notice.message, sizeof(notice.message), "%s", text.c_str());
    sintra::world() << notice;
}

std::string describe_failure(Actor actor, int phase, FailureCode code, const char* message)
{
    std::ostringstream oss;
    oss << "actor=" << static_cast<int>(actor)
        << " phase=" << phase
        << " code=" << static_cast<int>(code)
        << " message=" << message;
    return oss.str();
}

// ---------------------------------------------------------------------------
// Coordinator process
// ---------------------------------------------------------------------------

int coordinator_process()
{
    using namespace sintra;

    std::mutex mutex;
    std::condition_variable cv;
    std::array<bool, kPhaseCount> completed{};
    bool stop_requested = false;
    bool failure_observed = false;

    auto phase_complete_slot = [&](const PhaseComplete& msg) {
        if (msg.phase < 0 || static_cast<std::size_t>(msg.phase) >= kPhaseCount) {
            send_failure_notice(Actor::Coordinator, msg.phase, FailureCode::CompletionMismatch,
                                "phase complete outside range");
            return;
        }
        const auto expected = kPhaseTotals[msg.phase];
        if (msg.ack_count != expected) {
            send_failure_notice(Actor::Coordinator, msg.phase, FailureCode::CompletionMismatch,
                                "ack total mismatch");
            failure_observed = true;
        }

        {
            std::lock_guard<std::mutex> lk(mutex);
            completed[msg.phase] = true;
        }
        cv.notify_all();
    };

    auto fence_slot = [&](const PhaseFence& fence) {
        if (fence.phase >= 0 && static_cast<std::size_t>(fence.phase) < kPhaseCount) {
            const auto& name = fence_names()[fence.phase];
            sintra::barrier<processing_fence_t>(name);
        } else {
            send_failure_notice(Actor::Coordinator, fence.phase, FailureCode::FenceResent,
                                "received fence outside configured phases");
        }
    };

    auto stop_slot = [&](Stop) {
        std::lock_guard<std::mutex> lk(mutex);
        stop_requested = true;
        cv.notify_one();
    };

    activate_slot(phase_complete_slot);
    activate_slot(fence_slot);
    activate_slot(stop_slot);

    const std::string group = "_sintra_external_processes";
    barrier("extreme-choreography-ready", group);

    for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
        world() << PhaseStart{static_cast<int>(phase)};

        std::unique_lock<std::mutex> lk(mutex);
        const bool completed_phase = cv.wait_for(
            lk, std::chrono::seconds(15), [&] { return completed[phase]; });
        if (!completed_phase) {
            failure_observed = true;
            send_failure_notice(Actor::Coordinator, static_cast<int>(phase),
                                FailureCode::CoordinatorTimeout, "phase completion timed out");
        }
    }

    world() << Stop{};

    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return stop_requested; });
    }

    deactivate_all_slots();
    barrier("extreme-choreography-finished", "_sintra_all_processes");

    return failure_observed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Producer processes
// ---------------------------------------------------------------------------

template<int ProducerId>
int producer_process()
{
    using namespace sintra;

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;

    const auto seed = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid_component = static_cast<unsigned>(_getpid());
#else
    const auto pid_component = static_cast<unsigned>(getpid());
#endif
    std::seed_seq seed_seq{seed, pid_component, static_cast<unsigned>(ProducerId)};
    std::mt19937 rng(seed_seq);
    std::uniform_int_distribution<int> jitter_us(0, 120);

    auto phase_start_slot = [&](const PhaseStart& start) {
        if (start.phase < 0 || static_cast<std::size_t>(start.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), start.phase,
                                FailureCode::UnexpectedPhaseStart, "phase index out of range");
            return;
        }

        const int payloads = kProducerPayloadPlan[ProducerId][start.phase];
        for (int seq = 0; seq < payloads; ++seq) {
            world() << Heartbeat{start.phase, ProducerId, seq};
            if (jitter_us(rng) % 3 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
            }
            world() << Payload{start.phase, ProducerId, seq};
            if (jitter_us(rng) % 2 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
            }
        }
    };

    auto fence_slot = [&](const PhaseFence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), fence.phase,
                                FailureCode::FenceResent, "fence outside valid phase range");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const PhaseComplete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), complete.phase,
                                FailureCode::CompletionMismatch, "completion outside valid range");
            return;
        }
        const int expected = kPhaseTotals[complete.phase];
        if (complete.ack_count != expected) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), complete.phase,
                                FailureCode::CompletionMismatch, "unexpected ack total from aggregator");
        }
    };

    auto stop_slot = [&](Stop) {
        std::lock_guard<std::mutex> lk(mutex);
        stop_requested = true;
        cv.notify_one();
    };

    activate_slot(phase_start_slot);
    activate_slot(fence_slot);
    activate_slot(completion_slot);
    activate_slot(stop_slot);

    barrier("extreme-choreography-ready", "_sintra_external_processes");

    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return stop_requested; });

    deactivate_all_slots();
    barrier("extreme-choreography-finished", "_sintra_all_processes");

    return 0;
}

// ---------------------------------------------------------------------------
// Consumer processes
// ---------------------------------------------------------------------------

template<int ConsumerId>
int consumer_process()
{
    using namespace sintra;

    SequenceTracker tracker = make_sequence_tracker();
    std::array<int, kPhaseCount> processed{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;

    const auto seed = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid_component = static_cast<unsigned>(_getpid());
#else
    const auto pid_component = static_cast<unsigned>(getpid());
#endif
    std::seed_seq seed_seq{seed, pid_component, static_cast<unsigned>(ConsumerId + 10)};
    std::mt19937 rng(seed_seq);
    std::uniform_int_distribution<int> jitter_us(10, 200);

    auto payload_slot = [&](const Payload& payload) {
        if (payload.phase < 0 || static_cast<std::size_t>(payload.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                FailureCode::SequenceOutOfRange, "payload phase out of range");
            return;
        }
        if (payload.producer < 0 || static_cast<std::size_t>(payload.producer) >= kProducerCount) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                FailureCode::SequenceOutOfRange, "payload producer invalid");
            return;
        }

        const bool handles_even = (ConsumerId == 0);
        const bool should_handle = ((payload.sequence % 2) == 0) == handles_even;
        if (!should_handle) {
            return;
        }

        bool duplicate = false;
        bool range_error = false;

        {
            std::lock_guard<std::mutex> lk(mutex);
            auto& seen = tracker[payload.phase][payload.producer];
            if (payload.sequence < 0 || payload.sequence >= static_cast<int>(seen.size())) {
                range_error = true;
            } else if (seen[payload.sequence]) {
                duplicate = true;
            } else {
                seen[payload.sequence] = true;
                processed[payload.phase]++;
            }
        }

        if (range_error) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                FailureCode::SequenceOutOfRange, "payload sequence outside configured range");
            return;
        }
        if (duplicate) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                FailureCode::DuplicatePayload, "duplicate payload detected");
            return;
        }

        if (jitter_us(rng) % 5 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
        }
        world() << Ack{payload.phase, payload.producer, payload.sequence, ConsumerId};
    };

    auto fence_slot = [&](const PhaseFence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), fence.phase,
                                FailureCode::FenceResent, "invalid fence phase");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const PhaseComplete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= kPhaseCount) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                FailureCode::CompletionMismatch, "completion outside range");
            return;
        }
        const int expected = kConsumerExpectations[ConsumerId][complete.phase];
        const int observed = processed[complete.phase];
        if (observed != expected) {
            std::ostringstream oss;
            oss << "consumer processed=" << observed << " expected=" << expected;
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                FailureCode::CompletionMismatch, oss.str());
        }
        if (complete.ack_count != kPhaseTotals[complete.phase]) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                FailureCode::CompletionMismatch, "aggregator ack mismatch reported");
        }
    };

    auto stop_slot = [&](Stop) {
        std::lock_guard<std::mutex> lk(mutex);
        stop_requested = true;
        cv.notify_one();
    };

    activate_slot(payload_slot);
    activate_slot(fence_slot);
    activate_slot(completion_slot);
    activate_slot(stop_slot);

    barrier("extreme-choreography-ready", "_sintra_external_processes");

    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return stop_requested; });

    deactivate_all_slots();
    barrier("extreme-choreography-finished", "_sintra_all_processes");

    return 0;
}

// ---------------------------------------------------------------------------
// Monitor process
// ---------------------------------------------------------------------------

int monitor_process()
{
    using namespace sintra;

    std::array<std::array<int, kPhaseCount>, kProducerCount> heartbeat_counts{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;

    auto heartbeat_slot = [&](const Heartbeat& heartbeat) {
        if (heartbeat.phase < 0 || static_cast<std::size_t>(heartbeat.phase) >= kPhaseCount) {
            send_failure_notice(Actor::Monitor, heartbeat.phase, FailureCode::SequenceOutOfRange,
                                "heartbeat phase out of range");
            return;
        }
        if (heartbeat.producer < 0 || static_cast<std::size_t>(heartbeat.producer) >= kProducerCount) {
            send_failure_notice(Actor::Monitor, heartbeat.phase, FailureCode::SequenceOutOfRange,
                                "heartbeat producer invalid");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        int& counter = heartbeat_counts[heartbeat.producer][heartbeat.phase];
        if (heartbeat.tick != counter) {
            std::ostringstream oss;
            oss << "tick=" << heartbeat.tick << " expected=" << counter;
            send_failure_notice(Actor::Monitor, heartbeat.phase, FailureCode::HeartbeatOutOfOrder,
                                oss.str());
            counter = heartbeat.tick + 1; // resynchronise to avoid cascading failures
        } else {
            ++counter;
        }
    };

    auto fence_slot = [&](const PhaseFence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= kPhaseCount) {
            send_failure_notice(Actor::Monitor, fence.phase, FailureCode::FenceResent,
                                "monitor received fence outside range");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const PhaseComplete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= kPhaseCount) {
            send_failure_notice(Actor::Monitor, complete.phase, FailureCode::CompletionMismatch,
                                "monitor completion outside range");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
            const int expected = kProducerPayloadPlan[producer][complete.phase];
            const int observed = heartbeat_counts[producer][complete.phase];
            if (observed != expected) {
                std::ostringstream oss;
                oss << "producer=" << producer << " observed=" << observed << " expected=" << expected;
                send_failure_notice(Actor::Monitor, complete.phase, FailureCode::HeartbeatMissing,
                                    oss.str());
            }
        }
    };

    auto stop_slot = [&](Stop) {
        std::lock_guard<std::mutex> lk(mutex);
        stop_requested = true;
        cv.notify_one();
    };

    activate_slot(heartbeat_slot);
    activate_slot(fence_slot);
    activate_slot(completion_slot);
    activate_slot(stop_slot);

    barrier("extreme-choreography-ready", "_sintra_external_processes");

    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return stop_requested; });

    deactivate_all_slots();
    barrier("extreme-choreography-finished", "_sintra_all_processes");

    return 0;
}

// ---------------------------------------------------------------------------
// Aggregator process
// ---------------------------------------------------------------------------

int aggregator_process()
{
    using namespace sintra;

    const auto shared_dir = get_shared_directory();
    const auto summary_path = shared_dir / "extreme_summary.txt";

    SequenceTracker tracker = make_sequence_tracker();
    std::array<int, kPhaseCount> ack_counts{};
    std::array<bool, kPhaseCount> phase_started{};
    std::array<bool, kPhaseCount> phase_finalised{};
    std::array<int, kPhaseCount> phase_completion_counts{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;
    bool failure_flag = false;
    std::vector<std::string> failure_messages;

    auto record_failure_locked = [&](Actor actor, int phase, FailureCode code, const char* message) {
        failure_flag = true;
        failure_messages.push_back(describe_failure(actor, phase, code, message));
    };

    auto phase_start_slot = [&](const PhaseStart& start) {
        if (start.phase < 0 || static_cast<std::size_t>(start.phase) >= kPhaseCount) {
            send_failure_notice(Actor::Aggregator, start.phase, FailureCode::UnexpectedPhaseStart,
                                "aggregator received invalid phase start");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        if (phase_started[start.phase]) {
            record_failure_locked(Actor::Aggregator, start.phase, FailureCode::UnexpectedPhaseStart,
                                  "duplicate phase start");
        } else {
            phase_started[start.phase] = true;
            ack_counts[start.phase] = 0;
            auto& phase_tracker = tracker[start.phase];
            for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
                std::fill(phase_tracker[producer].begin(), phase_tracker[producer].end(), false);
            }
        }
    };

    auto ack_slot = [&](const Ack& ack) {
        bool trigger_completion = false;
        int completion_phase = -1;
        int completion_count = 0;

        {
            std::lock_guard<std::mutex> lk(mutex);
            if (ack.phase < 0 || static_cast<std::size_t>(ack.phase) >= kPhaseCount) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::AckBeforeStart,
                                      "ack phase outside range");
                return;
            }
            if (!phase_started[ack.phase]) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::AckBeforeStart,
                                      "ack received before phase start");
                return;
            }
            if (phase_finalised[ack.phase]) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::AckOverflow,
                                      "ack received after phase finalised");
                return;
            }
            if (ack.producer < 0 || static_cast<std::size_t>(ack.producer) >= kProducerCount) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::AckBeforeStart,
                                      "ack producer invalid");
                return;
            }

            auto& flags = tracker[ack.phase][ack.producer];
            if (ack.sequence < 0 || ack.sequence >= static_cast<int>(flags.size())) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::SequenceOutOfRange,
                                      "ack sequence out of range");
                return;
            }
            if (flags[ack.sequence]) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::DuplicatePayload,
                                      "duplicate ack received");
                return;
            }

            flags[ack.sequence] = true;
            ++ack_counts[ack.phase];

            const int expected = kPhaseTotals[ack.phase];
            if (ack_counts[ack.phase] == expected) {
                phase_finalised[ack.phase] = true;
                phase_completion_counts[ack.phase] = ack_counts[ack.phase];
                trigger_completion = true;
                completion_phase = ack.phase;
                completion_count = ack_counts[ack.phase];
            } else if (ack_counts[ack.phase] > expected) {
                record_failure_locked(Actor::Aggregator, ack.phase, FailureCode::AckOverflow,
                                      "ack count exceeded expected total");
            }
        }

        if (trigger_completion) {
            world() << PhaseFence{completion_phase};
            const auto& name = fence_names()[completion_phase];
            barrier<processing_fence_t>(name);
            world() << PhaseComplete{completion_phase, completion_count};
        }
    };

    auto failure_slot = [&](const FailureNotice& notice) {
        std::lock_guard<std::mutex> lk(mutex);
        failure_flag = true;
        failure_messages.emplace_back(describe_failure(static_cast<Actor>(notice.actor),
                                                       notice.phase,
                                                       static_cast<FailureCode>(notice.code),
                                                       notice.message));
    };

    auto fence_slot = [&](const PhaseFence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= kPhaseCount) {
            std::lock_guard<std::mutex> lk(mutex);
            record_failure_locked(Actor::Aggregator, fence.phase, FailureCode::FenceResent,
                                  "aggregator unexpectedly received fence broadcast");
        }
    };

    auto stop_slot = [&](Stop) {
        std::lock_guard<std::mutex> lk(mutex);
        stop_requested = true;
        cv.notify_one();
    };

    activate_slot(phase_start_slot);
    activate_slot(ack_slot);
    activate_slot(failure_slot);
    activate_slot(fence_slot);
    activate_slot(stop_slot);

    barrier("extreme-choreography-ready", "_sintra_external_processes");

    {
        std::unique_lock<std::mutex> lk(mutex);
        cv.wait(lk, [&] { return stop_requested; });
    }

    deactivate_all_slots();
    barrier("extreme-choreography-finished", "_sintra_all_processes");

    std::ofstream out(summary_path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << (failure_flag ? "fail" : "ok") << '\n';
        int completed_phases = 0;
        for (bool value : phase_finalised) {
            if (value) {
                ++completed_phases;
            }
        }
        out << "completed_phases " << completed_phases << '\n';
        out << "ack_counts";
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
            out << ' ' << phase_completion_counts[phase];
        }
        out << '\n';
        out << "failures " << failure_messages.size() << '\n';
        for (const auto& msg : failure_messages) {
            out << msg << '\n';
        }
    }

    return failure_flag ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();
    const auto summary_path = shared_dir / "extreme_summary.txt";

    if (!is_spawned) {
        std::error_code ec;
        std::filesystem::remove(summary_path, ec);
    }

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(producer_process<0>);
    processes.emplace_back(producer_process<1>);
    processes.emplace_back(consumer_process<0>);
    processes.emplace_back(consumer_process<1>);
    processes.emplace_back(aggregator_process);
    processes.emplace_back(monitor_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("extreme-choreography-finished", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        std::ifstream in(summary_path, std::ios::binary);
        if (!in) {
            cleanup_directory(shared_dir);
            return 1;
        }

        std::string status;
        std::getline(in, status);
        std::string completed_line;
        std::getline(in, completed_line);
        std::string ack_line;
        std::getline(in, ack_line);
        std::string failure_line;
        std::getline(in, failure_line);

        bool ok = (status == "ok");

        int completed_phases = 0;
        {
            std::istringstream iss(completed_line);
            std::string label;
            iss >> label >> completed_phases;
            if (label != "completed_phases") {
                ok = false;
            }
        }

        if (completed_phases != static_cast<int>(kPhaseCount)) {
            ok = false;
        }

        {
            std::istringstream iss(ack_line);
            std::string label;
            iss >> label;
            if (label != "ack_counts") {
                ok = false;
            }
            for (std::size_t phase = 0; phase < kPhaseCount; ++phase) {
                int value = -1;
                iss >> value;
                if (value != kPhaseTotals[phase]) {
                    ok = false;
                }
            }
        }

        {
            std::istringstream iss(failure_line);
            std::string label;
            int failure_count = 0;
            iss >> label >> failure_count;
            if (label != "failures" || failure_count != 0) {
                ok = false;
            }
        }

        cleanup_directory(shared_dir);
        return ok ? 0 : 1;
    }

    return 0;
}

