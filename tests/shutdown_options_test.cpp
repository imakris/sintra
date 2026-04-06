// Tests for shutdown(shutdown_options) with coordinator_shutdown_hook.
//
// Verifies:
// 1. The coordinator hook runs after the collective processing fence.
// 2. Workers wait for the hook to finish before teardown.
// 3. The overall protocol completes cleanly.
// 4. Post-shutdown verification can see the hook's side effect.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_data_mutex;
std::vector<int> g_collected_values;

int worker_process(int value)
{
    // Signal readiness, then send our value.
    sintra::barrier("hook-test-ready", "_sintra_all_processes");
    sintra::world() << value;
    return 0;
}

int worker0_process() { return worker_process(10); }
int worker1_process() { return worker_process(20); }

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    std::filesystem::path shared_dir;
    bool hook_ran = false;

    return sintra::test::run_multi_process_shutdown_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "shutdown_options",
        {worker0_process, worker1_process},
        sintra::shutdown_options{
            .coordinator_shutdown_hook = [&] {
                // This hook runs on the coordinator after the processing fence,
                // before raw teardown.  Workers wait at the internal hook-done
                // barrier.  Write the collected values to a summary file.
                std::lock_guard<std::mutex> lock(g_data_mutex);

                const auto summary_path = shared_dir / "summary.txt";
                sintra::test::write_lines(summary_path, {
                    "values=" + std::to_string(g_collected_values.size()),
                    "hook_ran=1"
                });
                hook_ran = true;
            }
        },
        [](const std::filesystem::path&) {
            // no-op setup
        },
        [&](const std::filesystem::path& dir) {
            shared_dir = dir;
            g_collected_values.clear();
            hook_ran = false;

            sintra::activate_slot([](int value) {
                std::lock_guard<std::mutex> lock(g_data_mutex);
                g_collected_values.push_back(value);
            });
            sintra::barrier("hook-test-ready", "_sintra_all_processes");
            return 0;
        },
        [&](const std::filesystem::path& dir) {
            // Verify the hook ran and produced the expected output.
            const auto summary_path = dir / "summary.txt";
            if (!std::filesystem::exists(summary_path)) {
                sintra::test::print_test_message(
                    "shutdown_options_test: ", "summary file not found");
                return 1;
            }

            const auto lines = sintra::test::read_lines(summary_path);
            bool values_ok = false;
            bool hook_ok = false;
            for (const auto& line : lines) {
                if (line == "values=2") values_ok = true;
                if (line == "hook_ran=1") hook_ok = true;
            }

            if (!values_ok) {
                sintra::test::print_test_message(
                    "shutdown_options_test: ",
                    "expected values=2 in summary");
                return 1;
            }
            if (!hook_ok) {
                sintra::test::print_test_message(
                    "shutdown_options_test: ",
                    "expected hook_ran=1 in summary");
                return 1;
            }

            return 0;
        });
}
