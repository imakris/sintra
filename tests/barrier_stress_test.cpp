// Barrier Stress Test
// Attempts to expose race conditions and timing issues in barrier implementation

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>

constexpr std::size_t kProcessCount = 4;
constexpr std::size_t kIterations = 500;  // Many iterations to increase chance of races

std::atomic<int> worker_failures{0};
std::atomic<int> coordinator_failures{0};

struct Worker_done
{
    std::uint32_t worker_index;
};

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> delay_dist(0, 5);  // 0-5 microseconds

    try {
        for (std::uint32_t iter = 0; iter < kIterations; ++iter) {
            // Add random tiny delay to increase chance of race conditions
            if (delay_dist(gen) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }

            // Call barrier
            auto seq = barrier("stress-barrier");

            // Verify sequence is valid (non-zero)
            if (seq == 0) {
                std::fprintf(stderr, "Worker %u iter %u: got sequence 0!\n",
                            worker_index, iter);
                worker_failures++;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Worker %u exception: %s\n", worker_index, e.what());
        return 1;
    }

    sintra::world() << Worker_done{worker_index};
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }
int worker3_process() { return worker_process(3); }

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);
    processes.emplace_back(worker2_process);
    processes.emplace_back(worker3_process);

    auto start = std::chrono::steady_clock::now();

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        std::mutex completion_mutex;
        std::condition_variable completion_cv;
        std::size_t completed_workers = 0;
        bool timed_out = false;

        [[maybe_unused]] auto guard = sintra::activate_slot(
            [&](const Worker_done&) {
                std::lock_guard<std::mutex> lock(completion_mutex);
                ++completed_workers;
                if (completed_workers >= kProcessCount) {
                    completion_cv.notify_one();
                }
            });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(completion_mutex);
        if (!completion_cv.wait_until(
                lock,
                deadline,
                [&] { return completed_workers >= kProcessCount; })) {
            timed_out = true;
            std::fprintf(stderr, "Coordinator timed out waiting for workers to finish\n");
            coordinator_failures.fetch_add(1, std::memory_order_relaxed);
        }
        lock.unlock();

        sintra::deactivate_all_slots();

        if (timed_out) {
            sintra::finalize();
            return 1;
        }
    }

    sintra::finalize();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::printf("Barrier stress test completed in %lld ms\n", static_cast<long long>(duration.count()));
    std::printf("Worker failures: %d\n", worker_failures.load());
    std::printf("Coordinator failures: %d\n", coordinator_failures.load());

    if (worker_failures > 0 || coordinator_failures > 0) {
        std::fprintf(stderr, "TEST FAILED: Detected failures\n");
        return 1;
    }

    return 0;
}
