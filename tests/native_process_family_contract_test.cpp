// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#if defined(_WIN32) || defined(__linux__)

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr auto k_host_iid = sintra::compose_instance(63u, 1ull);

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

void mark(const fs::path& path, int value = 1)
{
    std::ofstream(path) << value << '\n';
}

bool await_file(const fs::path& path, Clock::time_point deadline)
{
    while (Clock::now() < deadline) {
        if (fs::exists(path)) {
            std::ifstream file(path);
            int value = 0;
            if (file >> value) {
                return true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

int read_pid(const fs::path& path)
{
    int pid = 0;
    std::ifstream(path) >> pid;
    return pid;
}

class Test_child
{
public:
    bool spawn(const std::string& binary, const std::vector<std::string>& args,
        uintptr_t job = 0)
    {
        std::vector<std::string> all_args{binary};
        all_args.insert(all_args.end(), args.begin(), args.end());
        sintra::C_string_vector argv(std::move(all_args));
        sintra::Spawn_detached_options options;
        options.prog = binary.c_str();
        options.argv = argv.v();
#ifdef _WIN32
        options.child_process_handle_out = &m_process;
        options.native_family_job = reinterpret_cast<HANDLE>(job);
#else
        (void)job;
#endif
        const auto result = sintra::detail::spawn_detached_with_result(options);
        m_pid = result.pid;
        return result.created();
    }

    bool wait_success(Clock::time_point deadline)
    {
#ifdef _WIN32
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (WaitForSingleObject(m_process, static_cast<DWORD>(std::max<int64_t>(0, remaining.count()))) !=
            WAIT_OBJECT_0)
        {
            return false;
        }
        DWORD code = 1;
        GetExitCodeProcess(m_process, &code);
        CloseHandle(m_process);
        m_process = nullptr;
        m_pid = 0;
        return code == 0;
#else
        while (Clock::now() < deadline) {
            int status = 0;
            if (waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = 0;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
#endif
    }

    ~Test_child()
    {
#ifdef _WIN32
        if (m_process) {
            TerminateProcess(m_process, 2);
            WaitForSingleObject(m_process, 5000);
            CloseHandle(m_process);
        }
#else
        if (m_pid > 0) {
            // This fixture alone waits its direct child, so its unreaped PID
            // remains an exact owned process even on an assertion failure.
            kill(m_pid, SIGKILL);
            while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
#endif
    }

private:
    int m_pid = 0;
#ifdef _WIN32
    HANDLE m_process = nullptr;
#endif
};

int run_leaf(const fs::path& directory)
{
#ifndef _WIN32
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    alarm(25);
#endif
    mark(directory / "leaf", sintra::test::get_pid());
    std::this_thread::sleep_for(25s);
    return 0;
}

int run_shell(const std::string& binary, const fs::path& directory)
{
#ifdef _WIN32
    Test_child leaf;
    if (!leaf.spawn(binary, {"--family-role", "leaf", "--family-directory", directory.string()})) {
        return 2;
    }
    mark(directory / "shell", sintra::test::get_pid());
    std::this_thread::sleep_for(25s);
    return 0;
#else
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    alarm(25);
    const pid_t intermediate = fork();
    if (intermediate < 0) {
        return 2;
    }
    if (intermediate == 0) {
        if (setsid() < 0) {
            _exit(2);
        }
        const pid_t leaf = fork();
        if (leaf < 0) {
            _exit(2);
        }
        if (leaf != 0) {
            _exit(0);
        }
        _exit(run_leaf(directory));
    }
    mark(directory / "shell", getpid());
    while (waitpid(intermediate, nullptr, 0) < 0 && errno == EINTR) {}
    std::this_thread::sleep_for(25s);
    return 0;
#endif
}

int run_host(int argc, char* argv[], const std::string& binary, const fs::path& directory)
{
    sintra::init(argc, argv);
#ifdef _WIN32
    HANDLE nested_job = CreateJobObjectW(nullptr, nullptr);
    if (!nested_job) {
        return 2;
    }
    Test_child shell;
    if (!shell.spawn(binary, {"--family-role", "shell", "--family-directory", directory.string()},
            reinterpret_cast<uintptr_t>(nested_job)))
    {
        return 2;
    }
    CloseHandle(nested_job);
#else
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        return 2;
    }
    const std::string slave_path = ptsname(master);
    const std::string directory_arg = directory.string();
    const pid_t shell = fork();
    if (shell < 0) {
        return 2;
    }
    if (shell == 0) {
        if (setsid() < 0) {
            _exit(2);
        }
        const int slave = open(slave_path.c_str(), O_RDWR);
        if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) != 0) {
            _exit(2);
        }
        close(master);
        execl(binary.c_str(), binary.c_str(), "--family-role", "shell",
            "--family-directory", directory_arg.c_str(), static_cast<char*>(nullptr));
        _exit(2);
    }
#endif
    mark(directory / "host", sintra::test::get_pid());
    std::this_thread::sleep_for(25s);
    std::_Exit(2);
}

bool wait_family_empty(const sintra::Managed_child_native_change_signal& changes)
{
    const auto deadline = Clock::now() + 8s;
    while (Clock::now() < deadline) {
        const auto generation = changes.generation();
        const auto status = sintra::native_family_status();
        if (status.native_empty) {
            return true;
        }
        changes.wait_for_change(generation, deadline);
    }
    const auto status = sintra::native_family_status();
    std::fprintf(stderr, "family error=%u operation=%s pending=%d members=%zu\n",
        status.native_error, status.failed_operation.c_str(), status.pending_launches,
        status.observed_process_ids.size());
    return false;
}

int run_pending(int argc, char* argv[])
{
    if (!check(sintra::activate_native_family(), "activate pending test family before init")) {
        return 2;
    }
    sintra::init(argc, argv);
    sintra::Managed_child_native_change_signal changes;
    sintra::observe_native_family_changes(changes);
    auto record = sintra::s_mproc->accept_child_custody();
    {
        auto launch = sintra::s_mproc->admit_child_custody_occurrence(record, k_host_iid, 0);
        sintra::s_mproc->request_child_custody_release(record);
        sintra::close_native_family_admission();
        const auto pending = sintra::native_family_status();
        if (!check(pending.pending_launches && !pending.native_empty,
                "cancelled precreation occurrence prevents empty before setup settlement"))
        {
            return 2;
        }
    }
    bool valid = check(wait_family_empty(changes), "no-child settlement wakes family observer");
    valid &= check(sintra::shutdown(), "pending fixture releases original custody");
    return valid ? 0 : 1;
}

int run_owner(int argc, char* argv[], const std::string& binary, const fs::path& directory)
{
    if (!check(sintra::activate_native_family(), "activate owner family before init")) {
        return 2;
    }
    sintra::init(argc, argv);
    sintra::Managed_child_native_change_signal changes;
    sintra::observe_native_family_changes(changes);
    sintra::Spawn_options options;
    options.binary_path = binary;
    options.args = {"--family-role", "host", "--family-directory", directory.string()};
    options.process_instance_id = k_host_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    bool valid = check(await_file(directory / "leaf", Clock::now() + 8s), "owned escaping descendant ready");
    valid &= check(await_file(directory / "shell", Clock::now() + 2s), "owned PTY shell ready");
    const int leaf = read_pid(directory / "leaf");
    const auto leaf_stamp = sintra::query_process_start_stamp(static_cast<uint32_t>(leaf));
    valid &= check(leaf_stamp.has_value(), "independent exact descendant identity");
    mark(directory / "ready");
    valid &= check(await_file(directory / "close", Clock::now() + 12s), "fixture global close received");
#ifndef _WIN32
    auto pending_record = sintra::s_mproc->accept_child_custody();
    std::optional<sintra::detail::Managed_child_launch_attempt> pending_launch;
    pending_launch.emplace(sintra::s_mproc->admit_child_custody_occurrence(
        pending_record, sintra::compose_instance(65u, 1ull), 0));
    pending_launch->reserve_posix_reap_slot();
    sintra::s_mproc->request_child_custody_release(pending_record);
#endif
    sintra::close_native_family_admission();
    const auto roots = custody.native_snapshot();
    valid &= check(roots.size() == 1, "one original managed host occurrence");
    std::shared_ptr<sintra::detail::Managed_child_custody_record> retained_ticket_record;
    if (roots.size() == 1) {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        const auto entry = sintra::s_mproc->m_child_custodies.find(roots.front().occurrence.custody_identity);
        if (entry != sintra::s_mproc->m_child_custodies.end()) {
            retained_ticket_record = entry->second;
        }
    }
    valid &= check(sintra::request_native_family_termination(Clock::now() + 100ms),
        "family request may wait alongside original root custody");
    const auto family_attempt_deadline = Clock::now() + 2s;
    while (Clock::now() < family_attempt_deadline) {
        const auto generation = changes.generation();
        if (!sintra::native_family_status().action_active) {
            break;
        }
        changes.wait_for_change(generation, family_attempt_deadline);
    }
    valid &= check(roots.size() == 1 && custody.native_snapshot().front().state ==
        sintra::Managed_child_native_state::RUNNING,
        "family action cannot bypass original root native authority");
    if (roots.size() == 1) {
        const auto action = custody.request_native_termination(roots.front().occurrence, Clock::now() + 3s);
        valid &= check(action.admission == sintra::Managed_child_native_admission::STARTED,
            "hard host death through exact original custody");
        const auto deadline = Clock::now() + 4s;
        while (Clock::now() < deadline && custody.native_snapshot().front().state !=
            sintra::Managed_child_native_state::EXITED)
        {
            const auto generation = changes.generation();
            if (custody.native_snapshot().front().state == sintra::Managed_child_native_state::EXITED) {
                break;
            }
            changes.wait_for_change(generation, deadline);
        }
    }
#ifndef _WIN32
    valid &= check(custody.native_snapshot().front().state == sintra::Managed_child_native_state::EXITED,
        "known root native exit remains observable while another launch handoff is unresolved");
    valid &= check(sintra::native_family_status().pending_launches,
        "unresolved cancelled handoff still prevents family-empty proof");
    pending_launch.reset();
#endif
    const auto after_host = sintra::native_family_status();
    valid &= check(!after_host.native_empty, "direct host exit does not prove descendant containment");
    valid &= check(leaf_stamp && sintra::test::managed_child::exact_process_is_live(leaf, *leaf_stamp),
        "HUP/TERM-ignoring detached descendant survives hard host death");
    if (retained_ticket_record) {
        sintra::Managed_child_native_action completed_action;
        {
            std::lock_guard<std::mutex> lock(retained_ticket_record->mutex);
            auto& occurrence = retained_ticket_record->occurrences.front();
            valid &= check(occurrence.native.exited(), "retained ticket fixture starts with actual root exit");
            completed_action = occurrence.native_action;
            // Reproduce the provider's broker-lifetime state: native root is
            // exited, but the original ticket still owns an active action.
            occurrence.native_action.outcome = sintra::Managed_child_native_outcome::ACTIVE;
        }
        valid &= check(custody.terminate_until(Clock::now() + 3s).release_state ==
            sintra::Managed_child_release_state::complete, "ordinary custody retires while native ticket retains record");
        {
            std::unique_lock<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
            valid &= check(sintra::s_mproc->m_child_custody_changed.wait_until(lock, Clock::now() + 3s, [&]() {
                return sintra::s_mproc->m_child_custodies.count(retained_ticket_record->identity) == 0;
            }),
                "ticket record is absent from ordinary active custody registry");
        }
        valid &= check(!sintra::s_mproc->native_family_roots_settled(),
            "family observes active native ticket after ordinary registry retirement");
        {
            std::lock_guard<std::mutex> lock(retained_ticket_record->mutex);
            retained_ticket_record->occurrences.front().native_action = completed_action;
        }
        retained_ticket_record->changed.notify_all();
        valid &= check(sintra::s_mproc->native_family_roots_settled(),
            "settled original ticket releases family action gate");
    }
    else {
        valid &= check(false, "original native record retained for family action ownership test");
    }
    valid &= check(sintra::request_native_family_termination(Clock::now() + 6s),
        "bounded whole-family force request admitted");
    valid &= check(wait_family_empty(changes), "iterative exact adoption kill reap establishes empty");
    valid &= check(leaf_stamp && !sintra::test::managed_child::exact_process_is_live(leaf, *leaf_stamp),
        "original detached descendant no longer exists");
#ifndef _WIN32
    siginfo_t info{};
    valid &= check(waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT | __WALL) < 0 && errno == ECHILD,
        "independent kernel ECHILD confirms complete reaping");
#endif
    valid &= check(custody.terminate_until(Clock::now() + 6s).release_state ==
        sintra::Managed_child_release_state::complete, "original occurrence alone settles release");
    valid &= check(sintra::shutdown(), "family owner runtime shutdown");
    mark(directory / "result", valid ? 1 : 2);
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto binary = sintra::test::get_binary_path(argc, argv);
    const auto role = sintra::test::get_argv_value(argc, argv, "--family-role");
    const fs::path directory = sintra::test::get_argv_value(argc, argv, "--family-directory");
    if (role == "leaf")    { return run_leaf(directory); }
    if (role == "shell")   { return run_shell(binary, directory); }
    if (role == "host")    { return run_host(argc, argv, binary, directory); }
    if (role == "pending") { return run_pending(argc, argv); }
    if (role == "owner")   { return run_owner(argc, argv, binary, directory); }

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "native_family");
    const fs::path root = fs::temp_directory_path() /
        ("sintra_family_" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(root / "first");
    fs::create_directories(root / "second");
    bool valid = true;
#ifdef _WIN32
    const std::string invalid_binary = (root / "invalid-image.exe").string();
    std::ofstream(invalid_binary) << "This fixture is not an executable image.\n";
    const char* invalid_argv[] = {invalid_binary.c_str(), nullptr};
    sintra::Spawn_detached_options invalid_options;
    invalid_options.prog = invalid_binary.c_str();
    invalid_options.argv = invalid_argv;
    const std::wstring invalid_image = (root / "invalid-image.exe").wstring();
    const std::wstring oracle_command = L"\"" + invalid_image + L"\"";
    std::vector<wchar_t> oracle_buffer(oracle_command.begin(), oracle_command.end());
    oracle_buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL oracle_created = CreateProcessW(invalid_image.c_str(), oracle_buffer.data(),
        nullptr, nullptr, TRUE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process);
    const DWORD oracle_error = oracle_created ? ERROR_SUCCESS : GetLastError();
    if (process.hThread) {
        CloseHandle(process.hThread);
    }
    if (process.hProcess) {
        CloseHandle(process.hProcess);
    }
    valid &= check(!oracle_created, "direct native invalid-image oracle fails");
    const auto invalid_result = sintra::detail::spawn_detached_with_result(invalid_options);
    std::fprintf(stderr, "INVALID_IMAGE_NATIVE_ERROR created=%d value=%d category=%s message='%s' oracle=%lu\n",
        invalid_result.created(), invalid_result.error.value(), invalid_result.error.category().name(),
        invalid_result.error.message().c_str(), static_cast<unsigned long>(oracle_error));
    valid &= check(!invalid_result.created() &&
        invalid_result.error.category() == std::system_category() &&
        invalid_result.error.value() == static_cast<int>(oracle_error),
        "spawn failure retains actual Win32 error and category instead of mapped CRT errno");
#endif
    Test_child pending;
    valid &= check(pending.spawn(binary, {"--family-role", "pending"}), "spawn pending owner");
    valid &= check(pending.wait_success(Clock::now() + 12s), "pending native creation settlement contract");
    Test_child first;
    Test_child second;
    valid &= check(first.spawn(binary, {"--family-role", "owner", "--family-directory", (root / "first").string()}),
        "spawn first independent profile owner");
    valid &= check(second.spawn(binary, {"--family-role", "owner", "--family-directory", (root / "second").string()}),
        "spawn second independent profile owner");
    valid &= check(await_file(root / "first" / "ready", Clock::now() + 10s), "first owner ready");
    valid &= check(await_file(root / "second" / "ready", Clock::now() + 10s), "second owner ready");
    const int second_leaf = read_pid(root / "second" / "leaf");
    const auto second_stamp = sintra::query_process_start_stamp(static_cast<uint32_t>(second_leaf));
    mark(root / "first" / "close");
    valid &= check(first.wait_success(Clock::now() + 12s), "first family closes completely");
    valid &= check(second_stamp && sintra::test::managed_child::exact_process_is_live(second_leaf, *second_stamp),
        "first profile cleanup leaves second profile descendant alive");
    mark(root / "second" / "close");
    valid &= check(second.wait_success(Clock::now() + 12s), "second family closes independently");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    return valid ? 0 : 1;
}

#else

#include <cerrno>
#include <cstdio>

int main()
{
    const bool activated = sintra::activate_native_family();
    const auto status = sintra::native_family_status();
    if (activated || status.active || status.native_empty || status.native_error != ENOTSUP ||
        status.failed_operation.empty())
    {
        std::fprintf(stderr, "FAIL: unavailable native family must report ENOTSUP without activation "
            "or an empty-family claim: activated=%d active=%d empty=%d error=%u operation=%s\n",
            activated, status.active, status.native_empty, status.native_error, status.failed_operation.c_str());
        return 1;
    }
    return 0;
}

#endif
