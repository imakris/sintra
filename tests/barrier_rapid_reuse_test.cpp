// Barrier Rapid Reuse Test
// Specifically tests rapid reuse of the same barrier name
// to try to expose races during barrier cleanup/recreation

#include <sintra/sintra.h>

#include <atomic>
#include <cstdio>
#include <string_view>

constexpr std::size_t kProcessCount = 3;
constexpr std::size_t kIterations = 1000;

std::atomic<int> failures{0};

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

    try {
        for (std::uint32_t iter = 0; iter < kIterations; ++iter) {
            // All processes use the SAME barrier name repeatedly
            // This causes rapid create/destroy/recreate cycles
            auto seq1 = barrier("reuse");

            // Immediately call another barrier with a different name
            // to test interleaving
            auto seq2 = barrier("other");

            // Call the first barrier again immediately
            auto seq3 = barrier("reuse");

            if (seq1 == 0 || seq2 == 0 || seq3 == 0) {
                std::fprintf(stderr, "Worker %u iter %u: got zero sequence!\n",
                            worker_index, iter);
                failures++;
            }
        }
    } catch (const std::exception& e) {
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
    const bool is_spawned = has_branch_flag(argc, argv);

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

    return (failures > 0) ? 1 : 0;
}
