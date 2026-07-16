#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr auto k_watchdog_timeout = 15s;

struct Retirement_gate
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    uint64_t                   custody_identity   = 0;
    bool                       before_cache_erase = false;
    bool                       release            = false;
    bool                       complete           = false;
};

Retirement_gate* s_retirement_gate = nullptr;

void custody_retirement_stage(const char* stage, uint64_t custody_identity) noexcept
{
    auto* gate = s_retirement_gate;
    if (!gate || custody_identity != gate->custody_identity) {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    if (std::string_view(stage) == sintra::detail::test_hooks::
        k_managed_child_custody_retirement_before_cache_erase)
    {
        gate->before_cache_erase = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
    }
    else
    if (std::string_view(stage) == sintra::detail::test_hooks::
        k_managed_child_custody_retirement_complete)
    {
        gate->complete = true;
        gate->changed.notify_all();
    }
}

bool wait_for_retirement_stage(
    Retirement_gate&                       gate,
    bool Retirement_gate::*                stage,
    std::chrono::steady_clock::time_point  deadline)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.changed.wait_until(lock, deadline, [&] { return gate.*stage; });
}

bool mark_released(
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>& custody)
{
    std::lock_guard<std::mutex> lock(custody->mutex);
    const auto generation = custody->release_state.request(
        sintra::detail::Release_mode::passive);
    return custody->release_state.mark_released(generation);
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);
    auto& process = *sintra::s_mproc;

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + k_watchdog_timeout;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                    "MANAGED_CHILD_CUSTODY_CONDITION_WAIT_INVALID "
                    "watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(10ms);
        }
    });

    const bool empty_returns_immediately = process.wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + 1s);

    auto       first      = process.accept_child_custody();
    auto       second     = process.accept_child_custody();
    const auto first_iid  = sintra::compose_instance(32u, 1u);
    const auto second_iid = sintra::compose_instance(33u, 1u);
    {
        std::lock_guard<std::mutex> lock(process.m_child_custody_mutex);
        process.m_child_custody_by_process.emplace(
            first_iid,
            sintra::detail::Managed_child_active_occurrence{first, 0});
        process.m_child_custody_by_process.emplace(
            second_iid,
            sintra::detail::Managed_child_active_occurrence{second, 0});
    }
    const bool unresolved_times_out = !process.wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + 20ms);
    {
        std::lock_guard<std::mutex> lock(first->mutex);
        first->readiness = sintra::detail::Readiness_phase::pending;
    }

    auto waiter_1 = std::async(std::launch::async, [&] {
        return process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 2s);
    });
    auto waiter_2 = std::async(std::launch::async, [&] {
        return process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 2s);
    });
    process.m_child_custody_changed.notify_all();
    const bool spurious_notification_ignored =
        waiter_1.wait_for(20ms) == std::future_status::timeout &&
        waiter_2.wait_for(20ms) == std::future_status::timeout;

    const bool first_released = mark_released(first);
    process.retire_child_custody_if_complete(first);
    bool pending_record_retained = false;
    {
        std::lock_guard<std::mutex> lock(process.m_child_custody_mutex);
        pending_record_retained =
            process.m_child_custodies.find(first->identity) !=
                process.m_child_custodies.end() &&
            process.m_child_custody_by_process.find(first_iid) !=
                process.m_child_custody_by_process.end();
    }
    {
        std::lock_guard<std::mutex> lock(first->mutex);
        first->readiness = sintra::detail::Readiness_phase::reached;
    }
    process.retire_child_custody_if_complete(first);
    const bool one_remaining       = !process.all_child_custodies_released();
    bool       exact_first_retired = false;
    {
        std::lock_guard<std::mutex> lock(process.m_child_custody_mutex);
        const auto second_active =
            process.m_child_custody_by_process.find(second_iid);
        exact_first_retired =
            process.m_child_custodies.find(first->identity) ==
                process.m_child_custodies.end() &&
            process.m_child_custody_by_process.find(first_iid) ==
                process.m_child_custody_by_process.end() &&
            process.m_child_custodies.find(second->identity) !=
                process.m_child_custodies.end() &&
            second_active != process.m_child_custody_by_process.end() &&
            second_active->second.custody.lock() == second;
    }

    const bool second_released = mark_released(second);
    const bool terminal_record_still_registered =
        !process.all_child_custodies_released();
    const bool waiters_still_blocked =
        waiter_1.wait_for(50ms) == std::future_status::timeout &&
        waiter_2.wait_for(50ms) == std::future_status::timeout;

    process.retire_child_custody_if_complete(second);
    const bool waiters_completed = waiter_1.get() && waiter_2.get();
    bool       registries_empty  = false;
    {
        std::lock_guard<std::mutex> lock(process.m_child_custody_mutex);
        registries_empty = process.m_child_custodies.empty() &&
            process.m_child_custody_by_process.empty();
    }
    const bool retirement_before_wait_is_durable =
        process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 1s);

    auto       cache_retirement = process.accept_child_custody();
    const auto cache_iid        = sintra::compose_instance(34u, 1u);
    sintra::Managed_process::Spawn_swarm_process_args cached_spawn;
    cached_spawn.piid       = cache_iid;
    cached_spawn.occurrence = 1;
    cached_spawn.custody    = cache_retirement;
    {
        std::lock_guard<std::mutex> lock(process.m_cached_spawns_mutex);
        process.m_cached_spawns[cache_iid] = cached_spawn;
    }
    const bool cache_custody_released = mark_released(cache_retirement);
    Retirement_gate retirement_gate;
    retirement_gate.custody_identity = cache_retirement->identity;
    s_retirement_gate = &retirement_gate;
    sintra::test::managed_child::Scoped_test_hook retirement_hook(
        sintra::detail::test_hooks::s_managed_child_custody_retirement,
        &custody_retirement_stage);
    std::thread cache_retirement_thread([&] {
        process.retire_child_custody_if_complete(cache_retirement);
    });

    const bool retirement_before_cache_erase = wait_for_retirement_stage(
        retirement_gate,
        &Retirement_gate::before_cache_erase,
        std::chrono::steady_clock::now() + 1s);
    bool exact_cache_retained = false;
    {
        std::lock_guard<std::mutex> lock(process.m_cached_spawns_mutex);
        const auto cached = process.m_cached_spawns.find(cache_iid);
        exact_cache_retained =
            cached != process.m_cached_spawns.end() &&
            cached->second.custody == cache_retirement;
    }
    const bool wait_blocked_until_cache_retirement =
        retirement_before_cache_erase &&
        !process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 100ms);
    {
        std::lock_guard<std::mutex> lock(retirement_gate.mutex);
        retirement_gate.release = true;
    }
    retirement_gate.changed.notify_all();
    const bool cache_retirement_completed = wait_for_retirement_stage(
        retirement_gate,
        &Retirement_gate::complete,
        std::chrono::steady_clock::now() + 1s);
    cache_retirement_thread.join();
    s_retirement_gate = nullptr;
    const bool completed_wait_returns = process.wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + 1s);
    bool exact_cache_erased = false;
    {
        std::lock_guard<std::mutex> lock(process.m_cached_spawns_mutex);
        exact_cache_erased =
            process.m_cached_spawns.find(cache_iid) ==
                process.m_cached_spawns.end();
    }

    const bool finalized = sintra::detail::finalize();
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    const bool valid = empty_returns_immediately && unresolved_times_out &&
        spurious_notification_ignored && first_released &&
        pending_record_retained && one_remaining && exact_first_retired &&
        second_released && terminal_record_still_registered &&
        waiters_still_blocked && waiters_completed && registries_empty &&
        retirement_before_wait_is_durable && cache_custody_released &&
        retirement_before_cache_erase && exact_cache_retained &&
        wait_blocked_until_cache_retirement && cache_retirement_completed &&
        completed_wait_returns && exact_cache_erased && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "MANAGED_CHILD_CUSTODY_CONDITION_WAIT_INVALID "
            "empty=%d unresolved=%d spurious=%d first_released=%d "
            "pending=%d one_remaining=%d exact_first=%d second_released=%d "
            "terminal_registered=%d prior_waiters_blocked=%d prior_waiters=%d "
            "registries_empty=%d durable=%d cache_released=%d "
            "before_cache_erase=%d cache_retained=%d wait_blocked=%d "
            "retirement_complete=%d completed_wait=%d cache_erased=%d "
            "finalized=%d\n",
            empty_returns_immediately ? 1 : 0,
            unresolved_times_out ? 1 : 0,
            spurious_notification_ignored ? 1 : 0,
            first_released ? 1 : 0,
            pending_record_retained ? 1 : 0,
            one_remaining ? 1 : 0,
            exact_first_retired ? 1 : 0,
            second_released ? 1 : 0,
            terminal_record_still_registered ? 1 : 0,
            waiters_still_blocked ? 1 : 0,
            waiters_completed ? 1 : 0,
            registries_empty ? 1 : 0,
            retirement_before_wait_is_durable ? 1 : 0,
            cache_custody_released ? 1 : 0,
            retirement_before_cache_erase ? 1 : 0,
            exact_cache_retained ? 1 : 0,
            wait_blocked_until_cache_retirement ? 1 : 0,
            cache_retirement_completed ? 1 : 0,
            completed_wait_returns ? 1 : 0,
            exact_cache_erased ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid ? 0 : 2;
}
