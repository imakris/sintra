#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto k_ready_timeout = std::chrono::seconds(10);
constexpr auto k_event_timeout = std::chrono::seconds(10);
constexpr auto k_poll_interval = std::chrono::milliseconds(50);

std::mutex g_events_mutex;
std::condition_variable g_events_cv;
std::vector<sintra::process_lifecycle_event> g_events;

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

void lifecycle_handler(const sintra::process_lifecycle_event& event)
{
    std::lock_guard<std::mutex> lock(g_events_mutex);
    g_events.push_back(event);
    g_events_cv.notify_all();
}

int leave_worker()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "leave_lifecycle");
    const auto shared_dir = shared.path();
    const auto ready_path = shared_dir / "worker_ready.txt";
    const auto signal_path = shared_dir / "leave_signal.txt";
    const auto result_path = shared_dir / "worker_result.txt";

    write_text(ready_path, std::to_string(static_cast<unsigned long long>(sintra::process_of(sintra::s_mproc_id))));

    if (!sintra::test::wait_for_file(signal_path, k_ready_timeout, k_poll_interval)) {
        write_text(result_path, "timed_out_waiting_for_signal");
        return 1;
    }

    try {
        const bool leave_result = sintra::leave();
        const auto state_after =
            sintra::detail::s_shutdown_state.load(std::memory_order_acquire);
        if (leave_result &&
            state_after == sintra::detail::shutdown_protocol_state::idle) {
            write_text(result_path, "ok");
            return 0;
        }

        write_text(result_path, "leave_failed");
        return 1;
    }
    catch (const std::exception& e) {
        write_text(result_path, std::string("exception:") + e.what());
        return 1;
    }
}

int coordinator_action()
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "leave_lifecycle");
    const auto shared_dir = shared.path();
    const auto ready_path = shared_dir / "worker_ready.txt";
    const auto signal_path = shared_dir / "leave_signal.txt";
    const auto result_path = shared_dir / "worker_result.txt";

    sintra::set_lifecycle_handler(lifecycle_handler);

    const auto ready_deadline = std::chrono::steady_clock::now() + k_ready_timeout;
    sintra::instance_id_type worker_process_iid = sintra::invalid_instance_id;
    while (std::chrono::steady_clock::now() < ready_deadline) {
        const auto raw = read_text(ready_path);
        if (!raw.empty()) {
            worker_process_iid =
                static_cast<sintra::instance_id_type>(std::stoull(raw));
            break;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }

    if (worker_process_iid == sintra::invalid_instance_id) {
        std::fprintf(stderr, "[leave_lifecycle] worker did not become ready\n");
        return 1;
    }

    write_text(signal_path, "leave");

    if (!sintra::test::wait_for_file(result_path, k_ready_timeout, k_poll_interval)) {
        std::fprintf(stderr, "[leave_lifecycle] worker result missing\n");
        return 1;
    }

    const auto worker_result = read_text(result_path);
    if (worker_result != "ok") {
        std::fprintf(stderr, "[leave_lifecycle] worker leave() failed: %s\n",
                     worker_result.c_str());
        return 1;
    }

    std::unique_lock<std::mutex> lock(g_events_mutex);
    const auto deadline = std::chrono::steady_clock::now() + k_event_timeout;
    const bool got_normal_exit = g_events_cv.wait_until(lock, deadline, [&] {
        for (const auto& event : g_events) {
            if (event.process_iid == worker_process_iid &&
                event.why == sintra::process_lifecycle_event::reason::normal_exit) {
                return true;
            }
        }
        return false;
    });

    if (!got_normal_exit) {
        std::fprintf(stderr, "[leave_lifecycle] normal_exit event missing\n");
        return 1;
    }

    for (const auto& event : g_events) {
        if (event.process_iid != worker_process_iid) {
            continue;
        }
        if (event.why != sintra::process_lifecycle_event::reason::normal_exit) {
            std::fprintf(stderr, "[leave_lifecycle] unexpected lifecycle reason %d\n",
                         static_cast<int>(event.why));
            return 1;
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    return sintra::test::run_multi_process_test_raw(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "leave_lifecycle",
        {leave_worker},
        [](const std::filesystem::path&) {},
        [](const std::filesystem::path&) { return coordinator_action(); },
        [](const std::filesystem::path&) { return 0; });
}
