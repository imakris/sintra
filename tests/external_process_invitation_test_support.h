#pragma once

// Fixture shared by the external-process-invitation tests. Every one of them drives
// helper processes the same way: guard the runtime, hand work to a directly spawned
// child, exchange progress through marker files, and observe the child's exit. Those
// pieces were copied verbatim into each test; they live here instead, so a change to
// the observation rules lands in one place.
//
// The diagnostics carry a per-test failure prefix, which each translation unit binds
// once to its own constant rather than repeating it at every call.

#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sintra::test::invitation {

// Finalizes the runtime unless the test already shut it down, so a failing test does
// not leave a half-initialized process behind.
struct Runtime_guard
{
    bool active = false;

    ~Runtime_guard()
    {
        if (!active || !sintra::s_mproc) {
            return;
        }

        try {
            sintra::detail::finalize();
        }
        catch (...) {
        }
    }

    bool shutdown()
    {
        if (!active) {
            return true;
        }

        active = false;
        return sintra::shutdown();
    }
};

inline std::filesystem::path marker_path(
    const std::filesystem::path&   dir,
    const std::string&             marker)
{
    return dir / (marker + ".txt");
}

// Control files carry no value: the helper waits for the file to exist, so writing one
// is how a test releases a child that is holding at a checkpoint.
inline std::filesystem::path control_path(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const char*                    suffix)
{
    return dir / (marker + suffix);
}

inline void write_control_file(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const char*                    suffix)
{
    std::ofstream out(control_path(dir, marker, suffix), std::ios::binary | std::ios::trunc);
    out << "go\n";
}

inline bool wait_for_control_file(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const char*                    suffix,
    std::chrono::milliseconds      timeout)
{
    return sintra::test::wait_for_file(
        control_path(dir, marker, suffix),
        timeout,
        std::chrono::milliseconds(20));
}

inline void write_marker(
    const char*                    failure_prefix,
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               value)
{
    const auto    path = marker_path(dir, marker);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value << '\n';
    out.close();
    if (!out) {
        std::fprintf(stderr,
            "%sfailed to write marker '%s' at '%s'\n",
            failure_prefix,
            marker.c_str(),
            path.string().c_str());
        std::fflush(stderr);
    }
}

inline bool wait_for_marker(
    const char*                    failure_prefix,
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               expected,
    std::chrono::milliseconds      timeout)
{
    const auto  path = marker_path(dir, marker);
    std::string actual;

    if (sintra::test::wait_for_first_line(
            path,
            expected,
            actual,
            timeout,
            std::chrono::milliseconds(20)))
    {
        return true;
    }

    if (actual.empty()) {
        std::fprintf(stderr, "%smarker '%s' was not written\n", failure_prefix, marker.c_str());
    }
    else {
        std::fprintf(stderr,
            "%smarker '%s' mismatch: expected '%.*s', actual '%s'\n",
            failure_prefix,
            marker.c_str(),
            static_cast<int>(expected.size()),
            expected.data(),
            actual.c_str());
    }
    return false;
}

// Spawns a helper without going through the swarm, so the test keeps exact authority
// over the child process.
inline bool launch_direct_process(
    const std::string&                 binary_path,
    const std::vector<std::string>&    args,
    sintra::test::Exact_child&         child)
{
    std::vector<std::string> all_args;
    all_args.reserve(args.size() + 1);
    all_args.push_back(binary_path);
    all_args.insert(all_args.end(), args.begin(), args.end());

    sintra::C_string_vector cargs(all_args);
    return child.spawn(binary_path.c_str(), cargs.v());
}

inline bool assert_clean_exit(
    const char*                failure_prefix,
    sintra::test::Exact_child& child,
    std::chrono::milliseconds  timeout,
    const char*                message)
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
                std::fprintf(
                    stderr,
                    "%s%s: %s%s%s\n",
                    failure_prefix,
                    message,
                    status.c_str(),
                    settled ? "" : "; settlement failed: ",
                    settled ? "" : settle_diagnostic.c_str());
            }
            return sintra::test::assert_true(
                clean_exit && settled,
                failure_prefix,
                message);
        }
        if (state == sintra::test::Exact_child_state::error) {
            const auto observation = child.describe_status();
            std::string cleanup_diagnostic;
            const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
            std::fprintf(
                stderr,
                "%s%s: observation failed: %s; cleanup %s%s%s\n",
                failure_prefix,
                message,
                observation.c_str(),
                cleaned ? "settled" : "failed",
                cleanup_diagnostic.empty() ? "" : ": ",
                cleanup_diagnostic.c_str());
            return sintra::test::assert_true(false, failure_prefix, message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string cleanup_diagnostic;
    const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
    std::fprintf(
        stderr,
        "%s%s: timed out; cleanup %s%s%s\n",
        failure_prefix,
        message,
        cleaned ? "settled" : "failed",
        cleanup_diagnostic.empty() ? "" : ": ",
        cleanup_diagnostic.c_str());
    return sintra::test::assert_true(false, failure_prefix, message);
}

} // namespace sintra::test::invitation
