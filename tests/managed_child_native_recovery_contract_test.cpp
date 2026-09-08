// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using sintra::Managed_child_native_admission;
using sintra::Managed_child_native_error_domain;
using sintra::Managed_child_native_outcome;
using sintra::Managed_child_native_state;

constexpr const char* k_child_flag = "--native-recovery-child";
constexpr const char* k_nonce_flag = "--native-recovery-nonce";
constexpr auto k_child_iid = sintra::compose_instance(47u, 1ull);
#ifdef _WIN32
constexpr const char* k_ordinary_flag = "--native-ordinary-child";
constexpr const char* k_ordinary_ready = "native_ordinary_cleanup_ready";
constexpr auto k_ordinary_iid = sintra::compose_instance(49u, 1ull);

struct Ordinary_stages
{
    std::mutex        mutex;
    Clock::time_point entered;
    Clock::time_point soft;
    Clock::time_point hard;
};

Ordinary_stages* s_ordinary_stages = nullptr;

void ordinary_stage(const char* stage, sintra::instance_id_type iid, uint32_t)
{
    if (iid != k_ordinary_iid || !s_ordinary_stages) {
        return;
    }
    auto& stages = *s_ordinary_stages;
    std::lock_guard<std::mutex> lock(stages.mutex);
    const auto name = std::string_view(stage);
    if (name == sintra::detail::test_hooks::k_managed_child_cleanup_before_native_convergence) {
        stages.entered = Clock::now();
    }
    if (name == sintra::detail::test_hooks::k_managed_child_cleanup_soft_termination) {
        stages.soft = Clock::now();
    }
    if (name == sintra::detail::test_hooks::k_managed_child_cleanup_hard_termination) {
        stages.hard = Clock::now();
    }
}
#endif


class Held_call : public sintra::Derived_transceiver<Held_call>
{
public:
    int hold()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_entered = true;
        m_changed.notify_all();
        m_changed.wait_for(lock, std::chrono::seconds(35), [&]() { return m_released; });
        return 1;
    }
    SINTRA_RPC(hold)

    bool wait_for_entry()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5), [&]() { return m_entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_changed;
    bool                    m_entered = false;
    bool                    m_released = false;
};

struct Native_failure_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered = false;
    bool                    released = false;
    unsigned                calls = 0;
};

Native_failure_gate* s_native_gate = nullptr;

int native_permission_failure(sintra::instance_id_type process_iid, uint32_t occurrence)
{
    if (process_iid != k_child_iid || occurrence != 0 || !s_native_gate) {
        return 0;
    }
    auto& gate = *s_native_gate;
    std::unique_lock<std::mutex> lock(gate.mutex);
    ++gate.calls;
    gate.entered = true;
    gate.changed.notify_all();
    gate.changed.wait_for(lock, std::chrono::seconds(5), [&]() { return gate.released; });
#ifdef _WIN32
    return ERROR_ACCESS_DENIED;
#else
    return EPERM;
#endif
}

bool check(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return value;
}

template <typename Predicate>
bool wait_for_native(
    const sintra::Managed_child_custody& custody,
    const sintra::Managed_child_native_change_signal& changes,
    Predicate predicate,
    Clock::time_point deadline)
{
    while (true) {
        const auto generation = changes.generation();
        const auto snapshot = custody.native_snapshot();
        if (snapshot.size() == 1 && predicate(snapshot.front())) {
            return true;
        }
        if (Clock::now() >= deadline) {
            return false;
        }
        changes.wait_for_change(generation, deadline);
    }
}

int run_child(int argc, char* argv[])
{
    const auto nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    sintra::init(argc, argv);
    // This bounds a broken test; production custody must establish exit first.
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(40));
        std::_Exit(3);
    }).detach();
    Held_call ready;
    if (!ready.assign_name("native_recovery_ready_" + nonce)) {
        return 2;
    }
    const auto target = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, "native_recovery_hold_" + nonce);
    if (target == sintra::invalid_instance_id) {
        return 2;
    }
    try {
        (void)Held_call::rpc_hold(target);
    }
    catch (...) {}
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

#ifdef _WIN32
int run_ordinary_child(int argc, char* argv[])
{
    sintra::init(argc, argv);
    SetConsoleCtrlHandler([](DWORD signal) -> BOOL { return signal == CTRL_BREAK_EVENT; }, TRUE);
    Held_call ready;
    if (!ready.assign_name(k_ordinary_ready)) {
        return 2;
    }
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::_Exit(3);
}

