// Copyright (c) 2025, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

// Public-custody adverse cleanup contract. Production authority in this test is
// limited to the opaque Managed_child_custody API; hooks only hold/observe
// Sintra-owned cleanup and reap stages.

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag = "--managed_child_public_cleanup_child";
constexpr std::string_view k_fallback_child_flag =
    "--managed_child_public_cleanup_fallback_child";
constexpr std::string_view k_nonce_flag = "--managed_child_public_cleanup_nonce";
constexpr std::string_view k_ledger_file = "child_ledger.complete";
constexpr std::string_view k_fallback_ledger_file =
    "fallback_child_ledger.complete";
constexpr std::string_view k_fallback_exit_file = "fallback_child_exit.complete";
constexpr auto k_watchdog_timeout = 12s;
constexpr sintra::instance_id_type k_child_process_iid =
    sintra::compose_instance(37u, 1ull);
constexpr sintra::instance_id_type k_fallback_child_process_iid =
    sintra::compose_instance(38u, 1ull);

struct Ready_target : sintra::Derived_transceiver<Ready_target>
{};

struct Child_ledger
{
    std::string nonce;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    uint32_t occurrence = std::numeric_limits<uint32_t>::max();
    int pid = -1;
    bool start_stamp_available = false;
    uint64_t start_stamp = 0;
    std::string managed_name;
    std::string ready_name;
    sintra::instance_id_type ready_iid = sintra::invalid_instance_id;
};

fs::path marker_path(const fs::path& directory, std::string_view name)
{
    return directory / std::string(name);
}

std::optional<Child_ledger> read_ledger(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Child_ledger ledger;
    bool complete = false;
    std::string line;
    try {
        while (std::getline(in, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos) {
                return std::nullopt;
            }
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "nonce") ledger.nonce = value;
            else if (key == "piid") ledger.process_iid =
                static_cast<sintra::instance_id_type>(std::stoull(value));
            else if (key == "occurrence") ledger.occurrence =
                static_cast<uint32_t>(std::stoul(value));
            else if (key == "pid") ledger.pid = std::stoi(value);
            else if (key == "start_stamp_available")
                ledger.start_stamp_available = value == "1";
            else if (key == "start_stamp") ledger.start_stamp = std::stoull(value);
            else if (key == "managed_name") ledger.managed_name = value;
            else if (key == "ready_name") ledger.ready_name = value;
            else if (key == "ready_iid") ledger.ready_iid =
                static_cast<sintra::instance_id_type>(std::stoull(value));
            else if (key == "complete") complete = value == "1";
        }
    }
    catch (...) {
        return std::nullopt;
    }
    if (!complete || ledger.nonce.empty() || ledger.pid <= 0 ||
        ledger.managed_name.empty() || ledger.ready_name.empty())
    {
        return std::nullopt;
    }
    return ledger;
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

std::optional<sintra::instance_id_type> resolve_publicly(const std::string& name)
{
    try {
        return sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, name);
    }
    catch (...) {
        return std::nullopt;
    }
}

bool exact_child_is_live(const Child_ledger& ledger)
{
    if (!ledger.start_stamp_available || ledger.pid <= 0 ||
        !sintra::is_process_alive(static_cast<uint32_t>(ledger.pid)))
    {
        return false;
    }
    const auto stamp = sintra::query_process_start_stamp(
        static_cast<uint32_t>(ledger.pid));
    return stamp && *stamp == ledger.start_stamp;
}

struct Cleanup_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    sintra::instance_id_type expected_iid = sintra::invalid_instance_id;
    uint32_t expected_occurrence = 0;
    unsigned before_count = 0;
    unsigned lifeline_released_count = 0;
    unsigned retirement_confirmed_count = 0;
    unsigned passive_wait_count = 0;
    bool entered = false;
    bool release = false;
};

Cleanup_gate* s_cleanup_gate = nullptr;

