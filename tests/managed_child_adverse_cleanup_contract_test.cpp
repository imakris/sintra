//
// Managed-child adverse-cleanup baseline contract evidence (R7).
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
#include <unistd.h>
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
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag = "--managed_child_adverse_cleanup_child";
constexpr std::string_view k_native_child_flag =
    "--managed_child_native_escalation_child";
constexpr std::string_view k_native_retry_child_flag =
    "--managed_child_native_retry_child";
constexpr std::string_view k_nonce_flag = "--managed_child_adverse_cleanup_nonce";
constexpr std::string_view k_child_ledger_file = "child_ledger.complete";
constexpr std::string_view k_child_release_file = "child_release.complete";
constexpr std::string_view k_child_finalized_file = "child_finalized.complete";
constexpr std::string_view k_native_child_ledger_file =
    "native_child_ledger.complete";
constexpr std::string_view k_native_retry_child_ledger_file =
    "native_retry_child_ledger.complete";
constexpr auto k_requested_wait_timeout = std::chrono::milliseconds(350);
constexpr auto k_scheduling_tolerance = std::chrono::milliseconds(200);
constexpr auto k_native_retry_deadline = std::chrono::seconds(9);
constexpr auto k_native_retry_return_scheduling_margin =
    std::chrono::milliseconds(500);
constexpr auto k_native_retry_hard_hook_deadline = std::chrono::seconds(7);
constexpr auto k_watchdog_timeout = std::chrono::seconds(12);
constexpr sintra::instance_id_type k_child_process_iid =
    sintra::compose_instance(27u, 1ull);
constexpr sintra::instance_id_type k_native_child_process_iid =
    sintra::compose_instance(28u, 1ull);
constexpr sintra::instance_id_type k_native_retry_process_iid =
    sintra::compose_instance(29u, 1ull);

struct Child_ledger
{
    std::string               nonce;
    sintra::instance_id_type  process_iid = sintra::invalid_instance_id;
    uint32_t                  occurrence = std::numeric_limits<uint32_t>::max();
    int                       pid = -1;
    bool                      start_stamp_available = false;
    uint64_t                  start_stamp = 0;
    std::string               managed_name;
};

struct Cleanup_gate
{
    std::mutex                              mutex;
    std::condition_variable                 cv;
    bool                                    readiness_started = false;
    bool                                    cleanup_entered = false;
    std::chrono::steady_clock::time_point   cleanup_entered_at{};
    bool                                    released = false;
};

struct Spawn_call
{
    std::mutex               mutex;
    std::condition_variable  cv;
    bool                     done = false;
    sintra::Managed_child_custody custody;
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point completed_at{};
    bool                     threw = false;
};

struct Spawn_observation
{
    std::atomic<sintra::instance_id_type> process_iid{sintra::invalid_instance_id};
    std::atomic<int>                      pid{-1};
    std::atomic<bool>                     lifeline_enabled{true};
    std::atomic<bool>                     lifeline_write_retained{true};
    std::atomic<uint32_t>                 count{0};
};

Cleanup_gate* s_cleanup_gate = nullptr;
Spawn_observation s_spawn_observation;

struct Native_cleanup_observation
{
    std::atomic<sintra::instance_id_type> expected_iid{sintra::invalid_instance_id};
    std::atomic<uint32_t> soft{0};
    std::atomic<uint32_t> hard{0};
    std::atomic<uint32_t> exited{0};
};

Native_cleanup_observation s_native_cleanup;
std::atomic<bool> s_fail_native_hard{false};
std::atomic<uint32_t> s_native_hard_failure_hits{0};
std::atomic<int64_t> s_native_hard_failure_at_ns{0};

struct Native_retry_result
{
    bool    valid = false;
    bool    hard_hook_bounded = false;
    int64_t first_elapsed_ms = -1;
    int64_t hard_hook_elapsed_ms = -1;
};

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

fs::path native_child_ledger_path(const fs::path& dir)
{
    return dir / std::string(k_native_child_ledger_file);
}

fs::path native_retry_child_ledger_path(const fs::path& dir)
{
    return dir / std::string(k_native_retry_child_ledger_file);
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
            else if (key == "piid") {
                ledger.process_iid =
                    static_cast<sintra::instance_id_type>(std::stoull(value));
            }
            else if (key == "occurrence") {
                ledger.occurrence = static_cast<uint32_t>(std::stoul(value));
            }
            else if (key == "pid") {
                ledger.pid = std::stoi(value);
            }
            else if (key == "start_stamp_available") {
                ledger.start_stamp_available = value == "1";
            }
            else if (key == "start_stamp") {
                ledger.start_stamp = std::stoull(value);
            }
            else if (key == "managed_name") {
                ledger.managed_name = value;
            }
            else if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }

    if (!complete || ledger.nonce.empty() || ledger.pid <= 0 ||
        ledger.managed_name.empty())
    {
        return std::nullopt;
    }
    return ledger;
}

