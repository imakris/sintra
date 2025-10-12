/*
Sintra library, example 5

This example exercises the inter-process barrier mechanism while other
messages are flowing through the system. 
Two worker processes repeatedly send iteration markers to
a coordinator process and then rendezvous on a barrier.  The coordinator
waits until it has received the expected markers for the current iteration
before joining the barrier itself. 
*/

#include <sintra/sintra.h>

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>

namespace {

constexpr std::size_t kWorkerCount = 2;
constexpr std::size_t kIterations  = 64;

struct Iteration_marker
{
    std::uint32_t worker;
    std::uint32_t iteration;
};

int coordinator_process()
{
    using namespace sintra;

    std::mutex mutex;
    std::condition_variable cv;
    std::size_t messages_in_iteration = 0;
    std::uint32_t current_iteration   = 0;

    activate_slot([&](const Iteration_marker& marker) {
        std::lock_guard<std::mutex> lock(mutex);
        // Workers never advance to the next iteration before the coordinator
        // joins the barrier, therefore every marker must belong to the
        // coordinator's current iteration.
        if (marker.iteration != current_iteration) {
            throw std::runtime_error("iteration mismatch in coordinator");
        }
        ++messages_in_iteration;
        if (messages_in_iteration == kWorkerCount) {
            cv.notify_one();
        }
    });
    barrier("barrier-flush-ready");

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return messages_in_iteration == kWorkerCount; });
        messages_in_iteration = 0;
        ++current_iteration;
        lock.unlock();

        barrier("barrier-flush-iteration");
    }

    barrier("barrier-flush-done");
    std::cout << "Completed " << kIterations << " synchronized iterations.\n";
    return 0;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    barrier("barrier-flush-ready");

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        world() << Iteration_marker{worker_index, iteration};
        barrier("barrier-flush-iteration");
    }

    barrier("barrier-flush-done");
    return 0;
}

int worker0_process()
{
    return worker_process(0);
}

int worker1_process()
{
    return worker_process(1);
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv, coordinator_process, worker0_process, worker1_process);
    sintra::finalize();
    return 0;
}
