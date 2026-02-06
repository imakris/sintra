// children_crash_coordinator_deadlock_test.cpp
//
// Test for testing harness crash/deadlock detection capabilities.
//
// Scenario: Two children crash quickly, coordinator gets into a deadlock
// - Child 1 crashes immediately
// - Child 2 crashes after 2 seconds
// - Coordinator deadlocks (hangs waiting on mutex that's never released)
//
// Expected behavior: Testing harness should:
// - Detect both child crashes
// - Detect coordinator deadlock/hang
// - Collect stack traces from all processes
// - Exit on its own (via timeout)

#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

// Global mutex that will be used to create deadlock
std::mutex g_deadlock_mutex;

int coordinator_deadlock_process()
{
    std::fprintf(stderr, "Coordinator: Starting (will deadlock)\n");
    std::fflush(stderr);

    // Give children time to start
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::fprintf(stderr, "Coordinator: Entering deadlock...\n");
    std::fflush(stderr);

    // Lock the mutex
    g_deadlock_mutex.lock();

    std::fprintf(stderr, "Coordinator: Got first lock, attempting second lock (will deadlock)\n");
    std::fflush(stderr);

    // Try to lock it again - this will deadlock since std::mutex is not recursive
    g_deadlock_mutex.lock(); // DEADLOCK HERE

    std::fprintf(stderr, "Coordinator: This should never print\n");
    std::fflush(stderr);

    return 0; // unreachable
}

int child_crash_immediately()
{
    std::fprintf(stderr, "Child 1: Starting (will crash immediately)\n");
    std::fflush(stderr);

    sintra::detail::debug_aware_abort();

    return 0; // unreachable
}

int child_crash_after_short_delay()
{
    std::fprintf(stderr, "Child 2: Starting (will crash after 2 seconds)\n");
    std::fflush(stderr);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::fprintf(stderr, "Child 2: Crashing now\n");
    std::fflush(stderr);

    sintra::detail::debug_aware_abort();

    return 0; // unreachable
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);

    // Define child processes only (coordinator runs in parent)
    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(child_crash_immediately);
    processes.emplace_back(child_crash_after_short_delay);

    sintra::init(argc, argv, processes);

    // Coordinator process (parent): deadlocks on mutex
    if (!is_spawned) {
        coordinator_deadlock_process();
    }

    // Should never reach finalize due to crashes/deadlock
    sintra::finalize();

    return 0;
}
