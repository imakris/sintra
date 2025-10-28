#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

#ifdef __APPLE__
#include <cerrno>
#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
#include <cstdio>

// os_sync variant
#if __has_include(<os/os_sync_wait_on_address.h>) && __has_include(<os/clock.h>)
#include <os/os_sync_wait_on_address.h>
#include <os/clock.h>
#define HAS_OS_SYNC 1
#else
#define HAS_OS_SYNC 0
#endif

// Named semaphore implementation
class named_semaphore {
public:
    explicit named_semaphore(unsigned int initial_count = 0) {
        // Generate unique name
        static std::atomic<uint64_t> counter{0};
        uint64_t id = (static_cast<uint64_t>(getpid()) << 32) | counter.fetch_add(1);

        std::snprintf(m_name, sizeof(m_name), "/sintra_bench_%016llx",
                     static_cast<unsigned long long>(id));

        sem_unlink(m_name);
        m_sem = sem_open(m_name, O_CREAT | O_EXCL, 0600, initial_count);
        if (m_sem == SEM_FAILED) {
            throw std::runtime_error("sem_open failed");
        }
    }

    ~named_semaphore() {
        if (m_sem != SEM_FAILED) {
            sem_close(m_sem);
            sem_unlink(m_name);
        }
    }

    void post() {
        while (sem_post(m_sem) == -1) {
            if (errno == EINTR) continue;
            throw std::runtime_error("sem_post failed");
        }
    }

    void wait() {
        while (sem_wait(m_sem) == -1) {
            if (errno == EINTR) continue;
            throw std::runtime_error("sem_wait failed");
        }
    }

private:
    sem_t* m_sem = SEM_FAILED;
    char m_name[32]{};
};

#if HAS_OS_SYNC
// os_sync implementation
class os_sync_semaphore {
public:
    explicit os_sync_semaphore(unsigned int initial_count = 0) {
        m_count.store(static_cast<int32_t>(initial_count), std::memory_order_relaxed);
    }

    void post() {
        int32_t previous = m_count.fetch_add(1, std::memory_order_release);
        if (previous < 0) {
            wake_one();
        }
    }

    void wait() {
        int32_t previous = m_count.fetch_sub(1, std::memory_order_acq_rel);
        if (previous > 0) {
            return;
        }

        int32_t expected = previous - 1;
        while (true) {
            int rc = os_sync_wait_on_address(
                reinterpret_cast<void*>(&m_count),
                expected,
                sizeof(int32_t),
                OS_SYNC_WAIT_ON_ADDRESS_SHARED);

            if (rc >= 0) {
                int32_t observed = m_count.load(std::memory_order_acquire);
                if (observed >= 0) {
                    return;
                }
                expected = observed;
                continue;
            }

            if (errno == EINTR || errno == EFAULT) {
                continue;
            }
            throw std::runtime_error("os_sync_wait_on_address failed");
        }
    }

private:
    void wake_one() {
        while (true) {
            int rc = os_sync_wake_by_address_any(
                reinterpret_cast<void*>(&m_count),
                sizeof(int32_t),
                OS_SYNC_WAKE_BY_ADDRESS_SHARED);

            if (rc == 0 || (rc == -1 && errno == ENOENT)) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("os_sync_wake_by_address_any failed");
        }
    }

    std::atomic<int32_t> m_count{0};
};
#endif

template<typename SemaphoreT>
double benchmark_producer_consumer(int num_producers, int num_consumers, int items_per_producer) {
    SemaphoreT full_sem(0);
    SemaphoreT empty_sem(100); // Buffer capacity
    std::atomic<int> items_produced{0};
    std::atomic<int> items_consumed{0};
    std::atomic<bool> done{false};

    auto start = std::chrono::steady_clock::now();

    // Launch producers
    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < items_per_producer; ++i) {
                empty_sem.wait();
                items_produced.fetch_add(1, std::memory_order_relaxed);
                full_sem.post();
            }
        });
    }

    // Launch consumers
    std::vector<std::thread> consumers;
    int total_items = num_producers * items_per_producer;
    int items_per_consumer = total_items / num_consumers;
    int extra = total_items % num_consumers;

    for (int c = 0; c < num_consumers; ++c) {
        int my_items = items_per_consumer + (c < extra ? 1 : 0);
        consumers.emplace_back([&, my_items]() {
            for (int i = 0; i < my_items; ++i) {
                full_sem.wait();
                items_consumed.fetch_add(1, std::memory_order_relaxed);
                empty_sem.post();
            }
        });
    }

    // Wait for completion
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

int main() {
    std::cout << "=== macOS Semaphore Performance Benchmark ===" << std::endl;
    std::cout << std::endl;

    const int num_producers = 3;
    const int num_consumers = 4;
    const int items_per_producer = 5000;
    const int total_items = num_producers * items_per_producer;

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Producers: " << num_producers << std::endl;
    std::cout << "  Consumers: " << num_consumers << std::endl;
    std::cout << "  Items per producer: " << items_per_producer << std::endl;
    std::cout << "  Total items: " << total_items << std::endl;
    std::cout << std::endl;

    // Benchmark named semaphores
    std::cout << "Testing NAMED SEMAPHORES (sem_open/sem_post/sem_wait)..." << std::endl;
    double named_time = benchmark_producer_consumer<named_semaphore>(
        num_producers, num_consumers, items_per_producer);
    std::cout << "  Time: " << named_time << " seconds" << std::endl;
    std::cout << "  Throughput: " << (total_items / named_time) << " items/sec" << std::endl;
    std::cout << std::endl;

#if HAS_OS_SYNC
    // Benchmark os_sync semaphores
    std::cout << "Testing OS_SYNC SEMAPHORES (os_sync_wait_on_address)..." << std::endl;
    double os_sync_time = benchmark_producer_consumer<os_sync_semaphore>(
        num_producers, num_consumers, items_per_producer);
    std::cout << "  Time: " << os_sync_time << " seconds" << std::endl;
    std::cout << "  Throughput: " << (total_items / os_sync_time) << " items/sec" << std::endl;
    std::cout << std::endl;

    // Compare
    std::cout << "=== RESULTS ===" << std::endl;
    double ratio = named_time / os_sync_time;
    std::cout << "  Named semaphore: " << named_time << "s" << std::endl;
    std::cout << "  OS_sync semaphore: " << os_sync_time << "s" << std::endl;
    std::cout << "  Ratio (named/os_sync): " << ratio << "x" << std::endl;

    if (ratio > 1.1) {
        std::cout << "  => OS_SYNC is " << ratio << "x FASTER" << std::endl;
    } else if (ratio < 0.9) {
        std::cout << "  => NAMED is " << (1.0/ratio) << "x FASTER" << std::endl;
    } else {
        std::cout << "  => Performance is SIMILAR" << std::endl;
    }
#else
    std::cout << "os_sync_wait_on_address NOT AVAILABLE - cannot compare" << std::endl;
#endif

    std::cout << std::endl;
    std::cout << "Benchmark completed successfully." << std::endl;
    return 0;
}

#else
// Non-macOS platforms
int main() {
    std::cout << "This benchmark only runs on macOS." << std::endl;
    return 0;
}
#endif
