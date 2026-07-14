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
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr const char* k_child_arg      = "--ring-control-first-attach-child";
constexpr const char* k_dir_arg        = "--ring-control-first-attach-dir";
constexpr const char* k_index_arg      = "--ring-control-first-attach-index";
constexpr const char* k_failure_prefix = "ring_control_first_attach_process_test: ";
constexpr const char* k_ring_name      = "ring_data";

constexpr int k_child_count = 16;

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

bool wait_for_all_markers(
    const std::filesystem::path& directory,
    std::string_view             name,
    int                          count,
    std::chrono::milliseconds    timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (int i = 0; i < count; ++i) {
        if (!sintra::test::wait_for_file_until(
                marker_path(directory, name, i),
                deadline,
                std::chrono::milliseconds(5)))
        {
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

bool launch_child(
    const std::string&           binary_path,
    const std::filesystem::path& directory,
    int                          index,
    sintra::test::Exact_child&   child)
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
    return child.spawn(binary_path.c_str(), cargs.v());
}

bool wait_for_clean_child_exit(
    sintra::test::Exact_child& child,
    int                        index,
    std::chrono::milliseconds  timeout)
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
                    "%schild %d pid %d did not exit cleanly: %s%s%s\n",
                    k_failure_prefix,
                    index,
                    child.pid(),
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
                "%schild %d pid %d observation failed: %s; cleanup %s%s%s\n",
                k_failure_prefix,
                index,
                child.pid(),
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
        "%schild %d pid %d timed out; cleanup %s%s%s\n",
        k_failure_prefix,
        index,
        child.pid(),
        cleaned ? "settled" : "failed",
        cleanup_diagnostic.empty() ? "" : ": ",
        cleanup_diagnostic.c_str());
    return false;
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
        if (!sintra::test::wait_for_file(
                marker_path(directory, "go"),
                std::chrono::seconds(10),
                std::chrono::milliseconds(5)))
        {
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

        if (!sintra::test::wait_for_file(
                marker_path(directory, "release"),
                std::chrono::seconds(10),
                std::chrono::milliseconds(5)))
        {
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

    std::vector<std::unique_ptr<sintra::test::Exact_child>> children;
    children.reserve(k_child_count);
    for (int i = 0; i < k_child_count; ++i) {
        auto child = std::make_unique<sintra::test::Exact_child>(std::chrono::seconds(2));
        if (!launch_child(binary_path, temp.path, i, *child)) {
            const auto spawn_error = child->error();
            std::ostringstream cleanup_failures;
            std::string cleanup_diagnostic;
            if (!child->terminate_and_settle(cleanup_diagnostic)) {
                cleanup_failures << "; failed child cleanup failed: "
                                 << cleanup_diagnostic;
            }
            for (std::size_t child_index = 0; child_index < children.size(); ++child_index) {
                cleanup_diagnostic.clear();
                if (!children[child_index]->terminate_and_settle(cleanup_diagnostic)) {
                    cleanup_failures << "; child " << child_index
                                     << " cleanup failed: " << cleanup_diagnostic;
                }
            }
            std::fprintf(stderr,
                "%sfailed to launch child %d: %s%s\n",
                k_failure_prefix,
                i,
                spawn_error.c_str(),
                cleanup_failures.str().c_str());
            return 1;
        }
        children.push_back(std::move(child));
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

    for (std::size_t i = 0; i < children.size(); ++i) {
        if (!wait_for_clean_child_exit(*children[i], static_cast<int>(i), std::chrono::seconds(10))) {
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
