//
// LLDB Multi-Process Multi-Thread Stack Trace Test
//
// This test validates that lldb can capture stack traces from all threads
// of all processes when a test hangs and times out.
//
// Test structure:
// - Coordinator process: Manages ping-pong between two workers, with background threads
// - Worker 1: Responds to Ping with Pong (has multiple threads)
// - Worker 2: INTENTIONALLY HANGS after a few exchanges (has multiple threads)
//
// Expected behavior:
// - Test will hang when Worker 2 stops responding
// - Test harness should timeout
// - lldb should capture stacks from ALL threads of ALL 3 processes
// - Stack traces should include local variables
//

#include <sintra/sintra.h>

#include "test_environment.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Ping {
    int sequence_number;
    int worker_id;
};

struct Pong {
    int sequence_number;
    int worker_id;
};

struct Stop {};

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("lldb_multiprocess_hang");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "lldb_multiprocess_hang_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_log(const std::filesystem::path& file, const std::string& message)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    out << message << '\n';
}

// Background thread that does busy work with local variables
void background_worker_thread(int thread_id, std::atomic<bool>& running)
{
    // Local variables with interesting values for stack trace
    int loop_counter = 0;
    double accumulated_sum = 0.0;
    std::string thread_name = "BackgroundWorker-" + std::to_string(thread_id);

    std::fprintf(stderr, "[Thread %d] %s started\n", thread_id, thread_name.c_str());

    while (running.load(std::memory_order_acquire)) {
        loop_counter++;
        accumulated_sum += static_cast<double>(loop_counter) * 0.1;

        // Do some work to make the stack trace interesting
        volatile int computation = 0;
        for (int i = 0; i < 1000; ++i) {
            computation += i * thread_id;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // This variable should be visible in stack trace when hanging
        if (loop_counter % 10 == 0) {
            std::string status = "Thread " + std::to_string(thread_id) +
                                " iteration " + std::to_string(loop_counter);
            (void)status; // Should be visible in stack trace
        }
    }

    std::fprintf(stderr, "[Thread %d] %s stopping (completed %d iterations, sum=%.2f)\n",
                 thread_id, thread_name.c_str(), loop_counter, accumulated_sum);
}

// Thread that holds a mutex (to test deadlock detection)
void mutex_holder_thread(int thread_id, std::atomic<bool>& running, std::mutex& shared_mutex)
{
    std::string thread_name = "MutexHolder-" + std::to_string(thread_id);
    int lock_counter = 0;

    std::fprintf(stderr, "[Thread %d] %s started\n", thread_id, thread_name.c_str());

    while (running.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(shared_mutex);
            lock_counter++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::fprintf(stderr, "[Thread %d] %s stopping (held lock %d times)\n",
                 thread_id, thread_name.c_str(), lock_counter);
}

//
// Coordinator Process
//
int process_coordinator()
{
    std::fprintf(stderr, "[Coordinator] Starting\n");

    // Local variables for stack trace
    int total_messages_sent = 0;
    int pongs_received_worker1 = 0;
    int pongs_received_worker2 = 0;
    std::string coordinator_state = "initializing";

    // Start background threads
    std::atomic<bool> threads_running{true};
    std::mutex shared_mutex;
    std::vector<std::thread> background_threads;

    background_threads.emplace_back(background_worker_thread, 1, std::ref(threads_running));
    background_threads.emplace_back(background_worker_thread, 2, std::ref(threads_running));
    background_threads.emplace_back(mutex_holder_thread, 3, std::ref(threads_running), std::ref(shared_mutex));

    // Slot to receive Pong messages
    sintra::activate_slot([&](Pong pong) {
        std::string current_function = "coordinator_pong_handler";
        int received_sequence = pong.sequence_number;
        int from_worker = pong.worker_id;

        std::fprintf(stderr, "[Coordinator] Received Pong #%d from Worker %d\n",
                     received_sequence, from_worker);

        if (from_worker == 1) {
            pongs_received_worker1++;
        } else if (from_worker == 2) {
            pongs_received_worker2++;
        }

        // Send another Ping to the same worker
        if (total_messages_sent < 20) {
            Ping next_ping;
            next_ping.sequence_number = total_messages_sent + 1;
            next_ping.worker_id = from_worker;
            sintra::world() << next_ping;
            total_messages_sent++;

            std::fprintf(stderr, "[Coordinator] Sent Ping #%d to Worker %d\n",
                         next_ping.sequence_number, from_worker);
        }
    });

    sintra::barrier("ping-pong-slot-activation");

    coordinator_state = "running";

    // Send initial Ping messages to both workers
    {
        Ping ping1;
        ping1.sequence_number = 0;
        ping1.worker_id = 1;
        sintra::world() << ping1;
        total_messages_sent++;

        std::fprintf(stderr, "[Coordinator] Sent initial Ping #%d to Worker 1\n",
                     ping1.sequence_number);

        Ping ping2;
        ping2.sequence_number = 1;
        ping2.worker_id = 2;
        sintra::world() << ping2;
        total_messages_sent++;

        std::fprintf(stderr, "[Coordinator] Sent initial Ping #%d to Worker 2\n",
                     ping2.sequence_number);
    }

    // Wait for stop signal (which will never come - test will hang)
    coordinator_state = "waiting_for_stop";
    std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    sintra::barrier("stop-slot-ready");

    std::fprintf(stderr, "[Coordinator] Waiting for Stop signal (will hang here)...\n");

    // This will hang forever - test timeout should trigger
    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });

    // Should never reach here
    coordinator_state = "stopping";
    threads_running.store(false, std::memory_order_release);
    for (auto& t : background_threads) {
        t.join();
    }

    sintra::deactivate_all_slots();
    std::fprintf(stderr, "[Coordinator] Exiting normally (this shouldn't happen)\n");
    return 0;
}

