//
// Sintra library, example 6
//
// This example demonstrates the SINTRA_UNICAST functionality for fire-and-forget
// unicast messaging.
//
// In this example, there are 3 user processes:
// - Process 1 sends fire-and-forget messages directly to Process 2 using exported message handlers
// - Process 2 receives and counts the messages
// - Process 3 observes and verifies it doesn't receive unicast messages
//
// This demonstrates fire-and-forget unicast (no acknowledgement, unlike RPC) using the
// RPC infrastructure but without reply overhead.
//

#include <sintra/sintra.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>


using namespace std;
using namespace sintra;


struct Unicast_message
{
    uint64_t counter;
};

struct Id_exchange
{
    instance_id_type sender_id;
    uint64_t process_index;
};

struct Stop {};


instance_id_type g_process_1_id = 0;
instance_id_type g_process_2_id = 0;


// Transceiver type that can receive unicast messages
struct Message_receiver : Derived_transceiver<Message_receiver>
{
    atomic<uint64_t>* received_count_ptr;
    string instance_name_str;

    Message_receiver(atomic<uint64_t>* counter_ptr)
        : received_count_ptr(counter_ptr) {}

    void handle_unicast(const Unicast_message& msg)
    {
        (*received_count_ptr)++;
        console() << instance_name() << ": Received unicast message #" << msg.counter << "\n";
    }

    const char* instance_name() { return instance_name_str.c_str(); }

    // Export the handler as a fire-and-forget message (no reply sent)
    SINTRA_UNICAST(handle_unicast)
};


int process_1()
{
    // Process 1: Sender
    console() << "Process 1: Starting sender\n";

    std::atomic<uint64_t> received_count{0};
    Message_receiver receiver1(&received_count);
    receiver1.instance_name_str = "Process 1";
    g_process_1_id = receiver1.instance_id();

    // Receive ID exchange messages
    activate_slot([](Id_exchange msg) {
        if (msg.process_index == 2) {
            g_process_2_id = msg.sender_id;
            console() << "Process 1: Received Process 2 ID: " << g_process_2_id << "\n";
        }
    });

    barrier("transceiver creation barrier");

    // Broadcast our ID
    world() << Id_exchange{g_process_1_id, 1};

    barrier("id exchange barrier");

    console() << "Process 1: Sending unicast messages to Process 2 (ID: "
              << g_process_2_id << ")\n";

    // Send 5 fire-and-forget unicast messages to Process 2
    for (uint64_t i = 0; i < 5; i++) {
        Message_receiver::rpc_handle_unicast(g_process_2_id, Unicast_message{i});
        console() << "Process 1: Sent unicast message #" << i << " to Process 2\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait a bit to ensure messages are delivered
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    console() << "Process 1: Sent all messages\n";

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
    // Process 2: Receiver
    console() << "Process 2: Starting receiver\n";

    std::atomic<uint64_t> received_count{0};
    Message_receiver receiver2(&received_count);
    receiver2.instance_name_str = "Process 2";
    g_process_2_id = receiver2.instance_id();

    // Receive ID exchange messages
    activate_slot([](Id_exchange msg) {
        if (msg.process_index == 1) {
            g_process_1_id = msg.sender_id;
            console() << "Process 2: Received Process 1 ID: " << g_process_1_id << "\n";
        }
    });

    barrier("transceiver creation barrier");

    // Broadcast our ID
    world() << Id_exchange{g_process_2_id, 2};

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
    Message_receiver receiver3(&unicast_received);
    receiver3.instance_name_str = "Process 3";

    activate_slot([](Id_exchange msg) {
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
    }
    else {
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
        std::this_thread::sleep_for(2s);
        console() << "Main: Sending stop signal\n";
        world() << Stop();
    }

    // Ensure all processes have observed the stop signal
    barrier("example-6-finished", "_sintra_all_processes");

    finalize();

    return 0;
}
