#include <sintra/sintra.h>
#include <sintra/detail/process/managed_process.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* k_child_flag = "--recovery_unpublish_deadlock_child";
constexpr const char* k_fake_name  = "recovery_unpublish_deadlock_fake";

bool wait_for_done(const std::atomic<bool>& done, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

void seed_recoverable_fake_process(
    sintra::instance_id_type   fake_piid,
    const std::string&         binary_path)
{
    {
        std::lock_guard publish_lock(sintra::s_coord->m_publish_mutex);
        sintra::s_coord->m_transceiver_registry[fake_piid][fake_piid] =
            sintra::Tn_type{0, k_fake_name};
        sintra::s_mproc->m_instance_id_of_assigned_name.set_value(
            k_fake_name,
            fake_piid);
    }

    {
        std::lock_guard lifecycle_lock(sintra::s_coord->m_lifecycle_mutex);
        sintra::s_coord->m_requested_recovery.insert(fake_piid);
    }

    sintra::Managed_process::Spawn_swarm_process_args spawn_args;
    spawn_args.binary_name = binary_path;
    spawn_args.args = {
        binary_path,
        k_child_flag
    };
    spawn_args.piid = fake_piid;
    spawn_args.occurrence = 1;
    spawn_args.lifetime.enable_lifeline = false;
    sintra::s_mproc->m_cached_spawns[fake_piid] = std::move(spawn_args);
}

} // namespace

int main(int argc, char* argv[])
{
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return 0;
    }

    sintra::init(argc, argv);

    const auto fake_piid = sintra::make_process_instance_id();
    seed_recoverable_fake_process(
        fake_piid,
        sintra::test::get_binary_path(argc, argv));

    std::atomic<bool> unpublish_done{false};
    std::atomic<bool> unpublish_result{false};
    std::thread unpublisher([&] {
        unpublish_result.store(
            sintra::Coordinator::rpc_unpublish_transceiver(
                sintra::s_coord_id,
                fake_piid),
            std::memory_order_release);
        unpublish_done.store(true, std::memory_order_release);
    });

    if (!wait_for_done(unpublish_done, 5s)) {
        std::fprintf(stderr,
            "recovery_unpublish_deadlock_test: unpublish did not return\n");
        std::fflush(stderr);
        std::_Exit(1);
    }

    unpublisher.join();
    if (!unpublish_result.load(std::memory_order_acquire)) {
        sintra::detail::finalize();
        return 1;
    }

    std::this_thread::sleep_for(100ms);
    sintra::detail::finalize();
    return 0;
}
