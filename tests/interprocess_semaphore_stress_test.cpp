// Interprocess Semaphore Stress Test
// Comprehensive test suite for interprocess_semaphore implementation
// Attempts to expose race conditions, timing issues, and platform-specific bugs

#include <sintra/detail/interprocess_semaphore.h>
#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace sintra::detail;

// Test configuration
constexpr std::size_t kNumProcesses = 4;
constexpr std::size_t kNumThreadsPerProcess = 3;
constexpr std::size_t kBasicIterations = 100;
constexpr std::size_t kStressIterations = 500;
constexpr std::size_t kProducerConsumerItems = 200;

// Failure counters for different test categories
std::atomic<int> basic_failures{0};
std::atomic<int> concurrency_failures{0};
std::atomic<int> timing_failures{0};
std::atomic<int> producer_consumer_failures{0};
std::atomic<int> stress_failures{0};
std::atomic<int> edge_case_failures{0};

// Helper to report failures with context
#define REPORT_FAILURE(category, fmt, ...) \
    do { \
        std::fprintf(stderr, "[" #category " FAILURE] " fmt "\n", ##__VA_ARGS__); \
        category##_failures++; \
    } while(0)

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

unsigned get_process_id()
{
#ifdef _WIN32
    return static_cast<unsigned>(_getpid());
#else
    return static_cast<unsigned>(getpid());
#endif
}

// ============================================================================
// Test 1: Basic Post/Wait Semantics
// ============================================================================

void test_basic_post_wait(unsigned process_id, unsigned thread_id)
{
    try {
        // Test 1a: Simple post/wait
        {
            interprocess_semaphore sem(0);

            // Spawn thread to post after delay
            std::thread poster([&sem]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                sem.post();
            });

            auto start = std::chrono::steady_clock::now();
            sem.wait();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            poster.join();

            if (elapsed.count() < 5) {
                REPORT_FAILURE(basic, "P%u T%u: wait() returned too quickly (%lld ms)",
                    process_id, thread_id, static_cast<long long>(elapsed.count()));
            }
        }

        // Test 1b: Initial count
        {
            interprocess_semaphore sem(3);

            // Should succeed immediately 3 times
            for (int i = 0; i < 3; ++i) {
                if (!sem.try_wait()) {
                    REPORT_FAILURE(basic, "P%u T%u: try_wait() failed on iteration %d with initial_count=3",
                        process_id, thread_id, i);
                }
            }

            // Fourth try should fail
            if (sem.try_wait()) {
                REPORT_FAILURE(basic, "P%u T%u: try_wait() succeeded when semaphore should be empty",
                    process_id, thread_id);
            }
        }

        // Test 1c: Multiple posts
        {
            interprocess_semaphore sem(0);
            constexpr int kPosts = 5;

            for (int i = 0; i < kPosts; ++i) {
                sem.post();
            }

            for (int i = 0; i < kPosts; ++i) {
                if (!sem.try_wait()) {
                    REPORT_FAILURE(basic, "P%u T%u: try_wait() failed after %d posts (iteration %d)",
                        process_id, thread_id, kPosts, i);
                }
            }

            if (sem.try_wait()) {
                REPORT_FAILURE(basic, "P%u T%u: try_wait() succeeded after consuming all posts",
                    process_id, thread_id);
            }
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(basic, "P%u T%u: exception: %s", process_id, thread_id, e.what());
    }
}

// ============================================================================
// Test 2: Timing and timed_wait
// ============================================================================

void test_timing(unsigned process_id, unsigned thread_id)
{
    try {
        // Test 2a: Timeout on empty semaphore
        {
            interprocess_semaphore sem(0);

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            auto start = std::chrono::steady_clock::now();
            bool result = sem.timed_wait(deadline);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if (result) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait() succeeded on empty semaphore",
                    process_id, thread_id);
            }

            // Allow 20ms tolerance for scheduling jitter
            if (elapsed.count() < 30 || elapsed.count() > 120) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait(50ms) took %lld ms (expected ~50ms ±20ms)",
                    process_id, thread_id, static_cast<long long>(elapsed.count()));
            }
        }

        // Test 2b: Success before timeout
        {
            interprocess_semaphore sem(0);

            std::thread poster([&sem]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                sem.post();
            });

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            auto start = std::chrono::steady_clock::now();
            bool result = sem.timed_wait(deadline);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            poster.join();

            if (!result) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait() failed despite post before timeout",
                    process_id, thread_id);
            }

            if (elapsed.count() > 80) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait() took %lld ms (expected ~20-40ms)",
                    process_id, thread_id, static_cast<long long>(elapsed.count()));
            }
        }

        // Test 2c: Zero timeout
        {
            interprocess_semaphore sem(0);

            auto deadline = std::chrono::steady_clock::now();
            bool result = sem.timed_wait(deadline);

            if (result) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait(past deadline) succeeded on empty semaphore",
                    process_id, thread_id);
            }
        }

        // Test 2d: Already available with future deadline
        {
            interprocess_semaphore sem(1);

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            auto start = std::chrono::steady_clock::now();
            bool result = sem.timed_wait(deadline);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if (!result) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait() failed on available semaphore",
                    process_id, thread_id);
            }

            // Should return immediately
            if (elapsed.count() > 50) {
                REPORT_FAILURE(timing, "P%u T%u: timed_wait() took %lld ms on available semaphore (expected <50ms)",
                    process_id, thread_id, static_cast<long long>(elapsed.count()));
            }
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(timing, "P%u T%u: exception: %s", process_id, thread_id, e.what());
    }
}