void observe_cleanup(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_cleanup_gate;
    if (!gate || !stage || process_iid != gate->expected_iid ||
        occurrence != gate->expected_occurrence)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_cleanup_before_actions)
    {
        ++gate->before_count;
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&]() { return gate->release; });
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_cleanup_lifeline_released)
    {
        ++gate->lifeline_released_count;
        gate->changed.notify_all();
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_cleanup_retirement_confirmed)
    {
        ++gate->retirement_confirmed_count;
        gate->changed.notify_all();
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_release_waiting_passive)
    {
        ++gate->passive_wait_count;
        gate->changed.notify_all();
    }
}

struct Failure_plan
{
    std::mutex mutex;
    const char* stage = nullptr;
    sintra::instance_id_type expected_iid = sintra::invalid_instance_id;
    uint32_t expected_occurrence = 0;
    unsigned remaining = 0;
    unsigned hits = 0;
};

Failure_plan* s_failure_plan = nullptr;

bool inject_cleanup_failure(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence) noexcept
{
    auto* plan = s_failure_plan;
    if (!plan || !stage) {
        return false;
    }
    std::lock_guard<std::mutex> lock(plan->mutex);
    if (plan->remaining == 0 || process_iid != plan->expected_iid ||
        occurrence != plan->expected_occurrence ||
        std::string_view(stage) != plan->stage)
    {
        return false;
    }
    --plan->remaining;
    ++plan->hits;
    return true;
}

unsigned failure_hits(Failure_plan& plan)
{
    std::lock_guard<std::mutex> lock(plan.mutex);
    return plan.hits;
}

struct Windows_fallback_wake_result
{
    bool applicable = false;
    bool setup_ready = false;
    bool observer_registered = false;
    bool passive_wait_seen = false;
    bool wait_failed_injected = false;
    bool fallback_available = false;
    bool child_exited_naturally = false;
    bool bounded_incomplete = false;
    bool custody_retained = false;
    bool passive_completed_without_cleanup = false;
    bool cleanup_guard_complete = false;
    bool handle_closed = false;
    bool survivor_absent = false;

    bool passed() const noexcept
    {
        return !applicable ||
            (setup_ready && observer_registered && passive_wait_seen &&
             wait_failed_injected && fallback_available &&
             child_exited_naturally && passive_completed_without_cleanup &&
             cleanup_guard_complete && handle_closed && survivor_absent);
    }

    bool causal_red() const noexcept
    {
        return applicable && setup_ready && observer_registered &&
            passive_wait_seen && wait_failed_injected && fallback_available &&
            child_exited_naturally && bounded_incomplete && custody_retained &&
            !passive_completed_without_cleanup && cleanup_guard_complete &&
            handle_closed && survivor_absent;
    }
};

#ifdef _WIN32
struct Windows_fallback_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    sintra::instance_id_type expected_iid = sintra::invalid_instance_id;
    uint32_t expected_occurrence = 0;
    unsigned before_wait_count = 0;
    unsigned passive_wait_count = 0;
    unsigned fallback_available_count = 0;
    unsigned handle_closed_count = 0;
    bool release_observer_wait = false;
};

Windows_fallback_gate* s_windows_fallback_gate = nullptr;

void observe_windows_fallback(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    auto* gate = s_windows_fallback_gate;
    if (!gate || !stage || process_iid != gate->expected_iid ||
        occurrence != gate->expected_occurrence)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_native_observer_before_wait)
    {
        ++gate->before_wait_count;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&]() { return gate->release_observer_wait; });
    }
    else if (observed ==
        sintra::detail::test_hooks::k_managed_child_release_waiting_passive)
    {
        ++gate->passive_wait_count;
        gate->changed.notify_all();
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_fallback_available)
    {
        ++gate->fallback_available_count;
        gate->changed.notify_all();
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_windows_fallback_handle_closed ||
        observed == sintra::detail::test_hooks::
            k_managed_child_cleanup_native_exit_confirmed)
    {
        ++gate->handle_closed_count;
        gate->changed.notify_all();
    }
}
#endif

#ifndef _WIN32
struct Reap_observation
{
    std::atomic<pid_t> expected_pid{-1};
    std::atomic<unsigned> count{0};
    std::atomic<int> status{0};
};

Reap_observation s_reap;

