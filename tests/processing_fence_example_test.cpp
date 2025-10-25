//
// Sintra Processing Fence Example Test
//
// Validates the behaviour showcased in example 6. A coordinator publishes a
// work item to a worker and both processes wait on a processing fence barrier.
// The test verifies that the worker's handler completes before the barrier
// returns for the coordinator by observing a flag written to a shared directory.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Work_item
{
    std::uint32_t value;
};

constexpr std::string_view kEnvSharedDir   = "SINTRA_EXAMPLE6_TEST_DIR";
constexpr std::string_view kReadyBarrier   = "example-6-ready";
constexpr std::string_view kFenceBarrier   = "example-6-processing";
constexpr std::string_view kFinishedBarrier = "example-6-finished";
constexpr std::string_view kFinalBarrier   = "example-6-final";

std::filesystem::path ensure_shared_directory()
{
    if (const char* env = std::getenv(kEnvSharedDir.data()); env && *env) {
        std::filesystem::path dir(env);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_example6_test";
    std::filesystem::create_directories(base);

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::high_resolution_clock::now().time_since_epoch())
                         .count();

#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    const auto unique = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "run_" << now << '_' << pid << '_' << unique;

    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);

#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif

    return dir;
}

void cleanup_directory(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

std::filesystem::path shared_directory()
{
    const char* env = std::getenv(kEnvSharedDir.data());
    if (!env || !*env) {
        throw std::runtime_error("shared directory environment variable not set");
    }
    return std::filesystem::path(env);
}

int coordinator_process()
{
    using namespace sintra;

    const auto dir = shared_directory();
    const auto marker_path = dir / "worker_done.txt";

    barrier(kReadyBarrier.data());

    world() << Work_item{42};

    const bool barrier_result = barrier<processing_fence_t>(kFenceBarrier.data());
    const bool handler_done = std::filesystem::exists(marker_path);

    if (!barrier_result || !handler_done) {
        std::ostringstream oss;
        oss << "Processing fence failed: result=" << barrier_result
            << ", handler_done=" << handler_done;
        throw std::runtime_error(oss.str());
    }

    barrier(kFinishedBarrier.data());
    return 0;
}

int worker_process()
{
    using namespace sintra;

    const auto dir = shared_directory();
    const auto marker_path = dir / "worker_done.txt";

    std::atomic<bool> handler_completed{false};
    std::mutex write_mutex;

    activate_slot([&](const Work_item& item) {
        (void)item;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            std::lock_guard<std::mutex> lock(write_mutex);
            std::ofstream out(marker_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("failed to open worker_done.txt");
            }
            out << "done\n";
        }
        handler_completed.store(true, std::memory_order_release);
    });

    barrier(kReadyBarrier.data());

    const bool barrier_result = barrier<processing_fence_t>(kFenceBarrier.data());
    const bool completed = handler_completed.load(std::memory_order_acquire);

    if (!barrier_result || !completed) {
        std::ostringstream oss;
        oss << "Worker barrier_result=" << barrier_result
            << ", handler_completed=" << completed;
        throw std::runtime_error(oss.str());
    }

    barrier(kFinishedBarrier.data());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto dir = ensure_shared_directory();

    sintra::init(argc, argv, coordinator_process, worker_process);

    sintra::barrier(kFinalBarrier.data(), "_sintra_all_processes");

    sintra::finalize();

    if (sintra::process_index() == 0) {
        cleanup_directory(dir);
    }

    return 0;
}
