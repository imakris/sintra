//
// Sintra Ping-Pong Crash Test
//
// This test intentionally crashes the coordinator process while two
// processes exchange a single Ping/Pong message. It is designed to
// exercise crash handling and stack collection in the test harness.
//
// Test structure:
// - Coordinator process activates a Pong slot that aborts when invoked.
// - Worker process activates a Ping slot that responds with Pong and
//   then waits indefinitely so the process remains alive for stack
//   collection.
// - After synchronizing via a barrier, the coordinator sends a Ping.
//   Receipt of the Pong causes the coordinator to crash via std::abort.
//
// Expectations:
// - The coordinator process terminates abnormally.
// - The worker process remains running, allowing the test harness to
//   capture stack traces for both processes.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Ping {};
struct Pong {};

constexpr const char* kBarrierName = "two-process-ping-pong-ready";

int worker_process()
{
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool stop = false;

    // Respond to the coordinator's Ping with a Pong.
    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });

    // Signal readiness to the coordinator.
    sintra::barrier(kBarrierName);

    // Keep the worker process alive until externally terminated so that
    // stack traces can be collected after the coordinator crashes.
    std::unique_lock<std::mutex> lk(wait_mutex);
    wait_cv.wait(lk, [&] { return stop; });
    return 0;
}

bool is_spawned_process(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool spawned = is_spawned_process(argc, argv);

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);

    // Only the coordinator process should set up the crash-inducing slot
    // and send the initial Ping message.
    if (!spawned) {
        std::atomic<bool> crash_triggered{false};

        sintra::activate_slot([&](Pong) {
            crash_triggered.store(true, std::memory_order_release);
            std::abort();
        });

        // Spawn an auxiliary thread so that the crash produces multiple
        // thread stacks for debugging purposes.
        std::thread background_worker([&]() {
            while (!crash_triggered.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        background_worker.detach();

        sintra::barrier(kBarrierName);

        sintra::world() << Ping();

        // The coordinator is expected to crash immediately when the Pong
        // slot executes. If, for any reason, it does not crash, fail the
        // test explicitly.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sintra::finalize();
        return 1;
    }

    // Spawned worker processes should never reach this point because the
    // worker_process function blocks indefinitely. In case that changes in
    // the future, park the process so that stack capture remains possible.
    std::mutex parked_mutex;
    std::condition_variable parked_cv;
    std::unique_lock<std::mutex> lk(parked_mutex);
    parked_cv.wait(lk);
    return 0;
}
