#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace {

std::mutex g_seen_markers_mutex;
std::vector<int> g_seen_markers;

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
            sintra::activate_slot([](int worker_index) {
                std::lock_guard<std::mutex> lock(g_seen_markers_mutex);
                g_seen_markers.push_back(worker_index);
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
            return seen == expected ? 0 : 1;
        });
}
