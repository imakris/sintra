//
// Sintra Multi-Process Ping-Pong Hang Test
//
// This test validates that the test harness properly captures stack traces
// from all threads of all processes when a test hangs.
//
// Test structure:
// - Process 1 (ping_responder): Responds to Ping with Pong
// - Process 2 (pong_responder): Responds to Pong with Ping, but HANGS after a few iterations
// - Process 3 (monitor): Counts Ping messages (will hang waiting for completion)
//
// The test intentionally hangs to trigger the test harness timeout and lldb stack capture.
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

struct Ping {};
struct Pong {};
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

    auto base = sintra::test::scratch_subdirectory("ping_pong_hang");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "ping_pong_hang_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_count(const std::filesystem::path& file, int value)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << value << '\n';
}

// Process 1: Ping responder - has some local state to make stack traces interesting
int process_ping_responder()
{
    // Create some local variables with interesting values
    int response_count = 0;
    std::string process_name = "ping_responder";
    std::vector<std::string> message_history;
    double start_time = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Create a worker thread to show multi-threading in stack traces
    std::atomic<bool> keep_running{true};
    std::thread worker_thread([&]() {
        int worker_iteration = 0;
        std::string worker_name = "ping_worker_thread";
        while (keep_running) {
            worker_iteration++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    sintra::activate_slot([&](Ping) {
        response_count++;
        message_history.push_back("Ping-" + std::to_string(response_count));
        sintra::world() << Pong();
    });

    sintra::barrier("ping-pong-slot-activation");

    // This will hang indefinitely waiting for Stop that never comes
    std::mutex hang_mutex;
    std::condition_variable hang_cv;
    std::unique_lock<std::mutex> lk(hang_mutex);
    hang_cv.wait(lk, []{ return false; }); // Will never return

    keep_running = false;
    worker_thread.join();
    sintra::deactivate_all_slots();

    return 0;
}

// Process 2: Pong responder - hangs after a few iterations
int process_pong_responder()
{
    // Create some local variables with interesting values
    std::atomic<int> pong_count{0};
    std::string process_name = "pong_responder";
    std::vector<int> response_times;
    const int max_responses = 5; // Will hang after this many
    bool intentionally_hung = false;

    // Create multiple worker threads to show multi-threading
    std::atomic<bool> keep_running{true};
    std::vector<std::thread> worker_threads;

    for (int i = 0; i < 3; i++) {
        worker_threads.emplace_back([i, &keep_running]() {
            int thread_id = i;
            std::string thread_name = "pong_worker_" + std::to_string(i);
            int work_count = 0;
            while (keep_running) {
                work_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(30 + i * 10));
            }
        });
    }

    sintra::activate_slot([&](Pong) {
        int current_count = pong_count++;
        response_times.push_back(current_count * 10);

        if (current_count >= max_responses) {
            // HANG HERE - this is the intentional hang
            intentionally_hung = true;
            std::fprintf(stderr, "[HANG] Process pong_responder hanging after %d responses\n",
                        current_count);
            std::fflush(stderr);

            // Infinite loop to simulate a hang
            volatile bool stuck = true;
            while (stuck) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        else {
            sintra::world() << Ping();
        }
    });

    sintra::barrier("ping-pong-slot-activation");

    // Start the ping-pong
    sintra::world() << Ping();

    // This will hang waiting for a condition that never becomes true
    std::mutex hang_mutex;
    std::condition_variable hang_cv;
    std::unique_lock<std::mutex> lk(hang_mutex);
    hang_cv.wait(lk, []{ return false; });

    keep_running = false;
    for (auto& t : worker_threads) {
        t.join();
    }
    sintra::deactivate_all_slots();

    return 0;
}

// Process 3: Monitor - will hang waiting for completion
int process_monitor()
{
    // Create some local variables with interesting values
    static std::atomic<int> ping_counter{0};
    std::string process_name = "monitor";
    std::vector<std::chrono::steady_clock::time_point> ping_timestamps;
    double average_rate = 0.0;
    int max_pings_seen = 0;

    // Create a monitoring thread
    std::atomic<bool> keep_monitoring{true};
    std::thread monitor_thread([&]() {
        std::string monitor_thread_name = "monitor_thread";
        int monitor_iteration = 0;
        while (keep_monitoring) {
            monitor_iteration++;
            int current_count = ping_counter;
            if (current_count > max_pings_seen) {
                max_pings_seen = current_count;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    auto monitor_slot = [&](Ping) {
        int count = ping_counter++ + 1;
        ping_timestamps.push_back(std::chrono::steady_clock::now());

        if (ping_timestamps.size() >= 2) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                ping_timestamps.back() - ping_timestamps.front()).count();
            if (duration > 0) {
                average_rate = static_cast<double>(ping_timestamps.size()) /
                              (duration / 1000.0);
            }
        }
    };

    sintra::activate_slot(monitor_slot);
    sintra::barrier("ping-pong-slot-activation");

    // This will hang waiting for completion that never comes
    std::mutex hang_mutex;
    std::condition_variable hang_cv;
    std::unique_lock<std::mutex> lk(hang_mutex);
    hang_cv.wait(lk, []{ return false; });

    keep_monitoring = false;
    monitor_thread.join();

    const auto shared_dir = get_shared_directory();
    write_count(shared_dir / "ping_count.txt", ping_counter.load());

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_ping_responder);
    processes.emplace_back(process_pong_responder);
    processes.emplace_back(process_monitor);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        // Coordinator will hang here waiting for processes that never complete
        std::fprintf(stderr, "[COORDINATOR] Waiting for completion (will hang)...\n");
        std::fflush(stderr);

        std::mutex hang_mutex;
        std::condition_variable hang_cv;
        std::unique_lock<std::mutex> lk(hang_mutex);
        hang_cv.wait(lk, []{ return false; });
    }

    sintra::finalize();

    return 0;
}
