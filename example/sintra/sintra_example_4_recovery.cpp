//
// Sintra library, example 4
//
// This example demonstrates process recovery - the ability of the Sintra framework
// to automatically respawn and restart a process that crashes.
//
// When a process enables recovery using enable_recovery(), the coordinator monitors
// it and will automatically restart it if it terminates unexpectedly. The restarted
// process can detect which "occurrence" it is running (first run, second run, etc.)
// using the s_recovery_occurrence variable.
//
// In this example, we have two processes:
// - An "observer" process that waits for a completion signal
// - A "worker" process that deliberately crashes on its first run, then runs
//   successfully on its second run after being automatically restarted
//
// The program demonstrates:
// - How to enable recovery for a process
// - How to detect which recovery occurrence we are in
// - How the coordinator automatically respawns crashed processes
// - How to coordinate between the original and recovered process
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>
#include <iostream>
#include <chrono>
#include <thread>

using namespace std;
using namespace sintra;


struct Done {};


int process_observer()
{
    console() << "[Observer] Starting - waiting for Done signal\n";
    receive<Done>();
    console() << "[Observer] Received Done signal!\n";

    deactivate_all_slots();
    console() << "[Observer] Exiting normally\n";
    return 0;
}


int process_worker()
{
    // Enable recovery for this process - if it crashes, the coordinator will restart it
    enable_recovery();

    // s_recovery_occurrence tells us which run this is:
    // 0 = first run, 1 = first recovery, 2 = second recovery, etc.
    uint32_t occurrence = s_recovery_occurrence;

    if (occurrence == 0) {
        // First run - deliberately crash to demonstrate recovery
        console() << "[Worker] First run (occurrence " << occurrence << ") - will crash!\n";

        // Give time for the message to be sent
        this_thread::sleep_for(chrono::milliseconds(100));

        // Crash! (nullptr dereference causes segmentation fault)
        console() << "[Worker] About to crash...\n";
        int* null_ptr = nullptr;
        *null_ptr = 42;  // Crash by dereferencing null pointer
        return 1;  // Never reached, but suppresses compiler warning
    }
    else {
        // Recovered run - complete successfully
        console() << "[Worker] Recovered run (occurrence " << occurrence << ") - completing successfully\n";

        // Signal completion
        world() << Done();

        console() << "[Worker] Done signal sent, exiting normally\n";
        return 0;
    }
}


int main(int argc, char* argv[])
{
    init(argc, argv, process_observer, process_worker);

    // Coordinate teardown with the worker so the runtime is not finalized while
    // a recovered process is still completing its work.
    barrier("example-4-finished", "_sintra_all_processes");

    finalize();

    if (process_index() == 0) {
        cout << "\nRecovery demonstration complete.\n";
    }

    return 0;
}
