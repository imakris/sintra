//
// Sintra Coordinator Crash Ping-Pong Test
//
// This test intentionally crashes the coordinator process after receiving
// a Pong message from a worker process. The crash allows the infrastructure
// to exercise stack collection for all participating processes.
//
// Test structure:
// - Coordinator (branch index 0):
//     * Activates a Pong handler that logs a failure message, waits briefly
//       to give the stack collector time to attach, and then calls std::abort().
//     * Starts a background thread so that multi-threaded stack traces are
//       available when the crash occurs.
// - Worker process (branch index 1):
//     * Responds to Ping messages with Pong messages and then blocks
//       indefinitely so that its stack can also be sampled.
//
// Expected outcome:
// - The coordinator process crashes deliberately, causing the overall test to
//   fail. Stack capture mechanisms should provide call stacks for both the
//   coordinator and the worker process.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Ping {};
struct Pong {};

int worker_process()
{
    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });

    sintra::barrier("coordinator-crash-ready", "_sintra_all_processes");

    std::unique_lock<std::mutex> lock(wait_mutex);
    wait_cv.wait(lock, [] { return false; });

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
        auto keep_running = std::make_shared<std::atomic<bool>>(true);
        std::thread background_thread([keep_running] {
            while (keep_running->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        background_thread.detach();

        sintra::activate_slot([keep_running](Pong) {
            keep_running->store(false, std::memory_order_release);
            std::fprintf(stderr, "[FAIL] Coordinator crashing after receiving Pong\n");
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::abort();
        });

        sintra::barrier("coordinator-crash-ready", "_sintra_all_processes");

        sintra::world() << Ping();

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    sintra::finalize();

    if (is_spawned) {
        return 0;
    }

    return 1;
}
