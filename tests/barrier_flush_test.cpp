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

#include <array>
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
    std::array<std::size_t, kIterations> iteration_counts{};
    std::size_t total_messages        = 0;
    std::size_t iterations_completed  = 0;
    bool success                      = true;
    std::string failure_reason;
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

    activate_slot([&](const Iteration_marker& marker) {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (marker.iteration >= kIterations) {
            if (state.success) {
                std::ostringstream oss;
                oss << "coordinator received marker for iteration "
                    << marker.iteration
                    << " but only " << kIterations << " iterations were scheduled";
                state.failure_reason = oss.str();
                state.success = false;
            }
            state.cv.notify_all();
            return;
        }

        auto& count = state.iteration_counts[marker.iteration];
        ++count;
        ++state.total_messages;

        if (count > kWorkerCount && state.success) {
            std::ostringstream oss;
            oss << "coordinator observed " << count << " markers for iteration "
                << marker.iteration << " (expected " << kWorkerCount << ")";
            state.failure_reason = oss.str();
            state.success = false;
        }

        state.cv.notify_all();
    });

    barrier("barrier-flush-ready");

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {

        std::unique_lock<std::mutex> lock(state.mutex);
        if (state.success) {
            state.cv.wait(lock, [&] {
                return state.iteration_counts[iteration] >= kWorkerCount || !state.success;
            });
        }

        if (state.iteration_counts[iteration] >= kWorkerCount) {
            state.iterations_completed = iteration + 1;
        }

        lock.unlock();

        barrier("barrier-flush-iteration");

    }

    barrier("barrier-flush-done", "_sintra_all_processes");
    deactivate_all_slots();

    const auto shared_dir = get_shared_directory();
    std::size_t iterations_completed = 0;
    std::size_t total_messages = 0;
    bool success = true;
    std::string failure_reason;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        iterations_completed = state.iterations_completed;
        total_messages = state.total_messages;
        success = state.success;
        failure_reason = state.failure_reason;
    }
    write_result(shared_dir, success, iterations_completed, total_messages, failure_reason);
    return success ? 0 : 1;
}

int worker_process(std::uint32_t worker_index)
{
    using namespace sintra;

    barrier("barrier-flush-ready");

    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {

        world() << Iteration_marker{worker_index, iteration};

        barrier("barrier-flush-iteration");

    }

    barrier("barrier-flush-done", "_sintra_all_processes");
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
