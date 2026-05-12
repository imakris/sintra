//
// Sintra library, example 3
//
// This example demonstrates single-process message dispatch - using Sintra's
// messaging system within a single process without spawning multiple processes.
//
// This is useful when you want to use Sintra's type-safe message passing and
// slot activation features within a single process, perhaps for a lightweight
// component communication system or for testing individual components.
//
// In this example, there is only one process, with 3 registered slots:
// - A ping_slot that responds to ping_t messages by sending pong_t messages
// - A pong_slot that responds to pong_t messages by sending ping_t messages
// - A benchmark_slot that counts and reports the message throughput
//
// The example shows the same ping-pong pattern as example 1, but entirely
// within the coordinator process without any separate worker processes.
//

#include <sintra/sintra.h>
#include <iostream>


using namespace std;
using namespace sintra;


struct ping_t {};
struct pong_t {};

static double timeout_in_seconds = 2.0;

int main(int argc, char* argv[])
{
    // Initialize Sintra in single-process mode (no worker processes specified)
    init(argc, argv);

    // Define a slot that handles ping_t messages by sending pong_t responses
    auto ping_slot = [](ping_t) {
        world() << pong_t();
    };

    // Define a slot that handles pong_t messages by sending ping_t responses
    auto pong_slot = [](pong_t) {
        world() << ping_t();
    };

    // Set up benchmarking to measure message throughput
    double   ts      = get_wtime();
    double   next_ts = ts + 1.;
    uint64_t counter = 0;

    // Define a slot that counts ping_t messages and reports throughput
    auto benchmark_slot = [&](ping_t) {
        double ts = get_wtime();
        if (ts > next_ts) {
            next_ts = ts + 1.;
            console() << counter << " ping-pongs / second\n";
            counter = 0;
        }
        counter++;
    };

    // Activate all three slots
    activate_slot(ping_slot);
    activate_slot(pong_slot);
    activate_slot(benchmark_slot);

    // Send the initial ping_t message to start the ping-pong cycle
    world() << ping_t();

    // Let it run for the configured timeout period
    std::this_thread::sleep_for(std::chrono::duration<double>(timeout_in_seconds));

    // Clean up Sintra
    shutdown();

    return 0;
}
