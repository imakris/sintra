#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Ping {};
struct Pong {};
struct Stop {};

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

void clear_shared_directory_env()
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), "");
#else
    unsetenv(kEnvSharedDir.data());
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_tests";
    std::filesystem::create_directories(base);

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "ping_pong_multi_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_count(const std::filesystem::path& file, int value)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << value << '\n';
}

int read_count(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return -1;
    }
    int value = -1;
    in >> value;
    return value;
}

std::string barrier_name(const char* base, int iteration)
{
    std::ostringstream oss;
    oss << base << '-' << iteration;
    return oss.str();
}

void wait_for_stop(int iteration)
{
    static std::mutex stop_mutex;
    std::condition_variable cv;
    bool done = false;

    sintra::activate_slot([&](Stop) {
        std::lock_guard<std::mutex> lk(stop_mutex);
        done = true;
        cv.notify_one();
    });

    sintra::barrier(barrier_name("stop-slot-ready", iteration));

    std::unique_lock<std::mutex> lk(stop_mutex);
    cv.wait(lk, [&] { return done; });

    sintra::deactivate_all_slots();
}

constexpr int kTargetPingCount = 64;
constexpr int kRepetitions = 32;

int process_ping_responder()
{
    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        sintra::activate_slot([](Ping) {
            sintra::world() << Pong();
        });
        sintra::barrier(barrier_name("ping-pong-slot-activation", iteration));

        wait_for_stop(iteration);
    }
    return 0;
}

int process_pong_responder()
{
    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        sintra::activate_slot([](Pong) {
            sintra::world() << Ping();
        });
        sintra::barrier(barrier_name("ping-pong-slot-activation", iteration));

        sintra::world() << Ping();

        wait_for_stop(iteration);
    }
    return 0;
}

int process_monitor()
{
    const auto shared_dir = get_shared_directory();

    int min_count = kTargetPingCount;

    for (int iteration = 0; iteration < kRepetitions; ++iteration) {
        std::atomic<int> counter{0};
        std::atomic<bool> stop_sent{false};

        auto monitor_slot = [&](Ping) {
            if (stop_sent.load(std::memory_order_acquire)) {
                return;
            }
            int count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count >= kTargetPingCount) {
                bool expected = false;
                if (stop_sent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    sintra::world() << Stop();
                }
            }
        };

        sintra::activate_slot(monitor_slot);
        sintra::barrier(barrier_name("ping-pong-slot-activation", iteration));

        wait_for_stop(iteration);

        int iteration_count = counter.load(std::memory_order_relaxed);
        if (iteration_count < min_count) {
            min_count = iteration_count;
        }
    }

    write_count(shared_dir / "ping_count.txt", min_count);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_ping_responder);
    processes.emplace_back(process_pong_responder);
    processes.emplace_back(process_monitor);

    sintra::init(argc, argv, processes);
    sintra::finalize();

    int result = 0;
    if (!is_spawned) {
        const auto path = shared_dir / "ping_count.txt";
        const int count = read_count(path);
        if (count != kTargetPingCount) {
            result = 1;
        }

        try {
            std::filesystem::remove_all(shared_dir);
        }
        catch (...) {
        }
    }

    clear_shared_directory_env();
    return result;
}
