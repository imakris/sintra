//
// Sintra Barrier Flush Test
//
// Validates that repeated inter-process barrier synchronisation completes
// successfully while messages are flowing between processes. 
// Two worker processes send iteration markers to a coordinator process and
// immediately wait on a barrier.  The coordinator waits until it has received
// the expected number of markers for the current iteration, joins the barrier
// and repeats.  After all iterations complete the coordinator writes the
// result to a shared directory that the parent process verifies.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kWorkerCount         = 2;
constexpr std::size_t kIterations          = 128;
constexpr std::uint32_t kFuzzMinDelayUs    = 0U;
constexpr std::uint32_t kFuzzMaxDelayUs    = 1000U;
constexpr unsigned      kFuzzInjectionRate = 2U;

struct Iteration_marker
{
    std::uint32_t worker;
    std::uint32_t iteration;
};

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_tests";
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "barrier_flush_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

struct Coordinator_state
{
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t messages_in_iteration = 0;
    std::size_t total_messages        = 0;
    bool too_many_messages            = false;
};

struct delay_fuzz_settings_guard
{
    delay_fuzz_settings_guard(std::uint32_t min_delay_us,
                              std::uint32_t max_delay_us,
                              unsigned injection_rate) noexcept
        : m_previous_min(sintra::detail::deterministic_delay_fuzzer::min_delay())
        , m_previous_max(sintra::detail::deterministic_delay_fuzzer::max_delay())
        , m_previous_rate(sintra::detail::deterministic_delay_fuzzer::injection_rate())
    {
        sintra::set_delay_fuzzing_bounds(min_delay_us, max_delay_us);
        sintra::set_delay_fuzzing_injection_rate(injection_rate);
    }

    ~delay_fuzz_settings_guard() noexcept
    {
        sintra::set_delay_fuzzing_bounds(m_previous_min, m_previous_max);
        sintra::set_delay_fuzzing_injection_rate(m_previous_rate);
    }

    delay_fuzz_settings_guard(const delay_fuzz_settings_guard&) = delete;
    delay_fuzz_settings_guard& operator=(const delay_fuzz_settings_guard&) = delete;
    delay_fuzz_settings_guard(delay_fuzz_settings_guard&&) = delete;
    delay_fuzz_settings_guard& operator=(delay_fuzz_settings_guard&&) = delete;

private:
    std::uint32_t m_previous_min;
    std::uint32_t m_previous_max;
    unsigned      m_previous_rate;
};

void write_result(const std::filesystem::path& dir,
                  bool success,
                  std::size_t iterations_completed,
                  std::size_t total_messages,
                  const std::string& failure_reason)
{
    std::ofstream out(dir / "barrier_flush_result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open barrier_flush_result.txt for writing");
    }

    out << (success ? "ok" : "fail") << '\n';
    out << iterations_completed << '\n';
    out << total_messages << '\n';
    if (!success) {
        out << failure_reason << '\n';
    }
}

