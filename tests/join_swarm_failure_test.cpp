#define SINTRA_ENABLE_TEST_HOOKS 1
#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

using sintra::s_coord;

namespace {

struct Post_spawn_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
};

Post_spawn_gate* s_post_spawn_gate = nullptr;

void hold_join_post_spawn(
    sintra::detail::Managed_child_post_spawn_stage stage,
    sintra::instance_id_type,
    uint32_t)
{
    auto* gate = s_post_spawn_gate;
    if (!gate ||
        stage != sintra::detail::Managed_child_post_spawn_stage::join_no_child)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [&]() { return gate->release; });
}

bool settle_finalize()
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (sintra::detail::finalize()) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return sintra::detail::finalize();
}

bool run_join_failure_finalize_fence(const std::string& missing_binary)
{
    Post_spawn_gate gate;
    s_post_spawn_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_post_spawn.store(
        &hold_join_post_spawn, std::memory_order_release);

    std::atomic<bool> join_returned{false};
    std::atomic<sintra::instance_id_type> join_result{sintra::invalid_instance_id};
    std::thread join_thread([&]() {
        join_result.store(
            sintra::join_swarm(1, missing_binary), std::memory_order_release);
        join_returned.store(true, std::memory_order_release);
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        entered = gate.changed.wait_for(
            lock, 5s, [&]() { return gate.entered; });
    }
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.release = true;
        }
        gate.changed.notify_all();
        join_thread.join();
        sintra::detail::test_hooks::s_managed_child_post_spawn.store(
            nullptr, std::memory_order_release);
        s_post_spawn_gate = nullptr;
        return false;
    }

    std::atomic<bool> watchdog_released{false};
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.changed.wait_for(lock, 5s, [&]() { return gate.release; })) {
            watchdog_released.store(true, std::memory_order_release);
            gate.release = true;
            lock.unlock();
            gate.changed.notify_all();
        }
    });

    const bool first_finalize = sintra::detail::finalize();
    const bool finalized_while_held = first_finalize &&
        !watchdog_released.load(std::memory_order_acquire);
    if (finalized_while_held) {
        std::fprintf(
            stderr,
            "A3A_JOIN_LIFETIME_RED finalize_succeeded_before_join_rollback=1\n");
        std::fflush(stderr);
        std::_Exit(1);
    }

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.changed.notify_all();
    join_thread.join();
    watchdog.join();
    sintra::detail::test_hooks::s_managed_child_post_spawn.store(
        nullptr, std::memory_order_release);
    s_post_spawn_gate = nullptr;

    const bool finalized = first_finalize || settle_finalize();
    const bool valid = finalized &&
        join_returned.load(std::memory_order_acquire) &&
        join_result.load(std::memory_order_acquire) == sintra::invalid_instance_id;
    if (valid) {
        std::fprintf(
            stdout,
            "A3A_JOIN_LIFETIME_GREEN finalize_succeeded_before_join_rollback=0\n");
    }
    return valid;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    if (!s_coord) {
        sintra::detail::finalize();
        return 1;
    }

    const auto before_inflight = s_coord->m_inflight_joins.size();
    const auto before_joined   = s_coord->m_joined_process_branch.size();
    const auto before_groups   = s_coord->m_groups_of_process.size();
    const auto custody_count = []() {
        std::lock_guard lock(sintra::s_mproc->m_child_custody_mutex);
        return sintra::s_mproc->m_child_custodies.size();
    };
    const auto initialization_count = []() {
        std::lock_guard lock(s_coord->m_init_tracking_mutex);
        return s_coord->m_processes_in_initialization.size();
    };
    const auto before_custodies     = custody_count();
    const auto before_initializing  = initialization_count();

    const auto missing_dir     = sintra::test::unique_scratch_directory("join_swarm_failure");
    const auto missing_binary  = (missing_dir / "definitely_missing_sintra_binary").string();
    const auto result          = sintra::join_swarm(1, missing_binary);

    const bool inflight_ok     = (s_coord->m_inflight_joins.size() == before_inflight);
    const bool joined_ok       = (s_coord->m_joined_process_branch.size() == before_joined);
    const bool groups_ok       = (s_coord->m_groups_of_process.size() == before_groups);
    const auto custody_deadline = std::chrono::steady_clock::now() + 5s;
    while (
        custody_count() != before_custodies                      &&
        std::chrono::steady_clock::now() < custody_deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    const bool custody_ok       = custody_count() == before_custodies;
    const bool initialization_ok = initialization_count() == before_initializing;

    const bool lifetime_fence_ok =
        run_join_failure_finalize_fence(missing_binary);

    if (result != sintra::invalid_instance_id) {
        return 1;
    }

    return (inflight_ok && joined_ok && groups_ok && custody_ok &&
            initialization_ok && lifetime_fence_ok)
        ? 0
        : 1;
}
