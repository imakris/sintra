#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

constexpr const char* k_child_arg      = "--ring-lifecycle-attachment-child";
constexpr const char* k_dir_arg        = "--ring-lifecycle-attachment-dir";
constexpr const char* k_role_arg       = "--ring-lifecycle-attachment-role";
constexpr const char* k_failure_prefix = "ring_lifecycle_attachment_recovery_process_test: ";
constexpr const char* k_ring_name      = "ring_data";
#ifdef _WIN32
constexpr std::uint32_t k_exact_child_cleanup_exit_code = 0x53434c4b;
#endif

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

bool terminate_expected(
    sintra::test::Exact_child& child,
    const char*                role)
{
    const auto before = child.poll();
    std::string diagnostic;
    const bool settled = child.terminate_and_settle(diagnostic);
    const auto status  = child.describe_status();
    if (!settled) {
        std::fprintf(stderr,
            "%sfailed to terminate and settle %s child pid %d: %s%s%s\n",
            k_failure_prefix,
            role,
            child.pid(),
            status.c_str(),
            diagnostic.empty() ? "" : "; ",
            diagnostic.c_str());
        return false;
    }

#ifdef _WIN32
    const bool expected_termination =
        before == sintra::test::Exact_child_state::running &&
        child.exited_with_code(k_exact_child_cleanup_exit_code);
#else
    const bool expected_termination =
        before == sintra::test::Exact_child_state::running &&
        child.exited_from_signal(SIGKILL);
#endif

    if (!expected_termination) {
        std::fprintf(stderr,
            "%s%s child pid %d did not have the required termination status: %s\n",
            k_failure_prefix,
            role,
            child.pid(),
            status.c_str());
    }
    return expected_termination;
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

bool wait_for_data_control_removed(
    const std::filesystem::path& data_file,
    const std::filesystem::path& control_file)
{
    for (int retry = 0; retry < 80; ++retry) {
        if (!std::filesystem::exists(data_file) &&
            !std::filesystem::exists(control_file))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

bool check_final_files(
    const std::filesystem::path& directory,
    const std::filesystem::path& data_file,
    const std::filesystem::path& control_file,
    const std::filesystem::path& lifecycle_file)
{
    bool ok = true;
    if (!wait_for_data_control_removed(data_file, control_file)) {
        if (std::filesystem::exists(data_file)) {
            std::fprintf(stderr, "%sdata file remained\n", k_failure_prefix);
        }
        if (std::filesystem::exists(control_file)) {
            std::fprintf(stderr, "%scontrol file remained\n", k_failure_prefix);
        }
        ok = false;
    }

    if (!std::filesystem::exists(lifecycle_file)) {
        std::fprintf(stderr, "%slifecycle anchor missing\n", k_failure_prefix);
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

    if (has_temp_ring_publish_files(directory)) {
        ok = false;
    }

    return ok;
}

int run_reader_child(const std::filesystem::path& directory)
{
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    try {
        sintra::Ring_R<std::uint32_t> reader(
            directory.string(),
            k_ring_name,
            ring_elements,
            ring_elements / 2);
        touch_file(marker_path(directory, "reader_attached"));

        if (!sintra::test::wait_for_file(
                marker_path(directory, "release_reader"),
                std::chrono::seconds(30),
                std::chrono::milliseconds(5)))
        {
            write_text(marker_path(directory, "reader_status"), "timeout:release");
            return 1;
        }

        write_text(marker_path(directory, "reader_status"), "released");
    }
    catch (const std::exception& e) {
        try {
            write_text(marker_path(directory, "reader_status"), std::string("exception:") + e.what());
        }
        catch (...) {
        }
        return 1;
    }

    return 0;
}

int run_writer_child(const std::filesystem::path& directory)
{
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    try {
        sintra::Ring_W<std::uint32_t> writer(
            directory.string(),
            k_ring_name,
            ring_elements);
        touch_file(marker_path(directory, "writer_attached"));

        if (!sintra::test::wait_for_file(
                marker_path(directory, "release_writer"),
                std::chrono::seconds(30),
                std::chrono::milliseconds(5)))
        {
            write_text(marker_path(directory, "writer_status"), "timeout:release");
            return 1;
        }

        write_text(marker_path(directory, "writer_status"), "released");
    }
    catch (const std::exception& e) {
        try {
            write_text(marker_path(directory, "writer_status"), std::string("exception:") + e.what());
        }
        catch (...) {
        }
        return 1;
    }

    return 0;
}

bool run_killed_reader_with_live_writer_case(const std::string& binary_path)
{
    sintra::test::Temp_ring_dir temp("ring_lifecycle_attachment_live_writer");
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    const auto data_file      = temp.path / k_ring_name;
    const auto control_file   = temp.path / (std::string(k_ring_name) + "_control");
    const auto lifecycle_file = temp.path / (std::string(k_ring_name) + "_lifecycle");

    bool ok = true;
    {
        sintra::Ring_W<std::uint32_t> writer(
            temp.path.string(),
            k_ring_name,
            ring_elements);
        sintra::test::Exact_child reader(std::chrono::seconds(5));

        if (!launch_child(binary_path, temp.path, "reader", reader)) {
            std::fprintf(stderr,
                "%sfailed to launch reader child: %s\n",
                k_failure_prefix,
                reader.error().c_str());
            return false;
        }
        if (!sintra::test::wait_for_file(
                marker_path(temp.path, "reader_attached"),
                std::chrono::seconds(10),
                std::chrono::milliseconds(5)))
        {
            std::fprintf(stderr, "%sreader child did not attach\n", k_failure_prefix);
            return false;
        }

        std::uint64_t attached = 0;
        if (!read_control_attached(control_file, attached) || attached != 2) {
            std::fprintf(stderr,
                "%sdiagnostic num_attached before kill: expected 2 observed %llu\n",
                k_failure_prefix,
                static_cast<unsigned long long>(attached));
            ok = false;
        }

        if (!terminate_expected(reader, "reader")) {
            std::fprintf(stderr, "%sfailed to terminate reader child\n", k_failure_prefix);
            ok = false;
        }
    }

    return check_final_files(temp.path, data_file, control_file, lifecycle_file) && ok;
}

bool run_killed_sole_writer_second_generation_case(const std::string& binary_path)
{
    sintra::test::Temp_ring_dir temp("ring_lifecycle_attachment_sole_writer");
    const auto ring_elements =
        sintra::test::pick_ring_elements<std::uint32_t>(256);

    const auto data_file      = temp.path / k_ring_name;
    const auto control_file   = temp.path / (std::string(k_ring_name) + "_control");
    const auto lifecycle_file = temp.path / (std::string(k_ring_name) + "_lifecycle");

    sintra::test::Exact_child writer(std::chrono::seconds(5));
    if (!launch_child(binary_path, temp.path, "writer", writer)) {
        std::fprintf(stderr,
            "%sfailed to launch writer child: %s\n",
            k_failure_prefix,
            writer.error().c_str());
        return false;
    }

    if (!sintra::test::wait_for_file(
            marker_path(temp.path, "writer_attached"),
            std::chrono::seconds(10),
            std::chrono::milliseconds(5)))
    {
        std::fprintf(stderr, "%swriter child did not attach\n", k_failure_prefix);
        return false;
    }

    if (!terminate_expected(writer, "writer")) {
        std::fprintf(stderr, "%sfailed to terminate writer child\n", k_failure_prefix);
        return false;
    }

    if (!std::filesystem::exists(data_file) ||
        !std::filesystem::exists(control_file) ||
        !std::filesystem::exists(lifecycle_file))
    {
        std::fprintf(stderr, "%sstale files were not left for recovery\n", k_failure_prefix);
        return false;
    }

    bool ok = true;
    {
        sintra::Ring_W<std::uint32_t> second_writer(
            temp.path.string(),
            k_ring_name,
            ring_elements);

        std::uint64_t attached = 0;
        if (!read_control_attached(control_file, attached) || attached != 1) {
            std::fprintf(stderr,
                "%sdiagnostic num_attached after recovery: expected 1 observed %llu\n",
                k_failure_prefix,
                static_cast<unsigned long long>(attached));
            ok = false;
        }
    }

    return check_final_files(temp.path, data_file, control_file, lifecycle_file) && ok;
}

int run_parent(const std::string& binary_path)
{
    bool ok = true;
    ok = run_killed_reader_with_live_writer_case(binary_path) && ok;
    ok = run_killed_sole_writer_second_generation_case(binary_path) && ok;
    return ok ? 0 : 1;
}

int run_child(int argc, char* argv[])
{
    const auto directory = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);

    if (role == "reader") {
        return run_reader_child(directory);
    }
    if (role == "writer") {
        return run_writer_child(directory);
    }

    std::fprintf(stderr, "%sunknown child role: %s\n", k_failure_prefix, role.c_str());
    return 1;
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
