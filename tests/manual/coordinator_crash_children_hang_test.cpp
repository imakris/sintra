// coordinator_crash_children_hang_test.cpp
//
// Test for testing harness crash/hang detection capabilities.
//
// Scenario: Coordinator crashes immediately after init, leaving two children:
// - Child 1 crashes after 5 seconds
// - Child 2 hangs indefinitely
//
// Expected behavior: Testing harness should:
// - Detect coordinator crash
// - Detect child 1 crash after 5s
// - Detect child 2 hang
// - Collect stack traces from all processes
// - Exit on its own (via timeout)

#include <sintra/sintra.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int coordinator_process()
{
    std::fprintf(stderr, "Coordinator: Starting (will crash immediately)\n");
    std::fflush(stderr);

    // Crash immediately
    std::abort();

    return 0; // unreachable
}

int child_crash_after_delay()
{
    std::fprintf(stderr, "Child 1: Starting (will crash after 5 seconds)\n");
    std::fflush(stderr);

    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::fprintf(stderr, "Child 1: Crashing now\n");
    std::fflush(stderr);

    std::abort();

    return 0; // unreachable
}

int child_hang_forever()
{
    std::fprintf(stderr, "Child 2: Starting (will hang indefinitely)\n");
    std::fflush(stderr);

    // Infinite loop - process hangs here
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0; // unreachable
}

bool has_branch_flag(int argc, char* argv[])
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
    const bool is_spawned = has_branch_flag(argc, argv);

    // Define child processes only (coordinator runs in parent)
    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(child_crash_after_delay);
    processes.emplace_back(child_hang_forever);

    sintra::init(argc, argv, processes);

    // Coordinator process (parent): crashes immediately
    if (!is_spawned) {
        coordinator_process();
    }

    // Should never reach finalize due to crashes/hangs
    sintra::finalize();

    return 0;
}
