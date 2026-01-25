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

#include <sintra/detail/ipc/semaphore.h>

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdarg>
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

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
    #include <sys/mman.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#if defined(_WIN32)
namespace sintra::detail {
void test_get_win_semaphore_info(const interprocess_semaphore& sem,
                                  std::uint64_t& id, std::wstring& name)
{
    auto& backend = sem.m_impl;
    const auto& state = *reinterpret_cast<const ips_backend_win_state*>(backend.storage);

    id = (static_cast<std::uint64_t>(state.init_flag) & 0xFFFFFFFF) |
         (static_cast<std::uint64_t>(state.initial) << 32);
    name.assign(state.name);
}
} // namespace sintra::detail
#endif

#if !defined(_WIN32)
namespace sintra::detail {
void test_get_posix_semaphore_info(const interprocess_semaphore& sem,
                                   std::uint32_t& count,
                                   std::uint32_t& max)
{
    const auto& backend = sem.m_impl;
    const auto& state = *reinterpret_cast<const ips_backend_posix_state*>(backend.storage);
    count = state.count.load(std::memory_order_relaxed);
    max = state.max;
}
} // namespace sintra::detail
#endif

namespace
{

using sintra::detail::interprocess_semaphore;

#if defined(_WIN32)
inline int current_pid() { return _getpid(); }
#else
inline int current_pid() { return static_cast<int>(getpid()); }
#endif

inline uint64_t thread_id_numeric()
{
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

inline uint64_t elapsed_ms()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

inline std::mutex& log_mutex()
{
    static std::mutex mtx;
    return mtx;
}

inline void log_event(const char* tag, const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[interprocess_semaphore_test +%llums pid=%d tid=%llu] %s: ",
                 static_cast<unsigned long long>(elapsed_ms()),
                 current_pid(),
                 static_cast<unsigned long long>(thread_id_numeric()),
                 tag);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

struct Thread_stats
{
    std::uint64_t posts = 0;
    std::uint64_t waits = 0;
    std::uint64_t acquired = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t yields = 0;
    std::uint32_t seed = 0;
    std::uint32_t last_rng = 0;
    int last_errno = 0;
};

#if defined(_WIN32)
constexpr std::size_t k_windows_name_chars = 64;
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
    }                                                                                                \
    while (false)

#define REQUIRE_FALSE(expr) REQUIRE_TRUE(!(expr))

#define REQUIRE_EQ(expected, actual)                                                                 \
    do {                                                                                             \
        auto _exp = (expected);                                                                      \
        auto _act = (actual);                                                                        \
        if (!(_exp == _act)) {                                                                       \
            throw Test_failure(#expected " == " #actual, __FILE__, __LINE__,                         \
                               "expected " + std::to_string(_exp) + ", got " +                       \
                                   std::to_string(_act));                                            \
        }                                                                                            \
    }                                                                                                \
    while (false)

#define REQUIRE_LT(val, ref)                                                                         \
    do {                                                                                             \
        if (!((val) < (ref))) {                                                                      \
            throw Test_failure(#val " < " #ref, __FILE__, __LINE__);                                 \
        }                                                                                            \
    }                                                                                                \
    while (false)

#define REQUIRE_GE(val, ref)                                                                         \
    do {                                                                                             \
        if (!((val) >= (ref))) {                                                                     \
            throw Test_failure(#val " >= " #ref, __FILE__, __LINE__);                                \
        }                                                                                            \
    }                                                                                                \
    while (false)

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
    constexpr int k_producer_count = 3;
    constexpr int k_consumer_count = 4;
    constexpr int k_total_items = 2000;

    interprocess_semaphore sem(0);
    std::atomic<int> next_item{0};
    std::atomic<int> enqueued{0};
    std::atomic<int> processed{0};

    std::mutex queue_mutex;
    std::queue<int> queue;

    auto produce = [&]() {
        while (true) {
            int idx = next_item.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= k_total_items) {
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
                if (processed.load(std::memory_order_acquire) == k_total_items) {
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
            REQUIRE_LT(value, k_total_items);
            processed.fetch_add(1, std::memory_order_acq_rel);

            if ((value & 0xFF) == 0) {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> producers;
    for (int i = 0; i < k_producer_count; ++i) {
        producers.emplace_back(produce);
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < k_consumer_count; ++i) {
        consumers.emplace_back(consume);
    }

    for (auto& t : producers) {
        t.join();
    }

    REQUIRE_EQ(k_total_items, enqueued.load(std::memory_order_acquire));

    for (int i = 0; i < k_consumer_count; ++i) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push(-1); // Sentinel instructing consumer to exit.
        }
        sem.post();
    }

    for (auto& t : consumers) {
        t.join();
    }

    REQUIRE_EQ(k_total_items, processed.load(std::memory_order_acquire));
}

void test_release_local_handle_idempotent()
{
    // release_local_handle() is documented as a no-op on current backends.
    // This test verifies that calling it multiple times is safe and doesn't
    // break semaphore functionality.
    interprocess_semaphore sem(0);

    sem.release_local_handle();
    sem.release_local_handle();

    sem.post();
    REQUIRE_TRUE(sem.try_wait());
}

void test_multithreaded_contention()
{
    constexpr int k_threads = 8;
    constexpr int k_iterations_per_thread = 1500;

    interprocess_semaphore sem(0);
    std::atomic<int> posted{0};
    std::atomic<int> acquired{0};
    std::vector<Thread_stats> stats(static_cast<std::size_t>(k_threads));

    std::random_device rd;
    std::vector<std::thread> threads;
    threads.reserve(k_threads);

    for (int t = 0; t < k_threads; ++t) {
        threads.emplace_back([&, seed = rd(), index = t]() mutable {
            auto& local = stats[static_cast<std::size_t>(index)];
            local.seed = seed;
            std::mt19937 rng(seed);
            for (int i = 0; i < k_iterations_per_thread; ++i) {
                const auto sample = rng();
                local.last_rng = sample;
                if ((sample & 3) == 0) {
                    sem.post();
                    posted.fetch_add(1);
                    ++local.posts;
                }
                else {
                    ++local.waits;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
                    if (sem.timed_wait(deadline)) {
                        acquired.fetch_add(1);
                        ++local.acquired;
                    }
                    else {
                        ++local.timeouts;
                        local.last_errno = errno;
                    }
                }

                if ((i & 0x7F) == 0) {
                    std::this_thread::yield();
                    ++local.yields;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    int drained = 0;
    while (sem.try_wait()) {
        acquired.fetch_add(1);
        ++drained;
    }

    const auto posted_total = posted.load();
    const auto acquired_total = acquired.load();

    if (posted_total != acquired_total) {
        std::uint64_t total_posts = 0;
        std::uint64_t total_waits = 0;
        std::uint64_t total_acquired = 0;
        std::uint64_t total_timeouts = 0;
        std::uint64_t total_yields = 0;
        for (const auto& entry : stats) {
            total_posts += entry.posts;
            total_waits += entry.waits;
            total_acquired += entry.acquired;
            total_timeouts += entry.timeouts;
            total_yields += entry.yields;
        }

        log_event("contention", "mismatch posted=%d acquired=%d drained=%d totals posts=%llu waits=%llu acquired=%llu timeouts=%llu yields=%llu",
                  posted_total,
                  acquired_total,
                  drained,
                  static_cast<unsigned long long>(total_posts),
                  static_cast<unsigned long long>(total_waits),
                  static_cast<unsigned long long>(total_acquired),
                  static_cast<unsigned long long>(total_timeouts),
                  static_cast<unsigned long long>(total_yields));

#if defined(_WIN32)
        std::uint64_t sem_id = 0;
        std::wstring sem_name;
        test_get_win_semaphore_info(sem, sem_id, sem_name);
        log_event("contention", "win semaphore id=0x%llx name=%ls",
                  static_cast<unsigned long long>(sem_id),
                  sem_name.c_str());
#else
        std::uint32_t sem_count = 0;
        std::uint32_t sem_max = 0;
        test_get_posix_semaphore_info(sem, sem_count, sem_max);
        log_event("contention", "posix semaphore count=%u max=%u",
                  sem_count,
                  sem_max);
#endif

        for (std::size_t idx = 0; idx < stats.size(); ++idx) {
            const auto& entry = stats[idx];
            log_event("contention", "thread[%zu] seed=%u last_rng=%u last_errno=%d posts=%llu waits=%llu acquired=%llu timeouts=%llu yields=%llu",
                      idx,
                      entry.seed,
                      entry.last_rng,
                      entry.last_errno,
                      static_cast<unsigned long long>(entry.posts),
                      static_cast<unsigned long long>(entry.waits),
                      static_cast<unsigned long long>(entry.acquired),
                      static_cast<unsigned long long>(entry.timeouts),
                      static_cast<unsigned long long>(entry.yields));
        }
    }

    REQUIRE_EQ(posted_total, acquired_total);
}

#if defined(__unix__) || defined(__APPLE__)

struct Shared_state
{
    std::atomic<int> ready{0};
    std::atomic<int> start{0};
    std::atomic<int> processed{0};

    static constexpr int k_resources = 3;
    std::atomic<int> resource_owner[k_resources];

    typename std::aligned_storage<sizeof(interprocess_semaphore), alignof(interprocess_semaphore)>::type sem_storage;

    Shared_state()
    {
        for (auto& owner : resource_owner) {
            owner = -1;
        }
        new (&sem_storage) interprocess_semaphore(k_resources);
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
        for (int i = 0; i < Shared_state::k_resources; ++i) {
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
    constexpr int k_children = 4;
    constexpr int k_iterations_per_child = 120;
    constexpr auto k_overall_timeout = std::chrono::seconds(20);

    void* mapping = ::mmap(nullptr, sizeof(Shared_state), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "mmap");
    }

    Shared_state* shared = new (mapping) Shared_state();

    std::vector<pid_t> children;
    children.reserve(k_children);

    try {
        for (int i = 0; i < k_children; ++i) {
            pid_t pid = ::fork();
            if (pid == -1) {
                throw std::system_error(errno, std::generic_category(), "fork");
            }
            if (pid == 0) {
                run_child(shared, k_iterations_per_child);
            }
            children.push_back(pid);
        }

        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (shared->ready.load(std::memory_order_acquire) < k_children) {
            if (std::chrono::steady_clock::now() > ready_deadline) {
                throw Test_failure("child processes failed to report ready", __FILE__, __LINE__);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        shared->start.store(1);

        const int expected = k_children * k_iterations_per_child;
        const auto finish_deadline = std::chrono::steady_clock::now() + k_overall_timeout;
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

void attach_existing_semaphore(sintra::detail::interprocess_semaphore& sem,
                               const std::string& name_ascii,
                               std::uint64_t /*id*/)
{
#if defined(_WIN32)
    using sem_t = sintra::detail::interprocess_semaphore;
    std::wstring wname(name_ascii.begin(), name_ascii.end());
    // Reconstruct in-place using the named constructor to attach to the existing kernel semaphore.
    sem.~interprocess_semaphore();
    new (&sem) sem_t(0u, wname.c_str(), 0x7fffffff);
#else
    (void)sem; (void)name_ascii;
#endif
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
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[cross-process child] exception: %s\n", e.what());
        return 3;
    }
}

void test_cross_process_coordination()
{
    constexpr int k_children = 3;
    constexpr int k_iterations_per_child = 400;
    constexpr int k_burst = 3;

    interprocess_semaphore work_sem(0);
    interprocess_semaphore ack_sem(0);

    struct Windows_descriptor {
        std::uint64_t id;
        std::wstring name;
    };

    auto describe = [](const interprocess_semaphore& sem) {
        Windows_descriptor desc{};
        sintra::detail::test_get_win_semaphore_info(sem, desc.id, desc.name);
        return desc;
    };

    const Windows_descriptor work_desc = describe(work_sem);
    const Windows_descriptor ack_desc = describe(ack_sem);

    const std::string work_name = narrow_from_wide(work_desc.name.c_str());
    const std::string ack_name = narrow_from_wide(ack_desc.name.c_str());
    const std::uint64_t work_id = work_desc.id;
    const std::uint64_t ack_id = ack_desc.id;

    const std::string executable = current_executable_path();
    const std::string iterations_arg = std::to_string(k_iterations_per_child);
    const std::string work_id_arg = std::to_string(static_cast<unsigned long long>(work_id));
    const std::string ack_id_arg = std::to_string(static_cast<unsigned long long>(ack_id));

    std::vector<intptr_t> children;
    children.reserve(k_children);

    for (int i = 0; i < k_children; ++i) {
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

    const int total_iterations = k_children * k_iterations_per_child;
    for (int offset = 0; offset < total_iterations; offset += k_burst) {
        const int batch_size = std::min(k_burst, total_iterations - offset);
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
