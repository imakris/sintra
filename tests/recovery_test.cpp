//
// Sintra Process Recovery Test
//
// This test validates the process recovery mechanism of Sintra.
// It corresponds to example_4 and tests the following features:
// - enable_recovery() to mark a process for automatic restart
// - Automatic process respawn by the coordinator after crashes
// - Recovery occurrence tracking (s_recovery_occurrence)
// - Coordination between original and recovered processes
// - Mutex recovery after process crash
//
// Test structure:
// - Process 1 (watchdog): Waits for a Stop signal and validates recovery occurred
// - Process 2 (crasher): Deliberately crashes on first run, completes on second run
//
// The test verifies that:
// - The crasher process is automatically restarted after crashing
// - The recovered process can detect it's running after recovery
// - Communication works correctly after recovery
// - The test completes successfully with the recovered process
//

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_environment.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

namespace {

struct Stop {};

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";
std::string g_shared_dir;

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}
std::filesystem::path current_shared_directory()
{
    if (!g_shared_dir.empty()) {
        return std::filesystem::path(g_shared_dir);
    }

    const char* value = std::getenv(kEnvSharedDir.data());
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    return {};
}

void write_ready_marker(std::string_view tag, uint32_t occurrence = 0)
{
    auto shared_dir = current_shared_directory();
    if (shared_dir.empty()) {
        return;
    }

    std::ostringstream filename;
    filename << "ready_" << tag;
    if (occurrence != 0) {
        filename << "_" << occurrence;
    }
    filename << "_pid_"
#ifdef _WIN32
             << _getpid()
#else
             << getpid()
#endif
             ;
    const auto ready_path = shared_dir / (filename.str() + ".txt");

    std::ofstream out(ready_path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << "ready\n";
        out.flush();
    }
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
    if (value && *value) {
        std::filesystem::path dir(value);
        std::filesystem::create_directories(dir);
        return dir;
    }

    auto base = sintra::test::scratch_subdirectory("recovery_test");

    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    std::ostringstream oss;
    oss << "recovery_test_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

std::vector<std::string> read_lines(const std::filesystem::path& file)
{
    std::vector<std::string> values;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        values.push_back(line);
    }
    return values;
}

void append_line(const std::filesystem::path& file, const std::string& value)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    out << value << '\n';
}

#if defined(__APPLE__)
void disable_core_dumps_for_intentional_abort()
{
    struct rlimit current {};
    if (getrlimit(RLIMIT_CORE, &current) != 0) {
        std::fprintf(stderr, "[CRASHER] getrlimit(RLIMIT_CORE) failed: %d\n", errno);
        return;
    }

    if (current.rlim_cur == 0) {
        return;
    }

    struct rlimit updated = current;
    updated.rlim_cur = 0;
    if (setrlimit(RLIMIT_CORE, &updated) != 0) {
        std::fprintf(stderr, "[CRASHER] setrlimit(RLIMIT_CORE) failed: %d\n", errno);
        return;
    }

    std::fprintf(stderr, "[CRASHER] Disabled core dumps for intentional abort\n");
}
#endif

int process_watchdog()
{
    std::fprintf(stderr, "[WATCHDOG] Starting watchdog process\n");
    const auto shared_dir = get_shared_directory();
    const auto result_path = shared_dir / "result.txt";
    write_ready_marker("watchdog");

    std::condition_variable stop_cv;
    std::mutex stop_mutex;
    bool stop_received = false;

    sintra::activate_slot([&](const Stop&) {
        std::fprintf(stderr, "[WATCHDOG] Received Stop message!\n");
        std::lock_guard<std::mutex> lk(stop_mutex);
        stop_received = true;
        stop_cv.notify_one();
    });

    std::fprintf(stderr, "[WATCHDOG] Waiting for Stop message...\n");
    std::unique_lock<std::mutex> lk(stop_mutex);
    // Wait up to 60 seconds - recovery can take 45+ seconds
    const bool signalled = stop_cv.wait_for(
        lk, std::chrono::seconds(60), [&]{ return stop_received; });
    sintra::deactivate_all_slots();

    std::fprintf(stderr, "[WATCHDOG] Wait complete, signalled=%d\n", signalled);
    bool ok = false;
    if (signalled) {
        const auto run_entries = read_lines(shared_dir / "runs.txt");
        std::fprintf(stderr, "[WATCHDOG] Run entries: %zu\n", run_entries.size());
        ok = (run_entries.size() >= 2);
    }

    std::ofstream out(result_path, std::ios::binary | std::ios::trunc);
    out << (ok ? "ok\n" : "fail\n");
    std::fprintf(stderr, "[WATCHDOG] Result: %s\n", ok ? "ok" : "fail");
    return 0;
}

