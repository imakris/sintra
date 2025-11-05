//
// Sintra library, example 6
//
// This example demonstrates the new send_to() functionality for unicast messaging.
//
// In this example, there are 3 user processes:
// - Process 1 sends a unicast message directly to Process 2 using send_to
// - Process 2 responds with a unicast message back to Process 1
// - Process 3 observes and verifies it doesn't receive unicast messages
//
// This demonstrates "fire-and-forget" unicast (no acknowledgement, unlike RPC)
//

#include <sintra/sintra.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>


using namespace std;
using namespace sintra;


struct UnicastMessage
{
    uint64_t counter;
    instance_id_type target_id;  // Target for response
};

struct IdExchange
{
    instance_id_type sender_id;
    uint64_t process_index;
};

struct Stop {};


instance_id_type g_process_1_id = 0;
instance_id_type g_process_2_id = 0;


int process_1()
{
    // Process 1: Sender
    console() << "Process 1: Starting sender\n";

    Transceiver t1;
    g_process_1_id = t1.instance_id();

    std::atomic<uint64_t> received_count{0};

    // Receive unicast responses from Process 2
    t1.activate([&](UnicastMessage msg) {
        received_count++;
        console() << "Process 1: Received unicast response #" << msg.counter << "\n";
    }, Typed_instance_id<void>(any_local_or_remote));

    // Receive ID exchange messages
    activate_slot([](IdExchange msg) {
        if (msg.process_index == 2) {
            g_process_2_id = msg.sender_id;
            console() << "Process 1: Received Process 2 ID: " << g_process_2_id << "\n";
        }
    });

    barrier("transceiver creation barrier");

    // Broadcast our ID
    world() << IdExchange{g_process_1_id, 1};

    barrier("id exchange barrier");

    console() << "Process 1: Sending unicast messages to Process 2 (ID: "
              << g_process_2_id << ")\n";

    // Send 5 unicast messages to Process 2
    for (uint64_t i = 0; i < 5; i++) {
        using MT = Message<Enclosure<UnicastMessage>>;
        t1.send_to<MT, Transceiver>(g_process_2_id, UnicastMessage{i, g_process_1_id});
        console() << "Process 1: Sent unicast message #" << i << " to Process 2\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait a bit for responses
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    console() << "Process 1: Received " << received_count.load()
              << " unicast responses (expected 5)\n";

    // Wait for stop signal
    std::condition_variable cv;
    std::mutex m;
    bool done = false;

    activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]{return done;});

    deactivate_all_slots();
    return 0;
}


int process_2()
{
    // Process 2: Receiver and responder
    console() << "Process 2: Starting receiver\n";

    Transceiver t2;
    g_process_2_id = t2.instance_id();

    std::atomic<uint64_t> received_count{0};

    // Receive unicast messages from Process 1
    t2.activate([&](UnicastMessage msg) {
        received_count++;
        console() << "Process 2: Received unicast message #" << msg.counter << "\n";

        // Send unicast response back using the target_id from the message
        using MT = Message<Enclosure<UnicastMessage>>;
        t2.send_to<MT, Transceiver>(msg.target_id, UnicastMessage{msg.counter, g_process_2_id});
    }, Typed_instance_id<void>(any_local_or_remote));

    // Receive ID exchange messages
    activate_slot([](IdExchange msg) {
        if (msg.process_index == 1) {
            g_process_1_id = msg.sender_id;
            console() << "Process 2: Received Process 1 ID: " << g_process_1_id << "\n";
        }
    });

    barrier("transceiver creation barrier");

    // Broadcast our ID
    world() << IdExchange{g_process_2_id, 2};

    barrier("id exchange barrier");

    // Wait for stop signal
    std::condition_variable cv;
    std::mutex m;
    bool done = false;

    activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]{return done;});

    console() << "Process 2: Received " << received_count.load()
              << " total unicast messages (expected 5)\n";

    deactivate_all_slots();
    return 0;
}


int process_3()
{
    // Process 3: Observer (should NOT receive unicast messages)
    console() << "Process 3: Starting observer\n";

    std::atomic<uint64_t> unicast_received{0};

    activate_slot([&](UnicastMessage msg) {
        unicast_received++;
        console() << "Process 3: ERROR - Received unicast message #" << msg.counter
                  << " (should not happen!)\n";
    });

    activate_slot([](IdExchange msg) {
        console() << "Process 3: Observed ID exchange from process " << msg.process_index << "\n";
    });

    barrier("transceiver creation barrier");
    barrier("id exchange barrier");

    // Wait for stop signal
    std::condition_variable cv;
    std::mutex m;
    bool done = false;

    activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]{return done;});

    if (unicast_received.load() == 0) {
        console() << "Process 3: SUCCESS - Did not receive any unicast messages (as expected)\n";
    } else {
        console() << "Process 3: FAILURE - Received " << unicast_received.load()
                  << " unicast messages (should be 0)\n";
    }

    deactivate_all_slots();
    return 0;
}


int main(int argc, char* argv[])
{
    init(argc, argv, process_1, process_2, process_3);

    if (process_index() == 0) {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(3s);
        console() << "Main: Sending stop signal\n";
        world() << Stop();
    }

    // Ensure all processes have observed the stop signal
    barrier("example-6-finished", "_sintra_all_processes");

    finalize();

    return 0;
}
