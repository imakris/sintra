//
// Sintra Ring Cleanup Stress Test
//
// Seeds stale run markers, then initializes a coordinator to exercise its
// platform cleanup policy and normal directory teardown.
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/time_utils.h>

#include "test_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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

std::uint32_t count_subdirectories(const std::filesystem::path& base)
{
    std::uint32_t count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(base, ec);
        !ec && it != std::filesystem::directory_iterator(); ++it)
    {
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
    const auto scratch   = sintra::test::unique_scratch_directory("ring_cleanup_stress");
    const auto temp_root = scratch / "temp_root";
    std::filesystem::create_directories(temp_root);

    configure_temp_directory(temp_root);

    constexpr std::uint32_t stale_runs = 80;
    for (std::uint32_t i = 0; i < stale_runs; ++i) {
        const auto directory = sintra::detail::private_swarm_root(i + 1,
#ifndef _WIN32
            true
#else
            false
#endif
        );
        sintra::test::require_true(
            sintra::detail::create_private_swarm_directory_exclusive(directory) ==
                sintra::detail::private_swarm_create_result::created,
            "ring_cleanup_stress: ", "create direct private swarm directory");
#ifdef _WIN32
        sintra::run_marker_record_t record{};
        record.pid = 0; // Explicit cleanup marker; no coordinator identity remains.
        sintra::test::require_true(sintra::write_run_marker(directory, record),
            "ring_cleanup_stress: ", "publish cleanup marker");
        sintra::mark_run_directory_for_cleanup(directory);
#endif
    }
    sintra::init(argc, argv);
    sintra::detail::finalize();
    sintra::test::require_true(count_subdirectories(temp_root) == 0,
        "ring_cleanup_stress: ", "all stale direct swarm directories removed");
    return 0;
}
