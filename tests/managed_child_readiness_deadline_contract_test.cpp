//
// Managed-child hard readiness deadline baseline contract evidence (R1).
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
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
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag = "--managed_child_readiness_deadline_child";
constexpr std::string_view k_nonce_flag = "--managed_child_readiness_deadline_nonce";
constexpr std::string_view k_child_ledger_file = "child_ledger.complete";
constexpr std::string_view k_child_release_file = "child_release.complete";
constexpr std::string_view k_child_finalized_file = "child_finalized.complete";
constexpr auto k_requested_wait_timeout = std::chrono::milliseconds(1500);
// Deliberately exceeds the removed 150 ms caller-scheduling tolerance. This is
// a pre-call test stimulus, not a permitted deadline overrun.
constexpr auto k_expired_deadline_probe_delay = std::chrono::milliseconds(300);
constexpr auto k_watchdog_timeout = std::chrono::seconds(10);
constexpr sintra::instance_id_type k_child_process_iid = sintra::compose_instance(30u, 1ull);

struct Child_ledger
{
    std::string nonce;
    int       pid = -1;
    bool      start_stamp_available = false;
    uint64_t  start_stamp = 0;
};

struct Deadline_gate
{
    std::mutex                                  mutex;
    std::condition_variable                     cv;
    std::string                                 requested_target;
    bool                                        readiness_started = false;
    bool                                        resolution_entered = false;
    bool                                        released = false;
    std::chrono::steady_clock::time_point       resolution_entered_at;
};

struct Readiness_service : sintra::Derived_transceiver<Readiness_service>
{};

struct Spawn_call
{
    std::mutex               mutex;
    std::condition_variable  cv;
    bool                     spawn_returned = false;
    bool                     deadline_call_allowed = false;
    bool                     wait_called = false;
    bool                     done = false;
    sintra::Managed_child_custody custody;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point spawn_completed_at{};
    std::chrono::steady_clock::time_point wait_called_at{};
    std::chrono::steady_clock::time_point wait_returned_at{};
    sintra::Managed_child_status deadline_observation;
    bool                     wait_returned = false;
    bool                     threw = false;
};

Deadline_gate* s_deadline_gate = nullptr;

fs::path child_ledger_path(const fs::path& dir)
{
    return dir / std::string(k_child_ledger_file);
}

fs::path child_release_path(const fs::path& dir)
{
    return dir / std::string(k_child_release_file);
}

fs::path child_finalized_path(const fs::path& dir)
{
    return dir / std::string(k_child_finalized_file);
}

std::optional<Child_ledger> read_child_ledger(const fs::path& path)
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
            if (key == "nonce") {
                ledger.nonce = value;
            }
            else
            if (key == "pid") {
                ledger.pid = std::stoi(value);
            }
            else
            if (key == "start_stamp_available") {
                ledger.start_stamp_available = value == "1";
            }
            else
            if (key == "start_stamp") {
                ledger.start_stamp = std::stoull(value);
            }
            else
            if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }

    if (!complete || ledger.nonce.empty() || ledger.pid <= 0) {
        return std::nullopt;
    }
    return ledger;
}

void runtime_stage_callback(const char* stage)
{
    if (!stage ||
        std::string_view(stage) !=
            sintra::detail::test_hooks::k_stage_spawn_success_before_readiness_wait)
    {
        return;
    }

    Deadline_gate* gate = s_deadline_gate;
    if (!gate) {
        return;
    }

    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->readiness_started = true;
    gate->cv.notify_all();
}

