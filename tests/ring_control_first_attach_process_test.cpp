#include <sintra/sintra.h>

#include "test_ring_utils.h"
#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace {

constexpr const char* k_child_arg      = "--ring-control-first-attach-child";
constexpr const char* k_dir_arg        = "--ring-control-first-attach-dir";
constexpr const char* k_index_arg      = "--ring-control-first-attach-index";
constexpr const char* k_failure_prefix = "ring_control_first_attach_process_test: ";
constexpr const char* k_ring_name      = "ring_data";

constexpr int k_child_count = 16;

struct launched_process_t
{
    bool launched = false;
    int  pid      = -1;
};

struct process_exit_t
{
    bool exited = false;
    bool normal = false;
    int  code   = -1;
};

std::filesystem::path marker_path(
    const std::filesystem::path& directory,
    std::string_view             name,
    int                          index)
{
    return directory / (std::string(name) + "_" + std::to_string(index));
}

std::filesystem::path marker_path(
    const std::filesystem::path& directory,
    std::string_view             name)
{
    return directory / std::string(name);
}

void touch_file(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create marker " + path.string());
    }
    out << "ok\n";
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write " + path.string());
    }
    out << text << '\n';
}

std::string read_first_line(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::string   line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool wait_for_file(
    const std::filesystem::path& path,
    std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool wait_for_all_markers(
    const std::filesystem::path& directory,
    std::string_view             name,
    int                          count,
    std::chrono::milliseconds    timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (int i = 0; i < count; ++i) {
        if (!wait_for_file(marker_path(directory, name, i), deadline)) {
            std::fprintf(stderr,
                "%smissing marker %.*s_%d\n",
                k_failure_prefix,
                static_cast<int>(name.size()),
                name.data(),
                i);
            return false;
        }
    }
    return true;
}

launched_process_t launch_child(
    const std::string&           binary_path,
    const std::filesystem::path& directory,
    int                          index)
{
    std::vector<std::string> args = {
        binary_path,
        k_child_arg,
        k_dir_arg,
        directory.string(),
        k_index_arg,
        std::to_string(index)
    };

    sintra::C_string_vector cargs(args);
    sintra::Spawn_detached_options options;
    int child_pid = -1;
    options.prog          = binary_path.c_str();
    options.argv          = cargs.v();
    options.child_pid_out = &child_pid;
    return {sintra::spawn_detached(options), child_pid};
}

process_exit_t wait_for_process_exit(int pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return {true, true, 0};
    }

#ifdef _WIN32
    HANDLE handle = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!handle) {
        return {true, true, 0};
    }

    const DWORD wait_ms = static_cast<DWORD>(timeout.count());
    const DWORD result  = WaitForSingleObject(handle, wait_ms);
    if (result == WAIT_OBJECT_0) {
        DWORD      exit_code      = 1;
        const bool have_exit_code = GetExitCodeProcess(handle, &exit_code) != 0;
        CloseHandle(handle);
        return {true, have_exit_code && exit_code == 0, static_cast<int>(exit_code)};
    }
    if (result == WAIT_TIMEOUT) {
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 2000);
        CloseHandle(handle);
        return {false, false, -1};
    }
    CloseHandle(handle);
    return {false, false, -1};
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int         status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid)) {
            return {
                true,
                WIFEXITED(status) && WEXITSTATUS(status) == 0,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1
            };
        }
        if (result == 0 || errno == EINTR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        return {true, false, -1};
    }

    const auto child_pid = static_cast<pid_t>(pid);
    if (::kill(child_pid, SIGTERM) == 0) {
        const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < reap_deadline) {
            int         status = 0;
            const pid_t result = ::waitpid(child_pid, &status, WNOHANG);
            if (result == child_pid) {
                return {false, false, -1};
            }
            if (result == -1 && errno != EINTR) {
                return {false, false, -1};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        (void)::kill(child_pid, SIGKILL);
        int status = 0;
        while (::waitpid(child_pid, &status, 0) == -1 && errno == EINTR) {
            continue;
        }
    }
    return {false, false, -1};
#endif
}

