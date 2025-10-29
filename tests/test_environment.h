#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace sintra::test {

inline const std::filesystem::path& scratch_root()
{
    static const std::filesystem::path root = [] {
        const char* override_value = std::getenv("SINTRA_TEST_ROOT");
        if (override_value && *override_value) {
            std::filesystem::path override_path(override_value);
            std::filesystem::create_directories(override_path);
            return override_path;
        }

        auto fallback = std::filesystem::temp_directory_path() / "sintra_tests";
        std::filesystem::create_directories(fallback);
        return fallback;
    }();

    return root;
}

inline std::filesystem::path scratch_subdirectory(std::string_view name)
{
    auto directory = scratch_root() / std::filesystem::path(std::string(name));
    std::filesystem::create_directories(directory);
    return directory;
}

inline std::filesystem::path unique_scratch_directory(std::string_view prefix)
{
    static std::atomic<std::uint64_t> counter{0};

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    auto sequence = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << prefix << '_' << ticks << '_' << sequence;

    auto directory = scratch_subdirectory("runs") / oss.str();
    std::filesystem::create_directories(directory);
    return directory;
}

} // namespace sintra::test