// ============================================================================
// Test 3: Producer-Consumer Pattern
// ============================================================================

void test_producer_consumer(unsigned process_id, unsigned thread_id)
{
    struct SharedData {
        std::atomic<std::uint64_t> produced_count{0};
        std::atomic<std::uint64_t> consumed_count{0};
        std::atomic<std::uint64_t> checksum{0};
    };

    try {
        static thread_local std::unique_ptr<SharedData> shared_data;
        static thread_local std::unique_ptr<interprocess_semaphore> sem;

        // Initialize on first call in this thread
        if (!shared_data) {
            shared_data = std::make_unique<SharedData>();
            sem = std::make_unique<interprocess_semaphore>(0);
        }

        auto& data = *shared_data;

        // Producer thread
        std::thread producer([&data, sem_ptr = sem.get(), process_id, thread_id]() {
            for (std::uint64_t i = 1; i <= kProducerConsumerItems; ++i) {
                data.produced_count.fetch_add(1, std::memory_order_relaxed);
                data.checksum.fetch_add(i, std::memory_order_relaxed);
                sem_ptr->post();

                // Random small delays to increase interleaving
                if ((i % 10) == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            }
        });

        // Consumer thread
        std::uint64_t local_checksum = 0;
        for (std::uint64_t i = 1; i <= kProducerConsumerItems; ++i) {
            sem->wait();
            data.consumed_count.fetch_add(1, std::memory_order_relaxed);
            local_checksum += i;
        }

        producer.join();

        // Verify counts
        std::uint64_t produced = data.produced_count.load(std::memory_order_relaxed);
        std::uint64_t consumed = data.consumed_count.load(std::memory_order_relaxed);
        std::uint64_t expected_checksum = (kProducerConsumerItems * (kProducerConsumerItems + 1)) / 2;

        if (produced != kProducerConsumerItems) {
            REPORT_FAILURE(producer_consumer, "P%u T%u: produced %llu items, expected %zu",
                process_id, thread_id, static_cast<unsigned long long>(produced), kProducerConsumerItems);
        }

        if (consumed != kProducerConsumerItems) {
            REPORT_FAILURE(producer_consumer, "P%u T%u: consumed %llu items, expected %zu",
                process_id, thread_id, static_cast<unsigned long long>(consumed), kProducerConsumerItems);
        }

        if (local_checksum != expected_checksum) {
            REPORT_FAILURE(producer_consumer, "P%u T%u: checksum mismatch: got %llu, expected %llu",
                process_id, thread_id, static_cast<unsigned long long>(local_checksum),
                static_cast<unsigned long long>(expected_checksum));
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(producer_consumer, "P%u T%u: exception: %s", process_id, thread_id, e.what());
    }
}

// ============================================================================
// Test 4: High Concurrency Stress
// ============================================================================

void test_concurrency_stress(unsigned process_id, unsigned thread_id, std::mt19937& rng)
{
    try {
        // Shared semaphore across multiple threads
        static thread_local std::unique_ptr<interprocess_semaphore> shared_sem;
        if (!shared_sem) {
            shared_sem = std::make_unique<interprocess_semaphore>(0);
        }

        auto sem_ptr = shared_sem.get();
        std::uniform_int_distribution<> action_dist(0, 2);  // 0=post, 1=wait, 2=try_wait
        std::uniform_int_distribution<> delay_dist(0, 10);

        std::atomic<int> balance{0};  // Track logical semaphore value

        std::vector<std::thread> threads;
        constexpr int kConcurrentThreads = 4;

        for (int t = 0; t < kConcurrentThreads; ++t) {
            threads.emplace_back([sem_ptr, &balance, &rng, process_id, thread_id, t]() {
                std::mt19937 local_rng(rng());
                std::uniform_int_distribution<> local_action_dist(0, 2);
                std::uniform_int_distribution<> local_delay_dist(0, 10);

                for (int i = 0; i < 100; ++i) {
                    int action = local_action_dist(local_rng);

                    if (action == 0) {
                        // Post
                        sem_ptr->post();
                        balance.fetch_add(1, std::memory_order_relaxed);
                    } else if (action == 1 && balance.load(std::memory_order_relaxed) > 0) {
                        // Wait (only if we think something is available)
                        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
                        if (sem_ptr->timed_wait(deadline)) {
                            balance.fetch_sub(1, std::memory_order_relaxed);
                        }
                    } else {
                        // Try wait
                        if (sem_ptr->try_wait()) {
                            int prev_balance = balance.fetch_sub(1, std::memory_order_relaxed);
                            if (prev_balance <= 0) {
                                REPORT_FAILURE(concurrency,
                                    "P%u T%u thread%d: try_wait succeeded but balance was %d",
                                    process_id, thread_id, t, prev_balance);
                            }
                        }
                    }

                    // Random delays to increase interleaving
                    if (local_delay_dist(local_rng) == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        // Final balance should be non-negative
        int final_balance = balance.load(std::memory_order_relaxed);
        if (final_balance < 0) {
            REPORT_FAILURE(concurrency, "P%u T%u: final balance is negative: %d",
                process_id, thread_id, final_balance);
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(concurrency, "P%u T%u: exception: %s", process_id, thread_id, e.what());
    }
}

// ============================================================================
// Test 5: Edge Cases
// ============================================================================

void test_edge_cases(unsigned process_id, unsigned thread_id)
{
    try {
        // Test 5a: Many waiters, one poster
        {
            constexpr int kWaiters = 8;
            interprocess_semaphore sem(0);
            std::atomic<int> woken_count{0};

            std::vector<std::thread> waiters;
            for (int i = 0; i < kWaiters; ++i) {
                waiters.emplace_back([&sem, &woken_count]() {
                    sem.wait();
                    woken_count.fetch_add(1, std::memory_order_relaxed);
                });
            }

            // Let waiters settle
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Post for each waiter
            for (int i = 0; i < kWaiters; ++i) {
                sem.post();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            for (auto& th : waiters) {
                th.join();
            }

            int woken = woken_count.load(std::memory_order_relaxed);
            if (woken != kWaiters) {
                REPORT_FAILURE(edge_case, "P%u T%u: %d/%d waiters woken",
                    process_id, thread_id, woken, kWaiters);
            }
        }

        // Test 5b: Rapid create/destroy
        {
            for (int i = 0; i < 20; ++i) {
                interprocess_semaphore sem(i % 5);

                for (int j = 0; j < (i % 5); ++j) {
                    if (!sem.try_wait()) {
                        REPORT_FAILURE(edge_case, "P%u T%u: rapid create/destroy test failed at i=%d j=%d",
                            process_id, thread_id, i, j);
                    }
                }
            }
        }

        // Test 5c: Post overflow stress
        {
            interprocess_semaphore sem(0);
            constexpr int kManyPosts = 1000;

            for (int i = 0; i < kManyPosts; ++i) {
                sem.post();
            }

            int consumed = 0;
            while (sem.try_wait()) {
                consumed++;
            }

            if (consumed != kManyPosts) {
                REPORT_FAILURE(edge_case, "P%u T%u: posted %d, consumed %d",
                    process_id, thread_id, kManyPosts, consumed);
            }
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(edge_case, "P%u T%u: exception: %s", process_id, thread_id, e.what());
    }
}

// ============================================================================
// Test 6: Interprocess Coordination
// ============================================================================

void test_interprocess_coordination(unsigned process_id, std::mt19937& rng)
{
    try {
        // Use a shared semaphore placed in memory that multiple processes can access
        // In a real deployment, this would be in shared memory, but for this test
        // we'll use sintra's barrier to coordinate and each process has its own instance

        // Each process will do multiple iterations of coordinated post/wait
        for (std::size_t iter = 0; iter < kBasicIterations; ++iter) {
            // Synchronize all processes at start of iteration
            sintra::barrier("sem-stress-iter-start");

            // Create a fresh semaphore for this iteration
            interprocess_semaphore sem(0);

            // Process 0 posts, others wait
            if (process_id == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                for (std::size_t i = 0; i < kNumProcesses - 1; ++i) {
                    sem.post();
                }
            } else {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                if (!sem.timed_wait(deadline)) {
                    REPORT_FAILURE(stress, "P%u iter %zu: timed_wait failed in interprocess test",
                        process_id, iter);
                }
            }

            // Synchronize before next iteration
            sintra::barrier("sem-stress-iter-end");
        }

    } catch (const std::exception& e) {
        REPORT_FAILURE(stress, "P%u: exception in interprocess test: %s", process_id, e.what());
    }
}

// ============================================================================
// Main Worker Process
// ============================================================================

int worker_process(unsigned process_id)
{
    const auto now = static_cast<unsigned>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto pid = get_process_id();
    std::seed_seq seed{now, pid, process_id};
    std::mt19937 rng(seed);

    try {
        std::printf("Process %u starting tests...\n", process_id);

        // Run tests in multiple threads to increase stress
        std::vector<std::thread> threads;

        for (unsigned thread_id = 0; thread_id < kNumThreadsPerProcess; ++thread_id) {
            threads.emplace_back([process_id, thread_id, &rng]() {
                std::mt19937 local_rng(rng());

                // Run each test category
                test_basic_post_wait(process_id, thread_id);
                test_timing(process_id, thread_id);
                test_producer_consumer(process_id, thread_id);
                test_concurrency_stress(process_id, thread_id, local_rng);
                test_edge_cases(process_id, thread_id);
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        // Run interprocess coordination test (not in threads)
        test_interprocess_coordination(process_id, rng);

        std::printf("Process %u completed tests\n", process_id);

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Process %u exception: %s\n", process_id, e.what());
        return 1;
    }

    // Final barrier
    sintra::barrier("sem-stress-complete", "_sintra_all_processes");
    return 0;
}

int worker0_process() { return worker_process(0); }
int worker1_process() { return worker_process(1); }
int worker2_process() { return worker_process(2); }
int worker3_process() { return worker_process(3); }

// ============================================================================
// Main Entry Point
// ============================================================================

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
        sintra::barrier("sem-stress-complete", "_sintra_all_processes");
    }

    sintra::finalize();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Print summary
    std::printf("\n");
    std::printf("=================================================================\n");
    std::printf("Interprocess Semaphore Stress Test Summary\n");
    std::printf("=================================================================\n");
    std::printf("Duration: %lld ms\n", static_cast<long long>(duration.count()));
    std::printf("\n");
    std::printf("Failure counts by category:\n");
    std::printf("  Basic correctness:     %d\n", basic_failures.load());
    std::printf("  Timing:                %d\n", timing_failures.load());
    std::printf("  Producer-Consumer:     %d\n", producer_consumer_failures.load());
    std::printf("  Concurrency:           %d\n", concurrency_failures.load());
    std::printf("  Edge cases:            %d\n", edge_case_failures.load());
    std::printf("  Interprocess stress:   %d\n", stress_failures.load());
    std::printf("  ---------------------------------\n");

    int total_failures = basic_failures + timing_failures + producer_consumer_failures +
                         concurrency_failures + edge_case_failures + stress_failures;
    std::printf("  TOTAL:                 %d\n", total_failures);
    std::printf("=================================================================\n");

    if (total_failures > 0) {
        std::fprintf(stderr, "\nTEST FAILED: Detected %d failures\n", total_failures);
        return 1;
    }

    std::printf("\nTEST PASSED: All tests successful!\n");
    return 0;
}