void runtime_spawn_success_callback(
    sintra::instance_id_type process_iid,
    int                      os_pid,
    bool                     lifeline_enabled,
    bool                     lifeline_write_retained)
{
    if (process_iid != k_child_process_iid) {
        return;
    }
    s_spawn_observation.process_iid.store(process_iid, std::memory_order_relaxed);
    s_spawn_observation.pid.store(os_pid, std::memory_order_relaxed);
    s_spawn_observation.lifeline_enabled.store(
        lifeline_enabled, std::memory_order_relaxed);
    s_spawn_observation.lifeline_write_retained.store(
        lifeline_write_retained, std::memory_order_relaxed);
    s_spawn_observation.count.fetch_add(1, std::memory_order_release);

    Cleanup_gate* gate = s_cleanup_gate;
    if (!gate) {
        return;
    }

    std::lock_guard<std::mutex> lock(gate->mutex);
    gate->readiness_started = true;
    gate->cv.notify_all();
}

void cleanup_stage_callback(const char* stage)
{
    if (!stage ||
        std::string_view(stage) !=
            sintra::detail::test_hooks::k_stage_unpublish_pre_barrier_collection)
    {
        return;
    }

    Cleanup_gate* gate = s_cleanup_gate;
    if (!gate) {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->cleanup_entered = true;
    gate->cleanup_entered_at = std::chrono::steady_clock::now();
    gate->cv.notify_all();
    gate->cv.wait(lock, [&]() { return gate->released; });
}

void native_cleanup_stage_callback(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    if (!stage ||
        process_iid != s_native_cleanup.expected_iid.load(
            std::memory_order_acquire) ||
        occurrence != 0)
    {
        return;
    }
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_cleanup_soft_termination)
    {
        s_native_cleanup.soft.fetch_add(1, std::memory_order_release);
    }
    else
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_cleanup_hard_termination)
    {
        s_native_cleanup.hard.fetch_add(1, std::memory_order_release);
    }
    else
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_cleanup_native_exit_confirmed)
    {
        s_native_cleanup.exited.fetch_add(1, std::memory_order_release);
    }
}

bool fail_native_hard_termination(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence) noexcept
{
    if (!stage || process_iid != k_native_retry_process_iid || occurrence != 0 ||
        std::string_view(stage) !=
            sintra::detail::test_hooks::k_managed_child_fail_native_hard_termination)
    {
        return false;
    }
    bool expected = true;
    if (!s_fail_native_hard.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
    {
        return false;
    }
    s_native_hard_failure_at_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_release);
    s_native_hard_failure_hits.fetch_add(1, std::memory_order_release);
    return true;
}

bool wait_for_cleanup_entry(Cleanup_gate& gate, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(gate.mutex);
    return gate.cv.wait_for(lock, timeout, [&]() {
        return gate.readiness_started && gate.cleanup_entered;
    });
}

void release_cleanup(Cleanup_gate& gate)
{
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
    gate.cv.notify_all();
}

bool wait_for_spawn_call(Spawn_call& call, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(call.mutex);
    return call.cv.wait_for(lock, timeout, [&]() { return call.done; });
}

bool exact_name_map_absent(const std::string& name)
{
    if (!sintra::s_mproc) {
        return false;
    }
    auto names = sintra::s_mproc->m_instance_id_of_assigned_name.scoped();
    return names.get().find(name) == names.get().end();
}

bool exact_reader_present(sintra::instance_id_type process_iid)
{
    return sintra::s_mproc && sintra::s_mproc->has_process_reader(process_iid);
}

