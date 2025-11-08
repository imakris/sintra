// High-contention semaphore stress test
//
// This test stresses the semaphore implementation under extreme contention
// to verify correctness under conditions similar to the ring buffer usage.
//
// Key scenarios tested:
// - Many readers competing for limited semaphore count
// - Burst posting and consuming patterns
// - Wake-all behavior under high contention (especially macOS)
// - Mixed try_wait, timed_wait, and blocking wait patterns
// - Very high iteration counts to expose rare race conditions

#include <sintra/detail/ipc/semaphore.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using sintra::detail::interprocess_semaphore;

std::atomic<bool> g_test_failed{false};
std::atomic<int> g_failure_line{0};

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            g_test_failed = true, std::memory_order_relaxed; \
            g_failure_line = __LINE__, std::memory_order_relaxed; \
            std::fprintf(stderr, "[FAIL] %s:%d - " #expr "\n", __FILE__, __LINE__); \
            return; \
        } \
    } while (false)

int get_pid()
{
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// Test 1: High-contention multi-reader single-writer pattern
// This mimics the ring buffer usage: many readers competing for slots
void test_many_readers_limited_slots()
{
    constexpr int kReaders = 8;
    constexpr int kSlots = 3;
    constexpr int kIterationsPerReader = 50000;

    std::fprintf(stderr, "[TEST] Many readers, limited slots (%d readers, %d slots, %d iterations/reader)\n",
                 kReaders, kSlots, kIterationsPerReader);

    interprocess_semaphore sem(kSlots);
    std::atomic<int> slots_held{0};
    std::atomic<int> max_slots_held{0};
    std::atomic<long long> total_acquisitions{0};

    auto reader_thread = [&](int id) {
        std::mt19937 rng(std::random_device{}() ^ id);

        for (int i = 0; i < kIterationsPerReader; ++i) {
            // Acquire slot
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            CHECK(sem.timed_wait(deadline));

            // Track concurrent slot usage
            int held = slots_held.fetch_add(1, std::memory_order_acq_rel) + 1;
            CHECK(held <= kSlots);

            // Update max
            int current_max = max_slots_held.load(std::memory_order_relaxed);
            while (held > current_max) {
                if (max_slots_held.compare_exchange_weak(current_max, held,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            }

            total_acquisitions.fetch_add(1, std::memory_order_relaxed);

            // Hold slot briefly to create contention
            if ((rng() & 0xFF) < 10) {
                std::this_thread::yield();
            }

            // Release slot
            int released = slots_held.fetch_sub(1, std::memory_order_acq_rel);
            CHECK(released > 0);
            sem.post();

            if ((i & 0xFFF) == 0 && i > 0) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back(reader_thread, i);
    }

    for (auto& t : readers) {
        t.join();
    }

    CHECK(slots_held == 0);
    CHECK(total_acquisitions == kReaders * kIterationsPerReader);

    std::fprintf(stderr, "[PASS] Many readers test completed. Total acquisitions: %lld, Max concurrent: %d\n",
                 total_acquisitions, max_slots_held);
}

// Test 2: Wake-all stress test
// Many waiters, infrequent posts - stresses macOS wake-all behavior
void test_wake_all_stress()
{
    constexpr int kWaiters = 16;
    constexpr int kTotalPosts = 10000;

    std::fprintf(stderr, "[TEST] Wake-all stress (%d waiters, %d total posts)\n",
                 kWaiters, kTotalPosts);

    interprocess_semaphore sem(0);
    std::atomic<int> acquisitions{0};
    std::atomic<bool> stop{false};

    auto waiter_thread = [&]() {
        while (!stop.load(std::memory_order_acquire)) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            if (sem.timed_wait(deadline)) {
                acquisitions.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Drain remaining
        while (sem.try_wait()) {
            acquisitions.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> waiters;
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back(waiter_thread);
    }

    // Let waiters get blocked
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Post in bursts to create wake-all contention
    for (int i = 0; i < kTotalPosts; i += 3) {
        int burst = std::min(3, kTotalPosts - i);
        for (int j = 0; j < burst; ++j) {
            sem.post();
        }

        if ((i & 0x3FF) == 0) {
            std::this_thread::yield();
        }
    }

    // Wait for all posts to be consumed
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (acquisitions.load(std::memory_order_relaxed) < kTotalPosts) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "[FAIL] Timeout waiting for acquisitions: %d/%d\n", acquisitions, kTotalPosts);
            CHECK(false);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop = true, std::memory_order_release;

    for (auto& t : waiters) {
        t.join();
    }

    CHECK(acquisitions == kTotalPosts);
    std::fprintf(stderr, "[PASS] Wake-all stress test completed\n");
}

// Test 3: Mixed operations under extreme contention
void test_mixed_operations_extreme_contention()
{
    constexpr int kThreads = 12;
    constexpr int kIterationsPerThread = 30000;

    std::fprintf(stderr, "[TEST] Mixed operations extreme contention (%d threads, %d iterations/thread)\n",
                 kThreads, kIterationsPerThread);

    interprocess_semaphore sem(0);
    std::atomic<long long> posts{0};
    std::atomic<long long> successful_waits{0};
    std::atomic<long long> try_wait_successes{0};
    std::atomic<long long> timed_wait_timeouts{0};

    auto worker_thread = [&](int id) {
        std::mt19937 rng(std::random_device{}() ^ id ^ get_pid());

        for (int i = 0; i < kIterationsPerThread; ++i) {
            int op = rng() % 10;

            if (op < 3) {
                // Post
                sem.post();
                posts.fetch_add(1, std::memory_order_relaxed);
            } else if (op < 5) {
                // try_wait
                if (sem.try_wait()) {
                    try_wait_successes.fetch_add(1, std::memory_order_relaxed);
                    successful_waits.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (op < 8) {
                // Short timed_wait
                auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
                if (sem.timed_wait(deadline)) {
                    successful_waits.fetch_add(1, std::memory_order_relaxed);
                } else {
                    timed_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // Longer timed_wait
                auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
                if (sem.timed_wait(deadline)) {
                    successful_waits.fetch_add(1, std::memory_order_relaxed);
                } else {
                    timed_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if ((i & 0x3FF) == 0) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker_thread, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Drain remaining
    long long drained = 0;
    while (sem.try_wait()) {
        ++drained;
    }

    long long total_posts = posts;
    long long total_waits = successful_waits + drained;

    CHECK(total_posts == total_waits);

    std::fprintf(stderr, "[PASS] Mixed operations test completed. Posts: %lld, "
        "Waits: %lld (try: %lld, timeouts: %lld, drained: %lld)\n",
        total_posts, successful_waits, try_wait_successes, timed_wait_timeouts, drained);
}

// Test 4: Rapid post/wait cycling
// Tests for any lost wakeups or double-counting under rapid cycling
void test_rapid_post_wait_cycling()
{
    constexpr int kCyclers = 10;
    constexpr int kCyclesPerThread = 50000;

    std::fprintf(stderr, "[TEST] Rapid post/wait cycling (%d threads, %d cycles/thread)\n",
        kCyclers, kCyclesPerThread);

    interprocess_semaphore sem(0);
    std::atomic<long long> cycle_count{0};

    auto cycler_thread = [&]() {
        for (int i = 0; i < kCyclesPerThread; ++i) {
            sem.post();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            CHECK(sem.timed_wait(deadline));

            cycle_count.fetch_add(1, std::memory_order_relaxed);

            if ((i & 0xFFF) == 0) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> cyclers;
    for (int i = 0; i < kCyclers; ++i) {
        cyclers.emplace_back(cycler_thread);
    }

    for (auto& t : cyclers) {
        t.join();
    }

    CHECK(cycle_count == kCyclers * kCyclesPerThread);
    CHECK(!sem.try_wait());  // Should be empty

    std::fprintf(stderr, "[PASS] Rapid cycling test completed. Cycles: %lld\n", cycle_count);
}

} // namespace

int main()
{
    std::fprintf(stderr, "[INFO] Starting semaphore high-contention stress tests (pid=%d)\n", get_pid());
    std::fprintf(stderr, "[INFO] Hardware concurrency: %u\n", std::thread::hardware_concurrency());

    try {
        test_many_readers_limited_slots();
        if (g_test_failed) {
            std::fprintf(stderr, "[FAILED] Test failed at line %d\n", g_failure_line);
            return 1;
        }

        test_wake_all_stress();
        if (g_test_failed) {
            std::fprintf(stderr, "[FAILED] Test failed at line %d\n", g_failure_line);
            return 1;
        }

        test_mixed_operations_extreme_contention();
        if (g_test_failed) {
            std::fprintf(stderr, "[FAILED] Test failed at line %d\n", g_failure_line);
            return 1;
        }

        test_rapid_post_wait_cycling();
        if (g_test_failed) {
            std::fprintf(stderr, "[FAILED] Test failed at line %d\n", g_failure_line);
            return 1;
        }

        std::fprintf(stderr, "\n[SUCCESS] All semaphore stress tests passed\n");
        return 0;

    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[EXCEPTION] %s\n", e.what());
        return 1;
    }
}
