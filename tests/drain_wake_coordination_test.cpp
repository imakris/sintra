#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view k_prefix = "drain_wake_coordination_test: ";

struct Wait_result
{
    std::atomic<bool> done{false};
    bool success = false;
    std::chrono::milliseconds elapsed{0};
};

bool wait_for_flag(
    const std::atomic<bool>& flag,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return flag.load(std::memory_order_acquire);
}

bool wait_for_completion(
    const Wait_result& result,
    std::chrono::milliseconds timeout)
{
    return wait_for_flag(result.done, timeout);
}

sintra::instance_id_type make_probe_process()
{
    auto probe = sintra::make_process_instance_id();
    if (probe == sintra::s_mproc_id) {
        probe = sintra::make_process_instance_id();
    }
    return probe;
}

void add_initializing_probe(sintra::instance_id_type process_iid)
{
    const auto process_index = static_cast<size_t>(sintra::get_process_index(process_iid));
    {
        std::lock_guard<std::mutex> init_lock(sintra::s_coord->m_init_tracking_mutex);
        sintra::s_coord->m_processes_in_initialization.insert(process_iid);
    }
    sintra::s_coord->m_draining_process_states[process_index] = 0;
}

void remove_initializing_probe(sintra::instance_id_type process_iid)
{
    const auto process_index = static_cast<size_t>(sintra::get_process_index(process_iid));
    {
        std::lock_guard<std::mutex> init_lock(sintra::s_coord->m_init_tracking_mutex);
        sintra::s_coord->m_processes_in_initialization.erase(process_iid);
    }
    sintra::s_coord->m_draining_process_states[process_index] = 0;
}

void add_group_probe(const std::string& group_name, sintra::instance_id_type process_iid)
{
    std::lock_guard<std::mutex> groups_lock(sintra::s_coord->m_groups_mutex);
    auto& group = sintra::s_coord->m_groups[group_name];
    std::lock_guard<std::mutex> group_lock(group.m_call_mutex);
    group.m_process_ids.insert(sintra::s_mproc_id);
    group.m_process_ids.insert(process_iid);
}

void remove_group_probe(const std::string& group_name, sintra::instance_id_type process_iid)
{
    std::lock_guard<std::mutex> groups_lock(sintra::s_coord->m_groups_mutex);
    auto group_it = sintra::s_coord->m_groups.find(group_name);
    if (group_it == sintra::s_coord->m_groups.end()) {
        return;
    }

    auto& group = group_it->second;
    std::lock_guard<std::mutex> group_lock(group.m_call_mutex);
    group.m_process_ids.erase(process_iid);
    group.m_process_ids.insert(sintra::s_mproc_id);
}

bool test_wait_for_all_draining_wakes_after_state_change()
{
    const auto probe = make_probe_process();
    add_initializing_probe(probe);
    sintra::s_coord->set_drain_timeout(std::chrono::seconds(2));

    Wait_result result;
    std::thread waiter([&]() {
        const auto start = std::chrono::steady_clock::now();
        result.success = sintra::s_coord->wait_for_all_draining(sintra::s_mproc_id);
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        result.done.store(true, std::memory_order_release);
    });

    bool ok = sintra::test::assert_true(
        wait_for_flag(sintra::s_coord->m_waiting_for_all_draining, 2s),
        k_prefix,
        "wait_for_all_draining() did not enter wait state");

    ok &= sintra::test::assert_true(
        !wait_for_completion(result, 100ms),
        k_prefix,
        "wait_for_all_draining() completed before the probe process drained");

    const auto process_index = static_cast<size_t>(sintra::get_process_index(probe));
    sintra::s_coord->m_draining_process_states[process_index] = 1;
    sintra::s_coord->note_draining_state_change();

    ok &= sintra::test::assert_true(
        wait_for_completion(result, 2s),
        k_prefix,
        "wait_for_all_draining() did not wake after note_draining_state_change()");

    if (!result.done.load(std::memory_order_acquire)) {
        sintra::s_coord->m_draining_process_states[process_index] = 1;
        sintra::s_coord->note_draining_state_change();
    }

    waiter.join();
    remove_initializing_probe(probe);
    sintra::s_coord->set_drain_timeout(std::chrono::seconds(20));

    ok &= sintra::test::assert_true(
        result.success,
        k_prefix,
        "wait_for_all_draining() should succeed once the probe process is draining");
    ok &= sintra::test::assert_true(
        result.elapsed < 1500ms,
        k_prefix,
        "wait_for_all_draining() did not complete promptly after the state change");
    return ok;
}

bool test_shutdown_drain_wait_wakes_after_group_change()
{
    const auto probe = make_probe_process();
    const std::string group_name = "_sintra_all_processes";
    add_group_probe(group_name, probe);

    Wait_result result;
    std::thread waiter([&]() {
        const auto start = std::chrono::steady_clock::now();
        sintra::detail::shutdown_coordinator_drain_wait(group_name);
        result.success = true;
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        result.done.store(true, std::memory_order_release);
    });

    bool ok = sintra::test::assert_true(
        wait_for_flag(sintra::s_coord->m_waiting_for_all_draining, 2s),
        k_prefix,
        "shutdown_coordinator_drain_wait() did not enter wait state");

    ok &= sintra::test::assert_true(
        !wait_for_completion(result, 100ms),
        k_prefix,
        "shutdown_coordinator_drain_wait() completed before the group drained");

    remove_group_probe(group_name, probe);
    sintra::s_coord->note_draining_state_change();

    ok &= sintra::test::assert_true(
        wait_for_completion(result, 2s),
        k_prefix,
        "shutdown_coordinator_drain_wait() did not wake after note_draining_state_change()");

    if (!result.done.load(std::memory_order_acquire)) {
        remove_group_probe(group_name, probe);
        sintra::s_coord->note_draining_state_change();
    }

    waiter.join();
    remove_group_probe(group_name, probe);

    ok &= sintra::test::assert_true(
        result.success,
        k_prefix,
        "shutdown_coordinator_drain_wait() should return once only the coordinator remains");
    ok &= sintra::test::assert_true(
        result.elapsed < 1500ms,
        k_prefix,
        "shutdown_coordinator_drain_wait() did not complete promptly after the group drained");
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    sintra::init(argc, argv);

    bool ok = true;
    ok &= sintra::test::assert_true(sintra::s_coord != nullptr, k_prefix, "coordinator should exist after init()");

    if (ok) {
        ok &= test_wait_for_all_draining_wakes_after_state_change();
        ok &= test_shutdown_drain_wait_wakes_after_group_change();
    }

    sintra::detail::finalize();
    return ok ? 0 : 1;
}
