#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_env_barrier_wait_ms = "SINTRA_BARRIER_DRAIN_TIMEOUT_MS";
constexpr std::string_view k_env_worker_timeout_ms = "SINTRA_BARRIER_WORKER_TIMEOUT_MS";
constexpr std::string_view k_env_coord_watchdog_ms = "SINTRA_BARRIER_COORDINATOR_WATCHDOG_MS";

void write_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "ok";
}

void write_result(const std::filesystem::path& path, const std::string& result)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << result;
}

[[noreturn]] void watchdog_exit(const char* message)
{
    std::fprintf(stderr, "barrier_drain_and_unpublish_test watchdog: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

int worker_a()
{
    sintra::test::Shared_directory shared("SINTRA_BARRIER_DRAIN_DIR", "barrier_drain_unpublish");
    const auto dir = shared.path();
    const auto barrier1_ready = dir / "barrier1_ready.txt";
    const auto barrier2_ready = dir / "barrier2_ready.txt";
    const auto result_path = dir / "worker_a_result.txt";

    write_marker(barrier1_ready);

    bool ok1 = false;
    try {
        auto seq = sintra::barrier("external_barrier", "_sintra_external_processes");
        ok1 = sintra::barrier_completed(seq);
    }
    catch (...) {
        ok1 = false;
    }

    write_marker(barrier2_ready);

    std::atomic<bool> barrier2_done{false};
    std::thread watchdog([&]() {
        const auto timeout_ms =
            sintra::test::read_env_int(k_env_worker_timeout_ms.data(), 40000);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!barrier2_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit("worker A barrier2 timeout");
            }
            std::this_thread::sleep_for(10ms);
        }
    });

    bool ok2 = false;
    try {
        auto seq = sintra::barrier("coord_barrier", "_sintra_all_processes");
        ok2 = sintra::barrier_completed(seq);
    }
    catch (...) {
        ok2 = false;
    }

    barrier2_done.store(true, std::memory_order_release);
    watchdog.join();

    if (!ok1) {
        write_result(result_path, "fail: external_barrier returned false");
        return 1;
    }

    if (!ok2) {
        write_result(result_path, "fail: coord_barrier returned false");
        return 1;
    }

    write_result(result_path, "ok");
    return 0;
}

int worker_b()
{
    sintra::test::Shared_directory shared("SINTRA_BARRIER_DRAIN_DIR", "barrier_drain_unpublish");
    const auto dir = shared.path();
    const auto barrier1_ready = dir / "barrier1_ready.txt";

    if (!sintra::test::wait_for_file(barrier1_ready, 5s, 5ms)) {
        return 1;
    }

    std::this_thread::sleep_for(50ms);
    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash();
    std::abort();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_BARRIER_DRAIN_DIR", "barrier_drain_unpublish");
    const auto dir = shared.path();

    const auto barrier2_timeout_ms =
        sintra::test::read_env_int(k_env_barrier_wait_ms.data(), 40000);
    const auto coordinator_watchdog_ms =
        sintra::test::read_env_int(k_env_coord_watchdog_ms.data(), 60000);

    const auto barrier2_ready = dir / "barrier2_ready.txt";
    const auto result_path = dir / "worker_a_result.txt";

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(coordinator_watchdog_ms);
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit("coordinator timeout");
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    sintra::init(argc, argv, worker_a, worker_b);

    if (is_spawned) {
        sintra::detail::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 0;
    }

    if (!sintra::test::wait_for_file(barrier2_ready, std::chrono::milliseconds(barrier2_timeout_ms))) {
        sintra::detail::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 1;
    }

    sintra::detail::finalize();

    std::ifstream result_in(result_path, std::ios::binary);
    std::string result;
    if (result_in) {
        std::getline(result_in, result);
    }
    const bool ok = (result == "ok");
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    return ok ? 0 : 1;
}