bool ordinary_windows_cleanup(int argc, char* argv[])
{
    using namespace std::chrono_literals;
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {k_ordinary_flag};
    options.process_instance_id = k_ordinary_iid;
    options.readiness_instance_name = k_ordinary_ready;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    bool valid = check(custody.wait_for_readiness_until(Clock::now() + 8s).readiness_state ==
        sintra::Managed_child_readiness_state::reached, "ordinary Windows child readiness");
    Ordinary_stages stages;
    s_ordinary_stages = &stages;
    sintra::detail::test_hooks::s_managed_child_cleanup.store(&ordinary_stage);
    const auto released = custody.terminate_until(Clock::now() + 10s);
    sintra::detail::test_hooks::s_managed_child_cleanup.store(nullptr);
    s_ordinary_stages = nullptr;
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete,
        "ordinary Windows cleanup completes");
    {
        std::lock_guard<std::mutex> lock(stages.mutex);
        valid &= check(stages.entered != Clock::time_point{} && stages.soft >= stages.entered &&
            stages.soft - stages.entered >= 5800ms && stages.soft - stages.entered < 8s &&
            stages.hard >= stages.soft && stages.hard - stages.soft < 2s,
            "ordinary Windows cleanup preserves six-second grace before soft and bounded hard termination");
    }
    return valid;
}
#endif

int run_root(int argc, char* argv[])
{
    using namespace std::chrono_literals;
    sintra::init(argc, argv);
    Held_call held;
    const auto nonce = std::to_string(Clock::now().time_since_epoch().count());
    if (!held.assign_name("native_recovery_hold_" + nonce)) {
        return 2;
    }
    sintra::Spawn_options options;
    options.binary_path = sintra::test::get_binary_path(argc, argv);
    options.args = {k_child_flag, k_nonce_flag, nonce};
    options.process_instance_id = k_child_iid;
    options.readiness_instance_name = "native_recovery_ready_" + nonce;
    // The lifeline must not mask a blocked native cleanup action in this test.
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    sintra::Managed_child_native_change_signal changes;
    bool valid = check(custody.observe_native_changes(changes), "native watch registration");
    valid &= check(changes.generation() != 0, "watch registration wakes inventory");
    valid &= check(custody.wait_for_readiness_until(Clock::now() + 8s).readiness_state ==
        sintra::Managed_child_readiness_state::reached, "child readiness");
    valid &= check(held.wait_for_entry(), "real child RPC is held in parent");
    const auto initial = custody.native_snapshot();
    valid &= check(initial.size() == 1 && initial.front().state == Managed_child_native_state::RUNNING &&
        initial.front().ownership_ready,
        "native snapshot identifies one live original occurrence");
    if (initial.size() != 1) {
        held.release();
        custody.terminate_until(Clock::now() + 12s);
        held.destroy();
        sintra::shutdown();
        return 2;
    }
    const auto identity = initial.front().occurrence;
    const int child_pid = initial.front().pid;
    const auto stamp = sintra::query_process_start_stamp((uint32_t)child_pid);
    valid &= check(stamp.has_value(), "independent exact native identity captured");
    std::fprintf(stderr, "OWNED_NATIVE_RECOVERY parent=%d child=%d start=%llu custody=%llu occurrence=%u\n",
        sintra::test::get_pid(), child_pid, (unsigned long long)stamp.value_or(0),
        (unsigned long long)identity.custody_identity, identity.occurrence);

    sintra::test::managed_child::Managed_child_exit_capture exited;
    auto exit_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exited.record(event); });
    valid &= check((bool)exit_observation, "exact exit observation registered");
    const auto passive = custody.release_until(Clock::now() + 100ms);
    valid &= check(passive.release_state != sintra::Managed_child_release_state::complete,
        "held RPC prevents custody retirement");

    Native_failure_gate gate;
    s_native_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_native_termination_error.store(
        &native_permission_failure, std::memory_order_release);
    auto wrong_identity = identity;
    ++wrong_identity.custody_identity;
    valid &= check(custody.request_native_termination(wrong_identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::REJECTED, "another custody identity cannot authorize native kill");
    valid &= check(custody.request_native_termination(identity, Clock::now()).admission ==
        Managed_child_native_admission::REJECTED, "expired native attempt cannot begin");

    const auto started_at = Clock::now();
    sintra::Managed_child_native_request first;
    {
        // A held general lifecycle admission boundary cannot hide an already
        // owned child's native recovery access. No transport is forced here.
        std::lock_guard<std::mutex> admission(sintra::detail::s_teardown_admission_mutex);
        first = custody.request_native_termination(identity, Clock::now() + 5s);
    }
    valid &= check(first.admission == Managed_child_native_admission::STARTED &&
        Clock::now() - started_at < 500ms, "native request returns while communication is held");
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        valid &= check(gate.changed.wait_for(lock, 2s, [&]() { return gate.entered; }),
            "native syscall reached without releasing held RPC");
    }
    const auto duplicate = custody.request_native_termination(identity, Clock::now() + 5s);
    valid &= check(duplicate.admission == Managed_child_native_admission::ALREADY_ACTIVE &&
        duplicate.action_generation == first.action_generation, "concurrent retry shares exact native action");
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
        gate.changed.notify_all();
    }
    valid &= check(wait_for_native(custody, changes, [](const auto& state) {
        return state.action.outcome == Managed_child_native_outcome::FAILED;
    }, Clock::now() + 3s), "permission failure produces event-driven native status");
    const auto failed = custody.native_snapshot().front();
