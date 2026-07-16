#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

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

    const bool empty_returns_immediately = process.wait_for_all_child_custodies(
        std::chrono::steady_clock::now() + 1s);

    auto first = process.accept_child_custody();
    auto second = process.accept_child_custody();
    const auto first_iid = sintra::compose_instance(32u, 1u);
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
    const bool one_remaining = !process.all_child_custodies_released();
    bool exact_first_retired = false;
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
    bool registries_empty = false;
    {
        std::lock_guard<std::mutex> lock(process.m_child_custody_mutex);
        registries_empty = process.m_child_custodies.empty() &&
            process.m_child_custody_by_process.empty();
    }
    const bool retirement_before_wait_is_durable =
        process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 1s);

    auto cache_retirement = process.accept_child_custody();
    const auto cache_iid = sintra::compose_instance(34u, 1u);
    sintra::Managed_process::Spawn_swarm_process_args cached_spawn;
    cached_spawn.piid = cache_iid;
    cached_spawn.occurrence = 1;
    cached_spawn.custody = cache_retirement;
    {
        std::lock_guard<std::mutex> lock(process.m_cached_spawns_mutex);
        process.m_cached_spawns[cache_iid] = cached_spawn;
    }
    const bool cache_custody_released = mark_released(cache_retirement);
    auto cache_waiter = std::async(std::launch::async, [&] {
        return process.wait_for_all_child_custodies(
            std::chrono::steady_clock::now() + 3s);
    });

    std::unique_lock<std::mutex> cache_lock(process.m_cached_spawns_mutex);
    std::promise<void> retirement_started;
    auto retirement_started_future = retirement_started.get_future();
    std::thread cache_retirement_thread([&] {
        retirement_started.set_value();
        process.retire_child_custody_if_complete(cache_retirement);
    });
    retirement_started_future.get();

    bool registry_exposed_before_cache_cleanup = false;
    const auto exposure_deadline = std::chrono::steady_clock::now() + 1s;
    do {
        if (process.m_child_custody_mutex.try_lock()) {
            registry_exposed_before_cache_cleanup =
                process.m_child_custodies.find(cache_retirement->identity) ==
                    process.m_child_custodies.end();
            process.m_child_custody_mutex.unlock();
            if (registry_exposed_before_cache_cleanup) {
                break;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    while (std::chrono::steady_clock::now() < exposure_deadline);

    process.m_child_custody_changed.notify_all();
    const bool cache_waiter_blocked =
        cache_waiter.wait_for(100ms) == std::future_status::timeout;
    const auto cached = process.m_cached_spawns.find(cache_iid);
    const bool exact_cache_retained =
        cached != process.m_cached_spawns.end() &&
        cached->second.custody == cache_retirement;
    cache_lock.unlock();
    cache_retirement_thread.join();
    const bool cache_waiter_completed = cache_waiter.get();
    bool exact_cache_erased = false;
    {
        std::lock_guard<std::mutex> lock(process.m_cached_spawns_mutex);
        exact_cache_erased =
            process.m_cached_spawns.find(cache_iid) ==
                process.m_cached_spawns.end();
    }

    const bool finalized = sintra::detail::finalize();

    const bool valid = empty_returns_immediately && unresolved_times_out &&
        spurious_notification_ignored && first_released &&
        pending_record_retained && one_remaining && exact_first_retired &&
        second_released && terminal_record_still_registered &&
        waiters_still_blocked && waiters_completed && registries_empty &&
        retirement_before_wait_is_durable && cache_custody_released &&
        !registry_exposed_before_cache_cleanup && cache_waiter_blocked &&
        exact_cache_retained &&
        cache_waiter_completed && exact_cache_erased && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "MANAGED_CHILD_CUSTODY_CONDITION_WAIT_INVALID "
            "empty=%d unresolved=%d spurious=%d first_released=%d "
            "pending=%d one_remaining=%d exact_first=%d second_released=%d "
            "terminal_registered=%d prior_waiters_blocked=%d prior_waiters=%d "
            "registries_empty=%d durable=%d cache_released=%d "
            "registry_exposed_before_cache_cleanup=%d cache_waiter_blocked=%d "
            "cache_retained=%d cache_waiter_completed=%d cache_erased=%d "
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
            registry_exposed_before_cache_cleanup ? 1 : 0,
            cache_waiter_blocked ? 1 : 0,
            exact_cache_retained ? 1 : 0,
            cache_waiter_completed ? 1 : 0,
            exact_cache_erased ? 1 : 0,
            finalized ? 1 : 0);
    }
    return valid ? 0 : 2;
}
