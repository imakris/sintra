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

#include "test_utils.h"

#include <algorithm>
#include <array>
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

namespace {

constexpr int k_message_rounds = 5;
constexpr std::array<std::string_view, 4> k_base_string_messages{
    "good morning", "good afternoon", "good evening", "good night"};
constexpr std::array<int, 4> k_base_int_messages{1, 2, 3, 4};

std::string format_string_message(std::string_view base, int round)
{
    std::string result(base);
    result.append(" (round ");
    result.append(std::to_string(round));
    result.push_back(')');
    return result;
}

int format_int_message(int base, int round)
{
    return base + (round * 10);
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

// These vectors are modified by slot handlers (reader thread) and read
// by main thread after barrier synchronization
std::vector<std::string> g_received_strings;
std::vector<int> g_received_ints;

int process_sender()
{
    sintra::barrier("slots-ready");

    for (int round = 0; round < k_message_rounds; ++round) {
        for (auto base : k_base_string_messages) {
            sintra::world() << format_string_message(base, round);
        }
        for (int base : k_base_int_messages) {
            sintra::world() << format_int_message(base, round);
        }
    }

    sintra::barrier("messages-done");
    sintra::barrier("write-phase");

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "basic_pub_sub");
    const auto shared_dir = shared.path();
    const auto strings = read_strings(shared_dir / "strings.txt");
    const auto ints = read_ints(shared_dir / "ints.txt");

    std::vector<std::string> expected_strings;
    expected_strings.reserve(k_message_rounds * k_base_string_messages.size());
    for (int round = 0; round < k_message_rounds; ++round) {
        for (auto base : k_base_string_messages) {
            expected_strings.emplace_back(format_string_message(base, round));
        }
    }

    std::vector<int> expected_ints;
    expected_ints.reserve(k_message_rounds * k_base_int_messages.size());
    for (int round = 0; round < k_message_rounds; ++round) {
        for (int base : k_base_int_messages) {
            expected_ints.push_back(format_int_message(base, round));
        }
    }

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

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "basic_pub_sub");
    const auto shared_dir = shared.path();
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

    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "basic_pub_sub");
    const auto shared_dir = shared.path();
    write_ints(shared_dir / "ints.txt", g_received_ints);

    sintra::barrier("write-phase");
    sintra::barrier("result-ready", "_sintra_all_processes");
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const bool is_spawned = sintra::test::has_branch_flag(argc, argv);
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "basic_pub_sub");
    const auto shared_dir = shared.path();

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
        }
        else {
            in >> status;
            status_loaded = true;
        }

        if (exit_code == 0 && status_loaded) {
            exit_code = (status == "ok") ? 0 : 1;
        }

        return exit_code;
    }

    return 0;
}

