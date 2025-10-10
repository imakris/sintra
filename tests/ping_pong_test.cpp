#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <future>

struct Ping {};
struct Pong {};

namespace {

constexpr int kPingsPerIteration = 32;
constexpr int kRepetitions = 32;

int run_single_round()
{
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
        if (count >= kPingsPerIteration) {
            bool expected = false;
            if (done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                done_promise.set_value();
            }
        }
    };

    sintra::activate_slot(ping_slot);
    sintra::activate_slot(pong_slot);
    sintra::activate_slot(benchmarking_slot);

    sintra::world() << Ping();

    const auto wait_status = done_future.wait_for(std::chrono::milliseconds(100));
    sintra::deactivate_all_slots();

    if (wait_status != std::future_status::ready) {
        return 1;
    }

    return ping_count.load(std::memory_order_relaxed) == kPingsPerIteration ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    for (int i = 0; i < kRepetitions; ++i) {
        if (run_single_round() != 0) {
            sintra::finalize();
            return 1;
        }
    }

    sintra::finalize();
    return 0;
}
