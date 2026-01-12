//
// Sintra receive<MESSAGE_T>() Test
//
// This test validates the receive<MESSAGE_T>() synchronous message receiving function.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace {

struct Stop {};

struct DataMessage {
    int value;
    double score;
};

int sender_process()
{
    sintra::barrier("start");

    // Send messages in a loop
    for (int i = 0; i < 5; i++) {
        sintra::world() << DataMessage{42, 3.14};
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (int i = 0; i < 3; i++) {
        sintra::world() << Stop{};
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

int receiver_process()
{
    sintra::barrier("start");

    auto msg = sintra::receive<DataMessage>();
    if (msg.value != 42) {
        std::fprintf(stderr, "FAIL: expected 42, got %d\n", msg.value);
        std::abort();
    }

    sintra::receive<Stop>();
    std::fprintf(stderr, "receiver: OK\n");

    sintra::barrier("done", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // Check if this is a spawned child process
    const bool is_coordinator = !std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    sintra::init(argc, const_cast<const char* const*>(argv),
                 sender_process, receiver_process);

    if (is_coordinator) {
        sintra::barrier("done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (is_coordinator) {
        std::fprintf(stderr, "receive test PASSED\n");
    }

    return 0;
}
