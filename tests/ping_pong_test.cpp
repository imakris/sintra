//
// Sintra Single-Process Ping-Pong Test
//
// This test validates single-process message dispatch functionality.
// It corresponds to example_3 and tests the following features:
// - Message passing within a single process (no worker processes)
// - Multiple slots responding to different message types
// - High-frequency message throughput
// - Timeout-based test completion
//
// Test structure:
// - Three slots all within the coordinator process:
//   1. ping_slot: Responds to Ping with Pong
//   2. pong_slot: Responds to Pong with Ping
//   3. benchmarking_slot: Counts messages and triggers completion
//
// The test sends an initial Ping and waits for 1000 ping-pong cycles
// to complete within a 5-second timeout.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <future>

struct Ping {};
struct Pong {};

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    constexpr int kTargetPingCount = 1000;
    std::atomic<int> ping_count{0};
    std::atomic<bool> done{false};
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    auto ping_slot = [&](Ping) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        sintra::world() << Pong();
    };

    auto pong_slot = [&](Pong) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        sintra::world() << Ping();
    };

    auto benchmarking_slot = [&](Ping) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        int count = ping_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= kTargetPingCount) {
            bool expected = false;
            if (done.compare_exchange_strong(expected, true)) {
                done_promise.set_value();
            }
        }
    };

    sintra::activate_slot(ping_slot);
    sintra::activate_slot(pong_slot);
    sintra::activate_slot(benchmarking_slot);

    sintra::world() << Ping();

    const auto wait_status = done_future.wait_for(std::chrono::seconds(5));
    if (wait_status != std::future_status::ready) {
        sintra::finalize();
        return 1;
    }

    sintra::finalize();

    return ping_count.load(std::memory_order_relaxed) == kTargetPingCount ? 0 : 1;
}
