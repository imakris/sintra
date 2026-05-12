//
// Sintra Single-Process ping_t-pong_t Test
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
//   1. ping_slot: Responds to ping_t with pong_t
//   2. pong_slot: Responds to pong_t with ping_t
//   3. benchmarking_slot: Counts messages and triggers completion
//
// The test sends an initial ping_t and waits for 10'000 ping-pong cycles
// to complete within a 5-second timeout while periodically yielding to
// encourage scheduler churn.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

struct ping_t {};
struct pong_t {};

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    constexpr int k_target_ping_count = 10000;
    std::atomic<int> ping_count{0};
    std::atomic<bool> done{false};
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    auto ping_slot = [&](ping_t) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        sintra::world() << pong_t();
    };

    auto pong_slot = [&](pong_t) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        sintra::world() << ping_t();
    };

    auto benchmarking_slot = [&](ping_t) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        int count = ping_count.fetch_add(1) + 1;
        if (count >= k_target_ping_count) {
            bool expected = false;
            if (done.compare_exchange_strong(expected, true)) {
                done_promise.set_value();
            }
        }
        else
        if ((count % 256) == 0) {
            std::this_thread::yield();
        }
    };

    sintra::activate_slot(ping_slot);
    sintra::activate_slot(pong_slot);
    sintra::activate_slot(benchmarking_slot);

    sintra::world() << ping_t();

    const auto wait_status = done_future.wait_for(std::chrono::seconds(5));
    if (wait_status != std::future_status::ready) {
        sintra::shutdown();
        return 1;
    }

    sintra::shutdown();

    return ping_count.load() == k_target_ping_count ? 0 : 1;
}
