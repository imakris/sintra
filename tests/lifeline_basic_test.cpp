// Lifeline process lifetime tests.

#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_environment.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#else
#include <sys/wait.h>
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* k_role_arg = "--lifeline_role";
constexpr const char* k_case_arg = "--lifeline_case";
constexpr const char* k_dir_arg = "--lifeline_dir";

constexpr const char* k_role_owner = "owner";
constexpr const char* k_role_child = "child";
constexpr const char* k_role_missing = "missing";
constexpr const char* k_role_leak_owner = "leak_owner";
constexpr const char* k_role_respawn_owner = "respawn_owner";
constexpr const char* k_role_respawn_child = "respawn_child";

constexpr const char* k_case_normal = "normal";
constexpr const char* k_case_disabled = "disabled";
constexpr const char* k_case_crash = "crash";
constexpr const char* k_case_hung = "hung";
constexpr const char* k_case_leak = "leak";
constexpr const char* k_case_respawn = "respawn";

constexpr int k_owner_exit_timeout_ms = 4000;
constexpr int k_child_exit_timeout_ms = 2000;
constexpr int k_child_disable_check_ms = 300;
constexpr int k_hung_hard_exit_timeout_ms = 300;
constexpr int k_hung_child_exit_timeout_ms = 3000;
constexpr int k_rapid_spawn_iterations = 5;
constexpr int k_leak_iterations = 5;
constexpr int k_leak_child_lifetime_ms = 50;
constexpr int k_leak_owner_timeout_ms = 10000;
constexpr int k_respawn_owner_timeout_ms = 10000;
constexpr int k_respawn_child_exit_timeout_ms = 2000;

struct Process_handle
{
#ifdef _WIN32
    HANDLE handle = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
    bool waitable = true;
    bool exited = false;
    int exit_code = 0;
};

std::optional<std::string> find_arg_value(int argc, char* argv[], const char* name)
{
    if (!name || !*name) {
        return std::nullopt;
    }

    const std::string prefix = std::string(name) + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == name) {
            if (i + 1 < argc) {
                return std::string(argv[i + 1]);
            }
            return std::nullopt;
        }
        if (arg.rfind(prefix, 0) == 0) {
            return arg.substr(prefix.size());
        }
    }
    return std::nullopt;
}

std::string get_binary_path(int argc, char* argv[])
{
    if (argc > 0 && argv[0]) {
        return argv[0];
    }
    return {};
}

std::filesystem::path child_pid_path(const std::filesystem::path& dir, const std::string& test_case)
{
    return dir / ("child_pid_" + test_case + ".txt");
}

std::filesystem::path child_ready_path(const std::filesystem::path& dir, const std::string& test_case)
{
    return dir / ("child_ready_" + test_case + ".txt");
}

std::filesystem::path respawn_occurrence_path(const std::filesystem::path& dir, uint32_t occurrence)
{
    return dir / ("respawn_occurrence_" + std::to_string(occurrence) + ".txt");
}

std::filesystem::path respawn_recovery_ready_path(const std::filesystem::path& dir)
{
    return dir / "recovery_ready.txt";
}

std::filesystem::path respawn_test_ready_path(const std::filesystem::path& dir)
{
    return dir / "respawn_test_ready.txt";
}

bool write_text_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

bool wait_for_file(const std::filesystem::path& path, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::filesystem::exists(path);
}