void resolve_instance_callback(const std::string& assigned_name)
{
    Deadline_gate* gate = s_deadline_gate;
    if (!gate) {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    if (!gate->readiness_started || assigned_name != gate->requested_target) {
        return;
    }

    gate->resolution_entered = true;
    gate->resolution_entered_at = std::chrono::steady_clock::now();
    gate->cv.notify_all();
    gate->cv.wait(lock, [&]() {
        return gate->released;
    });
}

bool wait_for_resolution_entry(Deadline_gate& gate, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.cv.wait_for(lock, timeout, [&]() {
        return gate.readiness_started && gate.resolution_entered;
    });
}

void release_resolution(Deadline_gate& gate)
{
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
    gate.cv.notify_all();
}

bool wait_for_spawn_call(Spawn_call& call, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(call.mutex);
    return call.cv.wait_for(lock, timeout, [&]() {
        return call.done;
    });
}

bool exact_child_is_live(const Child_ledger& ledger)
{
    if (ledger.pid <= 0 ||
        !ledger.start_stamp_available ||
        !sintra::is_process_alive(static_cast<uint32_t>(ledger.pid)))
    {
        return false;
    }

    const auto observed = sintra::query_process_start_stamp(
        static_cast<uint32_t>(ledger.pid));
    return observed && *observed == ledger.start_stamp;
}

#ifndef _WIN32
struct Posix_reap_observation
{
    std::atomic<pid_t>     expected_pid{-1};
    std::atomic<uint32_t>  count{0};
    std::atomic<int>       status{0};
};

Posix_reap_observation s_posix_reap;

void posix_child_reaped(pid_t pid, int status) noexcept
{
    if (s_posix_reap.expected_pid.load(std::memory_order_acquire) != pid) {
        return;
    }

    s_posix_reap.status.store(status, std::memory_order_relaxed);
    s_posix_reap.count.fetch_add(1, std::memory_order_release);
}

bool wait_for_posix_reap(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (s_posix_reap.count.load(std::memory_order_acquire) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return s_posix_reap.count.load(std::memory_order_acquire) != 0;
}
#endif

bool terminate_exact_child(const Child_ledger& ledger)
{
    if (!exact_child_is_live(ledger)) {
        return false;
    }

#ifdef _WIN32
    HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(ledger.pid));
    if (!process) {
        return false;
    }
    const bool terminated = TerminateProcess(process, 2) != 0;
    if (terminated) {
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    return terminated;
#else
    return ::kill(static_cast<pid_t>(ledger.pid), SIGTERM) == 0;
#endif
}

int run_child(int argc, char* argv[], const fs::path& shared_dir)
{
    const std::string nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (nonce.empty()) {
        std::fprintf(stderr, "managed_child_readiness_deadline: child nonce missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_readiness_deadline: child init failed: %s\n", error.what());
        return 2;
    }

    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    std::ostringstream marker;
    marker << "nonce=" << nonce << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
           << "start_stamp=" << start_stamp.value_or(0) << '\n'
           << "complete=1\n";
    if (!write_complete_file(child_ledger_path(shared_dir), marker.str())) {
        sintra::detail::finalize();
        return 2;
    }

    Readiness_service readiness_service;
    if (!readiness_service.assign_name(
            "managed_child_deadline_target_" + nonce))
    {
        sintra::detail::finalize();
        return 2;
    }

    const bool released = sintra::test::wait_for_file(
        child_release_path(shared_dir),
        std::chrono::seconds(30),
        std::chrono::milliseconds(10));
    if (!released) {
        sintra::detail::finalize();
        return 2;
    }

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (...) {
        return 2;
    }

    if (!finalized ||
        !write_complete_file(
            child_finalized_path(shared_dir),
            "finalized=1\ncomplete=1\n"))
    {
        return 2;
    }
    return 0;
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
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_readiness_deadline: root init failed: %s\n", error.what());
        return 2;
    }

    const auto nonce_value = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string nonce = std::to_string(nonce_value) + "_" +
        std::to_string(sintra::test::get_pid());
    Deadline_gate gate;
    gate.requested_target = "managed_child_deadline_target_" + nonce;
    Spawn_call call;
    s_deadline_gate = &gate;
    sintra::detail::test_hooks::s_runtime_stage.store(
        &runtime_stage_callback,
        std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_resolve_instance.store(
        &resolve_instance_callback,
        std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &posix_child_reaped,
        std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_child_process_iid;
    options.readiness_instance_name = gate.requested_target;
    options.lifetime.enable_lifeline = false;

    std::thread spawn_thread([&]() {
        {
            std::lock_guard<std::mutex> lock(call.mutex);
            call.started_at = std::chrono::steady_clock::now();
        }
        try {
            call.custody = sintra::spawn_swarm_process(options);
            {
                std::lock_guard<std::mutex> lock(call.mutex);
                call.spawn_returned = true;
                call.spawn_completed_at = std::chrono::steady_clock::now();
            }
            call.cv.notify_all();

            {
                std::unique_lock<std::mutex> lock(call.mutex);
                call.cv.wait(lock, [&]() {
                    return call.deadline_call_allowed;
                });
                call.wait_called = true;
                call.wait_called_at = std::chrono::steady_clock::now();
            }
            const auto deadline_observation = call.custody.wait_for_readiness_until(
                call.started_at + k_requested_wait_timeout);
            const auto wait_returned_at = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(call.mutex);
                call.deadline_observation = deadline_observation;
                call.wait_returned_at = wait_returned_at;
                call.wait_returned = true;
            }
        }
        catch (...) {
            call.threw = true;
        }
        {
            std::lock_guard<std::mutex> lock(call.mutex);
            call.done = true;
        }
        call.cv.notify_all();
    });

    const bool resolution_entered = wait_for_resolution_entry(gate, k_watchdog_timeout);
    std::chrono::steady_clock::time_point resolution_entered_at;
    if (resolution_entered) {
        std::lock_guard<std::mutex> lock(gate.mutex);
        resolution_entered_at = gate.resolution_entered_at;
    }
    std::chrono::steady_clock::time_point call_started_at;
    std::chrono::steady_clock::time_point spawn_completed_at;
    bool spawn_returned_before_wait_deadline = false;
    {
        std::unique_lock<std::mutex> lock(call.mutex);
        call.cv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return call.spawn_returned;
        });
        call_started_at = call.started_at;
        spawn_completed_at = call.spawn_completed_at;
        spawn_returned_before_wait_deadline = call.spawn_returned &&
            spawn_completed_at < call.started_at + k_requested_wait_timeout;
    }

    const bool ledger_file_seen = sintra::test::wait_for_file(
        child_ledger_path(shared.path()),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_file_seen
        ? read_child_ledger(child_ledger_path(shared.path()))
        : std::nullopt;
    const bool ledger_identity_valid = ledger && ledger->nonce == nonce;
    const auto observed_start_stamp = ledger
        ? sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid))
        : std::nullopt;
    const bool start_stamp_verified =
        ledger_identity_valid &&
        ledger->start_stamp_available &&
        observed_start_stamp &&
        *observed_start_stamp == ledger->start_stamp;
    const bool child_alive_during_hold =
        start_stamp_verified && exact_child_is_live(*ledger);

