// Combined interprocess_semaphore tests
//
// This executable merges the strongest ideas from prior experimental
// pull requests into a cohesive suite:
//   * Basic semantics and timed wait behaviour (PR #575)
//   * Contended producer/consumer stress across multiple threads (PR #576)
//   * Cross-process resource coordination to ensure exclusivity (PR #577)
//
// The result is a balanced test that exercises correctness, timing
// guarantees, and multi-process usage without relying on additional
// frameworks.

#include <sintra/detail/interprocess_semaphore.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <new>
#include <queue>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace
{

using sintra::detail::interprocess_semaphore;

class Test_failure : public std::runtime_error
{
public:
    Test_failure(const std::string& expr, const char* file, int line, const std::string& message = {})
        : std::runtime_error(format(expr, file, line, message))
    {
    }

private:
    static std::string format(const std::string& expr, const char* file, int line, const std::string& message)
    {
        std::string out = file;
        out.append(":" + std::to_string(line) + " - assertion failed: " + expr);
        if (!message.empty()) {
            out.append(" (" + message + ")");
        }
        return out;
    }
};

#define REQUIRE_TRUE(expr)                                                                           \
    do {                                                                                             \
        if (!(expr)) {                                                                               \
            throw Test_failure(#expr, __FILE__, __LINE__);                                           \
        }                                                                                            \
    } while (false)

#define REQUIRE_FALSE(expr) REQUIRE_TRUE(!(expr))

#define REQUIRE_EQ(expected, actual)                                                                 \
    do {                                                                                             \
        auto _exp = (expected);                                                                      \
        auto _act = (actual);                                                                        \
        if (!(_exp == _act)) {                                                                       \
            throw Test_failure(#expected " == " #actual, __FILE__, __LINE__,                        \
                               "expected " + std::to_string(_exp) + ", got " +                    \
                                   std::to_string(_act));                                            \
        }                                                                                            \
    } while (false)

#define REQUIRE_LT(val, ref)                                                                         \
    do {                                                                                             \
        if (!((val) < (ref))) {                                                                      \
            throw Test_failure(#val " < " #ref, __FILE__, __LINE__);                               \
        }                                                                                            \
    } while (false)

#define REQUIRE_GE(val, ref)                                                                         \
    do {                                                                                             \
        if (!((val) >= (ref))) {                                                                     \
            throw Test_failure(#val " >= " #ref, __FILE__, __LINE__);                              \
        }                                                                                            \
    } while (false)

struct Test_case
{
    const char* name;
    std::function<void()> fn;
    bool is_stress = false;
};

void test_basic_semantics()
{
    interprocess_semaphore sem(2);

    REQUIRE_TRUE(sem.try_wait());
    REQUIRE_TRUE(sem.try_wait());
    REQUIRE_FALSE(sem.try_wait());

    sem.post();
    REQUIRE_TRUE(sem.try_wait());

    sem.post();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    REQUIRE_TRUE(sem.timed_wait(deadline));

    const auto start = std::chrono::steady_clock::now();
    const bool signalled = sem.timed_wait(start + std::chrono::milliseconds(40));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_FALSE(signalled);
    REQUIRE_GE(elapsed, std::chrono::milliseconds(30));
    REQUIRE_LT(elapsed, std::chrono::milliseconds(200));

    sem.post();
    sem.wait();
}

void test_threaded_producer_consumer()
{
    constexpr int kProducerCount = 3;
    constexpr int kConsumerCount = 4;
    constexpr int kTotalItems = 2000;

    interprocess_semaphore sem(0);
    std::atomic<int> next_item{0};
    std::atomic<int> enqueued{0};
    std::atomic<int> processed{0};

    std::mutex queue_mutex;
    std::queue<int> queue;

    auto produce = [&]() {
        while (true) {
            int idx = next_item.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= kTotalItems) {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push(idx);
            }

            enqueued.fetch_add(1, std::memory_order_acq_rel);
            sem.post();

            if ((idx & 0x3F) == 0) {
                std::this_thread::yield();
            }
        }
    };

    auto consume = [&]() {
        while (true) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (!sem.timed_wait(deadline)) {
                if (processed.load(std::memory_order_acquire) == kTotalItems) {
                    return;
                }
                throw Test_failure("consumer timed out waiting for semaphore", __FILE__, __LINE__);
            }

            int value = -2;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                REQUIRE_TRUE(!queue.empty());
                value = queue.front();
                queue.pop();
            }

            if (value == -1) {
                return;
            }

            REQUIRE_GE(value, 0);
            REQUIRE_LT(value, kTotalItems);
            processed.fetch_add(1, std::memory_order_acq_rel);

            if ((value & 0xFF) == 0) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> producers;
    for (int i = 0; i < kProducerCount; ++i) {
        producers.emplace_back(produce);
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < kConsumerCount; ++i) {
        consumers.emplace_back(consume);
    }

    for (auto& t : producers) {
        t.join();
    }

    REQUIRE_EQ(kTotalItems, enqueued.load(std::memory_order_acquire));

    for (int i = 0; i < kConsumerCount; ++i) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push(-1); // Sentinel instructing consumer to exit.
        }
        sem.post();
    }

    for (auto& t : consumers) {
        t.join();
    }

    REQUIRE_EQ(kTotalItems, processed.load(std::memory_order_acquire));
}

#if defined(__unix__) || defined(__APPLE__)

struct Shared_state
{
    std::atomic<int> ready{0};
    std::atomic<int> start{0};
    std::atomic<int> processed{0};

    static constexpr int kResources = 3;
    std::atomic<int> resource_owner[kResources];

    typename std::aligned_storage<sizeof(interprocess_semaphore), alignof(interprocess_semaphore)>::type sem_storage;

    Shared_state()
    {
        for (auto& owner : resource_owner) {
            owner.store(-1, std::memory_order_relaxed);
        }
        new (&sem_storage) interprocess_semaphore(kResources);
    }

    ~Shared_state()
    {
        semaphore()->~interprocess_semaphore();
    }

    interprocess_semaphore* semaphore()
    {
        return std::launder(reinterpret_cast<interprocess_semaphore*>(&sem_storage));
    }
};

[[noreturn]] void child_exit(int code)
{
    _exit(code);
}

void run_child(Shared_state* shared, int iterations)
{
    const int pid = static_cast<int>(::getpid());

    shared->ready.fetch_add(1, std::memory_order_acq_rel);
    while (shared->start.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    auto* sem = shared->semaphore();

    for (int iter = 0; iter < iterations; ++iter) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (!sem->timed_wait(deadline)) {
            child_exit(2);
        }

        int acquired = -1;
        for (int i = 0; i < Shared_state::kResources; ++i) {
            int expected = -1;
            if (shared->resource_owner[i].compare_exchange_strong(expected, pid, std::memory_order_acq_rel)) {
                acquired = i;
                break;
            }
        }

        if (acquired == -1) {
            child_exit(3);
        }

        if ((iter & 0x7) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        int expected = pid;
        if (!shared->resource_owner[acquired].compare_exchange_strong(expected, -1, std::memory_order_release)) {
            child_exit(4);
        }

        sem->post();
        shared->processed.fetch_add(1, std::memory_order_acq_rel);
    }

    child_exit(0);
}

void test_cross_process_coordination()
{
    constexpr int kChildren = 4;
    constexpr int kIterationsPerChild = 120;
    constexpr auto kOverallTimeout = std::chrono::seconds(20);

    void* mapping = ::mmap(nullptr, sizeof(Shared_state), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap");
    }

    Shared_state* shared = new (mapping) Shared_state();

    std::vector<pid_t> children;
    children.reserve(kChildren);

    try {
        for (int i = 0; i < kChildren; ++i) {
            pid_t pid = ::fork();
            if (pid == -1) {
                throw std::system_error(errno, std::generic_category(), "fork");
            }
            if (pid == 0) {
                run_child(shared, kIterationsPerChild);
            }
            children.push_back(pid);
        }

        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (shared->ready.load(std::memory_order_acquire) < kChildren) {
            if (std::chrono::steady_clock::now() > ready_deadline) {
                throw Test_failure("child processes failed to report ready", __FILE__, __LINE__);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        shared->start.store(1, std::memory_order_release);

        const int expected = kChildren * kIterationsPerChild;
        const auto finish_deadline = std::chrono::steady_clock::now() + kOverallTimeout;
        while (shared->processed.load(std::memory_order_acquire) < expected) {
            if (std::chrono::steady_clock::now() > finish_deadline) {
                throw Test_failure("processing did not finish before timeout", __FILE__, __LINE__);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        for (pid_t pid : children) {
            int status = 0;
            pid_t waited = ::waitpid(pid, &status, 0);
            if (waited != pid) {
                throw Test_failure("waitpid returned unexpected pid", __FILE__, __LINE__);
            }
            REQUIRE_TRUE(WIFEXITED(status));
            REQUIRE_EQ(0, WEXITSTATUS(status));
        }

        REQUIRE_EQ(expected, shared->processed.load(std::memory_order_acquire));
    }
    catch (...) {
        for (pid_t pid : children) {
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
        }
        shared->~Shared_state();
        ::munmap(mapping, sizeof(Shared_state));
        throw;
    }

    shared->~Shared_state();
    ::munmap(mapping, sizeof(Shared_state));
}

#else

void test_cross_process_coordination()
{
    std::cout << "Cross-process coordination test skipped on this platform." << std::endl;
}

#endif

} // namespace

int main()
{
    const std::vector<Test_case> tests = {
        {"basic_semantics", test_basic_semantics, false},
        {"threaded_producer_consumer", test_threaded_producer_consumer, true},
        {"cross_process_coordination", test_cross_process_coordination, true},
    };

    int failures = 0;

    for (const auto& test : tests) {
        std::cout << "[ RUN      ] " << test.name;
        if (test.is_stress) {
            std::cout << " (stress)";
        }
        std::cout << std::endl;

        try {
            test.fn();
            std::cout << "[       OK ] " << test.name << std::endl;
        }
        catch (const std::exception& ex) {
            ++failures;
            std::cout << "[  FAILED  ] " << test.name << " - " << ex.what() << std::endl;
        }
        catch (...) {
            ++failures;
            std::cout << "[  FAILED  ] " << test.name << " - unknown exception" << std::endl;
        }
    }

    if (failures != 0) {
        std::cout << failures << " test(s) failed." << std::endl;
    }
    else {
        std::cout << "All " << tests.size() << " test(s) passed." << std::endl;
    }

    return failures == 0 ? 0 : 1;
}
