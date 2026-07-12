//
// Managed-child custody finalization race baseline (Batch2-R8F).
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
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag = "--managed_child_custody_finalize_race_child";
constexpr std::string_view k_nonce_flag = "--managed_child_custody_finalize_race_nonce";
constexpr std::string_view k_lifeline_disable_flag = "--lifeline_disable";
constexpr std::string_view k_ledger_file = "child_ledger.complete";
constexpr std::string_view k_release_child_file = "release_child.complete";
constexpr std::string_view k_child_finalized_file = "child_finalized.complete";
constexpr auto k_watchdog_timeout = std::chrono::seconds(12);
constexpr sintra::instance_id_type k_child_process_iid = sintra::compose_instance(32u, 1ull);

struct Child_ledger
{
    std::string              nonce;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    uint32_t                 occurrence = std::numeric_limits<uint32_t>::max();
    int                      pid = -1;
    bool                     start_stamp_available = false;
    uint64_t                 start_stamp = 0;
    std::string              managed_name;
    bool                     self_publication_confirmed = false;
    bool                     begin_draining_confirmed = false;
    bool                     lifeline_disable_flag = false;
};

struct Spawn_hold_observation
{
    std::atomic<sintra::instance_id_type> expected_iid{sintra::invalid_instance_id};
    std::atomic<uint32_t> count{0};
    std::atomic<int> pid{-1};
    std::atomic<bool> lifeline_enabled{true};
    std::atomic<bool> lifeline_write_retained{true};
    std::atomic<bool> release{false};
};

Spawn_hold_observation s_spawn_hold;
std::atomic<sintra::instance_id_type> s_destroy_expected_iid{sintra::invalid_instance_id};
std::atomic<uint32_t> s_destroy_publication_count{0};

#ifndef _WIN32
struct Posix_reap_observation
{
    std::atomic<pid_t> expected_pid{-1};
    std::atomic<uint32_t> count{0};
    std::atomic<int> status{0};
};

Posix_reap_observation s_posix_reap;
#endif

