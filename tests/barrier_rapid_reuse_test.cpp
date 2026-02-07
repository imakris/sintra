// Barrier Rapid Reuse Test
// Specifically tests rapid reuse of the same barrier name
// to try to expose races during barrier cleanup/recreation

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>

constexpr std::size_t k_process_count = 3;
constexpr std::size_t k_iterations = 2048 / 20;

std::atomic<int> failures{0};

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    try {
        std::uint64_t last_reuse_seq = 0;
        std::uint64_t last_other_seq = 0;

        for (std::uint32_t iter = 0; iter < k_iterations; ++iter) {
            if (((iter + worker_index) & 0x7) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }

            // All processes use the SAME barrier name repeatedly
            // This causes rapid create/destroy/recreate cycles
            const std::uint64_t seq1 = barrier("reuse");
            if (seq1 == 0 || seq1 <= last_reuse_seq) {
                std::fprintf(stderr,
                             "Worker %u iter %u: unexpected reuse seq %llu after %llu\n",
                             worker_index,
                             iter,
                             static_cast<unsigned long long>(seq1),
                             static_cast<unsigned long long>(last_reuse_seq));
                failures++;
            }
            last_reuse_seq = seq1;

            // Immediately call another barrier with a different name
            // to test interleaving
            const std::uint64_t seq2 = barrier("other");
            if (seq2 == 0 || seq2 <= last_other_seq) {
                std::fprintf(stderr,
                             "Worker %u iter %u: unexpected other seq %llu after %llu\n",
                             worker_index,
                             iter,
                             static_cast<unsigned long long>(seq2),
                             static_cast<unsigned long long>(last_other_seq));
                failures++;
            }
            last_other_seq = seq2;

            // Call the first barrier again immediately
            const std::uint64_t seq3 = barrier("reuse");
            if (seq3 == 0 || seq3 <= last_reuse_seq) {
                std::fprintf(stderr,
                             "Worker %u iter %u: reuse tail seq %llu after %llu\n",
                             worker_index,
                             iter,
                             static_cast<unsigned long long>(seq3),
                             static_cast<unsigned long long>(last_reuse_seq));
                failures++;
            }
            last_reuse_seq = seq3;

            if ((iter & 0xF) == 0) {
                const std::uint64_t seq4 = barrier("reuse");
                if (seq4 == 0 || seq4 <= last_reuse_seq) {
                    std::fprintf(stderr,
                                 "Worker %u iter %u: reuse bonus seq %llu after %llu\n",
                                 worker_index,
                                 iter,
                                 static_cast<unsigned long long>(seq4),
                                 static_cast<unsigned long long>(last_reuse_seq));
                    failures++;
                }
                last_reuse_seq = seq4;
            }

            if ((iter & 0x1F) == 0) {
                const std::uint64_t seq5 = barrier("other");
                if (seq5 == 0 || seq5 <= last_other_seq) {
                    std::fprintf(stderr,
                                 "Worker %u iter %u: other bonus seq %llu after %llu\n",
                                 worker_index,
                                 iter,
                                 static_cast<unsigned long long>(seq5),
                                 static_cast<unsigned long long>(last_other_seq));
                    failures++;
                }
                last_other_seq = seq5;
            }
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Worker %u exception: %s\n", worker_index, e.what());
        return 1;
    }

    barrier("barrier-rapid-reuse-done", "_sintra_all_processes");
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);
    processes.emplace_back(worker2_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("barrier-rapid-reuse-done", "_sintra_all_processes");
    }

    sintra::finalize();

    std::printf("Barrier rapid reuse test completed\n");
    std::printf("Failures: %d\n", failures.load());

    return (failures.load() > 0) ? 1 : 0;
}
