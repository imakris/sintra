#pragma once

#include "exact_child_test_support.h"
#include "test_ring_utils.h"
#include "test_utils.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace sintra::test::isolated {

constexpr auto k_deadline = std::chrono::seconds(8);

inline void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::fprintf(stderr, "fixture check failed: %s\n", message.c_str());
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
inline bool await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + k_deadline;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

inline void signal_file(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    stream << "ready\n";
    stream.close();
    require(bool(stream), "cannot publish fixture phase: " + path.string());
}

[[noreturn]] inline void fixture_failure(const std::string& message)
{
    std::fprintf(stderr, "isolated fixture: %s\n", message.c_str());
    std::fflush(stderr);
    std::_Exit(2);
}

struct case_t
{
    const char* name;
    const char* poster_stage;
    const char* action;
};

template <typename Case, size_t Count>
const Case* find_case(const Case (&cases)[Count], std::string_view name)
{
    for (const auto& test : cases) {
        if (name == test.name) {
            return &test;
        }
    }
    return nullptr;
}

template <typename Case>
bool supervise_case(const std::string& binary, const Case& test)
{
    sintra::test::Temp_ring_dir directory(test.name);
    const std::string path = directory.str();
    Exact_child scenario(std::chrono::seconds(3));
    const char* args[] = {binary.c_str(), "--isolated-scenario", test.name, path.c_str(), nullptr};
    require(scenario.spawn(binary.c_str(), args), "cannot spawn scenario: " + scenario.error());
    const bool exited = await([&] { return scenario.poll() != Exact_child_state::running; });
    const bool passed = exited && scenario.exited_with_code(0);
    if (!passed) {
        std::fprintf(stderr, "%s: %s\n", test.name,
            exited ? scenario.describe_status().c_str() : "supervisor deadline: reader made no progress");
    }
    std::string diagnostic;
    require(scenario.terminate_and_settle(diagnostic), diagnostic);
    std::printf("%s: %s\n", test.name, passed ? "PASS" : "FAIL");
    std::fflush(stdout);
    return passed;
}

// A successful default run continues into the owning executable's other
// cases. Selected cases and subprocess roles return their result immediately.
template <typename Case, size_t Count, typename Run>
std::optional<int> run_cases(
    int argc, char* argv[], const Case (&cases)[Count], Run run, bool allow_other_cases = false)
{
    const bool selected = argc == 3 && std::string_view(argv[1]) == "--case";
    const bool scenario = argc == 4 && std::string_view(argv[1]) == "--isolated-scenario";
    if (argc != 1 && !selected && !scenario) {
        return std::nullopt;
    }
    try {
        const auto binary = get_binary_path(argc, argv);
        if (selected || scenario) {
            const auto* test = find_case(cases, argv[2]);
            if (!test) {
                if (selected && allow_other_cases) {
                    return std::nullopt;
                }
                throw std::runtime_error("unknown isolated case: " + std::string(argv[2]));
            }
            return scenario ? run(binary, *test, argv[3]) : (supervise_case(binary, *test) ? 0 : 1);
        }
        bool passed = true;
        for (const auto& test : cases) {
            passed = supervise_case(binary, test) && passed;
        }
        if (!passed) {
            return 1;
        }
        return std::nullopt;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "isolated fixture: %s\n", error.what());
        return 1;
    }
}

} // namespace sintra::test::isolated
