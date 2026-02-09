// Barrier Stress Test
// Attempts to expose race conditions and timing issues in barrier implementation

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string_view>
#include <thread>

constexpr std::size_t k_process_count = 4;
// Keep the stress loop bounded so slower CI hosts (e.g. FreeBSD jails) still
// show steady ctest progress. Hundreds of iterations remain enough to exercise
// the synchronization paths without monopolizing the worker for minutes.
constexpr std::size_t k_iterations = 220;

std::atomic<int> worker_failures{0};
std::atomic<int> coordinator_failures{0};

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    const auto now = static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto pid = static_cast<unsigned>(sintra::test::get_pid());
    std::seed_seq seed{now, pid, static_cast<unsigned>(worker_index)};
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> delay_dist(0, 5);  // 0-5 microseconds

    try {
        std::uint64_t last_seq = 0;
        std::uint64_t last_alt_seq = 0;
        for (std::uint32_t iter = 0; iter < k_iterations; ++iter) {
            // Add random tiny delay to increase chance of race conditions
            if (delay_dist(gen) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }

            // Call barrier
            if (((iter + worker_index) & 0x7) == 3) {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }

            const std::uint64_t seq = barrier("stress-barrier");

            // Verify sequence is valid (non-zero)
            if (seq == 0 || seq <= last_seq) {
                std::fprintf(stderr, "Worker %u iter %u: bad stress seq %llu after %llu!\n",
                            worker_index, iter,
                            static_cast<unsigned long long>(seq),
                            static_cast<unsigned long long>(last_seq));
                worker_failures++;
            }
            last_seq = seq;

            if ((iter & 0x7) == 0) {
                const std::uint64_t seq2 = barrier("stress-barrier");
                if (seq2 == 0 || seq2 <= last_seq) {
                    std::fprintf(stderr,
                                 "Worker %u iter %u: repeated stress seq %llu after %llu!\n",
                                 worker_index,
                                 iter,
                                 static_cast<unsigned long long>(seq2),
                                 static_cast<unsigned long long>(last_seq));
                    worker_failures++;
                }
                last_seq = seq2;
            }

            if ((iter & 0xF) == 0) {
                const std::uint64_t alt_seq = barrier("stress-barrier-alt");
                if (alt_seq == 0 || alt_seq <= last_alt_seq) {
                    std::fprintf(stderr,
                                 "Worker %u iter %u: bad alt seq %llu after %llu!\n",
                                 worker_index,
                                 iter,
                                 static_cast<unsigned long long>(alt_seq),
                                 static_cast<unsigned long long>(last_alt_seq));
                    worker_failures++;
                }
                last_alt_seq = alt_seq;
            }
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Worker %u exception: %s\n", worker_index, e.what());
        return 1;
    }

    sintra::barrier("barrier-stress-done", "_sintra_all_processes");
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }
int worker3_process() { return worker_process(3); }

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);
    processes.emplace_back(worker2_process);
    processes.emplace_back(worker3_process);

    auto start = std::chrono::steady_clock::now();

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("barrier-stress-done", "_sintra_all_processes");
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