bool exact_lifeline_absent(sintra::instance_id_type process_iid)
{
    if (!sintra::s_mproc) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sintra::s_mproc->m_lifeline_mutex);
    return sintra::s_mproc->m_lifeline_writes.find(process_iid) ==
        sintra::s_mproc->m_lifeline_writes.end();
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
    if (!exact_process_is_live(
            ledger.pid, ledger.start_stamp_available, ledger.start_stamp))
    {
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
    const bool signaled = TerminateProcess(process, 2) != 0;
    if (signaled) {
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    return signaled;
#else
    return ::kill(static_cast<pid_t>(ledger.pid), SIGTERM) == 0;
#endif
}

#ifdef _WIN32
BOOL WINAPI ignore_native_soft_termination(DWORD)
{
    return TRUE;
}
#endif

int run_native_escalation_child(
    int argc,
    char* argv[],
    sintra::instance_id_type process_iid,
    const fs::path& ledger_path)
{
    const std::string nonce =
        sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (nonce.empty()) {
        return 2;
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(&ignore_native_soft_termination, TRUE);
#else
    ::signal(SIGTERM, SIG_IGN);
#endif
    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    std::ostringstream marker;
    marker << "nonce=" << nonce << '\n'
        << "piid=" << static_cast<unsigned long long>(
            process_iid) << '\n'
        << "occurrence=0\n"
        << "pid=" << pid << '\n'
        << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
        << "start_stamp=" << start_stamp.value_or(0) << '\n'
        << "managed_name=native_adverse_" << pid << '\n'
        << "complete=1\n";
    if (!write_complete_file(ledger_path, marker.str())) {
        return 2;
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}

int run_child(int argc, char* argv[], const fs::path& shared_dir)
{
    const std::string nonce =
        sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (nonce.empty()) {
        std::fprintf(stderr, "managed_child_adverse_cleanup: child nonce missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "managed_child_adverse_cleanup: child init failed: %s\n",
            error.what());
        return 2;
    }

    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    const std::string managed_name = "sintra_process_" + std::to_string(pid);
    std::ostringstream marker;
    marker << "nonce=" << nonce << '\n'
           << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
           << "occurrence=" << sintra::s_recovery_occurrence << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
           << "start_stamp=" << start_stamp.value_or(0) << '\n'
           << "managed_name=" << managed_name << '\n'
           << "complete=1\n";

    if (!write_complete_file(child_ledger_path(shared_dir), marker.str())) {
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

bool run_native_escalation_phase(
    int argc,
    char* argv[],
    const fs::path& shared_dir,
    const std::string& binary_path,
    const std::string& nonce)
{
    std::error_code ignored;
    fs::remove(native_child_ledger_path(shared_dir), ignored);
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return false;
    }

    s_native_cleanup.expected_iid.store(
        k_native_child_process_iid, std::memory_order_relaxed);
    s_native_cleanup.soft.store(0, std::memory_order_relaxed);
    s_native_cleanup.hard.store(0, std::memory_order_relaxed);
    s_native_cleanup.exited.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        &native_cleanup_stage_callback, std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &posix_child_reaped, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_native_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_native_child_process_iid;
    options.readiness_instance_name = "managed_child_native_never_ready_" + nonce;
    options.lifetime.enable_lifeline = false;

    const auto started = std::chrono::steady_clock::now();
    auto custody = sintra::spawn_swarm_process(options);
    const auto first = custody.wait_for_readiness_until(started + k_requested_wait_timeout);
    const auto returned = std::chrono::steady_clock::now();
    const bool ledger_seen = sintra::test::wait_for_file(
        native_child_ledger_path(shared_dir),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_seen
        ? read_child_ledger(native_child_ledger_path(shared_dir))
        : std::nullopt;
    const bool ledger_valid = ledger && ledger->nonce == nonce &&
        ledger->process_iid == k_native_child_process_iid &&
        ledger->occurrence == 0 && ledger->start_stamp_available;
#ifndef _WIN32
    if (ledger_valid) {
        s_posix_reap.expected_pid.store(
            static_cast<pid_t>(ledger->pid), std::memory_order_release);
    }
#endif
    const bool caller_bounded =
        returned - started <= k_requested_wait_timeout + k_scheduling_tolerance;
    const bool live_after_caller = ledger_valid && exact_process_is_live(
        ledger->pid, ledger->start_stamp_available, ledger->start_stamp);

#ifdef _WIN32
    HANDLE process = ledger_valid
        ? OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            static_cast<DWORD>(ledger->pid))
        : nullptr;
#endif

    const auto completed = custody.terminate_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);
    const auto retried = custody.terminate_until(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    const bool survivor_absent = ledger_valid && !exact_process_is_live(
        ledger->pid, ledger->start_stamp_available, ledger->start_stamp);
    const uint32_t soft_count =
        s_native_cleanup.soft.load(std::memory_order_acquire);
    const uint32_t hard_count =
        s_native_cleanup.hard.load(std::memory_order_acquire);
    const uint32_t exit_count =
        s_native_cleanup.exited.load(std::memory_order_acquire);

    bool exact_status = false;
    uint32_t reap_count = 0;
    int reap_status = 0;
#ifdef _WIN32
    if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        exact_status = GetExitCodeProcess(process, &exit_code) != 0 &&
            exit_code == 137;
    }
    if (process) {
        CloseHandle(process);
    }
#else
    wait_for_posix_reap(std::chrono::seconds(1));
    reap_count = s_posix_reap.count.load(std::memory_order_acquire);
    reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    exact_status = reap_count == 1 && WIFSIGNALED(reap_status) &&
        WTERMSIG(reap_status) == SIGKILL;
#endif

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (...) {
    }
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        nullptr, std::memory_order_release);
    s_native_cleanup.expected_iid.store(
        sintra::invalid_instance_id, std::memory_order_release);
#ifndef _WIN32
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
#endif
    fs::remove(native_child_ledger_path(shared_dir), ignored);

    const bool valid = caller_bounded && ledger_valid && live_after_caller &&
        custody && first.created_occurrences == 1 &&
        first.readiness_state == sintra::Managed_child_readiness_state::pending &&
        first.release_state == sintra::Managed_child_release_state::open &&
        completed.release_state == sintra::Managed_child_release_state::complete &&
        completed.exited_occurrences == 1 &&
        retried.release_state == sintra::Managed_child_release_state::complete &&
        soft_count == 1 && hard_count == 1 && exit_count == 1 &&
        exact_status && survivor_absent && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "NATIVE_ESCALATION_INVALID bounded=%d ledger=%d live_after=%d "
            "first_incomplete=%d complete=%d retry=%d soft=%u hard=%u "
            "exit=%u status=%d survivor_absent=%d reap_count=%u "
            "reap_status=%d finalized=%d\n",
            caller_bounded ? 1 : 0,
            ledger_valid ? 1 : 0,
            live_after_caller ? 1 : 0,
            (custody && first.release_state !=
                sintra::Managed_child_release_state::complete) ? 1 : 0,
            completed.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            retried.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            static_cast<unsigned>(soft_count),
            static_cast<unsigned>(hard_count),
            static_cast<unsigned>(exit_count),
            exact_status ? 1 : 0,
            survivor_absent ? 1 : 0,
            static_cast<unsigned>(reap_count),
            reap_status,
            finalized ? 1 : 0);
    }
    return valid;
}

