#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#endif

namespace {

constexpr const char* k_child_arg      = "--ring-lifecycle-anchor-child";
constexpr const char* k_dir_arg        = "--ring-lifecycle-anchor-dir";
constexpr const char* k_role_arg       = "--ring-lifecycle-anchor-role";
constexpr const char* k_failure_prefix = "ring_lifecycle_anchor_race_process_test: ";
constexpr const char* k_ring_name      = "ring_data";

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

bool launch_child(
    const std::string&           binary_path,
    const std::filesystem::path& directory,
    std::string_view             role,
    sintra::test::Exact_child&   child)
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
    return child.spawn(binary_path.c_str(), cargs.v());
}

bool settle_after_failure(
    sintra::test::Exact_child& child,
    const char*                role)
{
    std::string diagnostic;
    const bool settled = child.terminate_and_settle(diagnostic);
    if (!settled) {
        std::fprintf(stderr,
            "%sfailed to settle %s child: %s\n",
            k_failure_prefix,
            role,
            diagnostic.c_str());
    }
    return settled;
}

class child_settlement_guard
{
public:
    child_settlement_guard(
        sintra::test::Exact_child& holder,
        sintra::test::Exact_child& joiner) noexcept
        : m_holder(holder)
        , m_joiner(joiner)
    {}

    ~child_settlement_guard()
    {
        if (!m_active) {
            return;
        }
        try {
            (void)settle_after_failure(m_joiner, "joiner");
            (void)settle_after_failure(m_holder, "holder");
        }
        catch (...) {
            std::fprintf(stderr,
                "%sexception while settling children during parent unwind\n",
                k_failure_prefix);
        }
    }

    void dismiss() noexcept { m_active = false; }

private:
    sintra::test::Exact_child& m_holder;
    sintra::test::Exact_child& m_joiner;
    bool                       m_active = true;
};