bool read_control_state(
    const std::filesystem::path& control_file,
    std::uint64_t&               fingerprint,
    std::uint64_t&               num_attached)
{
    using control_type = sintra::Ring<std::uint32_t, true>::Control;

    std::error_code ec;
    if (std::filesystem::file_size(control_file, ec) != sizeof(control_type) || ec) {
        return false;
    }

    try {
        sintra::ipc::file_mapping fm_control(control_file, sintra::ipc::read_write);
        sintra::ipc::mapped_region control_region(
            fm_control,
            sintra::ipc::read_write,
            0,
            0);
        const auto* control = static_cast<const control_type*>(control_region.data());
        fingerprint = control->abi_fingerprint.load(std::memory_order_acquire);
        num_attached = static_cast<std::uint64_t>(
            control->num_attached.load(std::memory_order_acquire));
        return true;
    }
    catch (...) {
        return false;
    }
}

bool read_u64_prefix(
    const std::filesystem::path& path,
    std::uint64_t&               value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

bool has_temp_ring_publish_files(const std::filesystem::path& directory)
{
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto filename = entry.path().filename().string();
        if (filename.find("_control.tmp.") != std::string::npos ||
            filename.find("_lifecycle.tmp.") != std::string::npos)
        {
            std::fprintf(stderr,
                "%stemporary ring publish file remains: %s\n",
                k_failure_prefix,
                filename.c_str());
            return true;
        }
    }
    return false;
}