int remaining_ms(const std::chrono::steady_clock::time_point& deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

bool wait_for_file_until(const std::filesystem::path& path,
    const std::chrono::steady_clock::time_point& deadline)
{
    const int remaining = remaining_ms(deadline);
    if (remaining <= 0) {
        return std::filesystem::exists(path);
    }
    return wait_for_file(path, remaining);
}

bool read_pid_file(const std::filesystem::path& path, long long& pid_out)
{
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    long long value = 0;
    in >> value;
    if (!in) {
        return false;
    }
    pid_out = value;
    return true;
}

bool wait_for_pid_value(const std::filesystem::path& path, int timeout_ms, long long& pid_out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (read_pid_file(path, pid_out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return read_pid_file(path, pid_out);
}

#ifdef _WIN32
std::wstring to_wide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wide[0], len);
    wide.resize(static_cast<size_t>(len - 1));
    return wide;
}

std::wstring build_command_line(const std::string& exe, const std::vector<std::string>& args)
{
    std::vector<std::string> all_args;
    all_args.reserve(args.size() + 1);
    all_args.push_back(exe);
    all_args.insert(all_args.end(), args.begin(), args.end());

    std::wstring cmdline;
    bool first = true;
    for (const auto& arg : all_args) {
        if (!first) {
            cmdline += L' ';
        }
        first = false;

        const std::wstring wide_arg = to_wide(arg);
        const bool needs_quoting = wide_arg.find(L' ') != std::wstring::npos || wide_arg.empty();

        if (needs_quoting) {
            cmdline += L'"';
        }

        for (size_t i = 0; i < wide_arg.length(); ++i) {
            wchar_t ch = wide_arg[i];
            if (ch == L'"') {
                cmdline += L"\\\"";
            }
            else if (ch == L'\\') {
                if (i + 1 < wide_arg.length() && wide_arg[i + 1] == L'"') {
                    cmdline += L"\\\\";
                }
                else if (i + 1 == wide_arg.length() && needs_quoting) {
                    cmdline += L"\\\\";
                }
                else {
                    cmdline += L'\\';
                }
            }
            else {
                cmdline += ch;
            }
        }

        if (needs_quoting) {
            cmdline += L'"';
        }
    }

    return cmdline;
}

bool spawn_process(const std::string& exe, const std::vector<std::string>& args, Process_handle& out)
{
    const std::wstring exe_wide = to_wide(exe);
    std::wstring cmdline = build_command_line(exe, args);
    if (exe_wide.empty() || cmdline.empty()) {
        return false;
    }

    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};

    const BOOL success = CreateProcessW(
        exe_wide.c_str(),
        cmdline_buf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!success) {
        return false;
    }

    out.pid = pi.dwProcessId;
    out.handle = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}

bool poll_exit(Process_handle& process)
{
    if (process.exited || !process.handle) {
        return process.exited;
    }

    DWORD wait_result = WaitForSingleObject(process.handle, 0);
    if (wait_result == WAIT_TIMEOUT) {
        return false;
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process.handle, &exit_code)) {
        process.exit_code = static_cast<int>(exit_code);
    }
    else {
        process.exit_code = 1;
    }
    process.exited = true;
    return true;
}

void close_process(Process_handle& process)
{
    if (process.handle) {
        CloseHandle(process.handle);
        process.handle = nullptr;
    }
}
#else
bool spawn_process(const std::string& exe, const std::vector<std::string>& args, Process_handle& out)
{
    pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }

    out.pid = pid;
    return true;
}

bool poll_exit(Process_handle& process)
{
    if (process.exited || process.pid <= 0) {
        return process.exited;
    }

    if (!process.waitable) {
        if (::kill(process.pid, 0) == 0) {
            return false;
        }
        if (errno == EPERM) {
            return false;
        }
        if (errno == ESRCH) {
            process.exit_code = 0;
            process.exited = true;
            return true;
        }
        process.exit_code = 1;
        process.exited = true;
        return true;
    }

    int status = 0;
    pid_t result = ::waitpid(process.pid, &status, WNOHANG);
    if (result == 0) {
        return false;
    }
    if (result < 0) {
        process.exit_code = 1;
        process.exited = true;
        return true;
    }

    if (WIFEXITED(status)) {
        process.exit_code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status)) {
        process.exit_code = 128 + WTERMSIG(status);
    }
    else {
        process.exit_code = 1;
    }
    process.exited = true;
    return true;
}

void close_process(Process_handle& /*process*/) {}
#endif

bool wait_for_exit(Process_handle& process, int timeout_ms, int& exit_code_out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (poll_exit(process)) {
            exit_code_out = process.exit_code;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (poll_exit(process)) {
        exit_code_out = process.exit_code;
        return true;
    }

    return false;
}

bool is_running(Process_handle& process)
{
    return !poll_exit(process);
}

bool process_exists(long long pid)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h) {
        CloseHandle(h);
        return true;
    }
    return GetLastError() != ERROR_INVALID_PARAMETER;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

