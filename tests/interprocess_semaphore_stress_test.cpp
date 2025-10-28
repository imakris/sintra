#include "sintra/detail/interprocess_semaphore.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <sched.h>
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

class Assertion_error : public std::runtime_error {
public:
    Assertion_error(const std::string& expr, const char* file, int line, const std::string& message = {})
        : std::runtime_error(make_message(expr, file, line, message)) {}

private:
    static std::string make_message(const std::string& expr, const char* file, int line, const std::string& message)
    {
        std::ostringstream oss;
        oss << file << ':' << line << " - assertion failed: " << expr;
        if (!message.empty()) {
            oss << " (" << message << ')';
        }
        return oss.str();
    }
};

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        throw Assertion_error(#expr, __FILE__, __LINE__); \
    } \
} while (false)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(expected, actual) do { \
    auto _exp = (expected); \
    auto _act = (actual); \
    if (!(_exp == _act)) { \
        std::ostringstream _oss; \
        _oss << "expected " << _exp << ", got " << _act; \
        throw Assertion_error(#expected " == " #actual, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)

#define ASSERT_NE(val1, val2) do { \
    auto _v1 = (val1); \
    auto _v2 = (val2); \
    if (_v1 == _v2) { \
        std::ostringstream _oss; \
        _oss << "values both equal to " << _v1; \
        throw Assertion_error(#val1 " != " #val2, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)

#define ASSERT_LE(val, ref) ASSERT_TRUE((val) <= (ref))
#define ASSERT_LT(val, ref) ASSERT_TRUE((val) < (ref))
#define ASSERT_GE(val, ref) ASSERT_TRUE((val) >= (ref))
#define ASSERT_GT(val, ref) ASSERT_TRUE((val) > (ref))

#define ASSERT_DURATION_GE(val, ref, actual) do { \
    if (!((val) >= (ref))) { \
        std::ostringstream _oss; \
        _oss << "expected duration >= " << std::chrono::duration_cast<std::chrono::milliseconds>(ref).count() \
             << "ms, observed " << std::chrono::duration_cast<std::chrono::milliseconds>(actual).count() << "ms"; \
        throw Assertion_error(#val " >= " #ref, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)

#define ASSERT_DURATION_LT(val, ref, actual) do { \
    if (!((val) < (ref))) { \
        std::ostringstream _oss; \
        _oss << "expected duration < " << std::chrono::duration_cast<std::chrono::milliseconds>(ref).count() \
             << "ms, observed " << std::chrono::duration_cast<std::chrono::milliseconds>(actual).count() << "ms"; \
        throw Assertion_error(#val " < " #ref, __FILE__, __LINE__, _oss.str()); \
    } \
} while (false)

struct Test_case {
    std::string name;
    std::function<void()> fn;
    bool is_stress = false;
};

inline std::vector<Test_case>& registry()
{
    static std::vector<Test_case> tests;
    return tests;
}

struct Register_test {
    Register_test(const std::string& name, std::function<void()> fn, bool is_stress)
    {
        registry().push_back({name, std::move(fn), is_stress});
    }
};

#define TEST_CASE(name) \
    void name(); \
    static Register_test name##_registrar(#name, name, false); \
    void name()

#define STRESS_TEST(name) \
    void name(); \
    static Register_test name##_registrar(#name, name, true); \
    void name()

using sintra::detail::interprocess_semaphore;

TEST_CASE(test_basic_counting_behaviour)
{
    interprocess_semaphore sem(2);

    ASSERT_TRUE(sem.try_wait());
    ASSERT_TRUE(sem.try_wait());
    ASSERT_FALSE(sem.try_wait());

    sem.post();
    ASSERT_TRUE(sem.try_wait());

    sem.post();
    auto deadline = std::chrono::steady_clock::now() + 50ms;
    ASSERT_TRUE(sem.timed_wait(deadline));

    ASSERT_FALSE(sem.try_wait());
}

TEST_CASE(test_timed_wait_timeout_accuracy)
{
    interprocess_semaphore sem(0);
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + 150ms;
    bool signaled = sem.timed_wait(deadline);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(signaled);

    // Allow a reasonable scheduling tolerance but assert that we waited most of the interval.
    ASSERT_DURATION_GE(elapsed, 100ms, elapsed);
}

TEST_CASE(test_timed_wait_responds_to_post)
{
    interprocess_semaphore sem(0);
    auto worker = std::thread([&sem] {
        std::this_thread::sleep_for(40ms);
        sem.post();
    });

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + 1s;
    bool signaled = sem.timed_wait(deadline);
    auto elapsed = std::chrono::steady_clock::now() - start;
    worker.join();

    ASSERT_TRUE(signaled);
    ASSERT_DURATION_LT(elapsed, 400ms, elapsed);
}

STRESS_TEST(test_multithreaded_contention)
{
    constexpr int kProducerThreads = 4;
    constexpr int kConsumerThreads = 4;
    constexpr int kItemsPerProducer = 2500;
    constexpr int kTotalItems = kProducerThreads * kItemsPerProducer;

    interprocess_semaphore sem(0);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    const auto global_deadline = std::chrono::steady_clock::now() + 15s;

    std::vector<std::thread> threads;
    threads.reserve(kProducerThreads + kConsumerThreads);

    for (int i = 0; i < kProducerThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int n = 0; n < kItemsPerProducer; ++n) {
                sem.post();
                produced.fetch_add(1, std::memory_order_acq_rel);

                if ((n & 0x3F) == 0) {
                    std::this_thread::sleep_for(100us * (1 + (i % 3)));
                }
            }
        });
    }

    for (int i = 0; i < kConsumerThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            auto local_deadline = std::chrono::steady_clock::now() + 200ms;
            while (true) {
                if (std::chrono::steady_clock::now() > global_deadline) {
                    throw Assertion_error("consumer timed out", __FILE__, __LINE__);
                }

                bool got = sem.timed_wait(local_deadline);
                if (got) {
                    int after = consumed.fetch_add(1, std::memory_order_acq_rel) + 1;
                    ASSERT_LE(after, kTotalItems);

                    if ((after % 256) == 0) {
                        std::this_thread::yield();
                    }

                    local_deadline = std::chrono::steady_clock::now() + 200ms;
                }
                else {
                    if (produced.load(std::memory_order_acquire) == kTotalItems &&
                        consumed.load(std::memory_order_acquire) == kTotalItems) {
                        break;
                    }
                    local_deadline = std::chrono::steady_clock::now() + 200ms;
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < (kProducerThreads + kConsumerThreads)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(kTotalItems, produced.load(std::memory_order_acquire));
    ASSERT_EQ(kTotalItems, consumed.load(std::memory_order_acquire));
}

#if defined(__unix__) || defined(__APPLE__)

STRESS_TEST(test_cross_process_signaling)
{
    struct Shared_state {
        std::atomic<int> ready;
        std::atomic<int> start;
        std::atomic<int> stop;
        std::atomic<int> processed;
        alignas(interprocess_semaphore) unsigned char sem_storage[sizeof(interprocess_semaphore)];

        interprocess_semaphore* semaphore()
        {
            return std::launder(reinterpret_cast<interprocess_semaphore*>(sem_storage));
        }
    };

    void* mapping = mmap(nullptr,
                         sizeof(Shared_state),
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS,
                         -1,
                         0);
    ASSERT_NE(mapping, MAP_FAILED);

    std::memset(mapping, 0, sizeof(Shared_state));
    auto* shared = static_cast<Shared_state*>(mapping);
    new (shared->sem_storage) interprocess_semaphore(0);

    constexpr int kChildren = 4;
    constexpr int kItemsPerChild = 1500;
    constexpr int kTotalItems = kChildren * kItemsPerChild;

    shared->ready.store(0, std::memory_order_relaxed);
    shared->start.store(0, std::memory_order_relaxed);
    shared->stop.store(0, std::memory_order_relaxed);
    shared->processed.store(0, std::memory_order_relaxed);

    std::vector<pid_t> children;
    children.reserve(kChildren);

    for (int i = 0; i < kChildren; ++i) {
        pid_t pid = ::fork();
        if (pid == -1) {
            throw Assertion_error("fork failed", __FILE__, __LINE__);
        }
        if (pid == 0) {
            // Child process
            shared->ready.fetch_add(1, std::memory_order_release);
            while (shared->start.load(std::memory_order_acquire) == 0) {
                sched_yield();
            }

            auto* sem = shared->semaphore();
            while (true) {
                sem->wait();
                if (shared->stop.load(std::memory_order_acquire)) {
                    break;
                }
                shared->processed.fetch_add(1, std::memory_order_acq_rel);
                if ((shared->processed.load(std::memory_order_relaxed) & 0xFF) == 0) {
                    sched_yield();
                }
            }
            _exit(0);
        }
        children.push_back(pid);
    }

    auto* sem = shared->semaphore();
    const auto overall_deadline = std::chrono::steady_clock::now() + 20s;

    while (shared->ready.load(std::memory_order_acquire) < kChildren) {
        if (std::chrono::steady_clock::now() > overall_deadline) {
            throw Assertion_error("children failed to initialise", __FILE__, __LINE__);
        }
        std::this_thread::sleep_for(1ms);
    }
    shared->start.store(1, std::memory_order_release);

    for (int i = 0; i < kTotalItems; ++i) {
        sem->post();
        if ((i & 0x7F) == 0) {
            std::this_thread::sleep_for(50us);
        }
    }

    while (shared->processed.load(std::memory_order_acquire) < kTotalItems) {
        if (std::chrono::steady_clock::now() > overall_deadline) {
            throw Assertion_error("processing did not complete", __FILE__, __LINE__);
        }
        std::this_thread::sleep_for(2ms);
    }

    shared->stop.store(1, std::memory_order_release);
    for (int i = 0; i < kChildren; ++i) {
        sem->post();
    }

    for (pid_t pid : children) {
        int status = 0;
        pid_t waited = ::waitpid(pid, &status, 0);
        if (waited != pid) {
            throw Assertion_error("waitpid returned unexpected pid", __FILE__, __LINE__);
        }
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(0, WEXITSTATUS(status));
    }

    ASSERT_EQ(kTotalItems, shared->processed.load(std::memory_order_acquire));

    sem->~interprocess_semaphore();
    ::munmap(mapping, sizeof(Shared_state));
}

#else

TEST_CASE(test_cross_process_signaling)
{
    std::cout << "Cross-process test is skipped on this platform." << std::endl;
}

#endif

} // namespace

int main()
{
    int failures = 0;
    for (const auto& test : registry()) {
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
        std::cout << "All " << registry().size() << " test(s) passed." << std::endl;
    }

    return failures == 0 ? 0 : 1;
}

