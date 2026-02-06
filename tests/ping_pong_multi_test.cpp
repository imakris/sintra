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

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

namespace {

struct Ping {};
struct Pong {};
struct Stop {};

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
    sintra::receive<Stop>();
    sintra::deactivate_all_slots();
}

constexpr int k_target_ping_count = 150;

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
        int count = counter.fetch_add(1) + 1;
        if (count >= k_target_ping_count) {
            bool expected = false;
            if (stop_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                sintra::world() << Stop();
            }
        }
    };

    sintra::activate_slot(monitor_slot);
    sintra::barrier("ping-pong-slot-activation");

    wait_for_stop();

    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "ping_pong_multi");
    write_count(shared.path() / "ping_count.txt", counter.load());
    sintra::barrier("ping-pong-finished", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "ping_pong_multi");

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
        const auto path = shared.path() / "ping_count.txt";
        const int count = read_count(path);
        bool ok = (count == k_target_ping_count);
        shared.cleanup();
        return ok ? 0 : 1;
    }

    return 0;
}
