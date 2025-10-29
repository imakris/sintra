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
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>
#include <cwchar>

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace
{

using sintra::detail::interprocess_semaphore;

#if defined(_WIN32)
constexpr std::size_t kWindowsNameChars = 64;
#endif

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
    interprocess_semaphore sem(3);

    for (int i = 0; i < 3; ++i) {
        REQUIRE_TRUE(sem.try_wait());
    }
    REQUIRE_FALSE(sem.try_wait());

    sem.post();
    REQUIRE_TRUE(sem.try_wait());

    std::thread blocker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sem.post();
    });
    const auto wait_start = std::chrono::steady_clock::now();
    sem.wait();
    const auto wait_elapsed = std::chrono::steady_clock::now() - wait_start;
    blocker.join();
    REQUIRE_GE(wait_elapsed, std::chrono::milliseconds(10));

    const auto timeout_start = std::chrono::steady_clock::now();
    const bool timed_out = sem.timed_wait(timeout_start + std::chrono::milliseconds(80));
    const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_start;
    REQUIRE_FALSE(timed_out);
    REQUIRE_GE(timeout_elapsed, std::chrono::milliseconds(60));
    REQUIRE_LT(timeout_elapsed, std::chrono::milliseconds(250));

    std::thread notifier([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        sem.post();
    });
    const auto signal_wait_start = std::chrono::steady_clock::now();
    const bool signalled = sem.timed_wait(signal_wait_start + std::chrono::milliseconds(500));
    const auto signal_wait_elapsed = std::chrono::steady_clock::now() - signal_wait_start;
    notifier.join();
    REQUIRE_TRUE(signalled);
    REQUIRE_LT(signal_wait_elapsed, std::chrono::milliseconds(200));

    REQUIRE_FALSE(sem.try_wait());
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

void test_release_local_handle_idempotent()
{
    interprocess_semaphore sem(0);

#if defined(_WIN32)
    struct Windows_descriptor {
        std::uint64_t id;
        std::wstring name;
    };

    auto capture_descriptor = [](const interprocess_semaphore& sem) {
        Windows_descriptor desc{};
        const auto* base = reinterpret_cast<const unsigned char*>(&sem);
        std::memcpy(&desc.id, base, sizeof(desc.id));
        wchar_t name_buffer[kWindowsNameChars];
        std::memcpy(name_buffer, base + sizeof(desc.id), sizeof(name_buffer));
        name_buffer[kWindowsNameChars - 1] = L'\0';
        desc.name.assign(name_buffer);
        return desc;
    };

    const Windows_descriptor descriptor = capture_descriptor(sem);
#endif

    sem.release_local_handle();
    sem.release_local_handle();

#if defined(_WIN32)
    HANDLE recreated = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, descriptor.name.c_str());
    if (!recreated) {
        throw std::system_error(::GetLastError(), std::system_category(), "CreateSemaphoreW");
    }

    unsigned char* base = reinterpret_cast<unsigned char*>(&sem);
    std::memcpy(base, &descriptor.id, sizeof(descriptor.id));
    wchar_t name_buffer[kWindowsNameChars]{};
    std::wcsncpy(name_buffer, descriptor.name.c_str(), kWindowsNameChars - 1);
    std::memcpy(base + sizeof(descriptor.id), name_buffer, sizeof(name_buffer));

    sintra::detail::interprocess_semaphore_detail::register_handle(descriptor.id, recreated);
#endif

    sem.post();
    REQUIRE_TRUE(sem.try_wait());
}