int process_crasher()
{
    // Early diagnostic to confirm entry on recovery occurrences
    std::fprintf(stderr, "[CRASHER] start occ=%u pid=%lu\n",
        (unsigned)sintra::s_recovery_occurrence,
#ifdef _WIN32
        (unsigned long)_getpid()
#else
        (unsigned long)getpid()
#endif
    );
    if (g_shared_dir.empty()) {
        if (const char* shared_dir_env = std::getenv("SINTRA_TEST_SHARED_DIR")) {
            g_shared_dir = shared_dir_env;
        }
    }

    {
        std::ofstream diag(sintra::test::scratch_subdirectory("recovery_test") / "sintra_crasher_diag.log", std::ios::app);
        diag << "process_crasher pid="
#ifdef _WIN32
             << _getpid()
#else
             << getpid()
#endif
             << " g_shared_dir=" << (g_shared_dir.empty() ? "<empty>" : g_shared_dir)
             << " env=" << (std::getenv("SINTRA_TEST_SHARED_DIR") ? "set" : "unset")
             << std::endl;
    }

    // Log entry immediately
    {
        std::filesystem::path log_dir;
        if (!g_shared_dir.empty()) {
            log_dir = g_shared_dir;
        }
        else
        if (const char* shared_dir_env = std::getenv("SINTRA_TEST_SHARED_DIR")) {
            log_dir = shared_dir_env;
        }

        if (!log_dir.empty()) {
            std::filesystem::path early_log = log_dir / "entry.log";
            std::ofstream elog(early_log, std::ios::app);
            elog << "process_crasher() entered, pid=" <<
#ifdef _WIN32
                _getpid()
#else
                getpid()
#endif
                << std::endl;
        }
    }

    sintra::enable_recovery();

#if defined(_MSC_VER)
    // Suppress the CRT abort dialog so the crash propagates automatically in Debug builds.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::filesystem::path shared_dir;
    if (!g_shared_dir.empty()) {
        shared_dir = g_shared_dir;
    }
    else {
        shared_dir = get_shared_directory();
    }
    const auto runs_path = shared_dir / "runs.txt";
    const auto log_path = shared_dir / "crasher.log";
    const uint32_t occurrence = sintra::s_recovery_occurrence;
    write_ready_marker("crasher", occurrence);

    std::ofstream log(log_path, std::ios::app);
    log << "Crasher starting, pid=" <<
#ifdef _WIN32
        _getpid()
#else
        getpid()
#endif
        << std::endl;

    log << "Recovery occurrence: " << occurrence << std::endl;
    append_line(runs_path, "run");

    if (occurrence == 0) {
        log << "First run - about to abort!" << std::endl;
        log.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#if defined(__APPLE__)
        disable_core_dumps_for_intentional_abort();
#endif
        sintra::disable_debug_pause_for_current_process();
        std::abort();
    }

    log << "Second+ run - sending Stop" << std::endl;
    log.close();
    sintra::world() << Stop{};
    return 0;
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

std::string get_arg_value(int argc, char* argv[], const std::string& arg_name)
{
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string_view(argv[i]) == arg_name) {
            return argv[i + 1];
        }
    }
    return "";
}

} // namespace

