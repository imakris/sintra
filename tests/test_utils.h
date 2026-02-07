// Shared test utilities for Sintra tests.
// Consolidates common boilerplate that was previously duplicated across 20+ test files.

#pragma once

#include "test_environment.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace sintra::test {


// ---------------------------------------------------------------------------
// Branch flag detection
// ---------------------------------------------------------------------------

/// Returns true if the current process was spawned by sintra (i.e., has --branch_index in argv).
inline bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}


// ---------------------------------------------------------------------------
// Shared directory management (RAII)
// ---------------------------------------------------------------------------

/// RAII wrapper for creating a unique shared test directory, propagating it
/// to child processes via an environment variable, and cleaning it up.
///
/// Usage:
///   Shared_directory shared("SINTRA_TEST_DIR", "my_test");
///   auto dir = shared.path();  // use the directory
///   // automatically cleaned up on destruction (coordinator only)
class Shared_directory
{
public:
    /// Construct a shared directory manager.
    /// @param env_var   Environment variable name used to pass the path to child processes.
    /// @param test_name Used as subdirectory hint and prefix for uniqueness.
    Shared_directory(std::string_view env_var, std::string_view test_name)
        : m_env_var(env_var)
    {
        // If the env var is already set (we're a child process), just use it.
        const char* value = std::getenv(m_env_var.c_str());
        if (value && *value) {
            m_path = std::filesystem::path(value);
            std::filesystem::create_directories(m_path);
            m_is_owner = false;
            return;
        }

        // We're the coordinator — create a unique directory.
        auto base = sintra::test::scratch_subdirectory(test_name);

        auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
#ifdef _WIN32
        unique_suffix ^= static_cast<long long>(_getpid());
#else
        unique_suffix ^= static_cast<long long>(getpid());
#endif
        static std::atomic<long long> counter{0};
        unique_suffix ^= counter.fetch_add(1);

        std::ostringstream oss;
        oss << test_name << '_' << unique_suffix;
        m_path = base / oss.str();
        std::filesystem::create_directories(m_path);
        set_env(m_path);
        m_is_owner = true;
    }

    /// Returns the shared directory path.
    const std::filesystem::path& path() const { return m_path; }

    /// Clean up the directory (with retries for filesystem latency).
    /// Safe to call multiple times. Only cleans up if this instance created the directory.
    void cleanup()
    {
        if (!m_is_owner || m_path.empty()) {
            return;
        }
        for (int retry = 0; retry < 3; ++retry) {
            try {
                std::filesystem::remove_all(m_path);
                m_path.clear();
                return;
            }
            catch (const std::exception& e) {
                if (retry < 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                else {
                    std::fprintf(stderr,
                                 "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                                 m_path.string().c_str(), e.what());
                }
            }
        }
    }

    ~Shared_directory()
    {
        cleanup();
    }

    // Non-copyable, movable
    Shared_directory(const Shared_directory&) = delete;
    Shared_directory& operator=(const Shared_directory&) = delete;
    Shared_directory(Shared_directory&& other) noexcept
        : m_env_var(std::move(other.m_env_var))
        , m_path(std::move(other.m_path))
        , m_is_owner(other.m_is_owner)
    {
        other.m_is_owner = false;
    }

private:
    void set_env(const std::filesystem::path& dir)
    {
#ifdef _WIN32
        _putenv_s(m_env_var.c_str(), dir.string().c_str());
#else
        setenv(m_env_var.c_str(), dir.string().c_str(), 1);
#endif
    }

    std::string             m_env_var;
    std::filesystem::path   m_path;
    bool                    m_is_owner = false;
};


// ---------------------------------------------------------------------------
// Custom terminate handler
// ---------------------------------------------------------------------------

/// Diagnostic terminate handler that prints the uncaught exception before aborting.
[[noreturn]] inline void custom_terminate_handler()
{
    std::fprintf(stderr, "std::terminate called!\n");
    std::fprintf(stderr, "Uncaught exceptions: %d\n", std::uncaught_exceptions());

    try {
        auto eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        }
        else {
            std::fprintf(stderr, "terminate called without an active exception\n");
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "Uncaught exception: %s\n", e.what());
    }
    catch (...) {
        std::fprintf(stderr, "Uncaught exception of unknown type\n");
    }

    std::abort();
}


// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

/// Write lines to a file (one per line).
inline void write_lines(const std::filesystem::path& file, const std::vector<std::string>& lines)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (const auto& line : lines) {
        out << line << '\n';
    }
}

/// Read lines from a file.
inline std::vector<std::string> read_lines(const std::filesystem::path& file)
{
    std::vector<std::string> lines;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return lines;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}


} // namespace sintra::test