Native_retry_result run_native_retry_phase(
    int argc,
    char* argv[],
    const fs::path& shared_dir,
    const std::string& binary_path,
    const std::string& nonce)
{
    std::error_code ignored;
    fs::remove(native_retry_child_ledger_path(shared_dir), ignored);
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return {};
    }

    s_native_cleanup.expected_iid.store(
        k_native_retry_process_iid, std::memory_order_relaxed);
    s_native_cleanup.soft.store(0, std::memory_order_relaxed);
    s_native_cleanup.hard.store(0, std::memory_order_relaxed);
    s_native_cleanup.exited.store(0, std::memory_order_relaxed);
    s_native_hard_failure_hits.store(0, std::memory_order_relaxed);
    s_native_hard_failure_at_ns.store(0, std::memory_order_relaxed);
    s_fail_native_hard.store(false, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        &native_cleanup_stage_callback, std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &posix_child_reaped, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_native_retry_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_native_retry_process_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    const auto before_cleanup = custody.status();
    const bool ledger_seen = sintra::test::wait_for_file(
        native_retry_child_ledger_path(shared_dir),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_seen
        ? read_child_ledger(native_retry_child_ledger_path(shared_dir))
        : std::nullopt;
    const bool ledger_valid = ledger && ledger->nonce == nonce &&
        ledger->process_iid == k_native_retry_process_iid &&
        ledger->occurrence == 0 && ledger->start_stamp_available;
#ifndef _WIN32
    if (ledger_valid) {
        s_posix_reap.expected_pid.store(
            static_cast<pid_t>(ledger->pid), std::memory_order_release);
    }
#endif

#ifdef _WIN32
    HANDLE process = ledger_valid
        ? OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            static_cast<DWORD>(ledger->pid))
        : nullptr;
#endif

    s_fail_native_hard.store(true, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &fail_native_hard_termination, std::memory_order_release);
    const auto first_started = std::chrono::steady_clock::now();
    const auto first = custody.terminate_until(
        first_started + k_native_retry_deadline);
    const auto first_returned = std::chrono::steady_clock::now();
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    s_fail_native_hard.store(false, std::memory_order_release);
    const auto first_elapsed = first_returned - first_started;
    const auto first_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            first_elapsed).count();
    const bool first_bounded = first_elapsed <=
        k_native_retry_deadline + k_native_retry_return_scheduling_margin;
    const auto hard_failure_at_ns =
        s_native_hard_failure_at_ns.load(std::memory_order_acquire);
    const auto first_started_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            first_started.time_since_epoch()).count();
    const auto hard_hook_elapsed_ns = hard_failure_at_ns > first_started_ns
        ? hard_failure_at_ns - first_started_ns
        : int64_t{-1};
    const auto hard_hook_elapsed_ms = hard_hook_elapsed_ns >= 0
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(hard_hook_elapsed_ns)).count()
        : int64_t{-1};
    const bool hard_hook_bounded = hard_hook_elapsed_ns >= 0 &&
        std::chrono::nanoseconds(hard_hook_elapsed_ns) <=
            k_native_retry_hard_hook_deadline;
    const bool retained_after_first = ledger_valid && exact_process_is_live(
        ledger->pid, ledger->start_stamp_available, ledger->start_stamp);
    const uint32_t failure_hits =
        s_native_hard_failure_hits.load(std::memory_order_acquire);

    const auto second = custody.terminate_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);
    bool survivor_absent = ledger_valid && !exact_process_is_live(
        ledger->pid, ledger->start_stamp_available, ledger->start_stamp);
    bool forced_cleanup = false;
    if (ledger_valid && !survivor_absent) {
        forced_cleanup = terminate_exact_child(*ledger);
        custody.release_until(
            std::chrono::steady_clock::now() + k_watchdog_timeout);
        survivor_absent = !exact_process_is_live(
            ledger->pid, ledger->start_stamp_available, ledger->start_stamp);
    }

    const uint32_t soft_count =
        s_native_cleanup.soft.load(std::memory_order_acquire);
    const uint32_t hard_count =
        s_native_cleanup.hard.load(std::memory_order_acquire);
    const uint32_t exit_count =
        s_native_cleanup.exited.load(std::memory_order_acquire);
    bool exact_status = false;
    uint32_t reap_count = 0;
    int reap_status = 0;
