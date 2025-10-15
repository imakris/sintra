//
// Sintra Barrier Processing Reentrancy Test
//
// Verifies that a message handler can call barrier<processing_fence_t>()
// without deadlocking on its own activity counter.  The coordinator process
// dispatches a Trigger to a worker whose handler joins the processing fence
// before sending an acknowledgement.  The coordinator joins the same barrier
// and only continues once the handler reports success.  Without the handler
// depth accounting fix the processing fence would wait for the handler to
// complete and the test would time out.
//

#include <sintra/sintra.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
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

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

struct Trigger
{
};

std::mutex g_coordinator_mutex;
std::condition_variable g_coordinator_condition;
bool g_acknowledged = false;
bool g_worker_barrier_ok = false;

std::mutex g_worker_mutex;
std::condition_variable g_worker_condition;
bool g_worker_should_exit = false;

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        return std::filesystem::path(value);
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

    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    unique_suffix ^= static_cast<long long>(dis(gen));

    std::ostringstream oss;
    oss << "processing_reentrancy_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_result(const std::filesystem::path& dir, bool success, const std::string& message)
{
    std::ofstream out(dir / "processing_reentrancy_result.txt",
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open result file for writing");
    }

    out << (success ? "ok" : "fail") << '\n';
    if (!success) {
        out << message << '\n';
    }
}

void cleanup_directory_with_retries(const std::filesystem::path& dir)
{
    constexpr int kMaxAttempts = 3;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int worker_process()
{
    using namespace sintra;

    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        g_worker_should_exit = false;
    }

    activate_slot([](const Trigger&) {
        bool barrier_ok = true;
        try {
            if (!barrier<processing_fence_t>("processing-reentrant")) {
                barrier_ok = false;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Worker processing barrier exception: %s\n", e.what());
            barrier_ok = false;
        }

        world() << barrier_ok;

        {
            std::lock_guard<std::mutex> lock(g_worker_mutex);
            g_worker_should_exit = true;
        }
        g_worker_condition.notify_all();
    });

    barrier("processing-ready");

    {
        std::unique_lock<std::mutex> lock(g_worker_mutex);
        g_worker_condition.wait(lock, [] {
            return g_worker_should_exit;
        });
    }

    barrier("processing-finish", "_sintra_all_processes");
    return 0;
}

int coordinator_process()
{
    using namespace sintra;

    const auto shared_dir = get_shared_directory();

    bool success = true;
    std::string failure_reason;

    {
        std::lock_guard<std::mutex> lock(g_coordinator_mutex);
        g_acknowledged = false;
        g_worker_barrier_ok = false;
    }

    activate_slot([](bool worker_ok) {
        std::lock_guard<std::mutex> lock(g_coordinator_mutex);
        g_acknowledged = true;
        g_worker_barrier_ok = worker_ok;
        g_coordinator_condition.notify_all();
    });

    if (success) {
        try {
            barrier("processing-ready");
        } catch (const std::exception& e) {
            success = false;
            failure_reason = std::string("Coordinator ready barrier exception: ") + e.what();
        }
    }

    bool coordinator_barrier_ok = true;

    if (success) {
        world() << Trigger{};

        try {
            if (!barrier<processing_fence_t>("processing-reentrant")) {
                coordinator_barrier_ok = false;
            }
        } catch (const std::exception& e) {
            success = false;
            failure_reason = std::string("Coordinator processing barrier exception: ") + e.what();
        }
    }

    if (success) {
        std::unique_lock<std::mutex> lock(g_coordinator_mutex);
        if (!g_coordinator_condition.wait_for(lock, std::chrono::seconds(5), [] {
                return g_acknowledged;
            }))
        {
            success = false;
            failure_reason = "Timed out waiting for worker acknowledgement";
            std::fprintf(stderr, "%s\n", failure_reason.c_str());
        }
        else if (!g_worker_barrier_ok) {
            success = false;
            failure_reason = "Worker reported processing barrier failure";
        }
        else if (!coordinator_barrier_ok) {
            success = false;
            failure_reason = "Coordinator observed processing barrier failure";
        }
    }

    barrier("processing-finish", "_sintra_all_processes");

    write_result(shared_dir, success, failure_reason);
    return success ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("processing-finish", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "processing_reentrancy_result.txt";
        if (!std::filesystem::exists(result_path)) {
            std::fprintf(stderr, "Result file not found at %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Failed to open result file %s\n", result_path.string().c_str());
            cleanup_directory_with_retries(shared_dir);
            return 1;
        }

        std::string status;
        std::getline(in, status);
        std::string message;
        std::getline(in, message);

        cleanup_directory_with_retries(shared_dir);

        if (status != "ok") {
            std::fprintf(stderr, "Processing reentrancy test reported failure: %s\n", message.c_str());
            return 1;
        }
    }

    return 0;
}
