#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <exception>

#ifdef __APPLE__
#include <cerrno>

// os_sync variant
#if __has_include(<os/os_sync_wait_on_address.h>) && __has_include(<os/clock.h>)
#include <os/os_sync_wait_on_address.h>
#include <os/clock.h>
#define HAS_OS_SYNC 1
#else
#define HAS_OS_SYNC 0
#endif

#if HAS_OS_SYNC
class os_sync_not_supported : public std::runtime_error {
public:
    explicit os_sync_not_supported(const char* function_name)
        : std::runtime_error(std::string(function_name) + " not supported on this macOS version")
    {
    }
};

[[noreturn]] static void handle_os_sync_error(const char* function_name)
{
    const int error = errno;
    if (error == ENOTSUP || error == ENOSYS) {
        throw os_sync_not_supported(function_name);
    }
    throw std::runtime_error(std::string(function_name) + " failed: " + std::strerror(error));
}

// os_sync implementation
class os_sync_semaphore {
public:
    explicit os_sync_semaphore(unsigned int initial_count = 0) {
        m_count = static_cast<int32_t>(initial_count);
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
            handle_os_sync_error("os_sync_wait_on_address");
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
            handle_os_sync_error("os_sync_wake_by_address_any");
        }
    }

    std::atomic<int32_t> m_count{0};
};

template<typename SemaphoreT>
double benchmark_producer_consumer(int num_producers, int num_consumers, int items_per_producer) {
    SemaphoreT full_sem(0);
    SemaphoreT empty_sem(100); // Buffer capacity
    std::atomic<int> items_produced{0};
    std::atomic<int> items_consumed{0};

    auto start = std::chrono::steady_clock::now();

    std::mutex exception_mutex;
    std::exception_ptr first_exception;
    auto record_exception = [&](std::exception_ptr ex) {
        if (!ex) {
            return;
        }
        std::lock_guard<std::mutex> lock(exception_mutex);
        if (!first_exception) {
            first_exception = std::move(ex);
        }
    };

    // Launch producers
    std::vector<std::thread> producers;
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, items_per_producer]() {
            try {
                for (int i = 0; i < items_per_producer; ++i) {
                    empty_sem.wait();
                    items_produced.fetch_add(1);
                    full_sem.post();
                }
            }
            catch (...) {
                record_exception(std::current_exception());
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
            try {
                for (int i = 0; i < my_items; ++i) {
                    full_sem.wait();
                    items_consumed.fetch_add(1);
                    empty_sem.post();
                }
            }
            catch (...) {
                record_exception(std::current_exception());
            }
        });
    }

    // Wait for completion
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - start).count();
}

int main() {
    std::cout << "=== macOS OS_SYNC SEMAPHORE Benchmark ===" << std::endl;
    std::cout << "Primitive: os_sync_wait_on_address/os_sync_wake_by_address_any" << std::endl;
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

    try {
        double time = benchmark_producer_consumer<os_sync_semaphore>(
            num_producers, num_consumers, items_per_producer);

        std::cout << "=== RESULT ===" << std::endl;
        std::cout << "  Time: " << time << " seconds" << std::endl;
        std::cout << "  Throughput: " << (total_items / time) << " items/sec" << std::endl;
        std::cout << std::endl;
        std::cout << "Benchmark completed successfully." << std::endl;
        return 0;
    }
    catch (const os_sync_not_supported&) {
        std::cout << "os_sync_wait_on_address NOT AVAILABLE on this macOS version." << std::endl;
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Benchmark failed: " << ex.what() << std::endl;
        return 1;
    }
}

#else
int main() {
    std::cout << "os_sync_wait_on_address NOT AVAILABLE on this macOS version." << std::endl;
    return 0;
}
#endif

#else
// Non-macOS platforms
int main() {
    std::cout << "This benchmark only runs on macOS." << std::endl;
    return 0;
}
#endif
