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

template<typename SemaphoreT>
double benchmark_producer_consumer(int num_producers, int num_consumers, int items_per_producer) {
    SemaphoreT full_sem(0);
    SemaphoreT empty_sem(100); // Buffer capacity
    std::atomic<int> items_produced{0};
    std::atomic<int> items_consumed{0};

    auto start = std::chrono::steady_clock::now();

    // Launch producers
    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&]() {
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
    std::cout << "=== macOS NAMED SEMAPHORE Benchmark ===" << std::endl;
    std::cout << "Primitive: sem_open/sem_post/sem_wait" << std::endl;
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

    double time = benchmark_producer_consumer<named_semaphore>(
        num_producers, num_consumers, items_per_producer);

    std::cout << "=== RESULT ===" << std::endl;
    std::cout << "  Time: " << time << " seconds" << std::endl;
    std::cout << "  Throughput: " << (total_items / time) << " items/sec" << std::endl;
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
