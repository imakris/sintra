#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_env_ready_timeout_ms = "SINTRA_RECOVERY_READY_TIMEOUT_MS";
constexpr std::string_view k_env_runner_timeout_ms = "SINTRA_RECOVERY_RUNNER_TIMEOUT_MS";
constexpr std::string_view k_env_go_timeout_ms = "SINTRA_RECOVERY_GO_TIMEOUT_MS";
constexpr std::string_view k_env_watchdog_timeout_ms = "SINTRA_RECOVERY_WATCHDOG_MS";

void disable_abort_dialog()
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

void write_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "ok";
}

bool wait_for_file(const std::filesystem::path& path, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::filesystem::exists(path);
}

[[noreturn]] void watchdog_exit(const char* message)
{
    std::fprintf(stderr, "recovery_runner_thread_test watchdog: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

int crash_worker()
{
    const sintra::test::Shared_directory shared("SINTRA_RECOVERY_THREAD_DIR", "recovery_runner_thread");
    const auto& dir = shared.path();
    const auto ready_path = dir / "crash_ready.txt";
    const auto go_path = dir / "crash_go.txt";
    const auto go_timeout_ms =
        sintra::test::read_env_int(k_env_go_timeout_ms.data(), 30000);

    sintra::enable_recovery();
    write_marker(ready_path);

    if (!wait_for_file(go_path, std::chrono::milliseconds(go_timeout_ms))) {
        return 1;
    }

    sintra::disable_debug_pause_for_current_process();
    disable_abort_dialog();
    std::abort();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_RECOVERY_THREAD_DIR", "recovery_runner_thread");
    const auto& dir = shared.path();

    const auto ready_timeout_ms =
        sintra::test::read_env_int(k_env_ready_timeout_ms.data(), 30000);
    const auto runner_timeout_ms =
        sintra::test::read_env_int(k_env_runner_timeout_ms.data(), 30000);
    const auto watchdog_timeout_ms =
        sintra::test::read_env_int(k_env_watchdog_timeout_ms.data(), 60000);

    const auto ready_path = dir / "crash_ready.txt";
    const auto go_path = dir / "crash_go.txt";

    std::atomic<bool> runner_seen{false};
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
        sintra::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 0;
    }

    sintra::set_recovery_runner([&](const sintra::Crash_info&, const sintra::Recovery_control&) {
        runner_seen.store(true, std::memory_order_release);
    });

    if (!wait_for_file(ready_path, std::chrono::milliseconds(ready_timeout_ms))) {
        sintra::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 1;
    }

    write_marker(go_path);

    const auto runner_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(runner_timeout_ms);
    while (!runner_seen.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= runner_deadline) {
            sintra::finalize();
            watchdog_done.store(true, std::memory_order_release);
            watchdog.join();
            return 1;
        }
        std::this_thread::sleep_for(10ms);
    }

    sintra::finalize();

    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    return 0;
}