void test_multithreaded_contention()
{
    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 1500;

    interprocess_semaphore sem(0);
    std::atomic<int> posted{0};
    std::atomic<int> acquired{0};

    std::random_device rd;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, seed = rd()]() mutable {
            std::mt19937 rng(seed);
            for (int i = 0; i < kIterationsPerThread; ++i) {
                if ((rng() & 3) == 0) {
                    sem.post();
                    posted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                    if (sem.timed_wait(deadline)) {
                        acquired.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if ((i & 0x7F) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    while (sem.try_wait()) {
        acquired.fetch_add(1, std::memory_order_relaxed);
    }

    REQUIRE_EQ(posted.load(std::memory_order_relaxed), acquired.load(std::memory_order_relaxed));
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

#elif defined(_WIN32)

std::string narrow_from_wide(const wchar_t* value)
{
    std::string result;
    if (!value) {
        return result;
    }
    while (*value) {
        result.push_back(static_cast<char>(*value++));
    }
    return result;
}

std::string current_executable_path()
{
    char buffer[MAX_PATH];
    DWORD length = ::GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        throw std::system_error(::GetLastError(), std::system_category(), "GetModuleFileNameA");
    }
    return std::string(buffer, length);
}

void attach_existing_semaphore(interprocess_semaphore& sem,
                               const std::string& name_ascii,
                               std::uint64_t id)
{
    std::wstring name_w(name_ascii.begin(), name_ascii.end());
    HANDLE handle = ::OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name_w.c_str());
    if (!handle) {
        handle = ::CreateSemaphoreW(nullptr, 0, LONG_MAX, name_w.c_str());
        if (!handle) {
            throw std::system_error(::GetLastError(), std::system_category(), "CreateSemaphoreW");
        }
    }

    sem.release_local_handle();
    unsigned char* base = reinterpret_cast<unsigned char*>(&sem);
    std::memcpy(base, &id, sizeof(id));
    wchar_t name_buffer[kWindowsNameChars]{};
    std::wcsncpy(name_buffer, name_w.c_str(), kWindowsNameChars - 1);
    std::memcpy(base + sizeof(id), name_buffer, sizeof(name_buffer));

    sintra::detail::interprocess_semaphore_detail::register_handle(id, handle);
}

int run_interprocess_child(const std::string& work_name,
                           std::uint64_t work_id,
                           const std::string& ack_name,
                           std::uint64_t ack_id,
                           int iterations)
{
    try {
        interprocess_semaphore work_sem(0);
        interprocess_semaphore ack_sem(0);
        attach_existing_semaphore(work_sem, work_name, work_id);
        attach_existing_semaphore(ack_sem, ack_name, ack_id);

        for (int i = 0; i < iterations; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            if (!work_sem.timed_wait(deadline)) {
                std::fprintf(stderr,
                             "[cross-process child] timed_wait timeout at iteration %d\n",
                             i);
                return 2;
            }

            if ((i & 0x3F) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }

            ack_sem.post();
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[cross-process child] exception: %s\n", e.what());
        return 3;
    }
}

void test_cross_process_coordination()
{
    constexpr int kChildren = 3;
    constexpr int kIterationsPerChild = 400;
    constexpr int kBurst = 3;

    interprocess_semaphore work_sem(0);
    interprocess_semaphore ack_sem(0);

    struct Windows_descriptor {
        std::uint64_t id;
        std::wstring name;
    };

    auto describe = [](const interprocess_semaphore& sem) {
        Windows_descriptor desc{};
        const auto* base = reinterpret_cast<const unsigned char*>(&sem);
        std::memcpy(&desc.id, base, sizeof(desc.id));
        wchar_t name_buffer[kWindowsNameChars];
        std::memcpy(name_buffer, base + sizeof(desc.id), sizeof(name_buffer));
        name_buffer[kWindowsNameChars - 1] = L'\0';
        desc.name.assign(name_buffer);
        return desc;
    };

    const Windows_descriptor work_desc = describe(work_sem);
    const Windows_descriptor ack_desc = describe(ack_sem);

    const std::string work_name = narrow_from_wide(work_desc.name.c_str());
    const std::string ack_name = narrow_from_wide(ack_desc.name.c_str());
    const std::uint64_t work_id = work_desc.id;
    const std::uint64_t ack_id = ack_desc.id;

    const std::string executable = current_executable_path();
    const std::string iterations_arg = std::to_string(kIterationsPerChild);
    const std::string work_id_arg = std::to_string(static_cast<unsigned long long>(work_id));
    const std::string ack_id_arg = std::to_string(static_cast<unsigned long long>(ack_id));

    std::vector<intptr_t> children;
    children.reserve(kChildren);

    for (int i = 0; i < kChildren; ++i) {
        std::vector<std::string> argv_strings = {
            executable,
            "--interprocess-semaphore-child",
            work_name,
            work_id_arg,
            ack_name,
            ack_id_arg,
            iterations_arg
        };

        std::vector<char*> argv_ptrs;
        argv_ptrs.reserve(argv_strings.size() + 1);
        for (auto& arg : argv_strings) {
            argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_ptrs.push_back(nullptr);

        intptr_t child = _spawnv(_P_NOWAIT, executable.c_str(), argv_ptrs.data());
        if (child == -1) {
            throw std::system_error(errno, std::generic_category(), "_spawnv");
        }
        children.push_back(child);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int total_iterations = kChildren * kIterationsPerChild;
    for (int offset = 0; offset < total_iterations; offset += kBurst) {
        const int batch_size = std::min(kBurst, total_iterations - offset);
        for (int j = 0; j < batch_size; ++j) {
            work_sem.post();
        }
        for (int j = 0; j < batch_size; ++j) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            if (!ack_sem.timed_wait(deadline)) {
                throw Test_failure("ack semaphore timed out", __FILE__, __LINE__);
            }
        }
    }

    for (intptr_t child : children) {
        int status = 0;
        intptr_t rc = _cwait(&status, child, WAIT_CHILD);
        if (rc == -1) {
            throw std::system_error(errno, std::generic_category(), "_cwait");
        }
        REQUIRE_EQ(0, status);
    }

    REQUIRE_FALSE(work_sem.try_wait());
    REQUIRE_FALSE(ack_sem.try_wait());
}

#else

void test_cross_process_coordination()
{
    std::cout << "Cross-process coordination test skipped on this platform." << std::endl;
}

#endif

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    if (argc == 7 && std::string_view(argv[1]) == "--interprocess-semaphore-child") {
        const std::string work_name = argv[2];
        const std::uint64_t work_id = std::strtoull(argv[3], nullptr, 10);
        const std::string ack_name = argv[4];
        const std::uint64_t ack_id = std::strtoull(argv[5], nullptr, 10);
        const int iterations = std::atoi(argv[6]);
        return run_interprocess_child(work_name, work_id, ack_name, ack_id, iterations);
    }
#else
    (void)argc;
    (void)argv;
#endif

    const std::vector<Test_case> tests = {
        {"basic_semantics", test_basic_semantics, false},
        {"release_local_handle_idempotent", test_release_local_handle_idempotent, false},
        {"threaded_producer_consumer", test_threaded_producer_consumer, true},
        {"multithreaded_contention", test_multithreaded_contention, true},
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
