//
// Sintra receive<MESSAGE_T>() Test
//
// This test validates the receive<MESSAGE_T>() synchronous message receiving function.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string_view>
#include <thread>

namespace {

struct Stop {};
struct Ack {};
struct StopAck {};

struct DataMessage {
    int value;
    double score;
};

int sender_process()
{
    std::atomic<bool> got_ack{false};
    std::atomic<bool> got_stop_ack{false};

    sintra::activate_slot([&](const Ack&) {
        got_ack.store(true, std::memory_order_release);
    });

    sintra::activate_slot([&](const StopAck&) {
        got_stop_ack.store(true, std::memory_order_release);
    });

    sintra::barrier("start");

    const int expected_value = 57;
    const double expected_score = 2.718;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (got_ack.load(std::memory_order_acquire)) {
            break;
        }
        sintra::world() << DataMessage{expected_value, expected_score};
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for Ack\n");
        std::abort();
    }

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (got_stop_ack.load(std::memory_order_acquire)) {
            break;
        }
        sintra::world() << Stop{};
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!got_stop_ack.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "FAIL: timed out waiting for StopAck\n");
        std::abort();
    }

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

int receiver_process()
{
    sintra::barrier("start");

    auto msg = sintra::receive<DataMessage>();
    if (msg.value != 57) {
        std::fprintf(stderr, "FAIL: expected 57, got %d\n", msg.value);
        std::abort();
    }
    if (std::fabs(msg.score - 2.718) > 0.02) {
        std::fprintf(stderr, "FAIL: expected score near 2.718, got %f\n", msg.score);
        std::abort();
    }
    sintra::world() << Ack{};

    sintra::receive<Stop>();
    sintra::world() << StopAck{};

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // Check if this is a spawned child process
    const bool is_coordinator = !std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    sintra::init(argc, const_cast<const char* const*>(argv),
                 sender_process, receiver_process);

    if (is_coordinator) {
        sintra::barrier("done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (is_coordinator) {
        std::fprintf(stderr, "receive test PASSED\n");
    }

    return 0;
}
