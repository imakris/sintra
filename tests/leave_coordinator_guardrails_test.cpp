#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

constexpr auto k_ready_timeout = std::chrono::seconds(10);
constexpr auto k_poll_interval = std::chrono::milliseconds(50);

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string value;
    std::getline(in, value);
    return value;
}

int worker_process()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "leave_coordinator_guardrails");
    const auto shared_dir = shared.path();
    const auto ready_path = shared_dir / "worker_ready.txt";
    const auto release_path = shared_dir / "worker_release.txt";
    const auto result_path = shared_dir / "worker_result.txt";

    write_text(
        ready_path,
        std::to_string(static_cast<unsigned long long>(sintra::process_of(sintra::s_mproc_id))));

    if (!sintra::test::wait_for_file(release_path, k_ready_timeout, k_poll_interval)) {
        write_text(result_path, "timed_out_waiting_for_release");
        return 1;
    }

    write_text(result_path, "ok");
    return 0;
}

int coordinator_action()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "leave_coordinator_guardrails");
    const auto shared_dir = shared.path();
    const auto ready_path = shared_dir / "worker_ready.txt";
    const auto release_path = shared_dir / "worker_release.txt";
    const auto result_path = shared_dir / "worker_result.txt";

    const auto ready_deadline = std::chrono::steady_clock::now() + k_ready_timeout;
    while (std::chrono::steady_clock::now() < ready_deadline) {
        if (!read_text(ready_path).empty()) {
            break;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }

    if (read_text(ready_path).empty()) {
        std::fprintf(stderr, "[leave_coordinator_guardrails] worker did not become ready\n");
        return 1;
    }

    bool caught = false;
    try {
        (void)sintra::leave();
    }
    catch (const std::logic_error&) {
        caught = true;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[leave_coordinator_guardrails] unexpected exception: %s\n",
                     e.what());
        return 1;
    }

    if (!caught) {
        std::fprintf(stderr,
                     "[leave_coordinator_guardrails] coordinator leave() unexpectedly succeeded\n");
        return 1;
    }

    const auto state_after =
        sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
    if (state_after != sintra::detail::shutdown_protocol_state::idle) {
        std::fprintf(stderr,
                     "[leave_coordinator_guardrails] shutdown state not reset after rejected leave()\n");
        return 1;
    }

    write_text(release_path, "exit");

    if (!sintra::test::wait_for_file(result_path, k_ready_timeout, k_poll_interval)) {
        std::fprintf(stderr, "[leave_coordinator_guardrails] worker result missing\n");
        return 1;
    }

    if (read_text(result_path) != "ok") {
        std::fprintf(stderr,
                     "[leave_coordinator_guardrails] worker did not exit cleanly: %s\n",
                     read_text(result_path).c_str());
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "leave_coordinator_guardrails",
        {worker_process},
        [](const std::filesystem::path&) { return coordinator_action(); },
        [](const std::filesystem::path&) { return 0; });
}
