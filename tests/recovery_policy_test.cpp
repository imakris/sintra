#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_env_ready_timeout_ms = "SINTRA_RECOVERY_POLICY_READY_TIMEOUT_MS";
constexpr std::string_view k_env_crash_timeout_ms = "SINTRA_RECOVERY_POLICY_CRASH_TIMEOUT_MS";
constexpr std::string_view k_env_watchdog_timeout_ms = "SINTRA_RECOVERY_POLICY_WATCHDOG_MS";

void write_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "ok";
}

[[noreturn]] void watchdog_exit(const char* message)
{
    std::fprintf(stderr, "recovery_policy_test watchdog: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

int crash_worker()
{
    const sintra::test::Shared_directory shared("SINTRA_RECOVERY_POLICY_DIR", "recovery_policy");
    const auto& dir = shared.path();
    const auto occurrence = sintra::s_recovery_occurrence;
    const auto ready_path = dir / ("ready_" + std::to_string(occurrence) + ".txt");
    const auto go_path = dir / "crash_go.txt";

    sintra::enable_recovery();
    write_marker(ready_path);

    if (occurrence > 0) {
        write_marker(dir / "respawned.txt");
        return 0;
    }

    if (!sintra::test::wait_for_file(go_path, 10s, 5ms)) {
        return 1;
    }

    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash();
    std::abort();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_RECOVERY_POLICY_DIR", "recovery_policy");
    const auto& dir = shared.path();

    const auto ready_timeout_ms =
        sintra::test::read_env_int(k_env_ready_timeout_ms.data(), 30000);
    const auto crash_timeout_ms =
        sintra::test::read_env_int(k_env_crash_timeout_ms.data(), 30000);
    const auto watchdog_timeout_ms =
        sintra::test::read_env_int(k_env_watchdog_timeout_ms.data(), 60000);

    const auto ready_path = dir / "ready_0.txt";
    const auto respawned_path = dir / "respawned.txt";
    const auto crash_seen_path = dir / "crash_seen.txt";
    const auto go_path = dir / "crash_go.txt";

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(watchdog_timeout_ms);
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit("coordinator timeout");
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    sintra::init(argc, argv, crash_worker);

    if (is_spawned) {
        sintra::detail::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 0;
    }

    sintra::set_recovery_policy([](const sintra::Crash_info&) {
        return false;
    });

    sintra::set_lifecycle_handler([&](const sintra::process_lifecycle_event& event) {
        if (event.why == sintra::process_lifecycle_event::reason::crash) {
            write_marker(crash_seen_path);
        }
    });

    if (!sintra::test::wait_for_file(ready_path, std::chrono::milliseconds(ready_timeout_ms), 5ms)) {
        sintra::detail::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 1;
    }

    write_marker(go_path);

    if (!sintra::test::wait_for_file(crash_seen_path, std::chrono::milliseconds(crash_timeout_ms), 5ms)) {
        sintra::detail::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 1;
    }

    std::this_thread::sleep_for(500ms);

    const bool respawned =
        std::filesystem::exists(respawned_path) ||
        std::filesystem::exists(dir / "ready_1.txt");

    sintra::detail::finalize();

    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    return respawned ? 1 : 0;
}