//
// Worker 1 Process - Responds normally
//
int process_worker1()
{
    std::fprintf(stderr, "[Worker1] Starting\n");

    // Local variables for stack trace
    int pings_received = 0;
    int pongs_sent = 0;
    std::string worker_state = "initializing";
    const int worker_id = 1;

    // Start background threads
    std::atomic<bool> threads_running{true};
    std::mutex shared_mutex;
    std::vector<std::thread> background_threads;

    background_threads.emplace_back(background_worker_thread, 10, std::ref(threads_running));
    background_threads.emplace_back(mutex_holder_thread, 11, std::ref(threads_running), std::ref(shared_mutex));

    // Slot to receive Ping messages and respond with Pong
    sintra::activate_slot([&](Ping ping) {
        std::string current_function = "worker1_ping_handler";
        int received_sequence = ping.sequence_number;
        int for_worker = ping.worker_id;

        if (for_worker != worker_id) {
            return; // Not for us
        }

        pings_received++;
        std::fprintf(stderr, "[Worker1] Received Ping #%d\n", received_sequence);

        // Simulate some processing with local variables
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Respond with Pong
        Pong response;
        response.sequence_number = received_sequence;
        response.worker_id = worker_id;
        sintra::world() << response;
        pongs_sent++;

        std::fprintf(stderr, "[Worker1] Sent Pong #%d\n", received_sequence);
    });

    sintra::barrier("ping-pong-slot-activation");

    worker_state = "running";

    // Wait for stop signal (which will never come)
    std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    sintra::barrier("stop-slot-ready");

    std::fprintf(stderr, "[Worker1] Waiting for Stop signal...\n");

    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });

    // Should never reach here
    worker_state = "stopping";
    threads_running.store(false, std::memory_order_release);
    for (auto& t : background_threads) {
        t.join();
    }

    sintra::deactivate_all_slots();
    std::fprintf(stderr, "[Worker1] Exiting normally (this shouldn't happen)\n");
    return 0;
}

//
// Worker 2 Process - INTENTIONALLY HANGS after a few responses
//
int process_worker2()
{
    std::fprintf(stderr, "[Worker2] Starting (WILL HANG after a few messages)\n");

    // Local variables for stack trace
    int pings_received = 0;
    int pongs_sent = 0;
    std::string worker_state = "initializing";
    const int worker_id = 2;
    constexpr int hang_after_count = 3; // Hang after this many Pings

    // Start background threads
    std::atomic<bool> threads_running{true};
    std::mutex shared_mutex;
    std::vector<std::thread> background_threads;

    background_threads.emplace_back(background_worker_thread, 20, std::ref(threads_running));
    background_threads.emplace_back(background_worker_thread, 21, std::ref(threads_running));
    background_threads.emplace_back(mutex_holder_thread, 22, std::ref(threads_running), std::ref(shared_mutex));

    // Slot to receive Ping messages and respond with Pong
    sintra::activate_slot([&](Ping ping) {
        std::string current_function = "worker2_ping_handler";
        int received_sequence = ping.sequence_number;
        int for_worker = ping.worker_id;

        if (for_worker != worker_id) {
            return; // Not for us
        }

        pings_received++;
        std::fprintf(stderr, "[Worker2] Received Ping #%d\n", received_sequence);

        // HANG after a few messages
        if (pings_received > hang_after_count) {
            std::fprintf(stderr, "[Worker2] HANGING NOW (received %d pings, threshold %d)\n",
                         pings_received, hang_after_count);
            std::fflush(stderr);

            worker_state = "INTENTIONALLY_HUNG";

            // Infinite loop - hang forever
            // This should be visible in the stack trace
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                // These local variables should be visible in lldb
                int hang_iteration = pings_received;
                std::string hang_reason = "intentional hang for lldb testing";
                (void)hang_iteration;
                (void)hang_reason;
            }
        }

        // Normal response
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        Pong response;
        response.sequence_number = received_sequence;
        response.worker_id = worker_id;
        sintra::world() << response;
        pongs_sent++;

        std::fprintf(stderr, "[Worker2] Sent Pong #%d\n", received_sequence);
    });

    sintra::barrier("ping-pong-slot-activation");

    worker_state = "running";

    // Wait for stop signal (which will never come)
    std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    sintra::barrier("stop-slot-ready");

    std::fprintf(stderr, "[Worker2] Waiting for Stop signal...\n");

    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });

    // Should never reach here
    worker_state = "stopping";
    threads_running.store(false, std::memory_order_release);
    for (auto& t : background_threads) {
        t.join();
    }

    sintra::deactivate_all_slots();
    std::fprintf(stderr, "[Worker2] Exiting normally (this shouldn't happen)\n");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
    const auto shared_dir = ensure_shared_directory();

    std::fprintf(stderr, "[Main] Test starting (is_spawned=%d)\n", is_spawned);

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_worker1);  // Branch 0
    processes.emplace_back(process_worker2);  // Branch 1

    sintra::init(argc, argv, processes);

    // Coordinator runs in parent process
    if (!is_spawned) {
        int result = process_coordinator();
        sintra::finalize();

        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
        }

        return result;
    }

    return 0;
}
