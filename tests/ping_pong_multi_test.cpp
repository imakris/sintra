//
// Sintra Multi-Process Ping-Pong Test
//
// This test validates multi-process ping-pong messaging.
// It corresponds to example_1 and tests the following features:
// - Message passing between separate processes
// - Slot activation in different processes
// - Barriers for synchronization
// - Stop signal to coordinate shutdown
// - Message throughput measurement
//
// Test structure:
// - Process 1 (ping responder): Responds to Ping with Pong
// - Process 2 (pong responder): Responds to Pong with Ping (initiates cycle)
// - Process 3 (monitor): Counts Ping messages and sends Stop after target count
//
// The test verifies that 500 ping-pong exchanges occur correctly across processes.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Ping {};
struct Pong {};
struct Stop {};

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

    std::ostringstream oss;
    oss << "ping_pong_multi_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_count(const std::filesystem::path& file, int value)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << value << '\n';
}

int read_count(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return -1;
    }
    int value = -1;
    in >> value;
    return value;
}

void wait_for_stop()
{
    static std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    sintra::barrier("stop-slot-ready");

    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });

    sintra::deactivate_all_slots();
}

constexpr int kTargetPingCount = 500;

int process_ping_responder()
{
    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });
    sintra::barrier("ping-pong-slot-activation");

    wait_for_stop();
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    return 0;
}

int process_pong_responder()
{
    sintra::activate_slot([](Pong) {
        sintra::world() << Ping();
    });
    sintra::barrier("ping-pong-slot-activation");

    sintra::world() << Ping();

    wait_for_stop();
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    return 0;
}

int process_monitor()
{
    static std::atomic<int> counter{0};
    static std::atomic<bool> stop_sent{false};

    auto monitor_slot = [](Ping) {
        if (stop_sent.load(std::memory_order_acquire)) {
            return;
        }
        int count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= kTargetPingCount) {
            bool expected = false;
            if (stop_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                sintra::world() << Stop();
            }
        }
    };

    sintra::activate_slot(monitor_slot);
    sintra::barrier("ping-pong-slot-activation");

    wait_for_stop();

    const auto shared_dir = get_shared_directory();
    write_count(shared_dir / "ping_count.txt", counter.load(std::memory_order_relaxed));
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_ping_responder);
    processes.emplace_back(process_pong_responder);
    processes.emplace_back(process_monitor);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto path = shared_dir / "ping_count.txt";
        const int count = read_count(path);
        bool ok = (count == kTargetPingCount);
        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
        }
        return ok ? 0 : 1;
    }

    return 0;
}
