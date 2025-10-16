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

#include "test_trace.h"

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

using sintra::test_trace::trace;

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
    trace("test.ping_pong_multi.wait_for_stop", [&](auto& os) { os << "event=start"; });
    static std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        trace("test.ping_pong_multi.wait_for_stop", [&](auto& os) { os << "event=stop_received"; });
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    trace("test.ping_pong_multi.wait_for_stop", [&](auto& os) { os << "event=barrier.enter name=stop-slot-ready"; });
    sintra::barrier("stop-slot-ready");
    trace("test.ping_pong_multi.wait_for_stop", [&](auto& os) { os << "event=barrier.exit name=stop-slot-ready"; });

    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });
    trace("test.ping_pong_multi.wait_for_stop", [&](auto& os) { os << "event=wait.complete"; });

    sintra::deactivate_all_slots();
}

constexpr int kTargetPingCount = 500;

int process_ping_responder()
{
    trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=start"; });
    sintra::activate_slot([](Ping) {
        trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=handle_ping action=send_pong"; });
        sintra::world() << Pong();
    });
    trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=barrier.enter name=slot-activation"; });
    sintra::barrier("ping-pong-slot-activation");
    trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=barrier.exit name=slot-activation"; });

    wait_for_stop();
    trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=barrier.enter name=finished"; });
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    trace("test.ping_pong_multi.ping", [&](auto& os) { os << "event=barrier.exit name=finished"; });
    return 0;
}

int process_pong_responder()
{
    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=start"; });
    sintra::activate_slot([](Pong) {
        trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=handle_pong action=send_ping"; });
        sintra::world() << Ping();
    });
    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=barrier.enter name=slot-activation"; });
    sintra::barrier("ping-pong-slot-activation");
    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=barrier.exit name=slot-activation"; });

    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=send_initial_ping"; });
    sintra::world() << Ping();

    wait_for_stop();
    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=barrier.enter name=finished"; });
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    trace("test.ping_pong_multi.pong", [&](auto& os) { os << "event=barrier.exit name=finished"; });
    return 0;
}

int process_monitor()
{
    static std::atomic<int> counter{0};
    static std::atomic<bool> stop_sent{false};

    trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=start"; });

    auto monitor_slot = [](Ping) {
        if (stop_sent.load(std::memory_order_acquire)) {
            return;
        }
        int count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=handle_ping count=" << count; });
        if (count >= kTargetPingCount) {
            bool expected = false;
            if (stop_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=send_stop"; });
                sintra::world() << Stop();
            }
        }
    };

    sintra::activate_slot(monitor_slot);
    trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=barrier.enter name=slot-activation"; });
    sintra::barrier("ping-pong-slot-activation");
    trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=barrier.exit name=slot-activation"; });

    wait_for_stop();

    const auto shared_dir = get_shared_directory();
    write_count(shared_dir / "ping_count.txt", counter.load(std::memory_order_relaxed));
    trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=barrier.enter name=finished"; });
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    trace("test.ping_pong_multi.monitor", [&](auto& os) { os << "event=barrier.exit name=finished"; });
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    using sintra::test_trace::trace;
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=start is_spawned=" << is_spawned; });
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_ping_responder);
    processes.emplace_back(process_pong_responder);
    processes.emplace_back(process_monitor);

    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=init.begin"; });
    sintra::init(argc, argv, processes);
    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=init.end"; });

    if (!is_spawned) {
        trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=barrier.enter name=finished"; });
        sintra::barrier("ping-pong-finished", "_sintra_all_processes");
        trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=barrier.exit name=finished"; });
    }

    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=finalize.begin"; });
    sintra::finalize();
    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=finalize.end"; });

    if (!is_spawned) {
        const auto path = shared_dir / "ping_count.txt";
        const int count = read_count(path);
        bool ok = (count == kTargetPingCount);
        trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=verify count=" << count << " expected=" << kTargetPingCount; });
        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
        }
        trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=exit success=" << ok; });
        return ok ? 0 : 1;
    }

    trace("test.ping_pong_multi.main", [&](auto& os) { os << "event=exit spawned"; });
    return 0;
}
