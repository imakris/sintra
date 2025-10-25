/*
Sintra library, example 6

Demonstrates the processing fence barrier mode. A coordinator process publishes
work to a worker. Both processes join a processing fence barrier so the
coordinator resumes only after the worker's handler finishes processing the
message.
*/

#include <sintra/sintra.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

struct Work_item
{
    std::uint32_t value;
};

int coordinator_process()
{
    using namespace sintra;

    console() << "[Coordinator] Waiting for worker to arm handler\n";
    barrier("example-6-ready");

    console() << "[Coordinator] Publishing work item\n";
    world() << Work_item{42};

    console() << "[Coordinator] Waiting for processing fence completion\n";
    barrier<processing_fence_t>("example-6-processing");

    console() << "[Coordinator] Processing fence complete\n";
    barrier("example-6-finished");
    return 0;
}

int worker_process()
{
    using namespace sintra;

    bool work_processed = false;

    activate_slot([&](const Work_item& item) {
        console() << "[Worker] Handling work item " << item.value << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        work_processed = true;
        console() << "[Worker] Handler completed\n";
    });

    console() << "[Worker] Ready for work\n";
    barrier("example-6-ready");

    console() << "[Worker] Entering processing fence\n";
    barrier<processing_fence_t>("example-6-processing");

    if (work_processed) {
        console() << "[Worker] Processing fence observed handler completion\n";
    }
    else {
        console() << "[Worker] ERROR: handler did not run before fence completion\n";
        return 1;
    }

    barrier("example-6-finished");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv, coordinator_process, worker_process);

    // Keep the coordinator alive until the worker finishes the final barrier.
    sintra::barrier("example-6-final", "_sintra_all_processes");

    sintra::finalize();
    return 0;
}