void observe_reap(pid_t pid, int status) noexcept
{
    if (pid != s_reap.expected_pid.load(std::memory_order_acquire)) {
        return;
    }
    s_reap.status.store(status, std::memory_order_relaxed);
    s_reap.count.fetch_add(1, std::memory_order_release);
}
#endif

int run_child(
    int argc,
    char* argv[],
    const fs::path& shared_directory,
    std::string_view ledger_file,
    bool exit_naturally)
{
    const std::string nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (nonce.empty()) {
        return 2;
    }
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    // The original cleanup role never performs graceful shutdown and exits
    // through the lifeline; the fallback role returns after its exit marker.
    // The watchdog only bounds a broken test.
    std::thread([]() {
        std::this_thread::sleep_for(10s);
        std::_Exit(3);
    }).detach();

    Ready_target ready;
    const std::string ready_name = "managed_child_public_cleanup_ready_" + nonce;
    if (!ready.assign_name(ready_name)) {
        return 2;
    }
    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    std::ostringstream record;
    record << "nonce=" << nonce << '\n'
           << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
           << "occurrence=" << sintra::s_recovery_occurrence << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
           << "start_stamp=" << start_stamp.value_or(0) << '\n'
           << "managed_name=sintra_process_" << pid << '\n'
           << "ready_name=" << ready_name << '\n'
           << "ready_iid=" << static_cast<unsigned long long>(ready.instance_id()) << '\n'
           << "complete=1\n";
    if (!write_complete_file(marker_path(shared_directory, ledger_file), record.str())) {
        return 2;
    }
    if (exit_naturally) {
        return sintra::test::wait_for_file(
            marker_path(shared_directory, k_fallback_exit_file),
            k_watchdog_timeout,
            10ms)
            ? 0
            : 3;
    }
    for (;;) {
        std::this_thread::sleep_for(100ms);
    }
}

