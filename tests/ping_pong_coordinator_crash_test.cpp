//
// Sintra Coordinator Crash Ping-Pong Test
//
// This regression test intentionally crashes the coordinator process after a
// handful of ping-pong exchanges with a single worker process.  The purpose is
// to exercise the crash-handling and stack-capture logic in the test harness.
//
// Test structure:
//   * Process 0 (coordinator) owns the main() entry point.
//   * Process 1 (worker) responds to Ping messages with Pong messages.
//   * After a small number of Pong responses the coordinator emits a fatal
//     error message, pauses briefly so that the test harness can attach, and
//     finally calls std::abort().
//
// Expectations:
//   * The overall test fails because the coordinator terminates abnormally.
//   * The failure message should include stack traces for both processes.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <string_view>

namespace {

struct Ping {};
struct Pong {};

constexpr int kCrashAfterPongs = 4;
constexpr auto kCrashMessage = "fatal error: coordinator crash requested";
constexpr auto kReadyBarrier = "coordinator-crash-ready";

int worker_process()
{
    // Echo Ping -> Pong to keep the ping-pong exchange alive.
    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });

    // Signal readiness to the coordinator and remain alive so the harness can
    // capture stacks from this process even after the coordinator aborts.
    sintra::barrier(kReadyBarrier);

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    return 0; // Unreachable
}

[[noreturn]] void coordinator_process()
{
    std::atomic<int> pong_count{0};

    // Keep an extra thread around so that stack dumps contain more than the
    // messaging thread.
    std::thread background([] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
    background.detach();

    auto pong_slot = [&](Pong) {
        int count = pong_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count < kCrashAfterPongs) {
            sintra::world() << Ping();
            return;
        }

        std::cerr << kCrashMessage << std::endl;
        // Give the test harness a small window to attach and capture stacks
        // before we abort.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::abort();
    };

    sintra::activate_slot(pong_slot);

    sintra::barrier(kReadyBarrier);

    // Kick off the ping-pong exchange.
    sintra::world() << Ping();

    // Wait indefinitely; the abort in pong_slot will terminate the process.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

bool is_spawned_process(int argc, char* argv[])
{
    return std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
}

} // namespace

int main(int argc, char* argv[])
{
    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);

    if (is_spawned_process(argc, argv)) {
        sintra::finalize();
        return 0;
    }

    coordinator_process();
}
