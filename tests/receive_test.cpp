//
// Sintra receive<MESSAGE_T>() Test
//
// This test validates the receive<MESSAGE_T>() synchronous message receiving function.
//
// The test verifies:
// - Blocking receive with empty message type (signal-style)
// - Blocking receive with POD message type containing data
// - Message data is correctly captured and returned
//
// Note: This test does NOT test calling receive() from within a handler
// (which would deadlock) as that is documented as undefined behavior.
//

#include <sintra/sintra.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

// Empty message type (signal-style)
struct Stop {};

// POD message type with data
struct DataMessage {
    int value;
    double score;
};

std::atomic<bool> g_test_passed{true};

int sender_process()
{
    // Wait for receiver to be ready
    sintra::barrier("receiver_ready");

    // Send a data message
    sintra::world() << DataMessage{42, 3.14};

    // Small delay to ensure message is processed
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send stop signal
    sintra::world() << Stop{};

    sintra::barrier("test_complete");
    return 0;
}

int receiver_process()
{
    // Signal that we're ready
    sintra::barrier("receiver_ready");

    // Use receive to wait for the data message
    auto msg = sintra::receive<DataMessage>();

    // Verify the received data
    if (msg.value != 42) {
        std::fprintf(stderr, "receiver: expected value 42, got %d\n", msg.value);
        g_test_passed = false;
    }

    if (msg.score < 3.13 || msg.score > 3.15) {
        std::fprintf(stderr, "receiver: expected score ~3.14, got %f\n", msg.score);
        g_test_passed = false;
    }

    // Use receive to wait for stop signal (empty message)
    sintra::receive<Stop>();

    std::fprintf(stderr, "receiver: received Stop signal\n");

    sintra::barrier("test_complete");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, const_cast<const char* const*>(argv),
                     sender_process, receiver_process);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Failed to initialize sintra: %s\n", e.what());
        return 1;
    }

    sintra::barrier("test_complete", "_sintra_all_processes");
    sintra::finalize();

    if (!g_test_passed) {
        std::fprintf(stderr, "receive test FAILED\n");
        return 1;
    }

    std::fprintf(stderr, "receive test PASSED\n");
    return 0;
}
