//
// Sintra Ring Cleanup Stress Test
//
// This test repeatedly initializes and finalizes Sintra while seeding the
// coordinator scratch directory with dozens of synthetic stale run markers.
// It exercises the cleanup logic introduced in PR #611 by ensuring that
// obsolete swarm directories are pruned before each run.
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/platform_utils.h>

#include "test_environment.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

void configure_temp_directory(const std::filesystem::path& path)
{
#if defined(_WIN32)
    _putenv_s("TEMP", path.string().c_str());
    _putenv_s("TMP", path.string().c_str());
#else
    setenv("TMPDIR", path.string().c_str(), 1);
    setenv("TMP", path.string().c_str(), 1);
    setenv("TEMP", path.string().c_str(), 1);
#endif
}

void emit_stale_run_directory(const std::filesystem::path& base,
                              std::uint32_t index)
{
    auto dir = base / ("stale_run_" + std::to_string(index));
    std::filesystem::create_directories(dir);

    sintra::run_marker_record record{};
    record.pid = 100000u + index; // Very unlikely to collide with a real PID
    record.start_stamp = static_cast<std::uint64_t>(index + 1);
    record.created_monotonic_ns = sintra::monotonic_now_ns();
    record.recovery_occurrence = index;

    if (!sintra::write_run_marker(dir, record)) {
        std::fprintf(stderr,
                     "ring_cleanup_stress: failed to create run marker %u\n",
                     index);
    }

    if ((index & 1u) != 0u) {
        sintra::mark_run_directory_for_cleanup(dir);
    }
}

std::uint32_t count_subdirectories(const std::filesystem::path& base)
{
    std::uint32_t count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(base, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        std::error_code status_ec;
        if (it->is_directory(status_ec) && !status_ec) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto scratch = sintra::test::unique_scratch_directory("ring_cleanup_stress");
    const auto temp_root = scratch / "temp_root";
    std::filesystem::create_directories(temp_root);

    configure_temp_directory(temp_root);

    const auto sintra_base = temp_root / "sintra";
    std::filesystem::create_directories(sintra_base);

    constexpr std::uint32_t kInitialStaleRuns = 48;
    for (std::uint32_t i = 0; i < kInitialStaleRuns; ++i) {
        emit_stale_run_directory(sintra_base, i);
    }

    std::uint32_t initial = count_subdirectories(sintra_base);
    if (initial < kInitialStaleRuns) {
        std::fprintf(stderr,
                     "ring_cleanup_stress: expected %u initial directories, saw %u\n",
                     kInitialStaleRuns,
                     initial);
        return 1;
    }

    constexpr std::uint32_t kIterations = 24;
    std::uint32_t leftover = initial;

    for (std::uint32_t iter = 0; iter < kIterations; ++iter) {
        sintra::init(argc, argv);
        sintra::finalize();

        leftover = count_subdirectories(sintra_base);
        if (leftover == 0) {
            break;
        }

        // Give the filesystem a moment on slower hosts.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (leftover != 0) {
        std::fprintf(stderr,
                     "ring_cleanup_stress: leftover directories after cleanup: %u\n",
                     leftover);
        return 1;
    }

    // Run a few extra init/finalize cycles to ensure directories stay clean.
    for (std::uint32_t iter = 0; iter < 8; ++iter) {
        sintra::init(argc, argv);
        sintra::finalize();
        if (count_subdirectories(sintra_base) != 0) {
            std::fprintf(stderr,
                         "ring_cleanup_stress: directories reappeared on cycle %u\n",
                         iter);
            return 1;
        }
    }

    return 0;
}