int coordinator_process()
{
    using namespace sintra;

    Coordinator_state state;
    bool success = true;
    std::string failure_reason;

    activate_slot([&](const Iteration_marker&) {
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-slot-before-lock");
        std::lock_guard<std::mutex> lock(state.mutex);
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-slot-after-lock");
        if (state.messages_in_iteration >= kWorkerCount) {
            state.too_many_messages = true;
        } else {
            ++state.messages_in_iteration;
            ++state.total_messages;
        }
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-slot-after-update");
        state.cv.notify_one();
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-slot-after-notify");
    });

    {
        sintra::set_delay_fuzzing_seed(0xC0FFEE0000000000ULL);
        const sintra::detail::delay_fuzz_scope ready_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-before-ready-barrier");
        barrier("barrier-flush-ready");
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-after-ready-barrier");
    }

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        sintra::set_delay_fuzzing_seed(static_cast<std::uint64_t>(iteration) << 32);
        const sintra::detail::delay_fuzz_scope fuzz_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-before-lock");

        std::unique_lock<std::mutex> lock(state.mutex);
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-after-lock");
        state.cv.wait(lock, [&] {
            return state.messages_in_iteration == kWorkerCount || state.too_many_messages;
        });
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-after-wait");
        if (state.too_many_messages && success) {
            success = false;
            failure_reason = "coordinator observed more messages than expected during an iteration";
        }
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-before-reset");
        state.messages_in_iteration = 0;
        state.too_many_messages = false;
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-after-reset");
        lock.unlock();

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("coordinator-before-barrier");

        barrier("barrier-flush-iteration");

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("coordinator-after-barrier");
    }

    {
        sintra::set_delay_fuzzing_seed(0xC0FFEE00D00E0000ULL);
        const sintra::detail::delay_fuzz_scope done_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-before-done-barrier");
        barrier("barrier-flush-done", "_sintra_all_processes");
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "coordinator-after-done-barrier");
    }
    deactivate_all_slots();

    const auto shared_dir = get_shared_directory();
    write_result(shared_dir, success, kIterations, state.total_messages, failure_reason);
    return success ? 0 : 1;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    {
        const std::uint64_t ready_seed = 0xFACE000000000000ULL
            ^ static_cast<std::uint64_t>(worker_index + 1);
        sintra::set_delay_fuzzing_seed(ready_seed);
        const sintra::detail::delay_fuzz_scope ready_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "worker-before-ready-barrier");
        barrier("barrier-flush-ready");
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "worker-after-ready-barrier");
    }

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::uint64_t seed = (static_cast<std::uint64_t>(iteration) << 32)
            ^ static_cast<std::uint64_t>(worker_index + 1);
        sintra::set_delay_fuzzing_seed(seed);
        const sintra::detail::delay_fuzz_scope fuzz_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("worker-before-send");
        world() << Iteration_marker{worker_index, iteration};

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("worker-after-send");

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("worker-before-barrier");
        barrier("barrier-flush-iteration");

        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay("worker-after-barrier");
    }

    {
        const std::uint64_t done_seed = 0xFACE00D00E000000ULL
            ^ static_cast<std::uint64_t>(worker_index + 1);
        sintra::set_delay_fuzzing_seed(done_seed);
        const sintra::detail::delay_fuzz_scope done_scope{true};
        const delay_fuzz_settings_guard fuzz_settings{
            kFuzzMinDelayUs, kFuzzMaxDelayUs, kFuzzInjectionRate};
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "worker-before-done-barrier");
        barrier("barrier-flush-done", "_sintra_all_processes");
        sintra::detail::deterministic_delay_fuzzer::maybe_inject_delay(
            "worker-after-done-barrier");
    }
    return 0;
}

int worker0_process()
{
    return worker_process(0);
}

int worker1_process()
{
    return worker_process(1);
}

void custom_terminate_handler()
{
    std::fprintf(stderr, "std::terminate called!\n");
    std::fprintf(stderr, "Uncaught exceptions: %d\n", std::uncaught_exceptions());

    try {
        auto eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        } else {
            std::fprintf(stderr, "terminate called without an active exception\n");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Uncaught exception of unknown type\n");
    }

    std::abort();
}

void cleanup_directory_with_retries(const std::filesystem::path& dir)
{
    bool cleanup_succeeded = false;
    for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
        try {
            std::filesystem::remove_all(dir);
            cleanup_succeeded = true;
        } catch (const std::exception& e) {
            if (retry < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                std::fprintf(stderr,
                             "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                             dir.string().c_str(), e.what());
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(custom_terminate_handler);

    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(worker0_process);
    processes.emplace_back(worker1_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("barrier-flush-done", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "barrier_flush_result.txt";
        if (!std::filesystem::exists(result_path)) {
            std::fprintf(stderr, "Error: result file not found at %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Error: failed to open result file %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::string status;
        std::size_t iterations_completed = 0;
        std::size_t total_messages = 0;
        std::string reason;

        std::getline(in, status);
        in >> iterations_completed;
        in >> total_messages;
        std::getline(in >> std::ws, reason);

        cleanup_directory_with_retries(shared_dir);

        if (status != "ok") {
            std::fprintf(stderr, "Barrier flush test reported failure: %s\n", reason.c_str());
            return 1;
        }
        if (iterations_completed != kIterations) {
            std::fprintf(stderr, "Expected %zu iterations, got %zu\n",
                         kIterations, iterations_completed);
            return 1;
        }
        const std::size_t expected_messages = kWorkerCount * kIterations;
        if (total_messages != expected_messages) {
            std::fprintf(stderr, "Expected %zu total messages, got %zu\n",
                         expected_messages, total_messages);
            return 1;
        }
    }

    return 0;
}