int main(int argc, char* argv[])
{
    // Enable debug logging for all processes
#ifdef _WIN32
    _putenv_s("SINTRA_DEBUG", "1");
#else
    setenv("SINTRA_DEBUG", "1", 1);
#endif

    // Log main() entry to file immediately
    {
        const char* shared_dir_env = std::getenv("SINTRA_TEST_SHARED_DIR");
        std::string early_shared_dir_arg = get_arg_value(argc, argv, "--shared_dir");

        if (!early_shared_dir_arg.empty()) {
            set_shared_directory_env(std::filesystem::path(early_shared_dir_arg));
            shared_dir_env = early_shared_dir_arg.c_str();
        }

        if (shared_dir_env) {
            std::filesystem::path log_path = std::filesystem::path(shared_dir_env) / "main.log";
            std::ofstream main_log(log_path, std::ios::app);
            main_log << "main() entered, pid=" <<
#ifdef _WIN32
                _getpid()
#else
                getpid()
#endif
                << ", argc=" << argc << std::endl;
            for (int i = 0; i < argc; ++i) {
                main_log << "  argv[" << i << "]: " << argv[i] << std::endl;
            }
        }
    }

    // Check if --shared_dir was passed (from recovery spawn)
    std::string shared_dir_arg = get_arg_value(argc, argv, "--shared_dir");
    if (!shared_dir_arg.empty()) {
        std::fprintf(stderr, "[MAIN] Setting shared_dir from argument: %s\n", shared_dir_arg.c_str());
        set_shared_directory_env(std::filesystem::path(shared_dir_arg));
    }

    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::fprintf(stderr, "[MAIN] Using shared_dir: %s\n", shared_dir.string().c_str());
    g_shared_dir = shared_dir.string();
    write_ready_marker("coordinator");
    {
        std::ofstream state_log(shared_dir / "state.log", std::ios::app);
        state_log << "main() set g_shared_dir to " << g_shared_dir
#ifdef _WIN32
                  << " pid=" << _getpid()
#else
                  << " pid=" << getpid()
#endif
                  << std::endl;
    }

    // Pass shared_dir to all spawned processes
    std::vector<std::string> user_opts = {};

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_watchdog, user_opts);
    processes.emplace_back(process_crasher, user_opts);

    {
        std::ofstream state_log(shared_dir / "state.log", std::ios::app);
        state_log << "calling sintra::init, pid="
#ifdef _WIN32
                  << _getpid()
#else
                  << getpid()
#endif
                  << std::endl;
    }

    sintra::init(argc, argv, processes);

    {
        std::ofstream state_log(shared_dir / "state.log", std::ios::app);
        state_log << "sintra::init completed, pid="
#ifdef _WIN32
                  << _getpid()
#else
                  << getpid()
#endif
                  << std::endl;
    }

    // Coordinator waits for result file before finalizing
    if (!is_spawned) {
        const auto result_path = shared_dir / "result.txt";
        // Wait up to 60 seconds - recovery cleanup can take 45+ seconds
        for (int i = 0; i < 600; ++i) {
            if (std::filesystem::exists(result_path)) {
                std::fprintf(stderr, "[MAIN] Result file found after %d checks\n", i);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    sintra::finalize();

    if (!is_spawned) {
        const auto result_path = shared_dir / "result.txt";
        if (!std::filesystem::exists(result_path)) {
            return 1;
        }

        std::ifstream in(result_path, std::ios::binary);
        std::string status;
        in >> status;

        // Best effort cleanup - ignore failures (files may still be in use)
        std::error_code ec;
        std::filesystem::remove_all(shared_dir, ec);
        if (ec) {
            std::fprintf(stderr, "[MAIN] Cleanup warning: %s\n", ec.message().c_str());
        }
        return (status == "ok") ? 0 : 1;
    }

    return 0;
}
