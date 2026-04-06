#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::mutex g_seen_markers_mutex;
std::vector<int> g_seen_markers;
std::atomic<int> g_started_handlers{0};
std::atomic<int> g_finished_handlers{0};

int worker_process(int worker_index)
{
    sintra::barrier("shutdown-helper-ready", "_sintra_all_processes");
    sintra::world() << worker_index;
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

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_shutdown_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "shutdown_helper",
        {worker0_process, worker1_process},
        [](const std::filesystem::path&) {
            std::lock_guard<std::mutex> lock(g_seen_markers_mutex);
            g_seen_markers.clear();
            g_started_handlers.store(0, std::memory_order_release);
            g_finished_handlers.store(0, std::memory_order_release);
            sintra::activate_slot([](int worker_index) {
                g_started_handlers.fetch_add(1, std::memory_order_acq_rel);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::lock_guard<std::mutex> lock(g_seen_markers_mutex);
                g_seen_markers.push_back(worker_index);
                g_finished_handlers.fetch_add(1, std::memory_order_acq_rel);
            });
            sintra::barrier("shutdown-helper-ready", "_sintra_all_processes");
            return 0;
        },
        [](const std::filesystem::path&) {
            std::vector<int> seen;
            {
                std::lock_guard<std::mutex> lock(g_seen_markers_mutex);
                seen = g_seen_markers;
            }

            std::sort(seen.begin(), seen.end());
            const std::vector<int> expected{0, 1};
            const bool handlers_started =
                g_started_handlers.load(std::memory_order_acquire) == 2;
            const bool handlers_finished =
                g_finished_handlers.load(std::memory_order_acquire) == 2;
            return (seen == expected && handlers_started && handlers_finished) ? 0 : 1;
        });
}
