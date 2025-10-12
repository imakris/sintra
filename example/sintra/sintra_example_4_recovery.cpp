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
#include <sintra/detail/managed_process.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;
using namespace sintra;


struct Done {};


int process_observer()
{
    console() << "[Observer] Starting - waiting for Done signal\n";

    condition_variable cv;
    mutex m;
    bool done = false;

    activate_slot([&](Done) {
        console() << "[Observer] Received Done signal!\n";
        lock_guard<mutex> lk(m);
        done = true;
        cv.notify_one();
    });

    // Wait for the Done signal
    unique_lock<mutex> lk(m);
    cv.wait(lk, [&]{return done;});

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
    finalize();

    if (process_index() == 0) {
        cout << "\nRecovery demonstration complete!\n";
        cout << "The worker process crashed on its first run, was automatically\n";
        cout << "restarted by the coordinator, and completed successfully on its\n";
        cout << "second run.\n\n";

        do {
            cout << "Press ENTER to continue...";
        } while (cin.get() != '\n');
    }

    return 0;
}