Windows_fallback_wake_result run_windows_fallback_wake(
    const std::string& binary_path,
    sintra::test::Shared_directory& shared)
{
    Windows_fallback_wake_result result;
#ifndef _WIN32
    (void)binary_path;
    (void)shared;
    return result;
#else
    result.applicable = true;
    const std::string nonce =
        "fallback_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(sintra::test::get_pid());
    const std::string ready_name =
        "managed_child_public_cleanup_ready_" + nonce;
    const auto ledger_path = marker_path(shared.path(), k_fallback_ledger_file);
    const auto exit_path = marker_path(shared.path(), k_fallback_exit_file);
    std::error_code ec;
    fs::remove(ledger_path, ec);
    fs::remove(exit_path, ec);

    Windows_fallback_gate gate;
    gate.expected_iid = k_fallback_child_process_iid;
    s_windows_fallback_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        &observe_windows_fallback, std::memory_order_release);
    Failure_plan failure_plan;
    failure_plan.stage =
        sintra::detail::test_hooks::k_managed_child_fail_native_observer_wait;
    failure_plan.expected_iid = k_fallback_child_process_iid;
    failure_plan.expected_occurrence = 0;
    failure_plan.remaining = 1;
    s_failure_plan = &failure_plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_cleanup_failure, std::memory_order_release);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_fallback_child_flag), std::string(k_nonce_flag), nonce};
    options.process_instance_id = k_fallback_child_process_iid;
    options.readiness_instance_name = ready_name;
    options.lifetime.enable_lifeline = false;

    sintra::Managed_child_custody custody;
    sintra::Managed_child_status ready;
    bool spawn_threw = false;
    try {
        custody = sintra::spawn_swarm_process(options);
        ready = custody.wait_for_readiness_until(
            std::chrono::steady_clock::now() + 8s);
    }
    catch (...) {
        spawn_threw = true;
    }

    const bool ledger_seen = sintra::test::wait_for_file(
        ledger_path, k_watchdog_timeout, 10ms);
    const auto ledger = ledger_seen ? read_ledger(ledger_path) : std::nullopt;
    HANDLE child_handle = ledger
        ? OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(ledger->pid))
        : nullptr;
    bool before_wait_seen = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        before_wait_seen = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.before_wait_count == 1;
        });
    }
    result.setup_ready = !spawn_threw && custody &&
        ready.readiness_state == sintra::Managed_child_readiness_state::reached &&
        ready.created_occurrences == 1 && ledger &&
        ledger->nonce == nonce &&
        ledger->process_iid == k_fallback_child_process_iid &&
        ledger->occurrence == 0 && child_handle &&
        WaitForSingleObject(child_handle, 0) == WAIT_TIMEOUT &&
        exact_child_is_live(*ledger);

    sintra::Managed_child_status passive;
    std::thread passive_caller([&]() {
        passive = custody.release_until(
            std::chrono::steady_clock::now() + k_watchdog_timeout);
    });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.passive_wait_seen = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.passive_wait_count == 1;
        });
        result.observer_registered = result.setup_ready && before_wait_seen &&
            result.passive_wait_seen;
        gate.release_observer_wait = true;
        gate.changed.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.fallback_available = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.fallback_available_count == 1;
        });
    }
    result.wait_failed_injected = failure_hits(failure_plan) == 1;

    const bool exit_requested = write_complete_file(exit_path, "exit=1\n");
    DWORD exit_code = STILL_ACTIVE;
    const bool external_exit = child_handle &&
        WaitForSingleObject(child_handle, 5000) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(child_handle, &exit_code) != 0;
    result.child_exited_naturally = exit_requested && external_exit &&
        exit_code == 0;
    result.survivor_absent = result.child_exited_naturally && ledger &&
        !exact_child_is_live(*ledger);

    const auto passive_check_begin = std::chrono::steady_clock::now();
    const auto after_exit = custody.release_until(passive_check_begin + 180ms);
    const auto passive_check_elapsed =
        std::chrono::steady_clock::now() - passive_check_begin;
    result.passive_completed_without_cleanup = custody &&
        after_exit.release_state == sintra::Managed_child_release_state::complete &&
        after_exit.exited_occurrences == 1;
    result.bounded_incomplete = custody &&
        after_exit.release_state == sintra::Managed_child_release_state::requested &&
        passive_check_elapsed >= 120ms && passive_check_elapsed < 2s;
    result.custody_retained = result.bounded_incomplete && custody;

    const auto cleanup_guard = custody.terminate_until(
        std::chrono::steady_clock::now() + 5s);
    if (passive_caller.joinable()) {
        passive_caller.join();
    }
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.handle_closed = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.handle_closed_count == 1;
        });
    }
    result.cleanup_guard_complete = custody && cleanup_guard.release_state ==
        sintra::Managed_child_release_state::complete &&
        cleanup_guard.created_occurrences == 1 &&
        cleanup_guard.exited_occurrences == 1 &&
        passive.release_state == sintra::Managed_child_release_state::complete;

    if (child_handle) {
        CloseHandle(child_handle);
    }
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_failure_plan = nullptr;
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        nullptr, std::memory_order_release);
    s_windows_fallback_gate = nullptr;
    fs::remove(ledger_path, ec);
    fs::remove(exit_path, ec);
    return result;
#endif
}

