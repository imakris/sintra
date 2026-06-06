#include <sintra/sintra.h>

#include "test_ring_utils.h"
#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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

constexpr const char* k_child_arg      = "--ring-lifecycle-anchor-child";
constexpr const char* k_dir_arg        = "--ring-lifecycle-anchor-dir";
constexpr const char* k_role_arg       = "--ring-lifecycle-anchor-role";
constexpr const char* k_failure_prefix = "ring_lifecycle_anchor_race_process_test: ";
constexpr const char* k_ring_name      = "ring_data";

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

bool set_env_var(const char* name, const std::string& value)
{
#ifdef _WIN32
    return SetEnvironmentVariableA(name, value.c_str()) != 0;
#else
    return ::setenv(name, value.c_str(), 1) == 0;
#endif
}

void unset_env_var(const char* name)
{
#ifdef _WIN32
    (void)SetEnvironmentVariableA(name, nullptr);
#else
    (void)::unsetenv(name);
#endif
}

launched_process_t launch_child(
    const std::string&           binary_path,
    const std::filesystem::path& directory,
    std::string_view             role)
{
    std::vector<std::string> args = {
        binary_path,
        k_child_arg,
        k_dir_arg,
        directory.string(),
        k_role_arg,
        std::string(role)
    };

    sintra::C_string_vector cargs(args);
    sintra::Spawn_detached_options options;
    int child_pid = -1;
    options.prog          = binary_path.c_str();
    options.argv          = cargs.v();
    options.child_pid_out = &child_pid;
    return {sintra::spawn_detached(options), child_pid};
}