#ifdef _WIN32
    if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        exact_status = GetExitCodeProcess(process, &exit_code) != 0 &&
            exit_code == 137;
    }
    if (process) {
        CloseHandle(process);
    }
#else
    wait_for_posix_reap(std::chrono::seconds(1));
    reap_count = s_posix_reap.count.load(std::memory_order_acquire);
    reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    exact_status = reap_count == 1 && WIFSIGNALED(reap_status) &&
        WTERMSIG(reap_status) == SIGKILL;
#endif

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (...) {
    }
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_cleanup.store(
        nullptr, std::memory_order_release);
    s_native_cleanup.expected_iid.store(
        sintra::invalid_instance_id, std::memory_order_release);
#ifndef _WIN32
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
#endif
    fs::remove(native_retry_child_ledger_path(shared_dir), ignored);

    const bool valid = custody &&
        before_cleanup.created_occurrences == 1 &&
        before_cleanup.release_state == sintra::Managed_child_release_state::open &&
        first_bounded && first.created_occurrences == 1 &&
        first.exited_occurrences == 0 &&
        first.release_state == sintra::Managed_child_release_state::requested &&
        failure_hits == 1 && hard_hook_bounded &&
        retained_after_first &&
        second.release_state == sintra::Managed_child_release_state::complete &&
        second.exited_occurrences == 1 && soft_count == 2 &&
        hard_count == 1 && exit_count == 1 && exact_status &&
        survivor_absent && !forced_cleanup && finalized;
    if (!valid) {
        std::fprintf(
            stderr,
            "NATIVE_RETRY_INVALID before=%d bounded=%d first_incomplete=%d "
            "failure_hits=%u hard_hook_bounded=%d retained=%d "
            "second_complete=%d soft=%u hard=%u "
            "exit=%u status=%d survivor_absent=%d forced=%d reap_count=%u "
            "reap_status=%d finalized=%d first_elapsed_ms=%lld "
            "hard_hook_elapsed_ms=%lld return_margin_ms=%lld "
            "hard_hook_deadline_ms=%lld\n",
            (custody && before_cleanup.created_occurrences == 1) ? 1 : 0,
            first_bounded ? 1 : 0,
            (custody && first.release_state !=
                sintra::Managed_child_release_state::complete &&
                first.exited_occurrences == 0) ? 1 : 0,
            static_cast<unsigned>(failure_hits),
            hard_hook_bounded ? 1 : 0,
            retained_after_first ? 1 : 0,
            second.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            static_cast<unsigned>(soft_count),
            static_cast<unsigned>(hard_count),
            static_cast<unsigned>(exit_count),
            exact_status ? 1 : 0,
            survivor_absent ? 1 : 0,
            forced_cleanup ? 1 : 0,
            static_cast<unsigned>(reap_count),
            reap_status,
            finalized ? 1 : 0,
            static_cast<long long>(first_elapsed_ms),
            static_cast<long long>(hard_hook_elapsed_ms),
            static_cast<long long>(
                k_native_retry_return_scheduling_margin.count()),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    k_native_retry_hard_hook_deadline).count()));
    }
    return {
        valid,
        hard_hook_bounded,
        static_cast<int64_t>(first_elapsed_ms),
        static_cast<int64_t>(hard_hook_elapsed_ms)};
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
        std::fprintf(
            stderr,
            "managed_child_adverse_cleanup: root init failed: %s\n",
            error.what());
        return 2;
    }

    const auto nonce_value =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string nonce = std::to_string(nonce_value) + "_" +
        std::to_string(sintra::test::get_pid());
    const std::string requested_target =
        "managed_child_adverse_cleanup_target_" + nonce;

    std::atomic<bool> requested_target_seen{false};
    auto publication_handler =
        [&requested_target, &requested_target_seen](
            const sintra::Coordinator::instance_published& message)
    {
        if (static_cast<std::string>(message.assigned_name) == requested_target) {
            requested_target_seen.store(true, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        publication_handler,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    Cleanup_gate gate;
    Spawn_call call;
    s_cleanup_gate = &gate;
    s_spawn_observation.process_iid.store(
        sintra::invalid_instance_id, std::memory_order_relaxed);
    s_spawn_observation.pid.store(-1, std::memory_order_relaxed);
    s_spawn_observation.lifeline_enabled.store(true, std::memory_order_relaxed);
    s_spawn_observation.lifeline_write_retained.store(
        true, std::memory_order_relaxed);
    s_spawn_observation.count.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_runtime_spawn_success.store(
        &runtime_spawn_success_callback, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &cleanup_stage_callback, std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &posix_child_reaped, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_child_process_iid;
    options.readiness_instance_name = requested_target;
    options.lifetime.enable_lifeline = false;

    std::thread spawn_thread([&]() {
        {
            std::lock_guard<std::mutex> lock(call.mutex);
            call.started_at = std::chrono::steady_clock::now();
        }
        try {
            call.custody = sintra::spawn_swarm_process(options);
            call.custody.wait_for_readiness_until(
                call.started_at + k_requested_wait_timeout);
            call.custody.terminate_until(
                call.started_at + k_requested_wait_timeout);
        }
        catch (...) {
            call.threw = true;
        }
        {
            std::lock_guard<std::mutex> lock(call.mutex);
            call.done = true;
            call.completed_at = std::chrono::steady_clock::now();
        }
        call.cv.notify_all();
    });

    const bool ledger_file_seen = sintra::test::wait_for_file(
        child_ledger_path(shared.path()),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_file_seen
        ? read_child_ledger(child_ledger_path(shared.path()))
        : std::nullopt;
    const bool ledger_identity_valid =
        ledger &&
        ledger->nonce == nonce &&
        ledger->process_iid == k_child_process_iid &&
        ledger->occurrence == 0 &&
        ledger->managed_name ==
            "sintra_process_" + std::to_string(ledger->pid);

#ifndef _WIN32
    if (ledger_identity_valid) {
        s_posix_reap.expected_pid.store(
            static_cast<pid_t>(ledger->pid), std::memory_order_release);
    }
#endif

    const bool cleanup_entered =
        wait_for_cleanup_entry(gate, k_watchdog_timeout);
    std::chrono::steady_clock::time_point cleanup_entered_at;
    std::chrono::steady_clock::time_point call_started_at;
    if (cleanup_entered) {
        std::lock_guard<std::mutex> lock(gate.mutex);
        cleanup_entered_at = gate.cleanup_entered_at;
    }
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        call_started_at = call.started_at;
    }

    const auto observation_deadline =
        call_started_at + k_requested_wait_timeout + k_scheduling_tolerance;
    if (cleanup_entered && std::chrono::steady_clock::now() < observation_deadline) {
        std::this_thread::sleep_until(observation_deadline);
    }
    const auto observed_at = std::chrono::steady_clock::now();

    bool caller_done_at_observation = false;
    std::chrono::steady_clock::time_point caller_completed_at;
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        caller_done_at_observation = call.done;
        caller_completed_at = call.completed_at;
    }

    const bool spawn_observation_valid =
        ledger_identity_valid &&
        s_spawn_observation.count.load(std::memory_order_acquire) == 1 &&
        s_spawn_observation.process_iid.load(std::memory_order_relaxed) ==
            k_child_process_iid &&
        s_spawn_observation.pid.load(std::memory_order_relaxed) == ledger->pid &&
        !s_spawn_observation.lifeline_enabled.load(std::memory_order_relaxed) &&
        !s_spawn_observation.lifeline_write_retained.load(std::memory_order_relaxed);
    const auto observed_start_stamp = ledger
        ? sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid))
        : std::nullopt;
    const bool start_stamp_verified =
        ledger_identity_valid &&
        ledger->start_stamp_available &&
        observed_start_stamp &&
        *observed_start_stamp == ledger->start_stamp;
    const bool child_alive_at_observation =
        start_stamp_verified && exact_process_is_live(
            ledger->pid, ledger->start_stamp_available, ledger->start_stamp);
    const bool name_absent_at_seam =
        ledger_identity_valid && exact_name_map_absent(ledger->managed_name);
    const bool reader_nonterminal_at_seam =
        exact_reader_present(k_child_process_iid);
    const bool lifeline_absent_at_seam =
        exact_lifeline_absent(k_child_process_iid);
    const bool requested_target_never_published =
        !requested_target_seen.load(std::memory_order_acquire) &&
        exact_name_map_absent(requested_target);
    const bool seam_after_requested_deadline =
        cleanup_entered &&
        cleanup_entered_at >= call_started_at + k_requested_wait_timeout;
    const bool returned_by_deadline =
        cleanup_entered &&
        caller_done_at_observation &&
        caller_completed_at <= observation_deadline;
    const auto overrun_ms = cleanup_entered
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
            observed_at - call_started_at - k_requested_wait_timeout).count()
        : -1;