int run_root(int argc, char* argv[], sintra::test::Shared_directory& shared)
{
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    if (binary_path.empty()) {
        return 2;
    }
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    const auto windows_fallback =
        run_windows_fallback_wake(binary_path, shared);

    const std::string nonce =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "_" + std::to_string(sintra::test::get_pid());
    const std::string ready_name = "managed_child_public_cleanup_ready_" + nonce;

    std::atomic<unsigned> ready_published{0};
    std::atomic<unsigned> ready_unpublished{0};
    std::atomic<unsigned> process_unpublished{0};
    auto published = [&](const sintra::Coordinator::instance_published& message) {
        if (static_cast<std::string>(message.assigned_name) == ready_name) {
            ready_published.fetch_add(1, std::memory_order_release);
        }
    };
    auto unpublished = [&](const sintra::Coordinator::instance_unpublished& message) {
        if (static_cast<std::string>(message.assigned_name) == ready_name) {
            ready_unpublished.fetch_add(1, std::memory_order_release);
        }
        if (message.instance_id == k_child_process_iid) {
            process_unpublished.fetch_add(1, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        published,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));
    sintra::activate_slot(
        unpublished,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    Cleanup_gate gate;
    gate.expected_iid = k_child_process_iid;
    s_cleanup_gate = &gate;
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        &observe_cleanup, std::memory_order_release);
    Failure_plan failure_plan;
    failure_plan.stage =
        sintra::detail::test_hooks::k_managed_child_fail_cleanup_actions;
    failure_plan.expected_iid = k_child_process_iid;
    failure_plan.expected_occurrence = 0;
    failure_plan.remaining = 1;
    s_failure_plan = &failure_plan;
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_cleanup_failure, std::memory_order_release);
#ifndef _WIN32
    s_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_reap.count.store(0, std::memory_order_relaxed);
    s_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &observe_reap, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {std::string(k_child_flag), std::string(k_nonce_flag), nonce};
    options.process_instance_id = k_child_process_iid;
    options.readiness_instance_name = ready_name;
    options.lifetime.enable_lifeline = true;
    options.lifetime.hard_exit_timeout_ms = 100;

    sintra::Managed_child_custody custody;
    bool spawn_threw = false;
    try {
        custody = sintra::spawn_swarm_process(options);
        custody.wait_for_readiness_until(std::chrono::steady_clock::now() + 8s);
    }
    catch (...) {
        spawn_threw = true;
    }

    const bool ledger_seen = sintra::test::wait_for_file(
        marker_path(shared.path(), k_ledger_file), k_watchdog_timeout, 10ms);
    const auto ledger = ledger_seen
        ? read_ledger(marker_path(shared.path(), k_ledger_file))
        : std::nullopt;
#ifdef _WIN32
    HANDLE child_handle = ledger ? OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, static_cast<DWORD>(ledger->pid)) : nullptr;
#endif
    if (ledger) {
#ifndef _WIN32
        s_reap.expected_pid.store(static_cast<pid_t>(ledger->pid), std::memory_order_release);
#endif
    }

    const auto launch = custody.status();
    const bool identity_valid = ledger && ledger->nonce == nonce &&
        ledger->process_iid == k_child_process_iid && ledger->occurrence == 0 &&
        ledger->ready_name == ready_name &&
        sintra::process_of(ledger->ready_iid) == k_child_process_iid;
    const bool ready_valid = !spawn_threw && custody &&
        launch.readiness_state == sintra::Managed_child_readiness_state::reached &&
        launch.created_occurrences == 1 &&
        identity_valid && ledger && exact_child_is_live(*ledger) &&
#ifdef _WIN32
        child_handle && WaitForSingleObject(child_handle, 0) == WAIT_TIMEOUT &&