process_exit_t wait_for_process_exit(
    int                       pid,
    std::chrono::milliseconds timeout,
    bool                      terminate_on_timeout)
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

    const DWORD result = WaitForSingleObject(handle, static_cast<DWORD>(timeout.count()));
    if (result == WAIT_OBJECT_0) {
        DWORD      exit_code      = 1;
        const bool have_exit_code = GetExitCodeProcess(handle, &exit_code) != 0;
        CloseHandle(handle);
        return {true, have_exit_code && exit_code == 0, static_cast<int>(exit_code)};
    }
    if (result == WAIT_TIMEOUT) {
        if (terminate_on_timeout) {
            TerminateProcess(handle, 1);
            WaitForSingleObject(handle, 2000);
        }
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

    if (!terminate_on_timeout) {
        return {false, false, -1};
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

bool read_control_attached(
    const std::filesystem::path& control_file,
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

int run_holder(const std::filesystem::path& directory)
{
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    try {
        {
            sintra::Ring_W<std::uint32_t> writer(
                directory.string(),
                k_ring_name,
                ring_elements);
            touch_file(marker_path(directory, "holder_attached"));

            if (!wait_for_file(
                    marker_path(directory, "release_holder"),
                    std::chrono::steady_clock::now() + std::chrono::seconds(10)))
            {
                write_text(marker_path(directory, "holder_status"), "timeout:release");
                return 1;
            }

            touch_file(marker_path(directory, "holder_releasing"));
        }

        write_text(marker_path(directory, "holder_status"), "done");
    }
    catch (const std::exception& e) {
        try {
            write_text(marker_path(directory, "holder_status"), std::string("exception:") + e.what());
        }
        catch (...) {
        }
        return 1;
    }
    catch (...) {
        try {
            write_text(marker_path(directory, "holder_status"), "exception:unknown");
        }
        catch (...) {
        }
        return 1;
    }

    return 0;
}

int run_joiner(const std::filesystem::path& directory)
{
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    try {
        {
            sintra::Ring_R<std::uint32_t> reader(
                directory.string(),
                k_ring_name,
                ring_elements,
                ring_elements / 2);
            write_text(marker_path(directory, "joiner_status"), "attached");
            touch_file(marker_path(directory, "joiner_attached"));

            if (!wait_for_file(
                    marker_path(directory, "release_joiner"),
                    std::chrono::steady_clock::now() + std::chrono::seconds(10)))
            {
                write_text(marker_path(directory, "joiner_status"), "timeout:release");
                return 1;
            }
        }
    }
    catch (const std::exception& e) {
        try {
            write_text(marker_path(directory, "joiner_status"), std::string("exception:") + e.what());
        }
        catch (...) {
        }
        return 1;
    }
    catch (...) {
        try {
            write_text(marker_path(directory, "joiner_status"), "exception:unknown");
        }
        catch (...) {
        }
        return 1;
    }

    return 0;
}

int run_child(int argc, char* argv[])
{
    const auto directory = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);

    if (role == "holder") {
        return run_holder(directory);
    }
    if (role == "joiner") {
        return run_joiner(directory);
    }

    std::fprintf(stderr, "%sunknown child role: %s\n", k_failure_prefix, role.c_str());
    return 1;
}

int run_parent(const std::string& binary_path)
{
    sintra::test::Temp_ring_dir temp("ring_lifecycle_anchor_race_process");

    const auto data_file      = temp.path / k_ring_name;
    const auto control_file   = temp.path / (std::string(k_ring_name) + "_control");
    const auto lifecycle_file = temp.path / (std::string(k_ring_name) + "_lifecycle");
    const auto paused_file    = marker_path(temp.path, "joiner_paused");
    const auto resume_file    = marker_path(temp.path, "resume_joiner");
    const auto release_waiting_file =
        marker_path(temp.path, "holder_waiting_for_lifecycle");
    const auto release_locked_file =
        marker_path(temp.path, "holder_locked_lifecycle");

    bool ok = true;

    const std::string hook_data_file = temp.path.string() + "/" + k_ring_name;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE", hook_data_file) && ok;
    ok = set_env_var(
        "SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE",
        release_waiting_file.string()) && ok;
    ok = set_env_var(
        "SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE",
        release_locked_file.string()) && ok;

    auto holder = launch_child(binary_path, temp.path, "holder");

    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE");

    if (!holder.launched) {
        std::fprintf(stderr, "%sfailed to launch holder\n", k_failure_prefix);
        return 1;
    }

    if (!wait_for_file(
            marker_path(temp.path, "holder_attached"),
            std::chrono::steady_clock::now() + std::chrono::seconds(10)))
    {
        std::fprintf(stderr, "%sholder did not attach\n", k_failure_prefix);
        ok = false;
    }

    ok = set_env_var("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE", hook_data_file) && ok;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_PAUSED_FILE", paused_file.string()) && ok;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_RESUME_FILE", resume_file.string()) && ok;

    auto joiner = launch_child(binary_path, temp.path, "joiner");

    unset_env_var("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_PAUSED_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RESUME_FILE");

    if (!joiner.launched) {
        std::fprintf(stderr, "%sfailed to launch joiner\n", k_failure_prefix);
        ok = false;
    }

    if (ok &&
        !wait_for_file(paused_file, std::chrono::steady_clock::now() + std::chrono::seconds(10)))
    {
        std::fprintf(stderr, "%sjoiner did not pause in lifecycle window\n", k_failure_prefix);
        ok = false;
    }

    if (ok) {
        touch_file(marker_path(temp.path, "release_holder"));
        if (!wait_for_file(
                release_waiting_file,
                std::chrono::steady_clock::now() + std::chrono::seconds(10)))
        {
            std::fprintf(stderr, "%sholder did not reach lifecycle release lock\n", k_failure_prefix);
            ok = false;
        }
    }

    if (ok) {
        if (wait_for_file(
                release_locked_file,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(500)))
        {
            std::fprintf(stderr,
                "%sholder acquired lifecycle lock before paused joiner resumed\n",
                k_failure_prefix);
            ok = false;
        }

        const auto early_exit =
            wait_for_process_exit(holder.pid, std::chrono::milliseconds(500), false);
        if (early_exit.exited) {
            std::fprintf(stderr,
                "%sholder exited before paused joiner resumed\n",
                k_failure_prefix);
            ok = false;
        }
    }

    if (ok) {
        std::uint64_t observed_attached = 0;
        if (!std::filesystem::exists(data_file) ||
            !std::filesystem::exists(control_file) ||
            !std::filesystem::exists(lifecycle_file))
        {
            std::fprintf(stderr, "%sshared ring file missing while joiner is paused\n", k_failure_prefix);
            ok = false;
        }
        else if (!read_control_attached(control_file, observed_attached)) {
            std::fprintf(stderr, "%sfailed to read control while joiner is paused\n", k_failure_prefix);
            ok = false;
        }
        else if (observed_attached != 1) {
            std::fprintf(stderr,
                "%snum_attached changed before paused joiner reached control: %llu\n",
                k_failure_prefix,
                static_cast<unsigned long long>(observed_attached));
            ok = false;
        }
    }

    try {
        touch_file(resume_file);
    }
    catch (...) {
        ok = false;
    }

    if (joiner.launched &&
        !wait_for_file(
            marker_path(temp.path, "joiner_attached"),
            std::chrono::steady_clock::now() + std::chrono::seconds(10)))
    {
        std::fprintf(stderr, "%sjoiner did not finish attaching\n", k_failure_prefix);
        ok = false;
    }

    if (holder.launched) {
        const auto holder_exit =
            wait_for_process_exit(holder.pid, std::chrono::seconds(10), true);
        if (!holder_exit.exited || !holder_exit.normal) {
            std::fprintf(stderr,
                "%sholder did not exit cleanly, code %d\n",
                k_failure_prefix,
                holder_exit.code);
            ok = false;
        }
    }

    if (!std::filesystem::exists(release_locked_file)) {
        std::fprintf(stderr, "%sholder never acquired lifecycle lock after resume\n", k_failure_prefix);
        ok = false;
    }

    if (ok) {
        std::uint64_t observed_attached = 0;
        if (!read_control_attached(control_file, observed_attached)) {
            std::fprintf(stderr, "%sfailed to read control after holder release\n", k_failure_prefix);
            ok = false;
        }
        else if (observed_attached != 1) {
            std::fprintf(stderr,
                "%snum_attached after holder release: expected 1 observed %llu\n",
                k_failure_prefix,
                static_cast<unsigned long long>(observed_attached));
            ok = false;
        }
    }

    try {
        touch_file(marker_path(temp.path, "release_joiner"));
    }
    catch (...) {
        ok = false;
    }

    if (joiner.launched) {
        const auto joiner_exit =
            wait_for_process_exit(joiner.pid, std::chrono::seconds(10), true);
        if (!joiner_exit.exited || !joiner_exit.normal) {
            std::fprintf(stderr,
                "%sjoiner did not exit cleanly, code %d\n",
                k_failure_prefix,
                joiner_exit.code);
            ok = false;
        }
    }

    for (int retry = 0; retry < 40; ++retry) {
        if (!std::filesystem::exists(control_file) &&
            !std::filesystem::exists(data_file))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (std::filesystem::exists(control_file)) {
        std::fprintf(stderr, "%scontrol file remained after final release\n", k_failure_prefix);
        ok = false;
    }
    if (std::filesystem::exists(data_file)) {
        std::fprintf(stderr, "%sdata file remained after final release\n", k_failure_prefix);
        ok = false;
    }
    if (!std::filesystem::exists(lifecycle_file)) {
        std::fprintf(stderr, "%slifecycle anchor missing after final release\n", k_failure_prefix);
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

    const auto holder_status = read_first_line(marker_path(temp.path, "holder_status"));
    if (holder_status != "done") {
        std::fprintf(stderr, "%sholder status: %s\n", k_failure_prefix, holder_status.c_str());
        ok = false;
    }

    const auto joiner_status = read_first_line(marker_path(temp.path, "joiner_status"));
    if (joiner_status != "attached") {
        std::fprintf(stderr, "%sjoiner status: %s\n", k_failure_prefix, joiner_status.c_str());
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
