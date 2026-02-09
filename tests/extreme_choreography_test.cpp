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
// Failure_notice message which the aggregator records before the root process
// inspects the final summary written to disk.

#include <sintra/sintra.h>

#include "test_choreography_utils.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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

namespace {

constexpr std::size_t k_producer_count = 2;
constexpr std::size_t k_consumer_count = 2;
constexpr std::size_t k_phase_count = 4;

// Payload choreography plan: each producer emits a specific number of payloads
// per phase.  The sequence indices start at zero for every phase and producer.
constexpr std::array<std::array<int, k_phase_count>, k_producer_count> k_producer_payload_plan{{
    {9, 6, 11, 8},
    {7, 13, 5, 10},
}};

// Compute aggregate payload counts per phase.
constexpr std::array<int, k_phase_count> compute_phase_totals()
{
    std::array<int, k_phase_count> totals{};
    for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
        int sum = 0;
        for (std::size_t producer = 0; producer < k_producer_count; ++producer) {
            sum += k_producer_payload_plan[producer][phase];
        }
        totals[phase] = sum;
    }
    return totals;
}

constexpr std::array<int, k_phase_count> k_phase_totals = compute_phase_totals();

// Consumer 0 processes even sequence numbers, consumer 1 processes odd ones.
constexpr std::array<std::array<int, k_phase_count>, k_consumer_count> compute_consumer_expectations()
{
    std::array<std::array<int, k_phase_count>, k_consumer_count> expectations{};
    for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
        for (std::size_t producer = 0; producer < k_producer_count; ++producer) {
            const int count = k_producer_payload_plan[producer][phase];
            expectations[0][phase] += (count + 1) / 2; // even sequences
            expectations[1][phase] += count / 2;        // odd sequences
        }
    }
    return expectations;
}

constexpr std::array<std::array<int, k_phase_count>, k_consumer_count> k_consumer_expectations =
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

constexpr Actor operator+(Actor base, std::size_t offset) noexcept
{
    return static_cast<Actor>(static_cast<int>(base) + static_cast<int>(offset));
}

constexpr Actor operator+(Actor base, int offset) noexcept
{
    return static_cast<Actor>(static_cast<int>(base) + offset);
}

enum class Failure_code : int {
    UnexpectedPhase_start = 1,
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

struct Phase_start
{
    int phase;
};

struct Payload
{
    int phase;
    int producer;
    int sequence;
};

struct Heartbeat
{
    int phase;
    int producer;
    int tick;
};

struct Ack
{
    int phase;
    int producer;
    int sequence;
    int consumer;
};

struct Phase_fence
{
    int phase;
};

struct Phase_complete
{
    int phase;
    int ack_count;
};

struct Failure_notice
{
    int actor;
    int phase;
    int code;
    char message[96];
};

struct Stop
{
};

const std::array<std::string, k_phase_count>& fence_names()
{
    static const std::array<std::string, k_phase_count> names = [] {
        std::array<std::string, k_phase_count> values{};
        for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
            values[phase] =
                sintra::test::make_barrier_name("extreme-choreography-fence-phase", phase);
        }
        return values;
    }();
    return names;
}

using Sequence_tracker = std::array<std::array<std::vector<bool>, k_producer_count>, k_phase_count>;

Sequence_tracker make_sequence_tracker()
{
    Sequence_tracker tracker;
    for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
        for (std::size_t producer = 0; producer < k_producer_count; ++producer) {
            tracker[phase][producer].assign(k_producer_payload_plan[producer][phase], false);
        }
    }
    return tracker;
}

void send_failure_notice(Actor actor, int phase, Failure_code code, const std::string& text)
{
    Failure_notice notice{};
    notice.actor = static_cast<int>(actor);
    notice.phase = phase;
    notice.code = static_cast<int>(code);
    std::snprintf(notice.message, sizeof(notice.message), "%s", text.c_str());
    sintra::world() << notice;
}