fs::path marker_path(const fs::path& dir, std::string_view name)
{
    return dir / std::string(name);
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
            if (key == "nonce") {
                ledger.nonce = value;
            }
            else if (key == "piid") {
                ledger.process_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
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
            else if (key == "self_publication_confirmed") {
                ledger.self_publication_confirmed = value == "1";
            }
            else if (key == "begin_draining_confirmed") {
                ledger.begin_draining_confirmed = value == "1";
            }
            else if (key == "lifeline_disable_flag") {
                ledger.lifeline_disable_flag = value == "1";
            }
            else if (key == "complete") {
                complete = value == "1";
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }

    if (!complete || ledger.nonce.empty() || ledger.pid <= 0 || ledger.managed_name.empty()) {
        return std::nullopt;
    }
    return ledger;
}

void hold_spawn_success(
    sintra::instance_id_type process_iid,
    int                      os_pid,
    bool                     lifeline_enabled,
    bool                     lifeline_write_retained)
{
    if (process_iid != s_spawn_hold.expected_iid.load(std::memory_order_acquire)) {
        return;
    }

    s_spawn_hold.pid.store(os_pid, std::memory_order_relaxed);
    s_spawn_hold.lifeline_enabled.store(lifeline_enabled, std::memory_order_relaxed);
    s_spawn_hold.lifeline_write_retained.store(
        lifeline_write_retained, std::memory_order_relaxed);
    s_spawn_hold.count.fetch_add(1, std::memory_order_release);

    while (!s_spawn_hold.release.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void observe_destroying_publication(sintra::instance_id_type process_iid) noexcept
{
    if (process_iid == s_destroy_expected_iid.load(std::memory_order_acquire)) {
        s_destroy_publication_count.fetch_add(1, std::memory_order_release);
    }
}

#ifndef _WIN32
void observe_posix_reap(pid_t pid, int status) noexcept
{
    if (pid != s_posix_reap.expected_pid.load(std::memory_order_acquire)) {
        return;
    }
    s_posix_reap.status.store(status, std::memory_order_relaxed);
    s_posix_reap.count.fetch_add(1, std::memory_order_release);
}

bool exact_posix_child_is_live(int pid, uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<uint32_t>(pid))) {
        return false;
    }
    const auto observed = sintra::query_process_start_stamp(static_cast<uint32_t>(pid));
    return observed && *observed == start_stamp;
}

bool signal_exact_posix_child(int pid, uint64_t start_stamp, int signal_number)
{
    return exact_posix_child_is_live(pid, start_stamp) &&
        ::kill(static_cast<pid_t>(pid), signal_number) == 0;
}
#endif

int run_child(int argc, char* argv[], const fs::path& shared_dir)
{
    const std::string nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    const bool lifeline_disable_flag =
        sintra::test::has_argv_flag(argc, argv, k_lifeline_disable_flag);
    if (nonce.empty() || !lifeline_disable_flag) {
        std::fprintf(stderr, "managed_child_custody_finalize_race: child input invalid\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_finalize_race: child init failed: %s\n", error.what());
        return 2;
    }

    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    const std::string managed_name = "sintra_process_" + std::to_string(pid);
    const auto resolved = resolve_publicly(managed_name);
    const bool self_publication_confirmed =
        resolved && *resolved == sintra::s_mproc_id;

    bool begin_draining_confirmed = false;
    try {
        auto handle = sintra::Coordinator::rpc_async_begin_process_draining(
            sintra::s_coord_id,
            sintra::s_mproc_id);
        (void)handle.get();
        begin_draining_confirmed = true;
    }
    catch (...) {
        begin_draining_confirmed = false;
    }

    std::ostringstream record;
    record << "nonce=" << nonce << '\n'
           << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
           << "occurrence=" << sintra::s_recovery_occurrence << '\n'
           << "pid=" << pid << '\n'
           << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
           << "start_stamp=" << start_stamp.value_or(0) << '\n'
           << "managed_name=" << managed_name << '\n'
           << "self_publication_confirmed=" << (self_publication_confirmed ? 1 : 0) << '\n'
           << "begin_draining_confirmed=" << (begin_draining_confirmed ? 1 : 0) << '\n'
           << "lifeline_disable_flag=" << (lifeline_disable_flag ? 1 : 0) << '\n'
           << "complete=1\n";

    if (!self_publication_confirmed || !begin_draining_confirmed || !start_stamp ||
        !write_complete_file(marker_path(shared_dir, k_ledger_file), record.str()) ||
        !sintra::test::wait_for_file(
            marker_path(shared_dir, k_release_child_file),
            std::chrono::seconds(30),
            std::chrono::milliseconds(10)))
    {
        return 2;
    }

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (...) {
        finalized = false;
    }

    if (!finalized ||
        !write_complete_file(
            marker_path(shared_dir, k_child_finalized_file),
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
        std::fprintf(stderr, "managed_child_custody_finalize_race: root init failed: %s\n", error.what());
        return 2;
    }

    const std::string nonce =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
        std::to_string(sintra::test::get_pid());
    const std::string managed_name_prefix = "sintra_process_";
    const std::string never_published_name = "managed_child_r8_never_" + nonce;

    std::atomic<uint32_t> exact_process_published{0};
    std::atomic<uint32_t> exact_process_unpublished{0};
    auto published = [&](const sintra::Coordinator::instance_published& event) {
        if (event.instance_id == k_child_process_iid) {
            exact_process_published.fetch_add(1, std::memory_order_release);
        }
    };
    auto unpublished = [&](const sintra::Coordinator::instance_unpublished& event) {
        if (event.instance_id == k_child_process_iid) {
            exact_process_unpublished.fetch_add(1, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        published,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));
    sintra::activate_slot(
        unpublished,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    s_spawn_hold.expected_iid.store(k_child_process_iid, std::memory_order_relaxed);
    s_spawn_hold.count.store(0, std::memory_order_relaxed);
    s_spawn_hold.pid.store(-1, std::memory_order_relaxed);
    s_spawn_hold.lifeline_enabled.store(true, std::memory_order_relaxed);
    s_spawn_hold.lifeline_write_retained.store(true, std::memory_order_relaxed);
    s_spawn_hold.release.store(false, std::memory_order_relaxed);
    s_destroy_expected_iid.store(k_child_process_iid, std::memory_order_relaxed);
    s_destroy_publication_count.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_runtime_spawn_success.store(
        &hold_spawn_success, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_destroying_publication.store(
        &observe_destroying_publication, std::memory_order_release);

#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(
        &observe_posix_reap, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_child_process_iid;
    options.wait_for_instance_name = never_published_name;
    options.wait_timeout = std::chrono::seconds(8);
    options.lifetime.enable_lifeline = false;

    std::atomic<bool> spawn_finished{false};
    sintra::Managed_child_custody custody;
    bool spawn_threw = false;
    std::thread spawn_thread([&]() {
        try {
            custody = sintra::spawn_swarm_process(options);
        }
        catch (...) {
            spawn_threw = true;
        }
        spawn_finished.store(true, std::memory_order_release);
    });

    const bool spawn_hold_entered = wait_until([&]() {
        return s_spawn_hold.count.load(std::memory_order_acquire) == 1;
    }, k_watchdog_timeout);
    const bool ledger_seen = sintra::test::wait_for_file(
        marker_path(shared.path(), k_ledger_file),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto ledger = ledger_seen
        ? read_ledger(marker_path(shared.path(), k_ledger_file))
        : std::nullopt;

    bool ledger_identity_valid = false;
    bool start_stamp_verified = false;
    bool native_alive_before = false;
    bool publication_confirmed = false;
#ifdef _WIN32
    HANDLE child_process = nullptr;
#endif
    if (ledger) {
        ledger_identity_valid =
            ledger->nonce == nonce &&
            ledger->process_iid == k_child_process_iid &&
            ledger->occurrence == 0 &&
            ledger->managed_name == managed_name_prefix + std::to_string(ledger->pid) &&
            ledger->self_publication_confirmed &&
            ledger->begin_draining_confirmed &&
            ledger->lifeline_disable_flag &&
            s_spawn_hold.pid.load(std::memory_order_acquire) == ledger->pid;

        const auto observed_stamp = sintra::query_process_start_stamp(
            static_cast<uint32_t>(ledger->pid));
        start_stamp_verified =
            ledger->start_stamp_available && observed_stamp &&
            *observed_stamp == ledger->start_stamp;
#ifdef _WIN32
        child_process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            static_cast<DWORD>(ledger->pid));
        native_alive_before = child_process && start_stamp_verified &&
            WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT;
#else
        s_posix_reap.expected_pid.store(static_cast<pid_t>(ledger->pid), std::memory_order_release);
        native_alive_before = start_stamp_verified &&
            exact_posix_child_is_live(ledger->pid, ledger->start_stamp);
#endif
        publication_confirmed = wait_until([&]() {
            return exact_process_published.load(std::memory_order_acquire) == 1;
        }, std::chrono::seconds(2));
        const auto resolved = resolve_publicly(ledger->managed_name);
        publication_confirmed = publication_confirmed && resolved &&
            *resolved == k_child_process_iid;
    }

    const bool no_lifeline_entry =
        !s_spawn_hold.lifeline_enabled.load(std::memory_order_acquire) &&
        !s_spawn_hold.lifeline_write_retained.load(std::memory_order_acquire);
    const bool accepted_like_hold =
        spawn_hold_entered && !spawn_finished.load(std::memory_order_acquire) &&
        ledger_identity_valid && start_stamp_verified && native_alive_before &&
        publication_confirmed && no_lifeline_entry &&
        exact_process_unpublished.load(std::memory_order_acquire) == 0;

    bool root_finalized = false;
    try {
        root_finalized = sintra::detail::finalize();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_finalize_race: root finalize failed: %s\n", error.what());
    }
    catch (...) {
        std::fprintf(stderr, "managed_child_custody_finalize_race: root finalize failed\n");
    }

    const bool runtime_state_retained =
        sintra::s_mproc != nullptr && sintra::s_coord != nullptr &&
        s_destroy_publication_count.load(std::memory_order_acquire) == 0;
    const bool caller_still_held_after_finalize =
        !spawn_finished.load(std::memory_order_acquire);
    const bool finalize_incomplete = !root_finalized && runtime_state_retained;

    bool platform_hold_valid = false;
#ifndef _WIN32
    const bool native_live_after_finalize = ledger &&
        exact_posix_child_is_live(ledger->pid, ledger->start_stamp) &&
        s_posix_reap.count.load(std::memory_order_acquire) == 0;
    platform_hold_valid = finalize_incomplete && caller_still_held_after_finalize &&
        native_live_after_finalize;
#else
    const bool native_live_after_finalize = ledger && child_process &&
        WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
        sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid)) ==
            std::optional<uint64_t>(ledger->start_stamp);
    platform_hold_valid =
        finalize_incomplete &&
        caller_still_held_after_finalize && native_live_after_finalize;
#endif

    s_spawn_hold.release.store(true, std::memory_order_release);
    spawn_thread.join();
    const auto held_observation = custody.status();
    const bool caller_returned_retained_custody =
        spawn_finished.load(std::memory_order_acquire) && !spawn_threw &&
        held_observation.accepted && held_observation.created_occurrences == 1 &&
        held_observation.release_requested && !held_observation.release_complete;

    const bool release_written = write_complete_file(
        marker_path(shared.path(), k_release_child_file),
        "release=1\ncomplete=1\n");
    bool forced_cleanup = false;
    bool cleanup_valid = false;
#ifdef _WIN32
    const bool child_finalized = sintra::test::wait_for_file(
        marker_path(shared.path(), k_child_finalized_file),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    bool native_exit_confirmed = false;
    bool normal_exit = false;
    bool survivor_absent = false;
    if (child_process && WaitForSingleObject(child_process, 10000) == WAIT_OBJECT_0) {
        DWORD exit_code = STILL_ACTIVE;
        native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
        normal_exit = native_exit_confirmed && exit_code == 0;
        survivor_absent = native_exit_confirmed;
    }
    if (!native_exit_confirmed && ledger && child_process &&
        WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT)
    {
        forced_cleanup = TerminateProcess(child_process, 2) != 0;
        survivor_absent = WaitForSingleObject(child_process, 2000) == WAIT_OBJECT_0;
    }
    cleanup_valid = release_written && child_finalized && native_exit_confirmed &&
        normal_exit && survivor_absent && !forced_cleanup;
    if (child_process) {
        CloseHandle(child_process);
    }
#else
    wait_until([&]() {
        return s_posix_reap.count.load(std::memory_order_acquire) == 1;
    }, k_watchdog_timeout);
    if (ledger && s_posix_reap.count.load(std::memory_order_acquire) == 0) {
        if (signal_exact_posix_child(ledger->pid, ledger->start_stamp, SIGTERM)) {
            forced_cleanup = true;
        }
    }
    const auto posix_reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    const bool posix_normal_exit =
        s_posix_reap.count.load(std::memory_order_acquire) == 1 &&
        WIFEXITED(posix_reap_status) && WEXITSTATUS(posix_reap_status) == 0;
    cleanup_valid = release_written && posix_normal_exit &&
        ledger && !exact_posix_child_is_live(ledger->pid, ledger->start_stamp) &&
        !forced_cleanup;
#endif

    const auto released_observation = custody.release_until(
        std::chrono::steady_clock::now() + k_watchdog_timeout);
    bool final_retry_succeeded = false;
    if (released_observation.release_complete) {
        try {
            final_retry_succeeded = sintra::detail::finalize();
        }
        catch (...) {
        }
    }
    cleanup_valid = cleanup_valid && released_observation.release_complete &&
        final_retry_succeeded && sintra::s_mproc == nullptr;

    sintra::detail::test_hooks::s_runtime_spawn_success.store(nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_destroying_publication.store(
        nullptr, std::memory_order_release);
    s_spawn_hold.expected_iid.store(sintra::invalid_instance_id, std::memory_order_release);
    s_destroy_expected_iid.store(sintra::invalid_instance_id, std::memory_order_release);
#ifndef _WIN32
    sintra::detail::test_hooks::s_child_reaped.store(nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
#endif

    const unsigned long long output_piid = ledger
        ? static_cast<unsigned long long>(ledger->process_iid)
        : 0ull;
    const unsigned output_occurrence = ledger
        ? ledger->occurrence
        : std::numeric_limits<unsigned>::max();
    const int output_pid = ledger ? ledger->pid : -1;
    const std::string output_stamp = ledger && ledger->start_stamp_available
        ? std::to_string(ledger->start_stamp)
        : "unavailable";

    if (accepted_like_hold && platform_hold_valid &&
        caller_returned_retained_custody && cleanup_valid)
    {
#ifdef _WIN32
        std::printf(
            "R8_W_GREEN_VALID nonce=%s piid=%llu occurrence=%u pid=%d start_stamp=%s "
            "lifeline_enabled=0 lifeline_entry=absent self_published=1 begin_draining=1 "
            "caller_held=1 finalize_returned_incomplete=1 runtime_state_retained=1 "
            "native_alive_after_finalize=1 custody_retained=1 release_complete=1 "
            "finalize_retry_succeeded=1 natural_cleanup=1 survivor_absent=1\n",
            nonce.c_str(), output_piid, output_occurrence, output_pid, output_stamp.c_str());
#else
        std::printf(
            "R8_P_GREEN_VALID nonce=%s piid=%llu occurrence=%u pid=%d start_stamp=%s "
            "lifeline_enabled=0 lifeline_entry=absent self_published=1 begin_draining=1 "
            "caller_held=1 finalize_returned_incomplete=1 runtime_state_retained=1 "
            "native_alive_after_finalize=1 custody_retained=1 release_complete=1 "
            "finalize_retry_succeeded=1 reap_count=%u reap_status=%d survivor_absent=1\n",
            nonce.c_str(), output_piid, output_occurrence, output_pid, output_stamp.c_str(),
            s_posix_reap.count.load(std::memory_order_acquire),
            s_posix_reap.status.load(std::memory_order_relaxed));
#endif
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(
        stderr,
        "R8_INVALID nonce=%s piid=%llu occurrence=%u pid=%d start_stamp=%s "
        "spawn_hold=%d ledger_valid=%d start_stamp_verified=%d native_alive_before=%d "
        "publication_confirmed=%d no_lifeline_entry=%d first_finalize_succeeded=%d "
        "destroy_publication_count=%u unpublished_count=%u caller_held=%d "
        "platform_hold_valid=%d custody_accepted=%d spawn_threw=%d cleanup_valid=%d forced_cleanup=%d\n",
        nonce.c_str(), output_piid, output_occurrence, output_pid, output_stamp.c_str(),
        spawn_hold_entered ? 1 : 0,
        ledger_identity_valid ? 1 : 0,
        start_stamp_verified ? 1 : 0,
        native_alive_before ? 1 : 0,
        publication_confirmed ? 1 : 0,
        no_lifeline_entry ? 1 : 0,
        root_finalized ? 1 : 0,
        s_destroy_publication_count.load(std::memory_order_acquire),
        exact_process_unpublished.load(std::memory_order_acquire),
        caller_still_held_after_finalize ? 1 : 0,
        platform_hold_valid ? 1 : 0,
        held_observation.accepted ? 1 : 0,
        spawn_threw ? 1 : 0,
        cleanup_valid ? 1 : 0,
        forced_cleanup ? 1 : 0);
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_CUSTODY_FINALIZE_RACE_DIR",
        "managed_child_custody_finalize_race_contract");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared);
}
