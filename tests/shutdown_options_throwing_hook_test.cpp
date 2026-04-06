// Tests for shutdown(shutdown_options) when the coordinator hook throws.
//
// Verifies:
// 1. The exception propagates to the coordinator caller.
// 2. Workers do not hang while waiting for the hook-done phase.
// 3. Shutdown still finalizes after the throwing hook path.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::mutex g_data_mutex;
std::vector<int> g_collected_values;

int worker_process(int value)
{
    sintra::barrier("hook-throw-ready", "_sintra_all_processes");
    sintra::world() << value;
    return 0;
}

int worker0_process() { return worker_process(10); }
int worker1_process() { return worker_process(20); }

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "shutdown_options_throwing_hook");

    sintra::init(argc, argv, {worker0_process, worker1_process});

    if (!is_spawned) {
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_collected_values.clear();
        }
        sintra::activate_slot([](int value) {
            std::lock_guard<std::mutex> slot_lock(g_data_mutex);
            g_collected_values.push_back(value);
        });
        sintra::barrier("hook-throw-ready", "_sintra_all_processes");
    }

    bool caught_expected_exception = false;

    try {
        sintra::shutdown(sintra::shutdown_options{
            .coordinator_shutdown_hook = [&] {
                sintra::test::write_lines(
                    shared.path() / "hook_throw_marker.txt",
                    {"hook-entered"});
                throw std::runtime_error("expected shutdown hook failure");
            }
        });
    }
    catch (const std::runtime_error& e) {
        if (is_spawned) {
            sintra::test::print_test_message(
                "shutdown_options_throwing_hook_test: ",
                std::string("worker unexpectedly caught exception: ") + e.what());
            return 1;
        }
        caught_expected_exception =
            std::string(e.what()) == "expected shutdown hook failure";
    }

    if (is_spawned) {
        return 0;
    }

    if (!caught_expected_exception) {
        sintra::test::print_test_message(
            "shutdown_options_throwing_hook_test: ",
            "coordinator did not receive the expected hook exception");
        return 1;
    }

    const auto marker_path = shared.path() / "hook_throw_marker.txt";
    if (!std::filesystem::exists(marker_path)) {
        sintra::test::print_test_message(
            "shutdown_options_throwing_hook_test: ",
            "hook marker file was not written");
        return 1;
    }

    std::vector<int> values;
    {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        values = g_collected_values;
    }
    std::sort(values.begin(), values.end());
    if (values != std::vector<int>{10, 20}) {
        sintra::test::print_test_message(
            "shutdown_options_throwing_hook_test: ",
            "coordinator did not observe both worker messages before hook failure");
        return 1;
    }

    return 0;
}