#ifdef _WIN32
    valid &= check(failed.action.error.domain == Managed_child_native_error_domain::WINDOWS &&
        failed.action.error.code == ERROR_ACCESS_DENIED && failed.action.error.operation == "TerminateProcess",
        "Windows termination error survives native cleanup");
#else
    valid &= check(failed.action.error.domain == Managed_child_native_error_domain::POSIX &&
        failed.action.error.code == EPERM && failed.action.error.operation == "kill(SIGKILL)",
        "POSIX termination error survives native cleanup");
#endif
    valid &= check(failed.state == Managed_child_native_state::RUNNING && stamp &&
        sintra::test::managed_child::exact_process_is_live(child_pid, *stamp),
        "injected native failure retains the exact live child");
    const auto release_observation_deadline = Clock::now() + 3s;
    while (custody.status().last_failure.kind == sintra::Managed_child_failure_kind::none &&
           Clock::now() < release_observation_deadline)
    {
        const auto generation = changes.generation();
        if (custody.status().last_failure.kind != sintra::Managed_child_failure_kind::none) {
            break;
        }
        changes.wait_for_change(generation, release_observation_deadline);
    }
    const auto retained_failure = custody.native_snapshot().front();
    valid &= check(retained_failure.action.generation == first.action_generation &&
        retained_failure.action.outcome == Managed_child_native_outcome::FAILED,
        "ordinary cleanup cannot replace a recovery failure with an unsolicited retry");
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        valid &= check(gate.calls == 1, "one serialized native syscall attempt");
    }
    sintra::detail::test_hooks::s_managed_child_native_termination_error.store(nullptr);
    const auto retry = custody.request_native_termination(identity, Clock::now() + 5s);
    valid &= check(retry.admission == Managed_child_native_admission::STARTED &&
        retry.action_generation > first.action_generation, "explicit retry starts newer bounded native action");
    valid &= check(wait_for_native(custody, changes, [](const auto& state) {
        return state.state == Managed_child_native_state::EXITED &&
            state.action.outcome == Managed_child_native_outcome::EXITED;
    }, Clock::now() + 6s), "exact native exit progresses while original RPC remains held");
    valid &= check(exited.wait_for_one(1s) && exited.exact(identity), "original native observer reports exact exit");
    valid &= check(custody.status().release_state != sintra::Managed_child_release_state::complete,
        "native exit does not invent communication retirement");
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::ALREADY_EXITED, "already exited child needs no further termination");
    valid &= check(custody.status().admitted_occurrences == 1, "cleanup fence prevents replacement occurrence");

    // A single aggregate waiter can stop without periodic polling or one
    // waiting thread per child, including when no further native event occurs.
    std::atomic<bool> waiter_stopped = false;
    std::jthread waiter([&](std::stop_token stop) {
        changes.wait_for_change(changes.generation(), Clock::time_point::max(), stop);
        waiter_stopped.store(true, std::memory_order_release);
    });
    waiter.request_stop();
    waiter.join();
    valid &= check(waiter_stopped.load(std::memory_order_acquire), "aggregate wait cancellation wakes immediately");

    held.release();
    const auto released = custody.terminate_until(Clock::now() + 12s);
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete,
        "original custody settles after held communication resumes");
    exit_observation.subscription.unsubscribe();
    s_native_gate = nullptr;
    valid &= check(stamp && !sintra::test::managed_child::exact_process_is_live(child_pid, *stamp),
        "owned child absent after native cleanup");
    held.destroy();
#ifdef _WIN32
    valid &= ordinary_windows_cleanup(argc, argv);
#endif
    valid &= check(sintra::shutdown(), "runtime shuts down after original cleanup settles");
    valid &= check(custody.native_snapshot().front().state == Managed_child_native_state::EXITED,
        "retained native facts remain readable after runtime shutdown");
    valid &= check(custody.request_native_termination(identity, Clock::now() + 5s).admission ==
        Managed_child_native_admission::REJECTED, "stale runtime cannot admit native work");
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "mc_native_recovery");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv);
    }
#ifdef _WIN32
    if (sintra::test::has_argv_flag(argc, argv, k_ordinary_flag)) {
        return run_ordinary_child(argc, argv);
    }
#endif
    return run_root(argc, argv);
}