#endif
        wait_until([&]() {
            return ready_published.load(std::memory_order_acquire) == 1;
        }, 2s) &&
        resolve_publicly(ready_name) ==
            std::optional<sintra::instance_id_type>(ledger->ready_iid);

    // Start graceful release on a separate caller. The observation seam proves
    // its one retained worker evaluated passive release mode and entered its
    // passive wait before the public cleanup escalation occurs.
    sintra::Managed_child_status passive;
    std::thread passive_caller([&]() {
        passive = custody.release_until(
            std::chrono::steady_clock::now() + k_watchdog_timeout);
    });
    bool passive_wait_seen = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        passive_wait_seen = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.passive_wait_count == 1;
        });
    }

    // The first public escalation is injected to fail before any cleanup
    // action. Custody and the child must remain retained, and the deadline
    // facing caller must still return bounded-incomplete.
    const auto failed_begin = std::chrono::steady_clock::now();
    const auto failed_cleanup = custody.terminate_until(failed_begin + 120ms);
    const auto failed_elapsed = std::chrono::steady_clock::now() - failed_begin;
    const bool failure_seen = failure_hits(failure_plan) == 1;
    const bool retained_after_failure = ledger && exact_child_is_live(*ledger) &&
        resolve_publicly(ready_name) ==
            std::optional<sintra::instance_id_type>(ledger->ready_iid);
    bool failed_bounded_incomplete = passive_wait_seen && failure_seen &&
        custody && failed_cleanup.release_state ==
            sintra::Managed_child_release_state::requested && failed_elapsed >= 80ms &&
        failed_elapsed < 2s && retained_after_failure;
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        failed_bounded_incomplete = failed_bounded_incomplete &&
            gate.before_count == 0 && gate.lifeline_released_count == 0 &&
            gate.retirement_confirmed_count == 0;
    }

    // Re-drive the same opaque custody. The successful worker is held before
    // its first cleanup action to prove a second bounded-incomplete return.
    const auto cleanup_begin = std::chrono::steady_clock::now();
    const auto held = custody.terminate_until(cleanup_begin + 120ms);
    const auto cleanup_elapsed = std::chrono::steady_clock::now() - cleanup_begin;
    bool cleanup_entered = false;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        cleanup_entered = gate.changed.wait_for(lock, 2s, [&]() { return gate.entered; });
    }

    const bool child_retained_while_held = ledger && exact_child_is_live(*ledger) &&
        resolve_publicly(ready_name) ==
            std::optional<sintra::instance_id_type>(ledger->ready_iid);
    const bool bounded_incomplete = failed_bounded_incomplete &&
        held.release_state == sintra::Managed_child_release_state::requested &&
        cleanup_elapsed >= 80ms && cleanup_elapsed < 2s && cleanup_entered &&
        child_retained_while_held &&
        ready_unpublished.load(std::memory_order_acquire) == 0 &&
        process_unpublished.load(std::memory_order_acquire) == 0;

    const bool first_finalize_succeeded = sintra::detail::finalize();
    const auto retry = custody.release_until(
        std::chrono::steady_clock::now() + 80ms);
    const bool retained_across_finalize = !first_finalize_succeeded && custody &&
        retry.release_state == sintra::Managed_child_release_state::requested && ledger &&
        exact_child_is_live(*ledger);

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
        gate.changed.notify_all();
    }

    const auto complete = custody.terminate_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);
    if (passive_caller.joinable()) {
        passive_caller.join();
    }
    const bool authoritative_cleanup_once = [&]() {
        std::lock_guard<std::mutex> lock(gate.mutex);
        return gate.before_count == 1 && gate.lifeline_released_count == 1 &&
            gate.retirement_confirmed_count == 1 &&
            gate.passive_wait_count == 1;
    }();

    bool exact_exit = false;
    bool expected_status = false;
    bool survivor_absent = false;
#ifdef _WIN32
    if (child_handle && WaitForSingleObject(child_handle, 5000) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        exact_exit = GetExitCodeProcess(child_handle, &exit_code) != 0;
        expected_status = exact_exit && exit_code == 99;
        survivor_absent = exact_exit && ledger && !exact_child_is_live(*ledger);
    }
    if (child_handle) {
        CloseHandle(child_handle);
    }
#else
    wait_until([&]() {
        return s_reap.count.load(std::memory_order_acquire) != 0;
    }, 5s);
    const unsigned reap_count = s_reap.count.load(std::memory_order_acquire);
    const int reap_status = s_reap.status.load(std::memory_order_relaxed);
    exact_exit = reap_count == 1;
    expected_status = exact_exit && WIFEXITED(reap_status) &&
        WEXITSTATUS(reap_status) == 99;
    survivor_absent = exact_exit && ledger && !exact_child_is_live(*ledger);
#endif

    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_failure_plan = nullptr;
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        nullptr, std::memory_order_release);
    s_cleanup_gate = nullptr;
#ifndef _WIN32
    sintra::detail::test_hooks::s_child_reaped.store(nullptr, std::memory_order_release);
    s_reap.expected_pid.store(-1, std::memory_order_release);