#ifdef _WIN32
    HANDLE child_process = ledger
        ? OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            static_cast<DWORD>(ledger->pid))
        : nullptr;
    const bool native_identity_verified =
        child_alive_during_hold &&
        child_process &&
        WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT;
#else
    if (ledger_identity_valid) {
        s_posix_reap.expected_pid.store(
            static_cast<pid_t>(ledger->pid),
            std::memory_order_release);
    }
    const bool native_identity_verified = child_alive_during_hold;
#endif

    const auto requested_deadline = call_started_at + k_requested_wait_timeout;
    const auto permit_deadline_call_at =
        requested_deadline + k_expired_deadline_probe_delay;
    if (std::chrono::steady_clock::now() < permit_deadline_call_at) {
        std::this_thread::sleep_until(permit_deadline_call_at);
    }
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        call.deadline_call_allowed = true;
    }
    call.cv.notify_all();

    // The public call receives an already-expired deadline while the readiness
    // resolver remains blocked. Completion therefore proves that deadline
    // expiry performs no nested readiness work; the watchdog is cleanup only.
    const bool returned_while_resolution_blocked =
        wait_for_spawn_call(call, k_watchdog_timeout);

    bool caller_done_at_observation = false;
    bool custody_retained_at_observation = false;
    bool wait_called = false;
    bool wait_returned = false;
    sintra::Managed_child_status deadline_observation;
    std::chrono::steady_clock::time_point wait_called_at;
    std::chrono::steady_clock::time_point wait_returned_at;
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        caller_done_at_observation = call.done;
        wait_called = call.wait_called;
        wait_called_at = call.wait_called_at;
        wait_returned = call.wait_returned;
        wait_returned_at = call.wait_returned_at;
        custody_retained_at_observation = static_cast<bool>(call.custody);
        deadline_observation = call.deadline_observation;
    }
    const bool resolution_entered_before_deadline =
        resolution_entered && resolution_entered_at < requested_deadline;
    const bool child_alive_at_observation =
        ledger && start_stamp_verified && exact_child_is_live(*ledger);
    const bool deadline_expired_before_call =
        wait_called && wait_called_at > requested_deadline;
    const auto call_started_after_deadline_ms = wait_called
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
            wait_called_at - requested_deadline).count()
        : -1;
    const auto call_duration_us = wait_called && wait_returned
        ? std::chrono::duration_cast<std::chrono::microseconds>(
            wait_returned_at - wait_called_at).count()
        : -1;

    release_resolution(gate);
    const bool spawn_call_completed = returned_while_resolution_blocked ||
        wait_for_spawn_call(call, k_watchdog_timeout);
    if (!spawn_call_completed) {
        write_complete_file(child_release_path(shared.path()), "release=1\ncomplete=1\n");
    }
    spawn_thread.join();

    {
        std::lock_guard<std::mutex> lock(call.mutex);
        deadline_observation = call.deadline_observation;
    }

    const auto progressed_observation = call.custody.wait_for_readiness_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);

    sintra::detail::test_hooks::s_coordinator_resolve_instance.store(
        nullptr,
        std::memory_order_release);
    sintra::detail::test_hooks::s_runtime_stage.store(
        nullptr,
        std::memory_order_release);
    s_deadline_gate = nullptr;

    bool call_threw = false;
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        call_threw = call.threw;
    }

    const bool child_release_written = write_complete_file(
        child_release_path(shared.path()),
        "release=1\ncomplete=1\n");
    const bool child_finalized = sintra::test::wait_for_file(
        child_finalized_path(shared.path()),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto terminated_observation = call.custody.terminate_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);

    bool native_exit_confirmed = false;
    bool native_normal_exit = false;
    bool survivor_absent = false;
    bool forced_cleanup = false;

