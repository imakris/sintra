#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include <chrono>
#include <future>
#include <mutex>

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
    const bool finalized = sintra::detail::finalize();

    return empty_returns_immediately && unresolved_times_out && first_released &&
        pending_record_retained && one_remaining && exact_first_retired &&
        second_released && terminal_record_still_registered &&
        waiters_still_blocked && waiters_completed && registries_empty &&
        retirement_before_wait_is_durable && finalized
        ? 0
        : 2;
}
