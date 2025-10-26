//
// Sintra Two-Process Ping-Pong Crash Test
//
// This test intentionally crashes the coordinator process after a single
// ping-pong exchange with a worker process. The failure path is used to
// validate that our test harness captures stack traces for every thread in
// every process that participates in the exchange.
//
// Test structure:
// - Coordinator process (main):
//     * Activates a Pong handler that aborts the process as soon as it
//       receives a response from the worker.
//     * Initiates the first Ping message once the worker signals readiness.
// - Worker process:
//     * Responds to Ping messages with Pong.
//     * Waits on a barrier that the coordinator never reaches, ensuring it
//       stays alive long enough for the harness to capture its stacks after
//       the coordinator crashes.
//
// Expected outcome:
//     The coordinator calls std::abort() from the Pong handler, which
//     terminates the process with a failure. The worker remains blocked on a
//     barrier so that the stack collection logic can inspect it.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>
#include <string_view>

namespace {

struct Ping {};
struct Pong {};

constexpr const char* kWorkerReadyBarrier = "ping-pong-worker-ready";
constexpr const char* kCrashBarrier = "coordinator-crashed";

int worker_process()
{
    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });

    // Ensure the coordinator waits until the worker is fully ready.
    sintra::barrier(kWorkerReadyBarrier);

    // Block indefinitely; the coordinator never arrives here after aborting.
    sintra::barrier(kCrashBarrier, "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        std::atomic<bool> pong_received{false};

        sintra::activate_slot([&](Pong) {
            pong_received.store(true, std::memory_order_release);
            // Crash immediately so the test fails and stack traces can be captured.
            std::abort();
        });

        sintra::barrier(kWorkerReadyBarrier);

        sintra::world() << Ping();

        // Give the runtime a short window to deliver the message and trigger the crash.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline &&
               !pong_received.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // This line is never reached once std::abort() fires, but remains for clarity.
        sintra::barrier(kCrashBarrier, "_sintra_all_processes");
    }

    sintra::finalize();

    return 0;
}
