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

#include "test_trace.h"

#include <atomic>
#include <chrono>
#include <future>

struct Ping {};
struct Pong {};

int main(int argc, char* argv[])
{
    using sintra::test_trace::trace;

    trace("test.ping_pong.main", [&](auto& os) { os << "event=init.begin"; });
    sintra::init(argc, argv);
    trace("test.ping_pong.main", [&](auto& os) { os << "event=init.end"; });

    constexpr int kTargetPingCount = 1000;
    std::atomic<int> ping_count{0};
    std::atomic<bool> done{false};
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    auto ping_slot = [&](Ping) {
        trace("test.ping_pong.slot", [&](auto& os) { os << "type=Ping handler=ping"; });
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        trace("test.ping_pong.slot", [&](auto& os) { os << "type=Ping action=send_pong"; });
        sintra::world() << Pong();
    };

    auto pong_slot = [&](Pong) {
        trace("test.ping_pong.slot", [&](auto& os) { os << "type=Pong handler=pong"; });
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        trace("test.ping_pong.slot", [&](auto& os) { os << "type=Pong action=send_ping"; });
        sintra::world() << Ping();
    };

    auto benchmarking_slot = [&](Ping) {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        int count = ping_count.fetch_add(1, std::memory_order_relaxed) + 1;
        trace("test.ping_pong.slot", [&](auto& os) { os << "type=Ping handler=benchmark count=" << count; });
        if (count >= kTargetPingCount) {
            bool expected = false;
            if (done.compare_exchange_strong(expected, true)) {
                trace("test.ping_pong.slot", [&](auto& os) { os << "type=Ping handler=benchmark event=complete"; });
                done_promise.set_value();
            }
        }
    };

    sintra::activate_slot(ping_slot);
    sintra::activate_slot(pong_slot);
    sintra::activate_slot(benchmarking_slot);

    trace("test.ping_pong.main", [&](auto& os) { os << "event=send_initial_ping"; });
    sintra::world() << Ping();

    trace("test.ping_pong.main", [&](auto& os) { os << "event=wait.begin"; });
    const auto wait_status = done_future.wait_for(std::chrono::seconds(5));
    trace("test.ping_pong.main", [&](auto& os) {
        os << "event=wait.end status="
           << (wait_status == std::future_status::ready ? "ready" : "timeout");
    });
    if (wait_status != std::future_status::ready) {
        trace("test.ping_pong.main", [&](auto& os) { os << "event=finalize.begin"; });
        sintra::finalize();
        trace("test.ping_pong.main", [&](auto& os) { os << "event=finalize.end"; });
        return 1;
    }

    trace("test.ping_pong.main", [&](auto& os) { os << "event=finalize.begin"; });
    sintra::finalize();
    trace("test.ping_pong.main", [&](auto& os) { os << "event=finalize.end"; });

    const bool success = ping_count.load(std::memory_order_relaxed) == kTargetPingCount;
    trace("test.ping_pong.main", [&](auto& os) { os << "event=exit success=" << success; });
    return success ? 0 : 1;
}
