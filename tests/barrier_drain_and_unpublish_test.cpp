#include <sintra/sintra.h>

#include "test_environment.h"

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

constexpr std::string_view k_env_shared_dir = "SINTRA_BARRIER_DRAIN_DIR";

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

void disable_abort_dialog()
{
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

std::filesystem::path shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (!value || !*value) {
        throw std::runtime_error("SINTRA_BARRIER_DRAIN_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(k_env_shared_dir.data(), dir.string().c_str());
#else
    setenv(k_env_shared_dir.data(), dir.string().c_str(), 1);
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
    std::fprintf(stderr, "barrier_drain_and_unpublish_test watchdog: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

int worker_a()
{
    const auto dir = shared_directory();
    const auto barrier1_ready = dir / "barrier1_ready.txt";
    const auto barrier2_ready = dir / "barrier2_ready.txt";
    const auto done_path = dir / "worker_a_done.txt";

    write_marker(barrier1_ready);

    bool ok1 = false;
    try {
        auto seq = sintra::barrier("external_barrier", "_sintra_external_processes");
        ok1 = (seq != 0);
    }
    catch (...) {
        ok1 = false;
    }

    write_marker(barrier2_ready);

    std::atomic<bool> barrier2_done{false};
    std::thread watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
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
        ok2 = (seq != 0);
    }
    catch (...) {
        ok2 = false;
    }

    barrier2_done.store(true, std::memory_order_release);
    watchdog.join();

    write_marker(done_path);
    return (ok1 && ok2) ? 0 : 1;
}

int worker_b()
{
    const auto dir = shared_directory();
    const auto barrier1_ready = dir / "barrier1_ready.txt";

    if (!wait_for_file(barrier1_ready, 5s)) {
        return 1;
    }

    std::this_thread::sleep_for(50ms);
    sintra::disable_debug_pause_for_current_process();
    disable_abort_dialog();
    std::abort();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = has_branch_flag(argc, argv);

    const auto dir = sintra::test::unique_scratch_directory("barrier_drain_unpublish");
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);

    const auto barrier2_timeout_ms =
        sintra::test::read_env_int("SINTRA_BARRIER_DRAIN_TIMEOUT_MS", 25000);

    const auto barrier2_ready = dir / "barrier2_ready.txt";
    const auto done_path = dir / "worker_a_done.txt";

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_exit("coordinator timeout");
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    sintra::init(argc, argv, worker_a, worker_b);

    if (is_spawned) {
        sintra::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 0;
    }

    if (!wait_for_file(barrier2_ready, std::chrono::milliseconds(barrier2_timeout_ms))) {
        sintra::finalize();
        watchdog_done.store(true, std::memory_order_release);
        watchdog.join();
        return 1;
    }

    sintra::finalize();

    const bool ok = std::filesystem::exists(done_path);
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    return ok ? 0 : 1;
}