int child_pid_value()
{
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

int run_child(const std::filesystem::path& dir, const std::string& test_case, int argc, char* argv[])
{
    sintra::init(argc, argv);

    const auto pid_file = child_pid_path(dir, test_case);
    const auto ready_file = child_ready_path(dir, test_case);

    if (!write_text_file(pid_file, std::to_string(child_pid_value()))) {
        std::fprintf(stderr, "[child] failed to write pid file\n");
        std::_Exit(1);
    }
    if (!write_text_file(ready_file, "ready")) {
        std::fprintf(stderr, "[child] failed to write ready file\n");
        std::_Exit(1);
    }

    if (test_case == k_case_disabled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::_Exit(0);
    }

    if (test_case == k_case_hung) {
        // Enter infinite loop to simulate a hung child.
        // The lifeline watcher thread should force exit after the hard_exit_timeout.
        volatile bool keep_running = true;
        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
        std::_Exit(0);
    }

    if (test_case == k_case_leak) {
        // Exit quickly to allow rapid iteration in leak test.
        // The lifeline write handle should be properly released by the owner.
        std::this_thread::sleep_for(std::chrono::milliseconds(k_leak_child_lifetime_ms));
        std::_Exit(0);
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::_Exit(0);
}

int run_owner(const std::filesystem::path& dir, const std::string& test_case, int argc, char* argv[])
{
    sintra::init(argc, argv);

    sintra::Spawn_options spawn_options;
    spawn_options.binary_path = get_binary_path(argc, argv);
    spawn_options.args = {
        k_role_arg, k_role_child,
        k_case_arg, test_case,
        k_dir_arg, dir.string()
    };

    if (test_case == k_case_disabled) {
        spawn_options.lifetime.enable_lifeline = false;
    }

    if (test_case == k_case_hung) {
        spawn_options.lifetime.hard_exit_timeout_ms = k_hung_hard_exit_timeout_ms;
    }

    const size_t spawned = sintra::spawn_swarm_process(spawn_options);
    if (spawned != 1) {
        std::fprintf(stderr, "[owner] failed to spawn child\n");
        std::_Exit(2);
    }

    if (!wait_for_file(child_ready_path(dir, test_case), 3000)) {
        std::fprintf(stderr, "[owner] child did not signal ready\n");
        std::_Exit(3);
    }

    if (test_case == k_case_crash) {
        // Crash the owner process abnormally to test lifeline under abnormal termination.
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 42);
#else
        raise(SIGKILL);
#endif
    }

    std::_Exit(0);
}

int run_leak_owner(const std::filesystem::path& dir, int argc, char* argv[])
{
    // This owner stays alive and spawns/kills children repeatedly.
    // If there are handle/fd leaks, they would accumulate in this process.
    sintra::init(argc, argv);

    const std::string binary_path = get_binary_path(argc, argv);
    const auto ready_file = child_ready_path(dir, k_case_leak);
    const auto pid_file = child_pid_path(dir, k_case_leak);

    for (int i = 0; i < k_leak_iterations; ++i) {
        // Remove marker files from previous iteration
        std::filesystem::remove(ready_file);
        std::filesystem::remove(pid_file);

        sintra::Spawn_options spawn_options;
        spawn_options.binary_path = binary_path;
        spawn_options.args = {
            k_role_arg, k_role_child,
            k_case_arg, k_case_leak,
            k_dir_arg, dir.string()
        };

        const size_t spawned = sintra::spawn_swarm_process(spawn_options);
        if (spawned != 1) {
            std::fprintf(stderr, "[leak_owner] failed to spawn child %d\n", i);
            std::_Exit(1);
        }

        // Wait for child to signal ready
        if (!wait_for_file(ready_file, 3000)) {
            std::fprintf(stderr, "[leak_owner] child %d did not signal ready\n", i);
            std::_Exit(2);
        }

        // Read child PID
        long long child_pid = 0;
        if (!read_pid_file(pid_file, child_pid)) {
            std::fprintf(stderr, "[leak_owner] failed to read child %d pid\n", i);
            std::_Exit(3);
        }

        // Wait for child to exit (it exits shortly after signaling ready).
        // Poll until child actually exits rather than just sleeping.
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(k_leak_child_lifetime_ms + 1000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!process_exists(child_pid)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Verify child actually exited
        if (process_exists(child_pid)) {
            std::fprintf(stderr, "[leak_owner] child %d (pid %lld) did not exit\n", i, child_pid);
            std::_Exit(4);
        }
    }

    // All iterations completed successfully
    std::_Exit(0);
}

int run_respawn_child(const std::filesystem::path& dir, int argc, char* argv[])
{
    // This child enables recovery, writes occurrence marker, then crashes on first run.
    // On second run (after respawn), it writes marker and waits for lifeline to break.
    sintra::init(argc, argv);

    const uint32_t occurrence = sintra::s_recovery_occurrence;
    const auto occurrence_file = respawn_occurrence_path(dir, occurrence);
    const auto pid_file = child_pid_path(dir, k_case_respawn);

    // Write occurrence marker with pid
    if (!write_text_file(occurrence_file, std::to_string(child_pid_value()))) {
        std::fprintf(stderr, "[respawn_child] failed to write occurrence file\n");
        std::_Exit(1);
    }

    // Also write to the standard pid file for the test runner to track
    if (!write_text_file(pid_file, std::to_string(child_pid_value()))) {
        std::fprintf(stderr, "[respawn_child] failed to write pid file\n");
        std::_Exit(1);
    }

    if (occurrence == 0) {
        // First run: enable recovery (synchronous RPC to coordinator).
        // Note: enable_recovery() uses Coordinator::rpc_enable_recovery() which is
        // a blocking RPC - it returns only after the coordinator has processed the
        // registration. The recovery_ready marker file confirms the call returned.
        sintra::enable_recovery();

        // Write marker to confirm recovery registration completed
        const auto recovery_ready_file = respawn_recovery_ready_path(dir);
        if (!write_text_file(recovery_ready_file, "ready")) {
            std::fprintf(stderr, "[respawn_child] failed to write recovery ready file\n");
            std::_Exit(1);
        }

        // Now crash to trigger respawn
        std::fprintf(stderr, "[respawn_child] first run, recovery enabled, crashing...\n");
#ifdef _WIN32
        // Suppress CRT abort dialog
#if defined(_MSC_VER)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif
        sintra::disable_debug_pause_for_current_process();
        std::abort();
    }

    // Second+ run: wait for lifeline to break (owner exit)
    std::fprintf(stderr, "[respawn_child] occurrence %u, waiting for lifeline...\n", occurrence);
    std::this_thread::sleep_for(std::chrono::seconds(60));
    // Should not reach here - lifeline should terminate us with exit code 99
    std::_Exit(0);
}

int run_respawn_owner(const std::filesystem::path& dir, int argc, char* argv[])
{
    // This owner spawns a child that will crash and be respawned.
    // Once respawned child is ready, owner exits to trigger lifeline.
    sintra::init(argc, argv);

    sintra::Spawn_options spawn_options;
    spawn_options.binary_path = get_binary_path(argc, argv);
    spawn_options.args = {
        k_role_arg, k_role_respawn_child,
        k_dir_arg, dir.string()
    };

    const size_t spawned = sintra::spawn_swarm_process(spawn_options);
    if (spawned != 1) {
        std::fprintf(stderr, "[respawn_owner] failed to spawn child\n");
        std::_Exit(2);
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(k_respawn_owner_timeout_ms);

    // Wait for first occurrence marker (child before crash)
    const auto first_occurrence_file = respawn_occurrence_path(dir, 0);
    if (!wait_for_file_until(first_occurrence_file, deadline)) {
        std::fprintf(stderr, "[respawn_owner] child did not write first occurrence marker\n");
        std::_Exit(3);
    }
    std::fprintf(stderr, "[respawn_owner] first occurrence marker found\n");

    // Wait for recovery registration confirmation before expecting crash/respawn
    const auto recovery_ready_file = respawn_recovery_ready_path(dir);
    if (!wait_for_file_until(recovery_ready_file, deadline)) {
        std::fprintf(stderr, "[respawn_owner] child did not confirm recovery registration\n");
        std::_Exit(5);
    }
    std::fprintf(stderr, "[respawn_owner] recovery registration confirmed\n");

    // Wait for second occurrence marker (child after respawn)
    const auto second_occurrence_file = respawn_occurrence_path(dir, 1);
    if (!wait_for_file_until(second_occurrence_file, deadline)) {
        std::fprintf(stderr, "[respawn_owner] child did not write second occurrence marker\n");
        std::_Exit(4);
    }
    std::fprintf(stderr, "[respawn_owner] second occurrence marker found\n");

    // Wait for the test runner to confirm it is ready to monitor the respawned child.
    const auto test_ready_file = respawn_test_ready_path(dir);
    if (!wait_for_file_until(test_ready_file, deadline)) {
        std::fprintf(stderr, "[respawn_owner] test runner did not confirm ready state\n");
        std::_Exit(6);
    }

    std::fprintf(stderr, "[respawn_owner] test ready, exiting to trigger lifeline\n");
    // Exit to trigger lifeline - respawned child should exit with code 99
    std::_Exit(0);
}

bool run_respawn_test(const std::string& binary_path, const std::filesystem::path& dir)
{
    // Test that respawned children get proper lifeline from coordinator.
    // Flow:
    // 1. Spawn respawn_owner
    // 2. respawn_owner spawns child via sintra
    // 3. Child enables recovery, writes occurrence 0 marker, crashes
    // 4. Sintra respawns child automatically
    // 5. Respawned child writes occurrence 1 marker
    // 6. Test opens handle to respawned child and signals readiness
    // 7. respawn_owner exits to trigger lifeline
    // 8. Respawned child should exit with code 99 due to lifeline

    std::vector<std::string> args = {
        k_role_arg, k_role_respawn_owner,
        k_dir_arg, dir.string()
    };

    Process_handle owner{};
    if (!spawn_process(binary_path, args, owner)) {
        std::fprintf(stderr, "[test] failed to spawn respawn owner\n");
        close_process(owner);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(k_respawn_owner_timeout_ms);

    const auto second_occurrence_file = respawn_occurrence_path(dir, 1);
    long long respawned_pid = 0;
    const int pid_wait_ms = remaining_ms(deadline);
    if (pid_wait_ms <= 0 ||
        !wait_for_pid_value(second_occurrence_file, pid_wait_ms, respawned_pid)) {
        std::fprintf(stderr, "[test] failed to read respawned child pid\n");
        close_process(owner);
        return false;
    }

    Process_handle respawned_child{};
#ifdef _WIN32
    respawned_child.pid = static_cast<DWORD>(respawned_pid);
    respawned_child.handle = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, respawned_child.pid);
    if (!respawned_child.handle) {
        std::fprintf(stderr, "[test] OpenProcess failed with error %lu\n", GetLastError());
        close_process(owner);
        return false;
    }
#else
    respawned_child.pid = static_cast<pid_t>(respawned_pid);
    respawned_child.waitable = false;
#endif

    if (!write_text_file(respawn_test_ready_path(dir), "ready")) {
        std::fprintf(stderr, "[test] failed to write respawn test ready file\n");
        close_process(owner);
        close_process(respawned_child);
        return false;
    }

    int owner_exit = 0;
    const int owner_wait_ms = remaining_ms(deadline);
    if (owner_wait_ms <= 0 || !wait_for_exit(owner, owner_wait_ms, owner_exit)) {
        std::fprintf(stderr, "[test] respawn owner did not exit in time\n");
        close_process(owner);
        close_process(respawned_child);
        return false;
    }
    close_process(owner);

    if (owner_exit != 0) {
        std::fprintf(stderr, "[test] respawn owner exited with code %d\n", owner_exit);
        close_process(respawned_child);
        return false;
    }

    // Wait for respawned child to exit
    int child_exit = 0;
    if (!wait_for_exit(respawned_child, k_respawn_child_exit_timeout_ms, child_exit)) {
        std::fprintf(stderr, "[test] respawned child did not exit in time\n");
        close_process(respawned_child);
        return false;
    }
    close_process(respawned_child);

    // Verify exit code
#ifdef _WIN32
    if (child_exit != 99) {
        std::fprintf(stderr, "[test] respawned child exit code: %d (expected 99)\n", child_exit);
        return false;
    }
    std::fprintf(stderr, "[test] respawned child exited with code 99 (verified)\n");
#else
    // On POSIX we just verify the process exited (we can't get exit code of non-child)
    std::fprintf(stderr, "[test] respawned child exited (POSIX: exit code not verifiable)\n");
#endif

    return true;
}

bool run_owner_case(
    const std::string& binary_path,
    const std::filesystem::path& dir,
    const std::string& test_case,
    int expected_child_exit,
    bool expect_child_alive_after_owner,
    bool allow_any_owner_exit = false,
    int child_exit_timeout_ms = k_child_exit_timeout_ms)
{
    std::vector<std::string> args = {
        k_role_arg, k_role_owner,
        k_case_arg, test_case,
        k_dir_arg, dir.string()
    };

    Process_handle owner{};
    if (!spawn_process(binary_path, args, owner)) {
        std::fprintf(stderr, "[test] failed to spawn owner\n");
        close_process(owner);
        return false;
    }

    long long child_pid_value_raw = 0;
    if (!wait_for_pid_value(child_pid_path(dir, test_case), 3000, child_pid_value_raw)) {
        std::fprintf(stderr, "[test] failed to read child pid\n");
        close_process(owner);
        return false;
    }

    Process_handle child{};
#ifdef _WIN32
    child.pid = static_cast<DWORD>(child_pid_value_raw);
    child.handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child.pid);
    if (!child.handle) {
        std::fprintf(stderr, "[test] failed to open child process\n");
        close_process(owner);
        return false;
    }
#else
    child.pid = static_cast<pid_t>(child_pid_value_raw);
    child.waitable = false;
#endif

    int owner_exit = 0;
    if (!wait_for_exit(owner, k_owner_exit_timeout_ms, owner_exit)) {
        std::fprintf(stderr, "[test] owner did not exit in time\n");
        close_process(owner);
        close_process(child);
        return false;
    }
    if (!allow_any_owner_exit && owner_exit != 0) {
        std::fprintf(stderr, "[test] owner exited with code %d\n", owner_exit);
        close_process(owner);
        close_process(child);
        return false;
    }
    close_process(owner);

    if (expect_child_alive_after_owner) {
        std::this_thread::sleep_for(std::chrono::milliseconds(k_child_disable_check_ms));
        if (!is_running(child)) {
            std::fprintf(stderr, "[test] child exited too early\n");
            close_process(child);
            return false;
        }
    }

    int child_exit = 0;
    if (!wait_for_exit(child, child_exit_timeout_ms, child_exit)) {
        std::fprintf(stderr, "[test] child did not exit in time\n");
        close_process(child);
        return false;
    }
    close_process(child);

    if (child.waitable && child_exit != expected_child_exit) {
        std::fprintf(stderr, "[test] unexpected child exit code: %d\n", child_exit);
        return false;
    }

    return true;
}

bool run_missing_lifeline_case(const std::string& binary_path)
{
    std::vector<std::string> args = {
        k_role_arg, k_role_missing,
        "--swarm_id", "123",
        "--instance_id", "2",
        "--coordinator_id", "1"
    };

    Process_handle proc{};
    if (!spawn_process(binary_path, args, proc)) {
        std::fprintf(stderr, "[test] failed to spawn missing lifeline process\n");
        close_process(proc);
        return false;
    }

    int exit_code = 0;
    if (!wait_for_exit(proc, 1500, exit_code)) {
        std::fprintf(stderr, "[test] missing lifeline process did not exit in time\n");
        close_process(proc);
        return false;
    }
    close_process(proc);

    if (exit_code != 99) {
        std::fprintf(stderr, "[test] unexpected missing lifeline exit code: %d\n", exit_code);
        return false;
    }

    return true;
}

bool run_leak_test(const std::string& binary_path, const std::filesystem::path& dir)
{
    // Spawn a leak_owner process that will spawn/kill children repeatedly.
    // This tests for handle/fd leaks in a single long-running owner process.
    std::vector<std::string> args = {
        k_role_arg, k_role_leak_owner,
        k_dir_arg, dir.string()
    };

    Process_handle owner{};
    if (!spawn_process(binary_path, args, owner)) {
        std::fprintf(stderr, "[test] failed to spawn leak test owner\n");
        close_process(owner);
        return false;
    }

    int exit_code = 0;
    if (!wait_for_exit(owner, k_leak_owner_timeout_ms, exit_code)) {
        std::fprintf(stderr, "[test] leak test owner did not exit in time\n");
        close_process(owner);
        return false;
    }
    close_process(owner);

    if (exit_code != 0) {
        std::fprintf(stderr, "[test] leak test owner exited with code %d\n", exit_code);
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto role = find_arg_value(argc, argv, k_role_arg);
    const auto test_case = find_arg_value(argc, argv, k_case_arg);
    const auto dir_value = find_arg_value(argc, argv, k_dir_arg);

    if (role && *role == k_role_missing) {
        sintra::init(argc, argv);
        return 1;
    }

    if (role && *role == k_role_child) {
        if (!test_case || !dir_value) {
            std::fprintf(stderr, "[child] missing args\n");
            return 1;
        }
        return run_child(std::filesystem::path(*dir_value), *test_case, argc, argv);
    }

    if (role && *role == k_role_owner) {
        if (!test_case || !dir_value) {
            std::fprintf(stderr, "[owner] missing args\n");
            return 1;
        }
        return run_owner(std::filesystem::path(*dir_value), *test_case, argc, argv);
    }

    if (role && *role == k_role_leak_owner) {
        if (!dir_value) {
            std::fprintf(stderr, "[leak_owner] missing dir arg\n");
            return 1;
        }
        return run_leak_owner(std::filesystem::path(*dir_value), argc, argv);
    }

    if (role && *role == k_role_respawn_owner) {
        if (!dir_value) {
            std::fprintf(stderr, "[respawn_owner] missing dir arg\n");
            return 1;
        }
        return run_respawn_owner(std::filesystem::path(*dir_value), argc, argv);
    }

    if (role && *role == k_role_respawn_child) {
        if (!dir_value) {
            std::fprintf(stderr, "[respawn_child] missing dir arg\n");
            return 1;
        }
        return run_respawn_child(std::filesystem::path(*dir_value), argc, argv);
    }

    const std::string binary_path = get_binary_path(argc, argv);
    if (binary_path.empty()) {
        std::fprintf(stderr, "[test] binary path missing\n");
        return 1;
    }

    bool ok = true;

    // Test 1: Normal owner exit - child should exit with lifeline code 99
    const auto normal_dir = sintra::test::unique_scratch_directory("lifeline_normal");
    ok &= run_owner_case(binary_path, normal_dir, k_case_normal, 99, false);

    // Test 2: Disabled lifeline - child should stay alive after owner exits
    const auto disabled_dir = sintra::test::unique_scratch_directory("lifeline_disabled");
    ok &= run_owner_case(binary_path, disabled_dir, k_case_disabled, 0, true);

    // Test 3: Missing lifeline environment variable - should fail fast
    ok &= run_missing_lifeline_case(binary_path);

    // Test 4: Owner crash (abnormal termination) - child should still exit with code 99
    const auto crash_dir = sintra::test::unique_scratch_directory("lifeline_crash");
    ok &= run_owner_case(binary_path, crash_dir, k_case_crash, 99, false,
                         /*allow_any_owner_exit=*/true);

    // Test 5: Hung child - should be forcefully terminated after hard_exit_timeout
    const auto hung_dir = sintra::test::unique_scratch_directory("lifeline_hung");
    ok &= run_owner_case(binary_path, hung_dir, k_case_hung, 99, false,
                         /*allow_any_owner_exit=*/false,
                         /*child_exit_timeout_ms=*/k_hung_child_exit_timeout_ms);

    // Test 6: Rapid spawn/kill cycles - detect handle/fd leaks (different owners)
    for (int i = 0; i < k_rapid_spawn_iterations && ok; ++i) {
        const auto rapid_dir = sintra::test::unique_scratch_directory(
            "lifeline_rapid_" + std::to_string(i));
        ok &= run_owner_case(binary_path, rapid_dir, k_case_normal, 99, false);
    }

    // Test 7: Proper leak test - single owner spawns/kills many children
    // This detects handle/fd leaks that accumulate in a long-running owner process.
    const auto leak_dir = sintra::test::unique_scratch_directory("lifeline_leak");
    ok &= run_leak_test(binary_path, leak_dir);

    // Test 8: Auto-respawn test - respawned child gets new lifeline from coordinator
    // Verifies that the lifeline mechanism works correctly with sintra's recovery feature.
    const auto respawn_dir = sintra::test::unique_scratch_directory("lifeline_respawn");
    ok &= run_respawn_test(binary_path, respawn_dir);

    return ok ? 0 : 1;
}