#ifdef _WIN32
    if (child_process && WaitForSingleObject(child_process, 10000) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
        native_normal_exit = native_exit_confirmed && exit_code == 0;
    }
#else
    wait_for_posix_reap(k_watchdog_timeout);
#endif

    if (ledger && !native_exit_confirmed && exact_child_is_live(*ledger)) {
        forced_cleanup = terminate_exact_child(*ledger);
#ifdef _WIN32
        if (child_process && WaitForSingleObject(child_process, 2000) == WAIT_OBJECT_0) {
            DWORD exit_code = STILL_ACTIVE;
            native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
        }
#else
        wait_for_posix_reap(std::chrono::seconds(2));
#endif
    }

    bool root_finalized = false;
    try {
        root_finalized = sintra::detail::finalize();
    }
    catch (...) {
    }

#ifndef _WIN32
    if (root_finalized) {
        wait_for_posix_reap(std::chrono::seconds(1));
    }
    const uint32_t reap_count = s_posix_reap.count.load(std::memory_order_acquire);
    const int reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    native_exit_confirmed = reap_count == 1;
    native_normal_exit =
        native_exit_confirmed &&
        WIFEXITED(reap_status) &&
        WEXITSTATUS(reap_status) == 0;
    sintra::detail::test_hooks::s_child_reaped.store(nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
    const bool child_reap_hook_cleared = true;
#else
    const uint32_t reap_count = 0;
    const int reap_status = 0;
    const bool child_reap_hook_cleared = true;
#endif

    survivor_absent =
        native_exit_confirmed &&
        ledger &&
        !exact_child_is_live(*ledger);

#ifdef _WIN32
    if (child_process) {
        CloseHandle(child_process);
    }
#endif

    const bool baseline_valid =
        resolution_entered &&
        spawn_returned_before_wait_deadline &&
        resolution_entered_before_deadline &&
        ledger_identity_valid &&
        start_stamp_verified &&
        native_identity_verified &&
        child_alive_during_hold &&
        deadline_expired_before_call &&
        returned_while_resolution_blocked &&
        caller_done_at_observation &&
        child_alive_at_observation &&
        spawn_call_completed &&
        custody_retained_at_observation &&
        deadline_observation.created_occurrences == 1 &&
        deadline_observation.readiness_state ==
            sintra::Managed_child_readiness_state::pending &&
        deadline_observation.release_state ==
            sintra::Managed_child_release_state::open &&
        progressed_observation.readiness_state ==
            sintra::Managed_child_readiness_state::reached &&
        progressed_observation.release_state ==
            sintra::Managed_child_release_state::open &&
        terminated_observation.release_state ==
            sintra::Managed_child_release_state::complete &&
        !call_threw &&
        child_release_written &&
        child_finalized &&
        native_exit_confirmed &&
        native_normal_exit &&
        survivor_absent &&
        !forced_cleanup &&
        child_reap_hook_cleared &&
        root_finalized;

    if (baseline_valid) {
        std::printf(
            "R1_GREEN_VALID wait_timeout_ms=%lld call_started_after_deadline_ms=%lld call_duration_us=%lld "
            "spawn_returned_before_wait=1 resolve_entered_before_deadline=1 "
            "deadline_expired_before_call=1 returned_while_resolution_blocked=1 "
            "native_identity_verified=1 child_alive_during_hold=1 custody_accepted=1 "
            "readiness_pending=1 timeout_release_requested=0 readiness_progressed=1 "
            "explicit_terminate=1 release_complete=1 "
            "child_finalized=1 native_exit_confirmed=1 normal_status=1 "
            "survivor_absent=1 reap_count=%s\n",
            static_cast<long long>(k_requested_wait_timeout.count()),
            static_cast<long long>(call_started_after_deadline_ms),
            static_cast<long long>(call_duration_us),
#ifdef _WIN32
            "not_applicable"
#else
            "1"
#endif
            );
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(
        stderr,
        "R1_INVALID resolution_entered=%d spawn_returned_before_wait=%d entered_before_deadline=%d "
        "deadline_expired_before_call=%d returned_while_resolution_blocked=%d "
        "caller_done_at_observation=%d call_started_after_deadline_ms=%lld call_duration_us=%lld "
        "spawn_completed=%d call_threw=%d "
        "ledger=%d ledger_identity=%d start_stamp_verified=%d "
        "native_identity_verified=%d child_alive_during_hold=%d child_alive_at_observation=%d "
        "timeout_accepted=%d timeout_readiness=%d timeout_release_requested=%d timeout_release_complete=%d "
        "custody_retained=%d progress_readiness=%d progress_release_requested=%d "
        "terminate_release_requested=%d terminate_release_complete=%d child_release=%d "
        "child_finalized=%d native_exit_confirmed=%d normal_status=%d "
        "survivor_absent=%d reap_count=%u reap_status=%d forced_cleanup=%d "
        "child_reap_hook_cleared=%d root_finalized=%d\n",
        resolution_entered ? 1 : 0,
        spawn_returned_before_wait_deadline ? 1 : 0,
        resolution_entered_before_deadline ? 1 : 0,
        deadline_expired_before_call ? 1 : 0,
        returned_while_resolution_blocked ? 1 : 0,
        caller_done_at_observation ? 1 : 0,
        static_cast<long long>(call_started_after_deadline_ms),
        static_cast<long long>(call_duration_us),
        spawn_call_completed ? 1 : 0,
        call_threw ? 1 : 0,
        ledger ? 1 : 0,
        ledger_identity_valid ? 1 : 0,
        start_stamp_verified ? 1 : 0,
        native_identity_verified ? 1 : 0,
        child_alive_during_hold ? 1 : 0,
        child_alive_at_observation ? 1 : 0,
        static_cast<bool>(call.custody) ? 1 : 0,
        deadline_observation.readiness_state ==
            sintra::Managed_child_readiness_state::reached ? 1 : 0,
        deadline_observation.release_state !=
            sintra::Managed_child_release_state::open ? 1 : 0,
        deadline_observation.release_state ==
            sintra::Managed_child_release_state::complete ? 1 : 0,
        custody_retained_at_observation ? 1 : 0,
        progressed_observation.readiness_state ==
            sintra::Managed_child_readiness_state::reached ? 1 : 0,
        progressed_observation.release_state !=
            sintra::Managed_child_release_state::open ? 1 : 0,
        terminated_observation.release_state !=
            sintra::Managed_child_release_state::open ? 1 : 0,
        terminated_observation.release_state ==
            sintra::Managed_child_release_state::complete ? 1 : 0,
        child_release_written ? 1 : 0,
        child_finalized ? 1 : 0,
        native_exit_confirmed ? 1 : 0,
        native_normal_exit ? 1 : 0,
        survivor_absent ? 1 : 0,
        static_cast<unsigned>(reap_count),
        reap_status,
        forced_cleanup ? 1 : 0,
        child_reap_hook_cleared ? 1 : 0,
        root_finalized ? 1 : 0);
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_READINESS_DEADLINE_DIR",
        "managed_child_readiness_deadline_contract");

    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared);
}