#ifdef _WIN32
    HANDLE child_process = ledger_identity_valid
        ? OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            static_cast<DWORD>(ledger->pid))
        : nullptr;
    const bool native_identity_verified =
        child_alive_at_observation &&
        child_process &&
        WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT;
#else
    const bool native_identity_verified = child_alive_at_observation;
#endif

    release_cleanup(gate);
    const bool spawn_call_completed = wait_for_spawn_call(call, k_watchdog_timeout);
    if (!spawn_call_completed) {
        write_complete_file(
            child_release_path(shared.path()), "release=1\ncomplete=1\n");
    }
    spawn_thread.join();

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_runtime_spawn_success.store(
        nullptr, std::memory_order_release);
    s_cleanup_gate = nullptr;

    sintra::Managed_child_status launch_observation;
    bool call_threw = false;
    {
        std::lock_guard<std::mutex> lock(call.mutex);
        launch_observation = call.custody.status();
        call_threw = call.threw;
    }

    const bool child_release_written = write_complete_file(
        child_release_path(shared.path()), "release=1\ncomplete=1\n");
    const bool child_finalized = sintra::test::wait_for_file(
        child_finalized_path(shared.path()),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto released_observation = call.custody.release_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);

    bool native_exit_confirmed = false;
    bool native_normal_exit = false;
    bool forced_cleanup = false;
