#include <sintra/sintra.h>

#include "test_environment.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
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

struct Work_message {};

struct Handler_done {};

constexpr auto k_handler_delay = std::chrono::milliseconds(300);
constexpr std::string_view k_env_shared_dir = "SINTRA_PROCESSING_FENCE_DIR";
constexpr std::string_view k_shared_dir_flag = "--shared-dir";

std::filesystem::path& shared_directory_storage()
{
    static std::filesystem::path storage;
    return storage;
}

const std::filesystem::path& shared_directory()
{
    const auto& dir = shared_directory_storage();
    if (dir.empty()) {
        throw std::logic_error("shared directory has not been initialised");
    }
    return dir;
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(k_env_shared_dir.data(), dir.string().c_str());
#else
    setenv(k_env_shared_dir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(k_env_shared_dir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("processing_fence");
    std::filesystem::create_directories(base);

    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    const auto unique = counter.fetch_add(1);

    std::ostringstream oss;
    oss << "run_" << now << '_' << pid << '_' << unique;

    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> find_shared_directory_arg(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == k_shared_dir_flag) {
            if (i + 1 >= argc) {
                throw std::runtime_error("--shared-dir requires a value");
            }
            return std::filesystem::path(argv[i + 1]);
        }

        if (arg.size() > k_shared_dir_flag.size() &&
            arg.substr(0, k_shared_dir_flag.size()) == k_shared_dir_flag &&
            arg[k_shared_dir_flag.size()] == '=')
        {
            return std::filesystem::path(std::string(arg.substr(k_shared_dir_flag.size() + 1)));
        }
    }

    return std::nullopt;
}

std::filesystem::path resolve_shared_directory(int argc, char* argv[])
{
    if (auto arg_path = find_shared_directory_arg(argc, argv)) {
        std::filesystem::create_directories(*arg_path);
        set_shared_directory_env(*arg_path);
        return *arg_path;
    }

    return ensure_shared_directory();
}

void cleanup_directory(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

int controller_process()
{
    using namespace sintra;

    const auto& shared_dir = shared_directory();
    const auto result_path = shared_dir / "result.txt";

    const std::string group = "_sintra_external_processes";
    barrier("processing-fence-setup", group);

    auto handler_done_promise = std::make_shared<std::promise<void>>();
    auto handler_done_future = handler_done_promise->get_future();
    auto handler_done_recorded = std::make_shared<std::atomic<bool>>(false);

    auto handler_done_deactivator = activate_slot([
        handler_done_promise,
        handler_done_recorded
    ](const Handler_done&) {
        bool expected = false;
        if (handler_done_recorded->compare_exchange_strong(expected, true)) {
            handler_done_promise->set_value();
        }
    });

    world() << Work_message{};

    const auto start = std::chrono::steady_clock::now();
    const bool barrier_result = barrier<processing_fence_t>(
        "processing-fence", group);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // On Windows we occasionally observe a tiny delay between the barrier
    // returning and the handler completion notification becoming visible to
    // this thread. Allow a short grace period before declaring failure to make
    // the test robust against the observed scheduling jitter while still
    // validating that processing completed promptly.
    const auto handler_done_deadline =
        start + k_handler_delay + std::chrono::milliseconds(200);
    const bool handler_done = handler_done_future.wait_until(handler_done_deadline) ==
        std::future_status::ready;

    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    out << (barrier_result && handler_done ? "ok" : "fail") << '\n';
    out << elapsed.count() << '\n';
    out << (handler_done ? "done" : "pending") << '\n';

    barrier("processing-fence-test-done", "_sintra_all_processes");

    handler_done_deactivator();

    return (barrier_result && handler_done) ? 0 : 1;
}

int worker_process()
{
    using namespace sintra;

    auto slot = [](const Work_message&) {
        std::this_thread::sleep_for(k_handler_delay);
        world() << Handler_done{};
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
    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = resolve_shared_directory(argc, argv);
    shared_directory_storage() = shared_dir;
    const auto result_path = shared_dir / "result.txt";

    if (!is_spawned) {
        std::filesystem::remove(result_path);
    }

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(controller_process);
    processes.back().user_options = {std::string(k_shared_dir_flag), shared_dir.string()};
    processes.emplace_back(worker_process);
    processes.back().user_options = {std::string(k_shared_dir_flag), shared_dir.string()};

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

        const long long expected_ms = k_handler_delay.count();
        const bool elapsed_ok = elapsed_ms >= expected_ms;
        const bool done_ok = (done_state == "done");
        const bool success = (status == "ok") && elapsed_ok && done_ok;

        cleanup_directory(shared_dir);
        return success ? 0 : 1;
    }

    return 0;
}
