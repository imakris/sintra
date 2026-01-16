// Lifeline process lifetime tests.

#include <sintra/sintra.h>

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

constexpr const char* k_case_normal = "normal";
constexpr const char* k_case_disabled = "disabled";

constexpr int k_owner_exit_timeout_ms = 4000;
constexpr int k_child_exit_timeout_ms = 2000;
constexpr int k_child_disable_check_ms = 300;

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

    const size_t spawned = sintra::spawn_swarm_process(spawn_options);
    if (spawned != 1) {
        std::fprintf(stderr, "[owner] failed to spawn child\n");
        std::_Exit(2);
    }

    if (!wait_for_file(child_ready_path(dir, test_case), 3000)) {
        std::fprintf(stderr, "[owner] child did not signal ready\n");
        std::_Exit(3);
    }

    std::_Exit(0);
}

bool run_owner_case(
    const std::string& binary_path,
    const std::filesystem::path& dir,
    const std::string& test_case,
    int expected_child_exit,
    bool expect_child_alive_after_owner)
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
    if (owner_exit != 0) {
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
    if (!wait_for_exit(child, k_child_exit_timeout_ms, child_exit)) {
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

    const std::string binary_path = get_binary_path(argc, argv);
    if (binary_path.empty()) {
        std::fprintf(stderr, "[test] binary path missing\n");
        return 1;
    }

    bool ok = true;

    const auto normal_dir = sintra::test::unique_scratch_directory("lifeline_normal");
    ok &= run_owner_case(binary_path, normal_dir, k_case_normal, 99, false);

    const auto disabled_dir = sintra::test::unique_scratch_directory("lifeline_disabled");
    ok &= run_owner_case(binary_path, disabled_dir, k_case_disabled, 0, true);

    ok &= run_missing_lifeline_case(binary_path);

    return ok ? 0 : 1;
}
