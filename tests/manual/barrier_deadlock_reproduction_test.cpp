// Stress test to reproduce the barrier deadlock seen in CI
//
// Strategy:
// 1. Run barrier choreography with multiple processes
// 2. Add artificial delays to increase likelihood of deadlock
// 3. Use timeout to detect when deadlock occurs
// 4. Report which scenario triggered it

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

std::atomic<int> g_test_iteration{0};

std::filesystem::path ensure_shared_directory()
{
    static std::filesystem::path dir;
    if (!dir.empty()) {
        return dir;
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_deadlock_repro";
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();

    dir = base / std::to_string(unique_suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

void worker_process(int worker_id, int num_workers, int num_barriers)
{
    const auto shared_dir = ensure_shared_directory();
    const auto swarm_path = (shared_dir / "swarm_id.txt").string();

    std::ifstream swarm_file(swarm_path);
    if (!swarm_file) {
        std::cerr << "Worker " << worker_id << ": Failed to read swarm_id\n";
        std::exit(1);
    }

    std::string swarm_id_str;
    std::getline(swarm_file, swarm_id_str);
    swarm_file.close();

    const auto swarm_id = std::stoull(swarm_id_str, nullptr, 16);

    sintra::Managed_process proc(swarm_id);
    auto& group = proc.get_default_group();

    // Signal ready
    const auto ready_file = shared_dir / ("worker_" + std::to_string(worker_id) + "_ready.txt");
    std::ofstream(ready_file) << "ready\n";

    // Wait for all workers to be ready
    const auto start_file = shared_dir / "start.txt";
    while (!fs::exists(start_file)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Worker " << worker_id << " starting barriers\n" << std::flush;

    for (int b = 0; b < num_barriers; ++b) {
        const std::string barrier_name = "barrier_" + std::to_string(b);

        try {
            // Use delivery_fence_t which calls wait_for_delivery_fence()
            if (!sintra::barrier<sintra::delivery_fence_t>(barrier_name, "default")) {
                std::cerr << "Worker " << worker_id << " barrier " << b << " failed\n";
                std::exit(1);
            }

            std::cout << "Worker " << worker_id << " completed barrier " << b << "\n" << std::flush;
        }
        catch (const std::exception& e) {
            std::cerr << "Worker " << worker_id << " exception in barrier " << b << ": " << e.what() << "\n";
            std::exit(1);
        }
    }

    std::cout << "Worker " << worker_id << " completed all barriers\n" << std::flush;

    // Signal completion
    const auto done_file = shared_dir / ("worker_" + std::to_string(worker_id) + "_done.txt");
    std::ofstream(done_file) << "done\n";
}

int main(int argc, char* argv[])
{
    if (argc == 4 && std::string(argv[1]) == "--worker") {
        const int worker_id = std::atoi(argv[2]);
        const int num_barriers = std::atoi(argv[3]);

        // Let parent know we're starting
        std::cout << "Worker " << worker_id << " process started, PID="
#ifdef _WIN32
                  << _getpid()
#else
                  << getpid()
#endif
                  << "\n" << std::flush;

        const int num_workers = 3; // hardcoded for now
        worker_process(worker_id, num_workers, num_barriers);
        return 0;
    }

    constexpr int NUM_ITERATIONS = 20;
    constexpr int NUM_WORKERS = 3;
    constexpr int NUM_BARRIERS_PER_ITERATION = 5;
    constexpr int TIMEOUT_SECONDS = 30;

    std::cout << "Barrier Deadlock Reproduction Test\n";
    std::cout << "===================================\n";
    std::cout << "Iterations: " << NUM_ITERATIONS << "\n";
    std::cout << "Workers per iteration: " << NUM_WORKERS << "\n";
    std::cout << "Barriers per iteration: " << NUM_BARRIERS_PER_ITERATION << "\n";
    std::cout << "Timeout: " << TIMEOUT_SECONDS << "s\n\n";

    int successes = 0;
    int timeouts = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::cout << "\n=== Iteration " << (iter + 1) << "/" << NUM_ITERATIONS << " ===\n" << std::flush;

        const auto shared_dir = ensure_shared_directory();

        // Clean up old iteration
        try {
            std::filesystem::remove_all(shared_dir);
            std::filesystem::create_directories(shared_dir);
        }
        catch (...) {
            // Ignore cleanup failures
        }

        // Create coordinator
        sintra::Managed_process coord;
        const auto swarm_id = coord.get_swarm_id();

        // Write swarm ID for workers
        const auto swarm_path = shared_dir / "swarm_id.txt";
        {
            std::ofstream swarm_file(swarm_path);
            swarm_file << std::hex << swarm_id << "\n";
        }

        // Spawn workers
        const auto exe_path = std::filesystem::path(argv[0]);

        for (int w = 0; w < NUM_WORKERS; ++w) {
            std::vector<std::string> worker_args;
            worker_args.push_back("--worker");
            worker_args.push_back(std::to_string(w));
            worker_args.push_back(std::to_string(NUM_BARRIERS_PER_ITERATION));

            coord.spawn(exe_path.string(), worker_args);
            std::cout << "Spawned worker " << w << "\n" << std::flush;
        }

        // Wait for workers to be ready
        std::cout << "Waiting for workers to be ready..." << std::flush;
        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int ready_count = 0;

        while (ready_count < NUM_WORKERS) {
            if (std::chrono::steady_clock::now() > ready_deadline) {
                std::cerr << "\nERROR: Workers failed to become ready\n";
                return 1;
            }

            ready_count = 0;
            for (int w = 0; w < NUM_WORKERS; ++w) {
                const auto ready_file = shared_dir / ("worker_" + std::to_string(w) + "_ready.txt");
                if (fs::exists(ready_file)) {
                    ready_count++;
                }
            }

            if (ready_count < NUM_WORKERS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        std::cout << " done\n" << std::flush;

        // Signal start
        std::ofstream(shared_dir / "start.txt") << "go\n";
        std::cout << "Workers started\n" << std::flush;

        // Wait for completion with timeout
        const auto test_start = std::chrono::steady_clock::now();
        const auto deadline = test_start + std::chrono::seconds(TIMEOUT_SECONDS);
        int done_count = 0;
        bool timed_out = false;

        while (done_count < NUM_WORKERS) {
            const auto now = std::chrono::steady_clock::now();
            if (now > deadline) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - test_start);
                std::cout << "\n*** DEADLOCK DETECTED at iteration " << (iter + 1)
                          << " after " << elapsed.count() << "s ***\n";
                std::cout << "This is likely the deadlock we're looking for!\n";
                std::cout << "\nWorker completion status:\n";
                for (int w = 0; w < NUM_WORKERS; ++w) {
                    const auto done_file = shared_dir / ("worker_" + std::to_string(w) + "_done.txt");
                    std::cout << "  Worker " << w << ": "
                              << (fs::exists(done_file) ? "DONE" : "STUCK") << "\n";
                }
                timed_out = true;
                timeouts++;
                break;
            }

            done_count = 0;
            for (int w = 0; w < NUM_WORKERS; ++w) {
                const auto done_file = shared_dir / ("worker_" + std::to_string(w) + "_done.txt");
                if (fs::exists(done_file)) {
                    done_count++;
                }
            }

            if (done_count < NUM_WORKERS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        if (!timed_out) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - test_start);
            std::cout << "Iteration completed successfully in " << elapsed.count() << "ms\n";
            successes++;
        }

        // Give some time for cleanup
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n========================================\n";
    std::cout << "RESULTS:\n";
    std::cout << "  Successful: " << successes << "/" << NUM_ITERATIONS << "\n";
    std::cout << "  Deadlocks:  " << timeouts << "/" << NUM_ITERATIONS << "\n";
    std::cout << "========================================\n";

    if (timeouts > 0) {
        std::cout << "\n*** DEADLOCK REPRODUCED! ***\n";
        std::cout << "The test successfully triggered the deadlock condition.\n";
        return 1;  // Indicate we found the deadlock
    }

    return 0;
}
