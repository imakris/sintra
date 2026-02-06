// Crash capture test: spawn a child that crashes, then crash the parent.
#include "test_utils.h"
#include "sintra/detail/debug_pause.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct Process_handle {
#ifdef _WIN32
    HANDLE handle = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif
    bool exited = false;
    int exit_code = 0;
};

int clamp_int(int value, int lo, int hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

std::uint64_t make_seed()
{
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(getpid());
#endif
    return now ^ (pid << 3U);
}

int pick_delay_ms(const char* min_env, const char* max_env, int fallback_min, int fallback_max)
{
    int min_ms = sintra::test::read_env_int(min_env, fallback_min);
    int max_ms = sintra::test::read_env_int(max_env, fallback_max);
    if (max_ms < min_ms) {
        std::swap(max_ms, min_ms);
    }
    min_ms = clamp_int(min_ms, 0, 60000);
    max_ms = clamp_int(max_ms, min_ms, 60000);

    std::mt19937 rng(static_cast<unsigned>(make_seed() ^ static_cast<std::uint64_t>(min_ms)));
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return dist(rng);
}

#ifdef _WIN32
std::wstring to_wide(const std::string& input)
{
    if (input.empty()) {
        return {};
    }

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, wide.data(), wide_len);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

void append_quoted_argument(std::wstring& cmdline, const std::string& arg)
{
    const std::wstring wide_arg = to_wide(arg);
    if (cmdline.empty()) {
        cmdline.reserve(wide_arg.size() + 2);
    }
    else {
        cmdline += L' ';
    }

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

std::wstring build_command_line(const std::string& exe, const std::vector<std::string>& args)
{
    std::wstring cmdline;
    append_quoted_argument(cmdline, exe);
    for (const auto& arg : args) {
        append_quoted_argument(cmdline, arg);
    }

    return cmdline;
}
#endif

bool spawn_process(const std::string& exe, const std::vector<std::string>& args, Process_handle& out)
{
#ifdef _WIN32
    const std::wstring exe_wide = to_wide(exe);
    std::wstring cmdline = build_command_line(exe, args);
    if (exe_wide.empty() || cmdline.empty()) {
        return false;
    }

    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    HANDLE std_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE std_err = GetStdHandle(STD_ERROR_HANDLE);
    bool inherit_handles = false;
    if (std_in && std_in != INVALID_HANDLE_VALUE
        && std_out && std_out != INVALID_HANDLE_VALUE
        && std_err && std_err != INVALID_HANDLE_VALUE) {
        // Ensure child stdout/stderr propagate so stack-capture pauses are visible.
        SetHandleInformation(std_in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(std_out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(std_err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = std_in;
        si.hStdOutput = std_out;
        si.hStdError = std_err;
        inherit_handles = true;
    }
    PROCESS_INFORMATION pi {};

    const BOOL success = CreateProcessW(
        exe_wide.c_str(),
        cmdline_buf.data(),
        nullptr,
        nullptr,
        inherit_handles ? TRUE : FALSE,
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
#else
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
#endif
}

bool wait_for_exit(Process_handle& process, int timeout_ms)
{
    if (process.exited) {
        return true;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        if (!process.handle) {
            return false;
        }
        DWORD wait_result = WaitForSingleObject(process.handle, 0);
        if (wait_result == WAIT_TIMEOUT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
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
#else
        int status = 0;
        pid_t result = ::waitpid(process.pid, &status, WNOHANG);
        if (result == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (result < 0) {
            process.exit_code = 1;
            process.exited = true;
            return true;
        }

        if (WIFSIGNALED(status)) {
            process.exit_code = 128 + WTERMSIG(status);
        }
        else if (WIFEXITED(status)) {
            process.exit_code = WEXITSTATUS(status);
        }
        else {
            process.exit_code = 1;
        }
        process.exited = true;
        return true;
#endif
    }

    return false;
}

void close_process(Process_handle& process)
{
#ifdef _WIN32
    if (process.handle) {
        CloseHandle(process.handle);
        process.handle = nullptr;
    }
#else
    (void)process;
#endif
}

int child_main(int delay_ms)
{
    std::fprintf(stderr, "[crash_capture_child] delay=%dms\n", delay_ms);
    std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    sintra::test::precrash_pause("child-precrash");
    sintra::test::emit_self_stack_trace();
    sintra::test::trigger_stack_capture_crash("child");
}

} // namespace

int main(int argc, char** argv)
{
    sintra::detail::install_debug_pause_handlers();

    if (argc >= 2 && std::string(argv[1]) == "--child") {
        int delay_ms = sintra::test::read_env_int("SINTRA_CRASH_CAPTURE_CHILD_DELAY_MS", 50);
        if (argc >= 4 && std::string(argv[2]) == "--delay_ms") {
            delay_ms = std::max(0, std::atoi(argv[3]));
        }
        return child_main(delay_ms);
    }

    const int child_delay = pick_delay_ms(
        "SINTRA_CRASH_CAPTURE_CHILD_MIN_MS",
        "SINTRA_CRASH_CAPTURE_CHILD_MAX_MS",
        25,
        150);
    const int parent_delay = pick_delay_ms(
        "SINTRA_CRASH_CAPTURE_PARENT_MIN_MS",
        "SINTRA_CRASH_CAPTURE_PARENT_MAX_MS",
        150,
        350);

    std::vector<std::string> args;
    args.emplace_back("--child");
    args.emplace_back("--delay_ms");
    args.emplace_back(std::to_string(child_delay));

    Process_handle child;
    const std::string exe = argv[0];
    if (!spawn_process(exe, args, child)) {
        std::fprintf(stderr, "[crash_capture_parent] failed to spawn child\n");
        std::fflush(stderr);
        return 1;
    }

    std::fprintf(stderr,
                 "[crash_capture_parent] child_delay=%dms parent_delay=%dms\n",
                 child_delay,
                 parent_delay);
    std::fflush(stderr);

    const bool exited = wait_for_exit(child, 4000);
    if (!exited) {
        std::fprintf(stderr, "[crash_capture_parent] child did not exit within timeout\n");
    }
    else {
        std::fprintf(stderr, "[crash_capture_parent] child exit_code=%d\n", child.exit_code);
    }
    std::fflush(stderr);
    close_process(child);

    std::this_thread::sleep_for(std::chrono::milliseconds(parent_delay));
    sintra::test::precrash_pause("parent-precrash");
    sintra::test::emit_self_stack_trace();
    sintra::test::trigger_stack_capture_crash("parent");
}
