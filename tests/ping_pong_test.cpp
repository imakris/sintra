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