#endif
    sintra::deactivate_all_slots();
    const bool final_retry_succeeded = sintra::detail::finalize();

    const bool valid = windows_fallback.passed() &&
        ready_valid && bounded_incomplete &&
        retained_across_finalize && complete.release_state ==
            sintra::Managed_child_release_state::complete &&
        complete.admitted_occurrences == 1 &&
        complete.created_occurrences == 1 && complete.exited_occurrences == 1 &&
        passive.release_state == sintra::Managed_child_release_state::complete &&
        failure_hits(failure_plan) == 1 &&
        authoritative_cleanup_once && exact_exit &&
        expected_status && survivor_absent && final_retry_succeeded;

    if (valid) {
        std::printf(
            "PUBLIC_CLEANUP_GREEN_VALID nonce=%s custody_ready=1 bounded_incomplete=1 "
            "passive_wait_observed=1 injected_failure_retained=1 "
            "retained_finalize=1 monotone_retry=1 lifeline_released=1 "
            "publication_retired=1 communication_retired=1 exact_exit=1 "
            "expected_status=99 survivor_absent=1 final_retry=1 "
            "windows_fallback_wake=%s\n",
            nonce.c_str(), windows_fallback.applicable ? "1" : "na");
        std::fflush(stdout);
        return 0;
    }

    if (windows_fallback.causal_red()) {
        std::fprintf(
            stderr,
            "WINDOWS_FALLBACK_WAKE_RED observer_registered=1 passive_wait=1 "
            "wait_failed=1 fallback_available=1 natural_exit=1 "
            "bounded_incomplete=1 custody_retained=1 cleanup_guard=1 "
            "handle_closed=1 survivor_absent=1 finalization=%d "
            "cause=release_wait_predicate_omits_fallback_availability\n",
            final_retry_succeeded ? 1 : 0);
    }
    else if (windows_fallback.applicable && !windows_fallback.passed()) {
        std::fprintf(
            stderr,
            "WINDOWS_FALLBACK_WAKE_INVALID setup=%d observer_registered=%d "
            "passive_wait=%d wait_failed=%d fallback_available=%d "
            "natural_exit=%d bounded_incomplete=%d custody_retained=%d "
            "passive_complete=%d cleanup_guard=%d handle_closed=%d "
            "survivor_absent=%d finalization=%d\n",
            windows_fallback.setup_ready ? 1 : 0,
            windows_fallback.observer_registered ? 1 : 0,
            windows_fallback.passive_wait_seen ? 1 : 0,
            windows_fallback.wait_failed_injected ? 1 : 0,
            windows_fallback.fallback_available ? 1 : 0,
            windows_fallback.child_exited_naturally ? 1 : 0,
            windows_fallback.bounded_incomplete ? 1 : 0,
            windows_fallback.custody_retained ? 1 : 0,
            windows_fallback.passive_completed_without_cleanup ? 1 : 0,
            windows_fallback.cleanup_guard_complete ? 1 : 0,
            windows_fallback.handle_closed ? 1 : 0,
            windows_fallback.survivor_absent ? 1 : 0,
            final_retry_succeeded ? 1 : 0);
    }

    unsigned before_count = 0;
    unsigned lifeline_count = 0;
    unsigned retirement_count = 0;
    unsigned passive_count = 0;
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        before_count = gate.before_count;
        lifeline_count = gate.lifeline_released_count;
        retirement_count = gate.retirement_confirmed_count;
        passive_count = gate.passive_wait_count;
    }
    std::fprintf(
        stderr,
        "PUBLIC_CLEANUP_INVALID ready=%d passive_wait=%d failure_hits=%u "
        "bounded=%d retained=%d complete=%d "
        "before_count=%u lifeline_count=%u retirement_count=%u passive_count=%u "
        "exact_exit=%d expected_status=%d "
        "survivor_absent=%d final_retry=%d fallback_passed=%d\n",
        ready_valid ? 1 : 0,
        passive_wait_seen ? 1 : 0,
        failure_hits(failure_plan),
        bounded_incomplete ? 1 : 0,
        retained_across_finalize ? 1 : 0,
        complete.release_state == sintra::Managed_child_release_state::complete ? 1 : 0,
        before_count,
        lifeline_count,
        retirement_count,
        passive_count,
        exact_exit ? 1 : 0,
        expected_status ? 1 : 0,
        survivor_absent ? 1 : 0,
        final_retry_succeeded ? 1 : 0,
        windows_fallback.passed() ? 1 : 0);
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_PUBLIC_CLEANUP_DIR",
        "managed_child_public_cleanup_contract");
    if (sintra::test::has_argv_flag(argc, argv, k_fallback_child_flag)) {
        return run_child(
            argc, argv, shared.path(), k_fallback_ledger_file, true);
    }
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path(), k_ledger_file, false);
    }
    return run_root(argc, argv, shared);
}
