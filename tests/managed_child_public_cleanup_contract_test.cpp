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
constexpr std::string_view k_nonce_flag = "--managed_child_public_cleanup_nonce";
constexpr std::string_view k_ledger_file = "child_ledger.complete";
constexpr auto k_watchdog_timeout = 12s;
constexpr sintra::instance_id_type k_child_process_iid =
    sintra::compose_instance(37u, 1ull);

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

int run_child(int argc, char* argv[], const fs::path& shared_directory)
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

    // This child deliberately never performs graceful shutdown. The watchdog
    // only bounds a broken test; successful cleanup exits through the lifeline.
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
    if (!write_complete_file(marker_path(shared_directory, k_ledger_file), record.str())) {
        return 2;
    }
    for (;;) {
        std::this_thread::sleep_for(100ms);
    }
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
    options.wait_for_instance_name = ready_name;
    options.wait_timeout = 8s;
    options.lifetime.enable_lifeline = true;
    options.lifetime.hard_exit_timeout_ms = 100;

    sintra::Managed_child_custody custody;
    bool spawn_threw = false;
    try {
        custody = sintra::spawn_swarm_process(options);
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
    const bool ready_valid = !spawn_threw && launch.accepted &&
        launch.readiness_reached && launch.created_occurrences == 1 &&
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
        failed_cleanup.accepted && failed_cleanup.release_requested &&
        !failed_cleanup.release_complete && failed_elapsed >= 80ms &&
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
        held.accepted && held.release_requested && !held.release_complete &&
        cleanup_elapsed >= 80ms && cleanup_elapsed < 2s && cleanup_entered &&
        child_retained_while_held &&
        ready_unpublished.load(std::memory_order_acquire) == 0 &&
        process_unpublished.load(std::memory_order_acquire) == 0;

    const bool first_finalize_succeeded = sintra::detail::finalize();
    const auto retry = custody.release_until(
        std::chrono::steady_clock::now() + 80ms);
    const bool retained_across_finalize = !first_finalize_succeeded && retry.accepted &&
        retry.release_requested && !retry.release_complete && ledger &&
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

    const bool valid = ready_valid && bounded_incomplete &&
        retained_across_finalize && complete.accepted && complete.release_requested &&
        complete.release_complete && complete.admitted_occurrences == 1 &&
        complete.created_occurrences == 1 && complete.exited_occurrences == 1 &&
        passive.accepted && passive.release_requested && passive.release_complete &&
        failure_hits(failure_plan) == 1 &&
        authoritative_cleanup_once && exact_exit &&
        expected_status && survivor_absent && final_retry_succeeded;

    if (valid) {
        std::printf(
            "PUBLIC_CLEANUP_GREEN_VALID nonce=%s custody_ready=1 bounded_incomplete=1 "
            "passive_wait_observed=1 injected_failure_retained=1 "
            "retained_finalize=1 monotone_retry=1 lifeline_released=1 "
            "publication_retired=1 communication_retired=1 exact_exit=1 "
            "expected_status=99 survivor_absent=1 final_retry=1\n",
            nonce.c_str());
        std::fflush(stdout);
        return 0;
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
        "survivor_absent=%d final_retry=%d\n",
        ready_valid ? 1 : 0,
        passive_wait_seen ? 1 : 0,
        failure_hits(failure_plan),
        bounded_incomplete ? 1 : 0,
        retained_across_finalize ? 1 : 0,
        complete.release_complete ? 1 : 0,
        before_count,
        lifeline_count,
        retirement_count,
        passive_count,
        exact_exit ? 1 : 0,
        expected_status ? 1 : 0,
        survivor_absent ? 1 : 0,
        final_retry_succeeded ? 1 : 0);
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_PUBLIC_CLEANUP_DIR",
        "managed_child_public_cleanup_contract");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared);
}
