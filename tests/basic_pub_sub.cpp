//
// Sintra Basic Pub/Sub Test
//
// This test validates the basic publish/subscribe messaging functionality of Sintra.
// It corresponds to example_0 and tests the following features:
// - Multi-process communication using a single executable
// - Type-safe message passing (strings and integers)
// - Slot activation for handling different message types
// - Barrier synchronization between processes
//
// Test structure:
// - Process 1 (sender): Sends string and integer messages to all receivers
// - Process 2 (string receiver): Receives and records string messages
// - Process 3 (int receiver): Receives and records integer messages
//
// The test verifies that all sent messages are correctly received by checking
// the recorded messages against expected values.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

namespace {

constexpr std::string_view kEnvSharedDir = "SINTRA_TEST_SHARED_DIR";

std::filesystem::path get_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (!value) {
        throw std::runtime_error("SINTRA_TEST_SHARED_DIR is not set");
    }
    return std::filesystem::path(value);
}

void set_shared_directory_env(const std::filesystem::path& dir)
{
#ifdef _WIN32
    _putenv_s(kEnvSharedDir.data(), dir.string().c_str());
#else
    setenv(kEnvSharedDir.data(), dir.string().c_str(), 1);
#endif
}

std::filesystem::path ensure_shared_directory()
{
    const char* value = std::getenv(kEnvSharedDir.data());
    if (value) {
        return std::filesystem::path(value);
    }

    auto base = std::filesystem::temp_directory_path() / "sintra_tests";
    std::filesystem::create_directories(base);

    // Generate a highly unique suffix combining timestamp, PID, and a monotonic counter
    auto unique_suffix = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now().time_since_epoch())
                             .count();
#ifdef _WIN32
    unique_suffix ^= static_cast<long long>(_getpid());
#else
    unique_suffix ^= static_cast<long long>(getpid());
#endif

    static std::atomic<long long> counter{0};
    unique_suffix ^= counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << "basic_pubsub_" << unique_suffix;
    auto dir = base / oss.str();
    std::filesystem::create_directories(dir);
    set_shared_directory_env(dir);
    return dir;
}

void write_strings(const std::filesystem::path& file, const std::vector<std::string>& values)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (const auto& value : values) {
        out << value << '\n';
    }
}

void write_ints(const std::filesystem::path& file, const std::vector<int>& values)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open " + file.string() + " for writing");
    }
    for (int value : values) {
        out << value << '\n';
    }
}

std::vector<std::string> read_strings(const std::filesystem::path& file)
{
    std::vector<std::string> values;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        values.push_back(line);
    }
    return values;
}

std::vector<int> read_ints(const std::filesystem::path& file)
{
    std::vector<int> values;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return values;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            values.push_back(std::stoi(line));
        }
    }
    return values;
}

void write_result(const std::filesystem::path& dir,
                  bool ok,
                  const std::vector<std::string>& strings,
                  const std::vector<int>& ints)
{
    std::ofstream out(dir / "result.txt", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open result file");
    }
    out << (ok ? "ok" : "fail") << '\n';
    if (!ok) {
        out << "strings:";
        for (const auto& value : strings) {
            out << ' ' << value;
        }
        out << "\nints:";
        for (int value : ints) {
            out << ' ' << value;
        }
        out << '\n';
    }
}

bool has_branch_flag(int argc, char* argv[])
{
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--branch_index") {
            return true;
        }
    }
    return false;
}

// These vectors are modified by slot handlers (reader thread) and read
// by main thread after barrier synchronization
std::vector<std::string> g_received_strings;
std::vector<int> g_received_ints;

int process_sender()
{
    sintra::barrier("slots-ready");

    sintra::world() << std::string("good morning");
    sintra::world() << std::string("good afternoon");
    sintra::world() << std::string("good evening");
    sintra::world() << std::string("good night");

    sintra::world() << 1;
    sintra::world() << 2;
    sintra::world() << 3;
    sintra::world() << 4;

    sintra::barrier("messages-done");
    sintra::barrier("write-phase");

    const auto shared_dir = get_shared_directory();
    const auto strings = read_strings(shared_dir / "strings.txt");
    const auto ints = read_ints(shared_dir / "ints.txt");

    const std::vector<std::string> expected_strings{
        "good morning", "good afternoon", "good evening", "good night"};
    const std::vector<int> expected_ints{1, 2, 3, 4};

    const bool ok = (strings == expected_strings) && (ints == expected_ints);
    write_result(shared_dir, ok, strings, ints);

    sintra::barrier("result-ready", "_sintra_all_processes");
    return 0;
}

int process_string_receiver()
{
    // Slot handler runs on reader thread, modifies g_received_strings
    auto string_slot = [](const std::string& value) {
        // Artificial delay to expose dual-ring race condition
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_received_strings.push_back(value);
    };
    sintra::activate_slot(string_slot);

    sintra::barrier("slots-ready");

    // After this barrier, we know all messages have been sent and processed
    // by the reader thread, so g_received_strings contains all messages
    sintra::barrier("messages-done");

    const auto shared_dir = get_shared_directory();
    write_strings(shared_dir / "strings.txt", g_received_strings);

    sintra::barrier("write-phase");
    sintra::barrier("result-ready", "_sintra_all_processes");
    return 0;
}

int process_int_receiver()
{
    // Slot handler runs on reader thread, modifies g_received_ints
    auto int_slot = [](int value) {
        // Artificial delay to expose dual-ring race condition
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_received_ints.push_back(value);
    };
    sintra::activate_slot(int_slot);

    sintra::barrier("slots-ready");

    // After this barrier, we know all messages have been sent and processed
    // by the reader thread, so g_received_ints contains all messages
    sintra::barrier("messages-done");

    const auto shared_dir = get_shared_directory();
    write_ints(shared_dir / "ints.txt", g_received_ints);

    sintra::barrier("write-phase");
    sintra::barrier("result-ready", "_sintra_all_processes");
    return 0;
}

} // namespace

void custom_terminate_handler() {
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

int main(int argc, char* argv[])
{
    std::set_terminate(custom_terminate_handler);

    const bool is_spawned = has_branch_flag(argc, argv);
    const auto shared_dir = ensure_shared_directory();

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(process_sender);
    processes.emplace_back(process_string_receiver);
    processes.emplace_back(process_int_receiver);

    sintra::init(argc, argv, processes);

    int exit_code = 0;
    std::string status;
    bool status_loaded = false;
    const auto result_path = shared_dir / "result.txt";

    if (!is_spawned) {
        sintra::barrier("result-ready", "_sintra_all_processes");
    }

    sintra::finalize();

    if (!is_spawned) {
        std::ifstream in(result_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "Error: failed to open result file at %s\n",
                          result_path.string().c_str());
            exit_code = 1;
        } else {
            in >> status;
            status_loaded = true;
        }

        bool cleanup_succeeded = false;
        for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
            try {
                std::filesystem::remove_all(shared_dir);
                cleanup_succeeded = true;
            }
            catch (const std::exception& e) {
                if (retry < 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } else {
                    std::fprintf(stderr,
                                  "Warning: failed to remove temp directory %s after 3 attempts: %s\n",
                                  shared_dir.string().c_str(), e.what());
                }
            }
        }

        if (exit_code == 0 && status_loaded) {
            exit_code = (status == "ok") ? 0 : 1;
        }

        return exit_code;
    }

    return 0;
}

