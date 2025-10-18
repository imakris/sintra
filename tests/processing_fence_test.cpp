#include <sintra/sintra.h>

#include "test_watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct Work_message
{
};

constexpr auto kHandlerDelay = std::chrono::milliseconds(300);
constexpr std::string_view kEnvSharedDir = "SINTRA_PROCESSING_FENCE_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("processing fence test shared directory is not set");
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

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        std::filesystem::path dir(value);
        if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
            return dir;
        }
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_processing_fence";
    std::filesystem::create_directories(base);

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();

#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    const auto unique = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "run_" << now << '_' << pid << '_' << unique;

    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

void cleanup_directory(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

int controller_process()
{
    using namespace sintra;

    const auto shared_dir = get_shared_directory();
    const auto flag_path = shared_dir / "handler_done.txt";
    const auto result_path = shared_dir / "result.txt";

    const std::string group = "_sintra_external_processes";
    barrier("processing-fence-setup", group);

    world() << Work_message{};

    const auto start = std::chrono::steady_clock::now();
    const bool barrier_result = barrier<processing_fence_t>(
        "processing-fence", group);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    const bool handler_done = std::filesystem::exists(flag_path);

    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    out << (barrier_result && handler_done ? "ok" : "fail") << '\n';
    out << elapsed.count() << '\n';
    out << (handler_done ? "done" : "pending") << '\n';

    barrier("processing-fence-test-done", "_sintra_all_processes");

    return (barrier_result && handler_done) ? 0 : 1;
}

int worker_process()
{
    using namespace sintra;

    const auto shared_dir = get_shared_directory();
    const auto flag_path = shared_dir / "handler_done.txt";

    auto slot = [flag_path](const Work_message&) {
        std::this_thread::sleep_for(kHandlerDelay);
        std::ofstream out(flag_path, std::ios::binary | std::ios::trunc);
        out << "done";
    };
    activate_slot(slot);

    const std::string group = "_sintra_external_processes";
    barrier("processing-fence-setup", group);
    barrier<processing_fence_t>("processing-fence", group);
    barrier("processing-fence-test-done", "_sintra_all_processes");

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    install_test_watchdog("processing_fence");
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();
    const auto flag_path = shared_dir / "handler_done.txt";
    const auto result_path = shared_dir / "result.txt";

    if (!is_spawned) {
        std::filesystem::remove(flag_path);
        std::filesystem::remove(result_path);
    }

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(controller_process);
    processes.emplace_back(worker_process);

    sintra::init(argc, argv, processes);
    if (!is_spawned) {
        sintra::barrier("processing-fence-test-done", "_sintra_all_processes");
    }
    sintra::finalize();

    if (!is_spawned) {
        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            cleanup_directory(shared_dir);
            return 1;
        }

        std::string status;
        long long elapsed_ms = 0;
        std::string done_state;
        std::getline(in, status);
        in >> elapsed_ms;
        std::getline(in >> std::ws, done_state);

        const long long expected_ms = kHandlerDelay.count();
        const bool elapsed_ok = elapsed_ms >= expected_ms;
        const bool done_ok = (done_state == "done");
        const bool success = (status == "ok") && elapsed_ok && done_ok;

        cleanup_directory(shared_dir);
        return success ? 0 : 1;
    }

    return 0;
}
