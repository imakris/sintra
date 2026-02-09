// Shared test utilities for Sintra tests.
// Consolidates common boilerplate that was previously duplicated across 20+ test files.

#pragma once

#include "test_environment.h"

#include <sintra/sintra.h>

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
#include <utility>
#include <vector>


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

inline bool has_argv_flag(int argc, char* argv[], std::string_view flag)
{
    if (flag.empty()) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }
        std::string_view view(arg);
        if (view == flag) {
            return true;
        }
        if (view.size() > flag.size() + 1 &&
            view.rfind(flag, 0) == 0 &&
            view[flag.size()] == '=') {
            return true;
        }
    }
    return false;
}

inline std::string get_argv_value(int argc,
                                  char* argv[],
                                  std::string_view flag,
                                  std::string_view default_value = {})
{
    if (flag.empty()) {
        return std::string(default_value);
    }
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }
        std::string_view view(arg);
        if (view == flag) {
            if (i + 1 < argc && argv[i + 1]) {
                return std::string(argv[i + 1]);
            }
            break;
        }
        if (view.size() > flag.size() + 1 &&
            view.rfind(flag, 0) == 0 &&
            view[flag.size()] == '=') {
            return std::string(view.substr(flag.size() + 1));
        }
    }
    return std::string(default_value);
}

inline std::string get_binary_path(int argc, char* argv[])
{
    if (argc > 0 && argv[0]) {
        return std::string(argv[0]);
    }
    return {};
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
        unique_suffix ^= static_cast<long long>(get_pid());
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
// Multi-process test runner
// ---------------------------------------------------------------------------

template <typename Setup,
          typename Coordinator_action,
          typename Verifier>
int run_multi_process_test(int argc,
                           char* argv[],
                           const char* env_var,
                           const char* test_name,
                           std::vector<sintra::Process_descriptor> processes,
                           Setup&& setup,
                           Coordinator_action&& coordinator_action,
                           Verifier&& verify,
                           const char* final_barrier = nullptr,
                           const char* barrier_group = "_sintra_all_processes")
{
    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared(env_var, test_name);

    if (!is_spawned) {
        setup(shared.path());
    }

    sintra::init(argc, argv, processes);

    int coordinator_result = 0;
    if (!is_spawned) {
        coordinator_result = coordinator_action(shared.path());
        if (final_barrier && *final_barrier) {
            sintra::barrier(final_barrier, barrier_group);
        }
    }

    sintra::finalize();

    if (!is_spawned) {
        if (coordinator_result != 0) {
            return coordinator_result;
        }
        return verify(shared.path());
    }

    return 0;
}

template <typename Coordinator_action,
          typename Verifier>
int run_multi_process_test(int argc,
                           char* argv[],
                           const char* env_var,
                           const char* test_name,
                           std::vector<sintra::Process_descriptor> processes,
                           Coordinator_action&& coordinator_action,
                           Verifier&& verify,
                           const char* final_barrier = nullptr,
                           const char* barrier_group = "_sintra_all_processes")
{
    return run_multi_process_test(argc,
                                  argv,
                                  env_var,
                                  test_name,
                                  std::move(processes),
                                  [](const std::filesystem::path&) {},
                                  std::forward<Coordinator_action>(coordinator_action),
                                  std::forward<Verifier>(verify),
                                  final_barrier,
                                  barrier_group);
}

template <typename Verifier>
int run_multi_process_test(int argc,
                           char* argv[],
                           const char* env_var,
                           const char* test_name,
                           std::vector<sintra::Process_descriptor> processes,
                           Verifier&& verify,
                           const char* final_barrier = nullptr,
                           const char* barrier_group = "_sintra_all_processes")
{
    return run_multi_process_test(argc,
                                  argv,
                                  env_var,
                                  test_name,
                                  std::move(processes),
                                  [](const std::filesystem::path&) { return 0; },
                                  std::forward<Verifier>(verify),
                                  final_barrier,
                                  barrier_group);
}


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
// Assertions
// ---------------------------------------------------------------------------

inline void print_test_message(std::string_view prefix, std::string_view message)
{
    std::fprintf(stderr,
                 "%.*s%.*s\n",
                 static_cast<int>(prefix.size()),
                 prefix.data(),
                 static_cast<int>(message.size()),
                 message.data());
}

[[noreturn]] inline void fail(std::string_view prefix, std::string_view message)
{
    print_test_message(prefix, message);
    std::exit(1);
}

inline void expect(bool condition, std::string_view prefix, std::string_view message)
{
    if (!condition) {
        fail(prefix, message);
    }
}

inline void require_true(bool condition, std::string_view prefix, std::string_view message)
{
    expect(condition, prefix, message);
}

inline bool assert_true(bool condition, std::string_view prefix, std::string_view message)
{
    if (!condition) {
        print_test_message(prefix, message);
    }
    return condition;
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

inline bool append_line(const std::filesystem::path& file, std::string_view line)
{
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) {
        return false;
    }
    out << line << '\n';
    return static_cast<bool>(out);
}

inline void append_line_or_throw(const std::filesystem::path& file, std::string_view line)
{
    if (!append_line(file, line)) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
}

inline void append_line_best_effort(const std::filesystem::path& file, std::string_view line)
{
    try {
        const auto path_str = file.string();
        std::ofstream out(path_str, std::ios::binary | std::ios::app);
        if (!out) {
            return;
        }
        out << line << '\n';
    }
    catch (...) {
        // Swallow I/O errors in best-effort logging.
    }
}

inline bool wait_for_file_until(const std::filesystem::path& path,
                                std::chrono::steady_clock::time_point deadline,
                                std::chrono::milliseconds poll = std::chrono::milliseconds(10))
{
    while (!std::filesystem::exists(path)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(poll);
    }
    return true;
}

inline bool wait_for_file(const std::filesystem::path& path,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
                          std::chrono::milliseconds poll = std::chrono::milliseconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    return wait_for_file_until(path, deadline, poll);
}

inline bool wait_for_path(const std::filesystem::path& path,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(30000),
                          std::chrono::milliseconds poll = std::chrono::milliseconds(10))
{
    return wait_for_file(path, timeout, poll);
}


} // namespace sintra::test
