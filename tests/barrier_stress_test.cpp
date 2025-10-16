// Barrier Stress Test
// Attempts to expose race conditions and timing issues in barrier implementation

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

constexpr std::size_t kMaxProcessCount = 4;
// Keep the stress loop bounded so slower CI hosts (e.g. FreeBSD jails) still
// show steady ctest progress. Hundreds of iterations remain enough to exercise
// the synchronization paths without monopolizing the worker for minutes.
constexpr std::size_t kIterations = 200;

std::size_t detect_process_count()
{
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        // FreeBSD jails occasionally report zero here; fall back to two
        // workers so the coordinator still has at least one partner to
        // synchronize with while leaving the host some headroom.
        hw = 2;
    }

    // Cap the worker fan-out because spawning more processes than hardware
    // threads provides little additional coverage and can actually slow the
    // FreeBSD CI VM enough that `ctest` appears to stall. Limiting the fan-out
    // lets the executor keep both exposed CPUs busy instead of time-slicing a
    // larger pool of mostly idle helpers.
    if (hw > kMaxProcessCount) {
        hw = static_cast<unsigned>(kMaxProcessCount);
    }

    // Always run with at least two workers so barriers still exercise the
    // multi-process path.
    if (hw < 2) {
        hw = 2;
    }

    return static_cast<std::size_t>(hw);
}

std::atomic<int> worker_failures{0};
std::atomic<int> coordinator_failures{0};

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

    const auto now = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<unsigned>(_getpid());
#else
    const auto pid = static_cast<unsigned>(getpid());
#endif
    std::seed_seq seed{now, pid, static_cast<unsigned>(worker_index)};
    std::mt19937 gen(seed);
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

    sintra::barrier("barrier-stress-done", "_sintra_all_processes");
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }
int worker3_process() { return worker_process(3); }

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);

    const std::size_t process_count = detect_process_count();

    using WorkerFn = int (*)();
    static constexpr WorkerFn kWorkers[] = {
        worker0_process,
        worker1_process,
        worker2_process,
        worker3_process,
    };

    std::vector<sintra::Process_descriptor> processes;
    processes.reserve(process_count);
    for (std::size_t i = 0; i < process_count; ++i) {
        processes.emplace_back(kWorkers[i]);
    }

    std::printf("Barrier stress using %zu worker processes for %zu iterations\n",
                process_count, kIterations);
    std::fflush(stdout);

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