std::string describe_failure(Actor actor, int phase, Failure_code code, const char* message)
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
    std::array<bool, k_phase_count> completed{};
    bool stop_requested = false;
    bool failure_observed = false;

    auto phase_complete_slot = [&](const Phase_complete& msg) {
        if (msg.phase < 0 || static_cast<std::size_t>(msg.phase) >= k_phase_count) {
            send_failure_notice(Actor::Coordinator, msg.phase, Failure_code::CompletionMismatch,
                                "phase complete outside range");
            return;
        }
        const auto expected = k_phase_totals[msg.phase];
        if (msg.ack_count != expected) {
            send_failure_notice(Actor::Coordinator, msg.phase, Failure_code::CompletionMismatch,
                                "ack total mismatch");
            failure_observed = true;
        }

        {
            std::lock_guard<std::mutex> lk(mutex);
            completed[msg.phase] = true;
        }
        cv.notify_all();
    };

    auto fence_slot = [&](const Phase_fence& fence) {
        if (fence.phase >= 0 && static_cast<std::size_t>(fence.phase) < k_phase_count) {
            const auto& name = fence_names()[fence.phase];
            sintra::barrier<processing_fence_t>(name);
        }
        else {
            send_failure_notice(Actor::Coordinator, fence.phase, Failure_code::FenceResent,
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

    for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
        world() << Phase_start{static_cast<int>(phase)};

        std::unique_lock<std::mutex> lk(mutex);
        const bool completed_phase = cv.wait_for(
            lk, std::chrono::seconds(15), [&] { return completed[phase]; });
        if (!completed_phase) {
            failure_observed = true;
            send_failure_notice(Actor::Coordinator, static_cast<int>(phase),
                                Failure_code::CoordinatorTimeout, "phase completion timed out");
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
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid_component = static_cast<unsigned>(sintra::test::get_pid());
    std::seed_seq seed_seq{seed, pid_component, static_cast<unsigned>(ProducerId)};
    std::mt19937 rng(seed_seq);
    std::uniform_int_distribution<int> jitter_us(0, 120);

    auto phase_start_slot = [&](const Phase_start& start) {
        if (start.phase < 0 || static_cast<std::size_t>(start.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), start.phase,
                                Failure_code::UnexpectedPhase_start, "phase index out of range");
            return;
        }

        const int payloads = k_producer_payload_plan[ProducerId][start.phase];
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

    auto fence_slot = [&](const Phase_fence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), fence.phase,
                                Failure_code::FenceResent, "fence outside valid phase range");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const Phase_complete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), complete.phase,
                                Failure_code::CompletionMismatch, "completion outside valid range");
            return;
        }
        const int expected = k_phase_totals[complete.phase];
        if (complete.ack_count != expected) {
            send_failure_notice(static_cast<Actor>(Actor::Producer0 + ProducerId), complete.phase,
                                Failure_code::CompletionMismatch, "unexpected ack total from aggregator");
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

    Sequence_tracker tracker = make_sequence_tracker();
    std::array<int, k_phase_count> processed{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;

    const auto seed = static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid_component = static_cast<unsigned>(sintra::test::get_pid());
    std::seed_seq seed_seq{seed, pid_component, static_cast<unsigned>(ConsumerId + 10)};
    std::mt19937 rng(seed_seq);
    std::uniform_int_distribution<int> jitter_us(10, 200);

    auto payload_slot = [&](const Payload& payload) {
        if (payload.phase < 0 || static_cast<std::size_t>(payload.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                Failure_code::SequenceOutOfRange, "payload phase out of range");
            return;
        }
        if (payload.producer < 0 || static_cast<std::size_t>(payload.producer) >= k_producer_count) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                Failure_code::SequenceOutOfRange, "payload producer invalid");
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
            }
            else
            if (seen[payload.sequence]) {
                duplicate = true;
            }
            else {
                seen[payload.sequence] = true;
                processed[payload.phase]++;
            }
        }

        if (range_error) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                Failure_code::SequenceOutOfRange, "payload sequence outside configured range");
            return;
        }
        if (duplicate) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), payload.phase,
                                Failure_code::DuplicatePayload, "duplicate payload detected");
            return;
        }

        if (jitter_us(rng) % 5 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(jitter_us(rng)));
        }
        world() << Ack{payload.phase, payload.producer, payload.sequence, ConsumerId};
    };

    auto fence_slot = [&](const Phase_fence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), fence.phase,
                                Failure_code::FenceResent, "invalid fence phase");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const Phase_complete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= k_phase_count) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                Failure_code::CompletionMismatch, "completion outside range");
            return;
        }
        const int expected = k_consumer_expectations[ConsumerId][complete.phase];
        const int observed = processed[complete.phase];
        if (observed != expected) {
            std::ostringstream oss;
            oss << "consumer processed=" << observed << " expected=" << expected;
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                Failure_code::CompletionMismatch, oss.str());
        }
        if (complete.ack_count != k_phase_totals[complete.phase]) {
            send_failure_notice(static_cast<Actor>(Actor::Consumer0 + ConsumerId), complete.phase,
                                Failure_code::CompletionMismatch, "aggregator ack mismatch reported");
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

    std::array<std::array<int, k_phase_count>, k_producer_count> heartbeat_counts{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;

    auto heartbeat_slot = [&](const Heartbeat& heartbeat) {
        if (heartbeat.phase < 0 || static_cast<std::size_t>(heartbeat.phase) >= k_phase_count) {
            send_failure_notice(Actor::Monitor, heartbeat.phase, Failure_code::SequenceOutOfRange,
                                "heartbeat phase out of range");
            return;
        }
        if (heartbeat.producer < 0 || static_cast<std::size_t>(heartbeat.producer) >= k_producer_count) {
            send_failure_notice(Actor::Monitor, heartbeat.phase, Failure_code::SequenceOutOfRange,
                                "heartbeat producer invalid");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        int& counter = heartbeat_counts[heartbeat.producer][heartbeat.phase];
        if (heartbeat.tick != counter) {
            std::ostringstream oss;
            oss << "tick=" << heartbeat.tick << " expected=" << counter;
            send_failure_notice(Actor::Monitor, heartbeat.phase, Failure_code::HeartbeatOutOfOrder,
                                oss.str());
            counter = heartbeat.tick + 1; // resynchronise to avoid cascading failures
        }
        else {
            ++counter;
        }
    };

    auto fence_slot = [&](const Phase_fence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= k_phase_count) {
            send_failure_notice(Actor::Monitor, fence.phase, Failure_code::FenceResent,
                                "monitor received fence outside range");
            return;
        }
        const auto& name = fence_names()[fence.phase];
        barrier<processing_fence_t>(name);
    };

    auto completion_slot = [&](const Phase_complete& complete) {
        if (complete.phase < 0 || static_cast<std::size_t>(complete.phase) >= k_phase_count) {
            send_failure_notice(Actor::Monitor, complete.phase, Failure_code::CompletionMismatch,
                                "monitor completion outside range");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        for (std::size_t producer = 0; producer < k_producer_count; ++producer) {
            const int expected = k_producer_payload_plan[producer][complete.phase];
            const int observed = heartbeat_counts[producer][complete.phase];
            if (observed != expected) {
                std::ostringstream oss;
                oss << "producer=" << producer << " observed=" << observed << " expected=" << expected;
                send_failure_notice(Actor::Monitor, complete.phase, Failure_code::HeartbeatMissing,
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

    sintra::test::Shared_directory shared("SINTRA_EXTREME_CHOREOGRAPHY_DIR", "extreme_choreography");
    const auto shared_dir = shared.path();
    const auto summary_path = shared_dir / "extreme_summary.txt";

    Sequence_tracker tracker = make_sequence_tracker();
    std::array<int, k_phase_count> ack_counts{};
    std::array<bool, k_phase_count> phase_started{};
    std::array<bool, k_phase_count> phase_finalised{};
    std::array<int, k_phase_count> phase_completion_counts{};

    std::mutex mutex;
    std::condition_variable cv;
    bool stop_requested = false;
    bool failure_flag = false;
    std::vector<std::string> failure_messages;

    auto record_failure_locked = [&](Actor actor, int phase, Failure_code code, const char* message) {
        failure_flag = true;
        failure_messages.push_back(describe_failure(actor, phase, code, message));
    };

    auto phase_start_slot = [&](const Phase_start& start) {
        if (start.phase < 0 || static_cast<std::size_t>(start.phase) >= k_phase_count) {
            send_failure_notice(Actor::Aggregator, start.phase, Failure_code::UnexpectedPhase_start,
                                "aggregator received invalid phase start");
            return;
        }

        std::lock_guard<std::mutex> lk(mutex);
        if (phase_started[start.phase]) {
            record_failure_locked(Actor::Aggregator, start.phase, Failure_code::UnexpectedPhase_start,
                                  "duplicate phase start");
        }
        else {
            phase_started[start.phase] = true;
            ack_counts[start.phase] = 0;
            auto& phase_tracker = tracker[start.phase];
            for (std::size_t producer = 0; producer < k_producer_count; ++producer) {
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
            if (ack.phase < 0 || static_cast<std::size_t>(ack.phase) >= k_phase_count) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::AckBeforeStart,
                                      "ack phase outside range");
                return;
            }
            if (!phase_started[ack.phase]) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::AckBeforeStart,
                                      "ack received before phase start");
                return;
            }
            if (phase_finalised[ack.phase]) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::AckOverflow,
                                      "ack received after phase finalised");
                return;
            }
            if (ack.producer < 0 || static_cast<std::size_t>(ack.producer) >= k_producer_count) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::AckBeforeStart,
                                      "ack producer invalid");
                return;
            }

            auto& flags = tracker[ack.phase][ack.producer];
            if (ack.sequence < 0 || ack.sequence >= static_cast<int>(flags.size())) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::SequenceOutOfRange,
                                      "ack sequence out of range");
                return;
            }
            if (flags[ack.sequence]) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::DuplicatePayload,
                                      "duplicate ack received");
                return;
            }

            flags[ack.sequence] = true;
            ++ack_counts[ack.phase];

            const int expected = k_phase_totals[ack.phase];
            if (ack_counts[ack.phase] == expected) {
                phase_finalised[ack.phase] = true;
                phase_completion_counts[ack.phase] = ack_counts[ack.phase];
                trigger_completion = true;
                completion_phase = ack.phase;
                completion_count = ack_counts[ack.phase];
            }
            else
            if (ack_counts[ack.phase] > expected) {
                record_failure_locked(Actor::Aggregator, ack.phase, Failure_code::AckOverflow,
                                      "ack count exceeded expected total");
            }
        }

        if (trigger_completion) {
            world() << Phase_fence{completion_phase};
            const auto& name = fence_names()[completion_phase];
            barrier<processing_fence_t>(name);
            world() << Phase_complete{completion_phase, completion_count};
        }
    };

    auto failure_slot = [&](const Failure_notice& notice) {
        std::lock_guard<std::mutex> lk(mutex);
        failure_flag = true;
        failure_messages.emplace_back(describe_failure(static_cast<Actor>(notice.actor),
                                                       notice.phase,
                                                       static_cast<Failure_code>(notice.code),
                                                       notice.message));
    };

    auto fence_slot = [&](const Phase_fence& fence) {
        if (fence.phase < 0 || static_cast<std::size_t>(fence.phase) >= k_phase_count) {
            std::lock_guard<std::mutex> lk(mutex);
            record_failure_locked(Actor::Aggregator, fence.phase, Failure_code::FenceResent,
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
        for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
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
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_EXTREME_CHOREOGRAPHY_DIR", "extreme_choreography");
    const auto shared_dir = shared.path();
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
            shared.cleanup();
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

        if (completed_phases != static_cast<int>(k_phase_count)) {
            ok = false;
        }

        {
            std::istringstream iss(ack_line);
            std::string label;
            iss >> label;
            if (label != "ack_counts") {
                ok = false;
            }
            for (std::size_t phase = 0; phase < k_phase_count; ++phase) {
                int value = -1;
                iss >> value;
                if (value != k_phase_totals[phase]) {
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

        shared.cleanup();
        return ok ? 0 : 1;
    }

    return 0;
}