bool remains_running_for(
    sintra::test::Exact_child& child,
    std::chrono::milliseconds  duration,
    const char*                role)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = child.poll();
        if (state == sintra::test::Exact_child_state::exited) {
            const auto status = child.describe_status();
            std::string settle_diagnostic;
            const bool settled = child.settle_observed_exit(settle_diagnostic);
            std::fprintf(stderr,
                "%s%s child exited during required-running window: %s%s%s\n",
                k_failure_prefix,
                role,
                status.c_str(),
                settled ? "" : "; settlement failed: ",
                settled ? "" : settle_diagnostic.c_str());
            return false;
        }
        if (state == sintra::test::Exact_child_state::error) {
            const auto observation = child.describe_status();
            std::string cleanup_diagnostic;
            const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
            std::fprintf(stderr,
                "%s%s child observation failed during required-running window: %s; cleanup %s%s%s\n",
                k_failure_prefix,
                role,
                observation.c_str(),
                cleaned ? "settled" : "failed",
                cleanup_diagnostic.empty() ? "" : ": ",
                cleanup_diagnostic.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool wait_for_clean_child_exit(
    sintra::test::Exact_child& child,
    std::chrono::milliseconds  timeout,
    const char*                role)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = child.poll();
        if (state == sintra::test::Exact_child_state::exited) {
            const bool clean_exit = child.exited_with_code(0);
            const auto status     = child.describe_status();
            std::string settle_diagnostic;
            const bool settled = child.settle_observed_exit(settle_diagnostic);
            if (!clean_exit || !settled) {
                std::fprintf(stderr,
                    "%s%s child did not exit cleanly: %s%s%s\n",
                    k_failure_prefix,
                    role,
                    status.c_str(),
                    settled ? "" : "; settlement failed: ",
                    settled ? "" : settle_diagnostic.c_str());
            }
            return clean_exit && settled;
        }
        if (state == sintra::test::Exact_child_state::error) {
            const auto observation = child.describe_status();
            std::string cleanup_diagnostic;
            const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
            std::fprintf(stderr,
                "%s%s child observation failed: %s; cleanup %s%s%s\n",
                k_failure_prefix,
                role,
                observation.c_str(),
                cleaned ? "settled" : "failed",
                cleanup_diagnostic.empty() ? "" : ": ",
                cleanup_diagnostic.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string cleanup_diagnostic;
    const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
    std::fprintf(stderr,
        "%s%s child timed out; cleanup %s%s%s\n",
        k_failure_prefix,
        role,
        cleaned ? "settled" : "failed",
        cleanup_diagnostic.empty() ? "" : ": ",
        cleanup_diagnostic.c_str());
    return false;
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

            if (!sintra::test::wait_for_file(
                    marker_path(directory, "release_holder"),
                    std::chrono::seconds(10),
                    std::chrono::milliseconds(5)))
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

            if (!sintra::test::wait_for_file(
                    marker_path(directory, "release_joiner"),
                    std::chrono::seconds(10),
                    std::chrono::milliseconds(5)))
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
    bool holder_requires_final_wait = true;
    sintra::test::Exact_child holder(std::chrono::seconds(2));
    sintra::test::Exact_child joiner(std::chrono::seconds(2));
    child_settlement_guard    settlement_guard(holder, joiner);

    const std::string hook_data_file = temp.path.string() + "/" + k_ring_name;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE", hook_data_file) && ok;
    ok = set_env_var(
        "SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE",
        release_waiting_file.string()) && ok;
    ok = set_env_var(
        "SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE",
        release_locked_file.string()) && ok;

    const bool holder_launched = launch_child(
        binary_path,
        temp.path,
        "holder",
        holder);

    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_DATA_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_WAITING_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RELEASE_LOCKED_FILE");

    if (!holder_launched) {
        const auto spawn_error = holder.error();
        (void)settle_after_failure(holder, "holder");
        settlement_guard.dismiss();
        std::fprintf(stderr,
            "%sfailed to launch holder: %s\n",
            k_failure_prefix,
            spawn_error.c_str());
        return 1;
    }

    if (!sintra::test::wait_for_file(
            marker_path(temp.path, "holder_attached"),
            std::chrono::seconds(10),
            std::chrono::milliseconds(5)))
    {
        std::fprintf(stderr, "%sholder did not attach\n", k_failure_prefix);
        ok = false;
    }

    ok = set_env_var("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE", hook_data_file) && ok;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_PAUSED_FILE", paused_file.string()) && ok;
    ok = set_env_var("SINTRA_RING_LIFECYCLE_RESUME_FILE", resume_file.string()) && ok;

    const bool joiner_launched = launch_child(
        binary_path,
        temp.path,
        "joiner",
        joiner);

    unset_env_var("SINTRA_RING_LIFECYCLE_PAUSE_DATA_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_PAUSED_FILE");
    unset_env_var("SINTRA_RING_LIFECYCLE_RESUME_FILE");

    if (!joiner_launched) {
        const auto spawn_error = joiner.error();
        (void)settle_after_failure(joiner, "joiner");
        (void)settle_after_failure(holder, "holder");
        settlement_guard.dismiss();
        std::fprintf(stderr,
            "%sfailed to launch joiner: %s\n",
            k_failure_prefix,
            spawn_error.c_str());
        return 1;
    }

    if (ok &&
        !sintra::test::wait_for_file(
            paused_file,
            std::chrono::seconds(10),
            std::chrono::milliseconds(5)))
    {
        std::fprintf(stderr, "%sjoiner did not pause in lifecycle window\n", k_failure_prefix);
        ok = false;
    }

    if (ok) {
        touch_file(marker_path(temp.path, "release_holder"));
        if (!sintra::test::wait_for_file(
                release_waiting_file,
                std::chrono::seconds(10),
                std::chrono::milliseconds(5)))
        {
            std::fprintf(stderr, "%sholder did not reach lifecycle release lock\n", k_failure_prefix);
            ok = false;
        }
    }

    if (ok) {
        if (sintra::test::wait_for_file(
                release_locked_file,
                std::chrono::milliseconds(500),
                std::chrono::milliseconds(5)))
        {
            std::fprintf(stderr,
                "%sholder acquired lifecycle lock before paused joiner resumed\n",
                k_failure_prefix);
            ok = false;
        }

        if (!remains_running_for(
                holder,
                std::chrono::milliseconds(500),
                "holder"))
        {
            holder_requires_final_wait = false;
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

    if (!sintra::test::wait_for_file(
            marker_path(temp.path, "joiner_attached"),
            std::chrono::seconds(10),
            std::chrono::milliseconds(5)))
    {
        std::fprintf(stderr, "%sjoiner did not finish attaching\n", k_failure_prefix);
        ok = false;
    }

    if (holder_requires_final_wait &&
        !wait_for_clean_child_exit(
            holder,
            std::chrono::seconds(10),
            "holder"))
    {
        ok = false;
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

    if (!wait_for_clean_child_exit(
            joiner,
            std::chrono::seconds(10),
            "joiner"))
    {
        ok = false;
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

    settlement_guard.dismiss();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_child_arg)) {
        return run_child(argc, argv);
    }

    try {
        return run_parent(sintra::test::get_binary_path(argc, argv));
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%sparent exception: %s\n", k_failure_prefix, e.what());
    }
    catch (...) {
        std::fprintf(stderr, "%sparent exception: unknown\n", k_failure_prefix);
    }
    return 1;
}