#ifdef _WIN32
    if (child_process &&
        WaitForSingleObject(child_process, 10000) == WAIT_OBJECT_0)
    {
        DWORD exit_code = STILL_ACTIVE;
        native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
        native_normal_exit = native_exit_confirmed && exit_code == 0;
    }
#else
    wait_for_posix_reap(k_watchdog_timeout);
#endif

    if (ledger && !native_exit_confirmed && exact_process_is_live(
            ledger->pid, ledger->start_stamp_available, ledger->start_stamp))
    {
        forced_cleanup = terminate_exact_child(*ledger);
#ifndef _WIN32
        wait_for_posix_reap(std::chrono::seconds(2));
#else
        if (child_process &&
            WaitForSingleObject(child_process, 2000) == WAIT_OBJECT_0)
        {
            DWORD exit_code = STILL_ACTIVE;
            native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
        }
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
    const uint32_t reap_count =
        s_posix_reap.count.load(std::memory_order_acquire);
    const int reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    native_exit_confirmed = reap_count == 1;
    native_normal_exit =
        native_exit_confirmed &&
        WIFEXITED(reap_status) &&
        WEXITSTATUS(reap_status) == 0;
    sintra::detail::test_hooks::s_child_reaped.store(
        nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
#else
    const uint32_t reap_count = 0;
    const int reap_status = 0;
#endif

    const bool survivor_absent =
        native_exit_confirmed && ledger && !exact_process_is_live(
            ledger->pid, ledger->start_stamp_available, ledger->start_stamp);

#ifdef _WIN32
    if (child_process) {
        CloseHandle(child_process);
    }
#endif

    const bool baseline_valid =
        cleanup_entered &&
        seam_after_requested_deadline &&
        returned_by_deadline &&
        ledger_identity_valid &&
        spawn_observation_valid &&
        start_stamp_verified &&
        native_identity_verified &&
        child_alive_at_observation &&
        name_absent_at_seam &&
        reader_nonterminal_at_seam &&
        lifeline_absent_at_seam &&
        requested_target_never_published &&
        spawn_call_completed &&
        call.custody &&
        launch_observation.created_occurrences == 1 &&
        launch_observation.readiness_state ==
            sintra::Managed_child_readiness_state::observation_stopped &&
        launch_observation.release_state ==
            sintra::Managed_child_release_state::requested &&
        !call_threw &&
        child_release_written &&
        child_finalized &&
        released_observation.release_state ==
            sintra::Managed_child_release_state::complete &&
        native_exit_confirmed &&
        native_normal_exit &&
        survivor_absent &&
        !forced_cleanup &&
        root_finalized;

    const bool native_escalation_valid = root_finalized &&
        run_native_escalation_phase(
            argc,
            argv,
            shared.path(),
            binary_path,
            nonce + "_native");
    Native_retry_result native_retry;
    if (native_escalation_valid) {
        native_retry = run_native_retry_phase(
            argc,
            argv,
            shared.path(),
            binary_path,
            nonce + "_retry");
    }
    const bool native_retry_valid = native_retry.valid;

    if (baseline_valid && native_escalation_valid && native_retry_valid) {
        std::printf(
            "R7_GREEN_VALID nonce=%s piid=%llu occurrence=%u pid=%d "
            "wait_timeout_ms=%lld tolerance_ms=%lld overrun_ms=%lld "
            "seam=unpublish_pre_barrier caller_done_at_observation=1 "
            "name_absent=1 reader_retirement_nonterminal=1 "
            "lifeline_enabled=0 lifeline_entry=absent native_alive_at_seam=1 "
            "custody_accepted=1 release_incomplete_at_seam=1 release_complete=1 "
            "child_finalized=1 native_exit_confirmed=1 "
            "normal_status=1 survivor_absent=1 forced_cleanup=0 reap_count=%s "
            "native_adverse_caller_bounded=1 soft_ignored=1 hard_escalation=1 "
            "native_exact_status=1 native_retry=1 native_survivor_absent=1 "
            "native_failed_pass_bounded=1 native_retry_latch_reopened=1 "
            "native_distinct_retry=1 native_hard_hook_bounded=1 "
            "native_first_elapsed_ms=%lld native_hard_hook_elapsed_ms=%lld\n",
            nonce.c_str(),
            static_cast<unsigned long long>(ledger->process_iid),
            ledger->occurrence,
            ledger->pid,
            static_cast<long long>(k_requested_wait_timeout.count()),
            static_cast<long long>(k_scheduling_tolerance.count()),
            static_cast<long long>(overrun_ms),
#ifdef _WIN32
            "not_applicable",
#else
            "1",
#endif
            static_cast<long long>(native_retry.first_elapsed_ms),
            static_cast<long long>(native_retry.hard_hook_elapsed_ms));
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(
        stderr,
        "R7_INVALID cleanup_entered=%d seam_after_deadline=%d returned_by_deadline=%d "
        "caller_done_at_observation=%d ledger=%d "
        "spawn_observation=%d start_stamp=%d native_identity=%d child_alive=%d "
        "name_absent=%d reader_nonterminal=%d lifeline_absent=%d target_never_published=%d "
        "spawn_completed=%d custody_valid=%d call_threw=%d child_release=%d "
        "child_finalized=%d native_exit=%d normal_status=%d survivor_absent=%d "
        "reap_count=%u reap_status=%d forced_cleanup=%d root_finalized=%d "
        "native_escalation=%d native_retry=%d native_hard_hook_bounded=%d "
        "native_first_elapsed_ms=%lld native_hard_hook_elapsed_ms=%lld\n",
        cleanup_entered ? 1 : 0,
        seam_after_requested_deadline ? 1 : 0,
        returned_by_deadline ? 1 : 0,
        caller_done_at_observation ? 1 : 0,
        ledger_identity_valid ? 1 : 0,
        spawn_observation_valid ? 1 : 0,
        start_stamp_verified ? 1 : 0,
        native_identity_verified ? 1 : 0,
        child_alive_at_observation ? 1 : 0,
        name_absent_at_seam ? 1 : 0,
        reader_nonterminal_at_seam ? 1 : 0,
        lifeline_absent_at_seam ? 1 : 0,
        requested_target_never_published ? 1 : 0,
        spawn_call_completed ? 1 : 0,
        (call.custody && launch_observation.release_state !=
            sintra::Managed_child_release_state::open &&
         released_observation.release_state ==
            sintra::Managed_child_release_state::complete) ? 1 : 0,
        call_threw ? 1 : 0,
        child_release_written ? 1 : 0,
        child_finalized ? 1 : 0,
        native_exit_confirmed ? 1 : 0,
        native_normal_exit ? 1 : 0,
        survivor_absent ? 1 : 0,
        static_cast<unsigned>(reap_count),
        reap_status,
        forced_cleanup ? 1 : 0,
        root_finalized ? 1 : 0,
        native_escalation_valid ? 1 : 0,
        native_retry_valid ? 1 : 0,
        native_retry.hard_hook_bounded ? 1 : 0,
        static_cast<long long>(native_retry.first_elapsed_ms),
        static_cast<long long>(native_retry.hard_hook_elapsed_ms));
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_ADVERSE_CLEANUP_DIR",
        "managed_child_adverse_cleanup_contract");

    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    if (sintra::test::has_argv_flag(argc, argv, k_native_child_flag)) {
        return run_native_escalation_child(
            argc,
            argv,
            k_native_child_process_iid,
            native_child_ledger_path(shared.path()));
    }
    if (sintra::test::has_argv_flag(argc, argv, k_native_retry_child_flag)) {
        return run_native_escalation_child(
            argc,
            argv,
            k_native_retry_process_iid,
            native_retry_child_ledger_path(shared.path()));
    }
    return run_root(argc, argv, shared);
}
