// Tests that shutdown() and shutdown(options) share the same fixed collective
// phases, even when participants use different entry points or hook presence.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_data_mutex;
std::vector<int> g_collected_values;

int worker_process(int value)
{
    sintra::barrier("shutdown-mixed-ready", "_sintra_all_processes");
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
    const std::string branch_index_arg =
        sintra::test::get_argv_value(argc, argv, "--branch_index");

    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "shutdown_options_mixed_modes");

    sintra::init(argc, argv, {worker0_process, worker1_process});

    if (!is_spawned) {
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_collected_values.clear();
        }
        sintra::activate_slot([](int value) {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_collected_values.push_back(value);
        });
        sintra::barrier("shutdown-mixed-ready", "_sintra_all_processes");
    }

    if (!is_spawned) {
        sintra::shutdown(sintra::shutdown_options{
            .coordinator_shutdown_hook = [&] {
                sintra::test::write_lines(
                    shared.path() / "mixed_shutdown_marker.txt",
                    {"hook-ran"});
            }
        });
    }
    else
    if (branch_index_arg == "1") {
        sintra::shutdown();
    }
    else
    if (branch_index_arg == "2") {
        sintra::shutdown(sintra::shutdown_options{});
    }
    else {
        sintra::test::print_test_message(
            "shutdown_options_mixed_modes_test: ",
            std::string("unexpected branch_index: ") + branch_index_arg);
        return 1;
    }

    if (is_spawned) {
        return 0;
    }

    const auto marker_path = shared.path() / "mixed_shutdown_marker.txt";
    if (!std::filesystem::exists(marker_path)) {
        sintra::test::print_test_message(
            "shutdown_options_mixed_modes_test: ",
            "coordinator hook marker file was not written");
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
            "shutdown_options_mixed_modes_test: ",
            "coordinator did not observe both worker messages");
        return 1;
    }

    return 0;
}
