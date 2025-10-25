#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

constexpr std::string_view kEnvSharedDir = "SINTRA_LIFECYCLE_TEST_DIR";
constexpr std::chrono::milliseconds kPollInterval{20};
constexpr std::chrono::seconds kDefaultTimeout{10};

bool has_branch_flag(int argc, char* const argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--branch_index" || arg.rfind("--branch_index=", 0) == 0) {
            return true;
        }
    }
    return false;
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_LIFECYCLE_TEST_DIR is not set");
    }
    return std::filesystem::path(value);
}

std::filesystem::path ensure_shared_directory()
{
    if (const char* value = std::getenv(kEnvSharedDir.data())) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_lifecycle";
    std::filesystem::create_directories(base);

    auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
#ifdef _WIN32
    timestamp ^= static_cast<long long>(_getpid());
#else
    timestamp ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    timestamp ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream name;
    name << "lifecycle_" << timestamp;
    auto dir = base / name.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void append_event(const std::string& role, const std::string& event)
{
    auto path = get_shared_directory() / (role + ".log");
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open log file " + path.string());
    }
    out << event << '\n';
}

bool wait_for_file(const std::filesystem::path& path,
                   std::chrono::milliseconds timeout = kDefaultTimeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool wait_for_line_count(const std::filesystem::path& path,
                         std::size_t expected,
                         std::chrono::milliseconds timeout = kDefaultTimeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::size_t lines = 0;
            std::string line;
            while (std::getline(in, line)) {
                ++lines;
            }
            if (lines >= expected) {
                return true;
            }
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::size_t lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++lines;
    }
    return lines >= expected;
}

bool verify_log(const std::filesystem::path& path,
                const std::vector<std::string>& expected)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "coordinator_lifecycle_test: missing log " << path << '\n';
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    if (lines != expected) {
        std::cerr << "coordinator_lifecycle_test: unexpected contents in "
                  << path << '\n';
        std::cerr << "  expected:";
        for (const auto& value : expected) {
            std::cerr << ' ' << value;
        }
        std::cerr << "\n  actual:";
        for (const auto& value : lines) {
            std::cerr << ' ' << value;
        }
        std::cerr << '\n';
        return false;
    }

    return true;
}

bool report_failure(const std::string& role, const std::string& message)
{
    std::cerr << "coordinator_lifecycle_test(" << role << "): " << message << '\n';
    return false;
}

int coordinator_process()
{
    try {
        const auto shared_dir = get_shared_directory();

        append_event("coordinator", "join");
        if (!sintra::barrier("lifecycle-join", "_sintra_all_processes")) {
            return report_failure("coordinator", "join barrier failed");
        }

        if (!wait_for_line_count(shared_dir / "alpha.log", 2) ||
            !wait_for_line_count(shared_dir / "beta.log", 2)) {
            return report_failure("coordinator", "ready stage timed out");
        }
        append_event("coordinator", "ready");

        if (!sintra::barrier("lifecycle-swing", "_sintra_all_processes")) {
            return report_failure("coordinator", "swing barrier failed");
        }
        append_event("coordinator", "swing");

        if (!wait_for_line_count(shared_dir / "alpha.log", 3) ||
            !wait_for_line_count(shared_dir / "beta.log", 3)) {
            return report_failure("coordinator", "swing acknowledgements timed out");
        }

        {
            std::ofstream depart(shared_dir / "depart_alpha", std::ios::binary | std::ios::trunc);
            if (!depart) {
                return report_failure("coordinator", "failed to create depart signal");
            }
            depart << "depart";
        }
        append_event("coordinator", "depart-issued");

        if (!wait_for_line_count(shared_dir / "alpha.log", 4)) {
            return report_failure("coordinator", "alpha did not acknowledge depart");
        }

        if (!wait_for_file(shared_dir / "alpha.done")) {
            return report_failure("coordinator", "alpha completion signal missing");
        }

        if (!sintra::barrier("lifecycle-cleanup", "_sintra_all_processes")) {
            return report_failure("coordinator", "cleanup barrier failed");
        }
        append_event("coordinator", "cleanup");
    }
    catch (const std::exception& e) {
        report_failure("coordinator", e.what());
        return 1;
    }

    return 0;
}

int alpha_process()
{
    try {
        const auto shared_dir = get_shared_directory();

        append_event("alpha", "join");
        if (!sintra::barrier("lifecycle-join", "_sintra_all_processes")) {
            return report_failure("alpha", "join barrier failed");
        }

        if (!sintra::barrier("lifecycle-ready", "_sintra_external_processes")) {
            return report_failure("alpha", "ready barrier failed");
        }
        append_event("alpha", "ready");

        if (!sintra::barrier("lifecycle-swing", "_sintra_all_processes")) {
            return report_failure("alpha", "swing barrier failed");
        }
        append_event("alpha", "swing");

        if (!wait_for_file(shared_dir / "depart_alpha")) {
            return report_failure("alpha", "depart signal missing");
        }
        append_event("alpha", "depart");

        std::ofstream done(shared_dir / "alpha.done", std::ios::binary | std::ios::trunc);
        if (!done) {
            return report_failure("alpha", "failed to create completion signal");
        }
        done << "done";
    }
    catch (const std::exception& e) {
        report_failure("alpha", e.what());
        return 1;
    }

    return 0;
}

int beta_process()
{
    try {
        const auto shared_dir = get_shared_directory();

        append_event("beta", "join");
        if (!sintra::barrier("lifecycle-join", "_sintra_all_processes")) {
            return report_failure("beta", "join barrier failed");
        }

        if (!sintra::barrier("lifecycle-ready", "_sintra_external_processes")) {
            return report_failure("beta", "ready barrier failed");
        }
        append_event("beta", "ready");

        if (!sintra::barrier("lifecycle-swing", "_sintra_all_processes")) {
            return report_failure("beta", "swing barrier failed");
        }
        append_event("beta", "swing");

        if (!wait_for_file(shared_dir / "depart_alpha")) {
            return report_failure("beta", "depart signal missing");
        }

        if (!wait_for_file(shared_dir / "alpha.done")) {
            return report_failure("beta", "alpha completion signal missing");
        }

        if (!sintra::barrier("lifecycle-cleanup", "_sintra_all_processes")) {
            return report_failure("beta", "cleanup barrier failed");
        }
        append_event("beta", "cleanup");
    }
    catch (const std::exception& e) {
        report_failure("beta", e.what());
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(coordinator_process);
    processes.emplace_back(alpha_process);
    processes.emplace_back(beta_process);

    sintra::init(argc, argv, processes);

    sintra::finalize();

    if (!spawned) {
        bool ok = true;
        ok &= verify_log(shared_dir / "coordinator.log",
                         {"join", "ready", "swing", "depart-issued", "cleanup"});
        ok &= verify_log(shared_dir / "alpha.log",
                         {"join", "ready", "swing", "depart"});
        ok &= verify_log(shared_dir / "beta.log",
                         {"join", "ready", "swing", "cleanup"});

        std::error_code ec;
        std::filesystem::remove_all(shared_dir, ec);

        return ok ? 0 : 1;
    }

    return 0;
}

