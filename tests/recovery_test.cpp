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

#include "test_utils.h"

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

namespace {

struct Stop {};

std::string g_shared_dir;

std::filesystem::path current_shared_directory()
{
    if (!g_shared_dir.empty()) {
        return std::filesystem::path(g_shared_dir);
    }

    const char* value = std::getenv("SINTRA_TEST_SHARED_DIR");
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
    filename << "_pid_" << sintra::test::get_pid();
    const auto ready_path = shared_dir / (filename.str() + ".txt");

    std::ofstream out(ready_path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << "ready\n";
        out.flush();
    }
}
int process_watchdog()
{
    std::fprintf(stderr, "[WATCHDOG] Starting watchdog process\n");
    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "recovery_test");
    const auto& shared_dir = shared.path();
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
        const auto run_entries = sintra::test::read_lines(shared_dir / "runs.txt");
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
        static_cast<unsigned long>(sintra::test::get_pid()));
    if (g_shared_dir.empty()) {
        if (const char* shared_dir_env = std::getenv("SINTRA_TEST_SHARED_DIR")) {
            g_shared_dir = shared_dir_env;
        }
    }

    {
        std::ofstream diag(sintra::test::scratch_subdirectory("recovery_test") / "sintra_crasher_diag.log", std::ios::app);
        diag << "process_crasher pid="
             << sintra::test::get_pid()
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
            elog << "process_crasher() entered, pid="
                << sintra::test::get_pid()
                << std::endl;
        }
    }

    sintra::enable_recovery();

    sintra::test::prepare_for_intentional_crash("CRASHER");

    std::filesystem::path shared_dir;
    if (!g_shared_dir.empty()) {
        shared_dir = g_shared_dir;
    }
    else {
        sintra::test::Shared_directory shared_obj("SINTRA_TEST_SHARED_DIR", "recovery_test");
        shared_dir = shared_obj.path();
    }
    const auto runs_path = shared_dir / "runs.txt";
    const auto log_path = shared_dir / "crasher.log";
    const uint32_t occurrence = sintra::s_recovery_occurrence;
    write_ready_marker("crasher", occurrence);

    std::ofstream log(log_path, std::ios::app);
    log << "Crasher starting, pid="
        << sintra::test::get_pid()
        << std::endl;

    log << "Recovery occurrence: " << occurrence << std::endl;
    sintra::test::append_line_or_throw(runs_path, "run");

    if (occurrence == 0) {
        log << "First run - about to abort!" << std::endl;
        log.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sintra::disable_debug_pause_for_current_process();
        std::abort();
    }

    log << "Second+ run - sending Stop" << std::endl;
    log.close();
    sintra::world() << Stop{};
    return 0;
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
        std::string early_shared_dir_arg = sintra::test::get_argv_value(argc, argv, "--shared_dir");

        if (!early_shared_dir_arg.empty()) {
#ifdef _WIN32
            _putenv_s("SINTRA_TEST_SHARED_DIR", early_shared_dir_arg.c_str());
#else
            setenv("SINTRA_TEST_SHARED_DIR", early_shared_dir_arg.c_str(), 1);
#endif
            shared_dir_env = early_shared_dir_arg.c_str();
        }

        if (shared_dir_env) {
            std::filesystem::path log_path = std::filesystem::path(shared_dir_env) / "main.log";
            std::ofstream main_log(log_path, std::ios::app);
            main_log << "main() entered, pid="
                     << sintra::test::get_pid()
                     << ", argc=" << argc << std::endl;
            for (int i = 0; i < argc; ++i) {
                main_log << "  argv[" << i << "]: " << argv[i] << std::endl;
            }
        }
    }

    // Check if --shared_dir was passed (from recovery spawn)
    std::string shared_dir_arg = sintra::test::get_argv_value(argc, argv, "--shared_dir");
    if (!shared_dir_arg.empty()) {
        std::fprintf(stderr, "[MAIN] Setting shared_dir from argument: %s\n", shared_dir_arg.c_str());
#ifdef _WIN32
        _putenv_s("SINTRA_TEST_SHARED_DIR", shared_dir_arg.c_str());
#else
        setenv("SINTRA_TEST_SHARED_DIR", shared_dir_arg.c_str(), 1);
#endif
    }

    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared_dir_raii("SINTRA_TEST_SHARED_DIR", "recovery_test");
    const auto& shared_dir = shared_dir_raii.path();

    std::fprintf(stderr, "[MAIN] Using shared_dir: %s\n", shared_dir.string().c_str());
    g_shared_dir = shared_dir.string();
    write_ready_marker("coordinator");
    {
        std::ofstream state_log(shared_dir / "state.log", std::ios::app);
        state_log << "main() set g_shared_dir to " << g_shared_dir
                  << " pid=" << sintra::test::get_pid()
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
                  << sintra::test::get_pid()
                  << std::endl;
    }

    sintra::init(argc, argv, processes);

    {
        std::ofstream state_log(shared_dir / "state.log", std::ios::app);
        state_log << "sintra::init completed, pid="
                  << sintra::test::get_pid()
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

        shared_dir_raii.cleanup();
        return (status == "ok") ? 0 : 1;
    }

    return 0;
}