int run_child(int argc, char* argv[])
{
    const auto directory = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const int index = std::stoi(
        sintra::test::get_argv_value(argc, argv, k_index_arg, "-1"));
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    try {
        touch_file(marker_path(directory, "ready", index));
        if (!wait_for_file(marker_path(directory, "go"), std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
            write_text(marker_path(directory, "status", index), "timeout:go");
            return 1;
        }

        sintra::Ring_R<std::uint32_t> reader(
            directory.string(),
            k_ring_name,
            ring_elements,
            ring_elements / 2);
        write_text(marker_path(directory, "status", index), "attached");
        touch_file(marker_path(directory, "attached", index));

        if (!wait_for_file(marker_path(directory, "release"), std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
            write_text(marker_path(directory, "status", index), "timeout:release");
            return 1;
        }
    }
    catch (const std::exception& e) {
        try {
            write_text(marker_path(directory, "status", index), std::string("exception:") + e.what());
        }
        catch (...) {
        }
        return 1;
    }
    catch (...) {
        try {
            write_text(marker_path(directory, "status", index), "exception:unknown");
        }
        catch (...) {
        }
        return 1;
    }

    return 0;
}

int run_parent(const std::string& binary_path)
{
    sintra::test::Temp_ring_dir temp("ring_control_first_attach_process");
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    std::vector<launched_process_t> children;
    children.reserve(k_child_count);
    for (int i = 0; i < k_child_count; ++i) {
        auto child = launch_child(binary_path, temp.path, i);
        if (!child.launched) {
            std::fprintf(stderr, "%sfailed to launch child %d\n", k_failure_prefix, i);
            return 1;
        }
        children.push_back(child);
    }

    bool ok = wait_for_all_markers(temp.path, "ready", k_child_count, std::chrono::seconds(10));
    if (ok) {
        touch_file(marker_path(temp.path, "go"));
        ok = wait_for_all_markers(
            temp.path,
            "attached",
            k_child_count,
            std::chrono::seconds(20));
    }

    const auto control_file = temp.path / (std::string(k_ring_name) + "_control");
    const auto lifecycle_file = temp.path / (std::string(k_ring_name) + "_lifecycle");
    if (ok) {
        std::uint64_t observed_fingerprint = 0;
        std::uint64_t observed_attached    = 0;
        if (!read_control_state(
                control_file,
                observed_fingerprint,
                observed_attached))
        {
            std::fprintf(stderr, "%sfailed to read control header\n", k_failure_prefix);
            ok = false;
        }
        else if (observed_fingerprint != sintra::detail::k_ring_abi_fingerprint) {
            std::fprintf(stderr, "%scontrol fingerprint mismatch\n", k_failure_prefix);
            ok = false;
        }
        else if (observed_attached != static_cast<std::uint64_t>(k_child_count)) {
            std::fprintf(stderr,
                "%snum_attached mismatch: expected %d observed %llu\n",
                k_failure_prefix,
                k_child_count,
                static_cast<unsigned long long>(observed_attached));
            ok = false;
        }

        if (!std::filesystem::exists(lifecycle_file)) {
            std::fprintf(stderr, "%slifecycle anchor missing after attach\n", k_failure_prefix);
            ok = false;
        }
        else {
            std::uint64_t observed_anchor_fingerprint = 0;
            if (!read_u64_prefix(lifecycle_file, observed_anchor_fingerprint)) {
                std::fprintf(stderr, "%sfailed to read lifecycle anchor fingerprint\n", k_failure_prefix);
                ok = false;
            }
            else if (observed_anchor_fingerprint !=
                     sintra::detail::k_ring_lifecycle_anchor_fingerprint)
            {
                std::fprintf(stderr, "%slifecycle anchor fingerprint mismatch\n", k_failure_prefix);
                ok = false;
            }
        }

        if (has_temp_ring_publish_files(temp.path)) {
            ok = false;
        }
    }

    touch_file(marker_path(temp.path, "release"));

    for (const auto& child : children) {
        const auto child_exit = wait_for_process_exit(child.pid, std::chrono::seconds(10));
        if (!child_exit.exited || !child_exit.normal) {
            std::fprintf(stderr,
                "%schild pid %d did not exit cleanly, code %d\n",
                k_failure_prefix,
                child.pid,
                child_exit.code);
            ok = false;
        }
    }

    for (int i = 0; i < k_child_count; ++i) {
        const auto status = read_first_line(marker_path(temp.path, "status", i));
        if (status != "attached") {
            std::fprintf(stderr,
                "%schild %d status: %s\n",
                k_failure_prefix,
                i,
                status.c_str());
            ok = false;
        }
    }

    for (int retry = 0; retry < 20; ++retry) {
        if (!std::filesystem::exists(control_file) &&
            !std::filesystem::exists(temp.path / k_ring_name))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (std::filesystem::exists(control_file)) {
        std::fprintf(stderr, "%scontrol file remained after release\n", k_failure_prefix);
        ok = false;
    }
    if (std::filesystem::exists(temp.path / k_ring_name)) {
        std::fprintf(stderr, "%sdata file remained after release\n", k_failure_prefix);
        ok = false;
    }
    if (!std::filesystem::exists(lifecycle_file)) {
        std::fprintf(stderr, "%slifecycle anchor missing after release\n", k_failure_prefix);
        ok = false;
    }
    else {
        std::uint64_t observed_anchor_fingerprint = 0;
        if (!read_u64_prefix(lifecycle_file, observed_anchor_fingerprint)) {
            std::fprintf(stderr, "%sfailed to read lifecycle anchor fingerprint after release\n", k_failure_prefix);
            ok = false;
        }
        else if (observed_anchor_fingerprint !=
                 sintra::detail::k_ring_lifecycle_anchor_fingerprint)
        {
            std::fprintf(stderr, "%slifecycle anchor fingerprint mismatch after release\n", k_failure_prefix);
            ok = false;
        }
    }
    if (has_temp_ring_publish_files(temp.path)) {
        ok = false;
    }

    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_child_arg)) {
        return run_child(argc, argv);
    }

    return run_parent(sintra::test::get_binary_path(argc, argv));
}
