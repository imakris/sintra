#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Ping {};
struct Pong {};

constexpr std::string_view kSharedDirEnv = "SINTRA_PING_PONG_CRASH_DIR";

std::filesystem::path ensure_shared_directory()
{
    if (const char* existing = std::getenv(kSharedDirEnv.data()); existing && *existing) {
        return std::filesystem::path(existing);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_ping_pong_crash";
    std::filesystem::create_directories(base);

    auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch();
    auto unique_value = std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp).count();
#ifdef _WIN32
    unique_value ^= static_cast<long long>(_getpid());
#else
    unique_value ^= static_cast<long long>(getpid());
#endif

    auto dir = base / std::to_string(unique_value);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

#ifdef _WIN32
    _putenv_s(kSharedDirEnv.data(), dir.string().c_str());
#else
    setenv(kSharedDirEnv.data(), dir.string().c_str(), 1);
#endif

    return dir;
}

std::filesystem::path shared_directory()
{
    return ensure_shared_directory();
}

void write_ready_marker(const std::filesystem::path& directory)
{
    std::ofstream ready(directory / "worker_ready", std::ios::binary | std::ios::trunc);
    ready << "ready\n";
    ready.flush();
}

bool wait_for_ready_marker(const std::filesystem::path& directory)
{
    const auto marker = directory / "worker_ready";
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (std::filesystem::exists(marker)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

[[noreturn]] void keep_worker_alive()
{
    using namespace std::chrono_literals;
    for (;;) {
        std::this_thread::sleep_for(1s);
    }
}

int worker_process()
{
    const auto directory = shared_directory();

    sintra::activate_slot([](Ping) {
        sintra::world() << Pong{};
    });

    write_ready_marker(directory);

    // Keep the worker process alive so gdb can capture its stacks.
    keep_worker_alive();
}

bool is_spawned_process(int argc, char* argv[])
{
    return std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });
}

} // namespace

int main(int argc, char* argv[])
{
    const auto directory = shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);

    if (is_spawned_process(argc, argv)) {
        keep_worker_alive();
    }

    if (!wait_for_ready_marker(directory)) {
        std::cerr << "Coordinator timed out waiting for worker readiness" << std::endl;
        sintra::finalize();
        return 1;
    }

    std::atomic<bool> seen_pong{false};

    sintra::activate_slot([&](Pong) {
        seen_pong.store(true, std::memory_order_release);
        std::cerr << "Coordinator intentionally aborting after receiving Pong" << std::endl;
        std::abort();
    });

    sintra::world() << Ping{};

    // If the abort does not trigger for some reason, sleep briefly before
    // finalizing to avoid tight loops.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sintra::finalize();

    (void)seen_pong.load(std::memory_order_acquire);
    return 1;
}
