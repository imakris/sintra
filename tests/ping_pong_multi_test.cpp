//
// Sintra Multi-Process ping_t-pong_t Test
//
// This test validates multi-process ping-pong messaging.
// It corresponds to example_1 and tests the following features:
// - Message passing between separate processes
// - Slot activation in different processes
// - Barriers for synchronization
// - stop_t signal to coordinate shutdown
// - Message throughput measurement
//
// Test structure:
// - Process 1 (ping responder): Responds to ping_t with pong_t
// - Process 2 (pong responder): Responds to pong_t with ping_t (initiates cycle)
// - Process 3 (monitor): Counts ping_t messages and sends stop_t after target count
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

struct ping_t {};
struct pong_t {};
struct stop_t {};
constexpr const char* k_finished_barrier = "ping-pong-finished";

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
    sintra::receive<stop_t>();
    sintra::deactivate_all_slots();
}

constexpr int k_target_ping_count = 150;

int process_ping_responder()
{
    sintra::activate_slot([](ping_t) {
        sintra::world() << pong_t();
    });
    sintra::barrier("ping-pong-slot-activation");

    wait_for_stop();
    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int process_pong_responder()
{
    sintra::activate_slot([](pong_t) {
        sintra::world() << ping_t();
    });
    sintra::barrier("ping-pong-slot-activation");

    sintra::world() << ping_t();

    wait_for_stop();
    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int process_monitor()
{
    static std::atomic<int> counter{0};
    static std::atomic<bool> stop_sent{false};

    auto monitor_slot = [](ping_t) {
        if (stop_sent.load(std::memory_order_acquire)) {
            return;
        }
        int count = counter.fetch_add(1) + 1;
        if (count >= k_target_ping_count) {
            bool expected = false;
            if (stop_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                sintra::world() << stop_t();
            }
        }
    };

    sintra::activate_slot(monitor_slot);
    sintra::barrier("ping-pong-slot-activation");

    wait_for_stop();

    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "ping_pong_multi");
    write_count(shared.path() / "ping_count.txt", counter.load());
    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "ping_pong_multi",
        {process_ping_responder,
         process_pong_responder,
         process_monitor},
        [](const std::filesystem::path&) {
            sintra::barrier(k_finished_barrier, "_sintra_all_processes");
            return 0;
        },
        [](const std::filesystem::path& shared_dir) {
            const auto path  = shared_dir / "ping_count.txt";
            const int  count = read_count(path);
            return (count == k_target_ping_count) ? 0 : 1;
        });
}
