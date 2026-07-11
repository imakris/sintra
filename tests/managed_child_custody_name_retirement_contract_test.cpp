//
// Managed-child custody name/retirement separation witness (R5-W).
//

#include <sintra/sintra.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/runtime.h>

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

constexpr std::string_view k_child_flag = "--managed_child_custody_name_retirement_child";
constexpr std::string_view k_nonce_flag = "--managed_child_custody_name_retirement_nonce";
constexpr std::string_view k_ledger_file = "child_ledger.complete";
constexpr std::string_view k_retire_request_file = "retire_requested.complete";
constexpr std::string_view k_retirement_hold_file = "retirement_hold.complete";
constexpr std::string_view k_release_file = "release.complete";
constexpr std::string_view k_release_seen_file = "release_seen.complete";
constexpr std::string_view k_finalized_file = "child_finalized.complete";
constexpr auto k_watchdog_timeout = std::chrono::seconds(12);
constexpr sintra::instance_id_type k_child_process_iid = sintra::compose_instance(31u, 1ull);

struct Requested_target : sintra::Derived_transceiver<Requested_target>
{};

struct Participation : sintra::Derived_transceiver<Participation>
{
    int prove_live(int value)
    {
        return value ^ 0x35a71;
    }
    SINTRA_RPC(prove_live)
};

struct Child_ledger
{
    std::string               nonce;
    sintra::instance_id_type  process_iid = sintra::invalid_instance_id;
    uint32_t                  occurrence = std::numeric_limits<uint32_t>::max();
    int                       pid = -1;
    bool                      start_stamp_available = false;
    uint64_t                  start_stamp = 0;
    std::string               managed_name;
    std::string               requested_name;
    sintra::instance_id_type  requested_iid = sintra::invalid_instance_id;
    std::string               participation_name;
    sintra::instance_id_type  participation_iid = sintra::invalid_instance_id;
};

struct Event_observation
{
    std::atomic<uint32_t> requested_published{0};
    std::atomic<uint32_t> requested_unpublished{0};
    std::atomic<uint32_t> participation_published{0};
    std::atomic<uint32_t> participation_unpublished{0};
    std::atomic<uint32_t> process_published{0};
    std::atomic<uint32_t> process_unpublished{0};
};

std::string const* s_expected_requested_name = nullptr;
std::atomic<uint32_t> s_exact_retirement_count{0};
std::atomic<sintra::instance_id_type> s_exact_retirement_iid{sintra::invalid_instance_id};

fs::path marker_path(const fs::path& dir, std::string_view name)
{
    return dir / std::string(name);
}

bool write_complete_file(const fs::path& path, const std::string& contents)
{
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << contents;
        out.flush();
        if (!out) {
            return false;
        }
    }

    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
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
            else if (key == "requested_name") {
                ledger.requested_name = value;
            }
            else if (key == "requested_iid") {
                ledger.requested_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
            }
            else if (key == "participation_name") {
                ledger.participation_name = value;
            }
            else if (key == "participation_iid") {
                ledger.participation_iid = static_cast<sintra::instance_id_type>(std::stoull(value));
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
        ledger.managed_name.empty() || ledger.requested_name.empty() ||
        ledger.participation_name.empty())
    {
        return std::nullopt;
    }
    return ledger;
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

bool wait_for_resolution(
    const std::string&       name,
    sintra::instance_id_type expected,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const auto resolved = resolve_publicly(name);
        if (resolved && *resolved == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto resolved = resolve_publicly(name);
    return resolved && *resolved == expected;
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

void exact_name_retired(
    sintra::instance_id_type instance_id,
    const std::string&       assigned_name)
{
    const std::string* expected = s_expected_requested_name;
    if (!expected || assigned_name != *expected) {
        return;
    }
    s_exact_retirement_iid.store(instance_id, std::memory_order_relaxed);
    s_exact_retirement_count.fetch_add(1, std::memory_order_release);
}

#ifndef _WIN32
struct Posix_reap_observation
{
    std::atomic<pid_t> expected_pid{-1};
    std::atomic<uint32_t> count{0};
    std::atomic<int> status{0};
};

Posix_reap_observation s_posix_reap;

void posix_child_reaped(pid_t pid, int status) noexcept
{
    if (s_posix_reap.expected_pid.load(std::memory_order_acquire) != pid) {
        return;
    }
    s_posix_reap.status.store(status, std::memory_order_relaxed);
    s_posix_reap.count.fetch_add(1, std::memory_order_release);
    s_posix_reap.count.notify_all();
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
    if (nonce.empty()) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: child nonce missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: child init failed: %s\n", error.what());
        return 2;
    }

    const int pid = sintra::test::get_pid();
    const auto start_stamp = sintra::current_process_start_stamp();
    const std::string managed_name = "sintra_process_" + std::to_string(pid);
    const std::string requested_name = "managed_child_requested_" + nonce;
    const std::string participation_name = "managed_child_participation_" + nonce;

    bool child_valid = true;
    {
        Participation participation;
        if (!participation.assign_name(participation_name)) {
            std::fprintf(stderr, "managed_child_custody_name_retirement: participation publish failed\n");
            child_valid = false;
        }

        {
            Requested_target requested;
            if (!requested.assign_name(requested_name)) {
                std::fprintf(stderr, "managed_child_custody_name_retirement: requested target publish failed\n");
                child_valid = false;
            }

            std::ostringstream record;
            record << "nonce=" << nonce << '\n'
                   << "piid=" << static_cast<unsigned long long>(sintra::s_mproc_id) << '\n'
                   << "occurrence=" << sintra::s_recovery_occurrence << '\n'
                   << "pid=" << pid << '\n'
                   << "start_stamp_available=" << (start_stamp.has_value() ? 1 : 0) << '\n'
                   << "start_stamp=" << start_stamp.value_or(0) << '\n'
                   << "managed_name=" << managed_name << '\n'
                   << "requested_name=" << requested_name << '\n'
                   << "requested_iid=" << static_cast<unsigned long long>(requested.instance_id()) << '\n'
                   << "participation_name=" << participation_name << '\n'
                   << "participation_iid=" << static_cast<unsigned long long>(participation.instance_id()) << '\n'
                   << "complete=1\n";

            if (!child_valid ||
                !write_complete_file(marker_path(shared_dir, k_ledger_file), record.str()) ||
                !sintra::test::wait_for_file(
                    marker_path(shared_dir, k_retire_request_file),
                    std::chrono::seconds(30),
                    std::chrono::milliseconds(10)))
            {
                std::fprintf(stderr, "managed_child_custody_name_retirement: child setup/retire latch failed\n");
                child_valid = false;
            }
        }

        if (!write_complete_file(
                marker_path(shared_dir, k_retirement_hold_file),
                "requested_target_unpublish_returned=1\ncomplete=1\n") ||
            !sintra::test::wait_for_file(
                marker_path(shared_dir, k_release_file),
                std::chrono::seconds(30),
                std::chrono::milliseconds(10)) ||
            !write_complete_file(
                marker_path(shared_dir, k_release_seen_file),
                "release_seen=1\ncomplete=1\n"))
        {
            std::fprintf(stderr, "managed_child_custody_name_retirement: retirement hold/release failed\n");
            child_valid = false;
        }
    }

    bool finalized = false;
    try {
        finalized = sintra::detail::finalize();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: child finalize failed: %s\n", error.what());
    }
    catch (...) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: child finalize failed\n");
    }

    if (!child_valid || !finalized ||
        !write_complete_file(
            marker_path(shared_dir, k_finalized_file),
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
        std::fprintf(stderr, "managed_child_custody_name_retirement: binary path missing\n");
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: root init failed: %s\n", error.what());
        return 2;
    }

    const auto nonce_value = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string nonce = std::to_string(nonce_value) + "_" +
        std::to_string(sintra::test::get_pid());
    const std::string requested_name = "managed_child_requested_" + nonce;
    const std::string participation_name = "managed_child_participation_" + nonce;
    Event_observation events;

    auto published = [&](const sintra::Coordinator::instance_published& message) {
        const std::string name = static_cast<std::string>(message.assigned_name);
        if (name == requested_name) {
            events.requested_published.fetch_add(1, std::memory_order_release);
        }
        else if (name == participation_name) {
            events.participation_published.fetch_add(1, std::memory_order_release);
        }
        if (message.instance_id == k_child_process_iid) {
            events.process_published.fetch_add(1, std::memory_order_release);
        }
    };
    auto unpublished = [&](const sintra::Coordinator::instance_unpublished& message) {
        const std::string name = static_cast<std::string>(message.assigned_name);
        if (name == requested_name) {
            events.requested_unpublished.fetch_add(1, std::memory_order_release);
        }
        else if (name == participation_name) {
            events.participation_unpublished.fetch_add(1, std::memory_order_release);
        }
        if (message.instance_id == k_child_process_iid) {
            events.process_unpublished.fetch_add(1, std::memory_order_release);
        }
    };
    sintra::activate_slot(
        published,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));
    sintra::activate_slot(
        unpublished,
        sintra::Typed_instance_id<sintra::Coordinator>(sintra::s_coord_id));

    s_expected_requested_name = &requested_name;
    s_exact_retirement_count.store(0, std::memory_order_relaxed);
    s_exact_retirement_iid.store(sintra::invalid_instance_id, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_coordinator_name_retired.store(
        &exact_name_retired,
        std::memory_order_release);
#ifndef _WIN32
    s_posix_reap.expected_pid.store(-1, std::memory_order_relaxed);
    s_posix_reap.count.store(0, std::memory_order_relaxed);
    s_posix_reap.status.store(0, std::memory_order_relaxed);
    sintra::detail::test_hooks::s_child_reaped.store(&posix_child_reaped, std::memory_order_release);
#endif

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_nonce_flag),
        nonce,
    };
    options.process_instance_id = k_child_process_iid;
    options.wait_for_instance_name = requested_name;
    options.wait_timeout = std::chrono::seconds(8);
    options.lifetime.enable_lifeline = false;

    sintra::Managed_child_custody custody;
    bool spawn_threw = false;
    try {
        custody = sintra::spawn_swarm_process(options);
    }
    catch (...) {
        spawn_threw = true;
    }

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
    bool initial_publications_valid = false;
    bool initial_communication_live = false;
#ifdef _WIN32
    HANDLE child_process = nullptr;
#endif

    if (ledger) {
        ledger_identity_valid =
            ledger->nonce == nonce &&
            ledger->process_iid == k_child_process_iid &&
            ledger->occurrence == 0 &&
            ledger->requested_name == requested_name &&
            ledger->participation_name == participation_name &&
            sintra::process_of(ledger->requested_iid) == k_child_process_iid &&
            sintra::process_of(ledger->participation_iid) == k_child_process_iid &&
            ledger->requested_iid != ledger->participation_iid &&
            ledger->managed_name == "sintra_process_" + std::to_string(ledger->pid);

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

        const bool publish_events_seen = wait_until([&]() {
            return events.requested_published.load(std::memory_order_acquire) == 1 &&
                events.participation_published.load(std::memory_order_acquire) == 1 &&
                events.process_published.load(std::memory_order_acquire) == 1;
        }, std::chrono::seconds(3));
        initial_publications_valid =
            publish_events_seen &&
            wait_for_resolution(requested_name, ledger->requested_iid, std::chrono::seconds(2)) &&
            wait_for_resolution(participation_name, ledger->participation_iid, std::chrono::seconds(2)) &&
            wait_for_resolution(ledger->managed_name, k_child_process_iid, std::chrono::seconds(2)) &&
            s_exact_retirement_count.load(std::memory_order_acquire) == 0 &&
            events.requested_unpublished.load(std::memory_order_acquire) == 0 &&
            events.participation_unpublished.load(std::memory_order_acquire) == 0 &&
            events.process_unpublished.load(std::memory_order_acquire) == 0;

        try {
            initial_communication_live =
                Participation::rpc_prove_live(ledger->participation_iid, 0x2244) ==
                (0x2244 ^ 0x35a71);
        }
        catch (...) {
            initial_communication_live = false;
        }
    }

    const auto launch_observation = sintra::observe_managed_child(custody);
    const bool pre_retirement_valid =
        !spawn_threw && launch_observation.accepted &&
        launch_observation.readiness_reached &&
        launch_observation.created_occurrences == 1 && ledger_identity_valid &&
        start_stamp_verified && native_alive_before &&
        initial_publications_valid && initial_communication_live;

    bool retire_request_written = write_complete_file(
        marker_path(shared.path(), k_retire_request_file),
        "retire_requested=1\ncomplete=1\n");
    const bool exact_transition_seen = wait_until([&]() {
        return s_exact_retirement_count.load(std::memory_order_acquire) == 1;
    }, k_watchdog_timeout);
    const bool child_held_after_transition = sintra::test::wait_for_file(
        marker_path(shared.path(), k_retirement_hold_file),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));

    bool target_absent = false;
    bool same_occurrence_live = false;
    bool communication_still_live = false;
    bool later_facts_nonterminal = false;
    if (ledger && child_held_after_transition) {
        const bool target_unpublish_event_seen = wait_until([&]() {
            return events.requested_unpublished.load(std::memory_order_acquire) == 1;
        }, std::chrono::seconds(3));
        target_absent =
            target_unpublish_event_seen &&
            wait_for_resolution(
                requested_name,
                sintra::invalid_instance_id,
                std::chrono::seconds(2));

#ifdef _WIN32
        const bool native_still_live = child_process &&
            WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
            sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid)) ==
                std::optional<uint64_t>(ledger->start_stamp);
#else
        const bool native_still_live =
            exact_posix_child_is_live(ledger->pid, ledger->start_stamp);
#endif
        same_occurrence_live =
            native_still_live &&
            wait_for_resolution(ledger->managed_name, k_child_process_iid, std::chrono::seconds(2)) &&
            wait_for_resolution(
                participation_name,
                ledger->participation_iid,
                std::chrono::seconds(2));
        try {
            communication_still_live =
                Participation::rpc_prove_live(ledger->participation_iid, 0x5912) ==
                (0x5912 ^ 0x35a71);
        }
        catch (...) {
            communication_still_live = false;
        }

        later_facts_nonterminal =
            events.participation_unpublished.load(std::memory_order_acquire) == 0 &&
            events.process_unpublished.load(std::memory_order_acquire) == 0 &&
            !fs::exists(marker_path(shared.path(), k_release_seen_file)) &&
            !fs::exists(marker_path(shared.path(), k_finalized_file))
#ifndef _WIN32
            && s_posix_reap.count.load(std::memory_order_acquire) == 0
#endif
            ;
    }

    const bool witness_valid =
        pre_retirement_valid && retire_request_written && exact_transition_seen &&
        child_held_after_transition && ledger &&
        s_exact_retirement_iid.load(std::memory_order_acquire) == ledger->requested_iid &&
        s_exact_retirement_count.load(std::memory_order_acquire) == 1 &&
        events.requested_published.load(std::memory_order_acquire) == 1 &&
        events.requested_unpublished.load(std::memory_order_acquire) == 1 &&
        target_absent && same_occurrence_live && communication_still_live &&
        later_facts_nonterminal;

    const auto held_release_observation = sintra::release_managed_child(
        custody,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    const bool release_incomplete_at_witness =
        held_release_observation.accepted &&
        held_release_observation.release_requested &&
        !held_release_observation.release_complete;

    const bool release_written = write_complete_file(
        marker_path(shared.path(), k_release_file),
        "release=1\ncomplete=1\n");
    const bool release_seen = sintra::test::wait_for_file(
        marker_path(shared.path(), k_release_seen_file),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const bool child_finalized = sintra::test::wait_for_file(
        marker_path(shared.path(), k_finalized_file),
        k_watchdog_timeout,
        std::chrono::milliseconds(10));
    const auto released_observation = sintra::wait_managed_child(
        custody,
        std::chrono::steady_clock::now() + k_watchdog_timeout);

    bool native_exit_confirmed = false;
    bool native_normal_exit = false;
    bool survivor_absent = false;
    bool forced_cleanup = false;
#ifndef _WIN32
    uint32_t reap_count = 0;
    int reap_status = 0;
#endif
    if (ledger && release_written) {
#ifdef _WIN32
        if (child_process && WaitForSingleObject(child_process, 10000) == WAIT_OBJECT_0) {
            DWORD exit_code = STILL_ACTIVE;
            native_exit_confirmed = GetExitCodeProcess(child_process, &exit_code) != 0;
            native_normal_exit = native_exit_confirmed && exit_code == 0;
            survivor_absent = native_exit_confirmed;
        }
#else
        wait_until([&]() {
            return s_posix_reap.count.load(std::memory_order_acquire) != 0;
        }, k_watchdog_timeout);
#endif
    }

    const bool later_retirements_seen = wait_until([&]() {
        return events.participation_unpublished.load(std::memory_order_acquire) == 1 &&
            events.process_unpublished.load(std::memory_order_acquire) == 1;
    }, std::chrono::seconds(3));

#ifdef _WIN32
    if (ledger && !native_exit_confirmed && child_process &&
        WaitForSingleObject(child_process, 0) == WAIT_TIMEOUT &&
        sintra::query_process_start_stamp(static_cast<uint32_t>(ledger->pid)) ==
            std::optional<uint64_t>(ledger->start_stamp))
    {
        forced_cleanup = TerminateProcess(child_process, 2) != 0;
        survivor_absent = WaitForSingleObject(child_process, 2000) == WAIT_OBJECT_0;
    }
#else
    if (ledger && s_posix_reap.count.load(std::memory_order_acquire) == 0) {
        if (signal_exact_posix_child(ledger->pid, ledger->start_stamp, SIGTERM)) {
            forced_cleanup = true;
            wait_until([&]() {
                return s_posix_reap.count.load(std::memory_order_acquire) != 0;
            }, std::chrono::seconds(2));
        }
        if (s_posix_reap.count.load(std::memory_order_acquire) == 0 &&
            signal_exact_posix_child(ledger->pid, ledger->start_stamp, SIGKILL))
        {
            forced_cleanup = true;
            wait_until([&]() {
                return s_posix_reap.count.load(std::memory_order_acquire) != 0;
            }, std::chrono::seconds(2));
        }
    }
#endif

    sintra::detail::test_hooks::s_coordinator_name_retired.store(nullptr, std::memory_order_release);
    s_expected_requested_name = nullptr;
    sintra::deactivate_all_slots();

    bool root_finalized = false;
    try {
        root_finalized = sintra::detail::finalize();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: root finalize failed: %s\n", error.what());
    }
    catch (...) {
        std::fprintf(stderr, "managed_child_custody_name_retirement: root finalize failed\n");
    }

#ifndef _WIN32
    if (root_finalized) {
        wait_until([&]() {
            return s_posix_reap.count.load(std::memory_order_acquire) != 0;
        }, std::chrono::seconds(1));
    }
    reap_count = s_posix_reap.count.load(std::memory_order_acquire);
    reap_status = s_posix_reap.status.load(std::memory_order_relaxed);
    native_exit_confirmed = reap_count == 1;
    native_normal_exit = native_exit_confirmed &&
        WIFEXITED(reap_status) && WEXITSTATUS(reap_status) == 0;
    survivor_absent = native_exit_confirmed && ledger &&
        !exact_posix_child_is_live(ledger->pid, ledger->start_stamp);
    sintra::detail::test_hooks::s_child_reaped.store(nullptr, std::memory_order_release);
    s_posix_reap.expected_pid.store(-1, std::memory_order_release);
#else
    if (child_process) {
        CloseHandle(child_process);
    }
#endif

    const bool cleanup_valid =
        release_incomplete_at_witness && release_written && release_seen && child_finalized &&
        released_observation.release_complete &&
        later_retirements_seen &&
        events.requested_unpublished.load(std::memory_order_acquire) == 1 &&
        events.participation_unpublished.load(std::memory_order_acquire) == 1 &&
        events.process_unpublished.load(std::memory_order_acquire) == 1 &&
        native_exit_confirmed && native_normal_exit && survivor_absent &&
        !forced_cleanup && root_finalized;

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
#ifdef _WIN32
    const std::string reap_record = "reap_count=not_applicable normal_status=" +
        std::to_string(native_normal_exit ? 1 : 0);
#else
    const std::string reap_record = "reap_count=" + std::to_string(reap_count) +
        " reap_status=" + std::to_string(reap_status) +
        " normal_status=" + std::to_string(native_normal_exit ? 1 : 0);
#endif

    if (witness_valid && cleanup_valid) {
        std::printf(
            "R5_GREEN_VALID nonce=%s piid=%llu occurrence=%u pid=%d "
            "start_stamp=%s custody_accepted=1 target_published=1 exact_name_retirement=1 "
            "target_name_after=absent same_occurrence_live=1 communication_live=1 "
            "later_facts_nonterminal=1 release_incomplete_at_witness=1 "
            "release_complete=1 native_exit_confirmed=1 survivor_absent=1 %s\n",
            nonce.c_str(),
            output_piid,
            output_occurrence,
            output_pid,
            output_stamp.c_str(),
            reap_record.c_str());
        std::fflush(stdout);
        return 0;
    }

    std::fprintf(
        stderr,
        "R5_WITNESS_INVALID nonce=%s piid=%llu occurrence=%u pid=%d start_stamp=%s "
        "custody_accepted=%d spawn_threw=%d ledger_valid=%d target_published=%u "
        "exact_retirement_count=%u target_absent=%d same_occurrence_live=%d "
        "communication_live=%d later_facts_nonterminal=%d release_seen=%d "
        "child_finalized=%d native_exit_confirmed=%d survivor_absent=%d "
        "forced_cleanup=%d root_finalized=%d %s\n",
        nonce.c_str(),
        output_piid,
        output_occurrence,
        output_pid,
        output_stamp.c_str(),
        launch_observation.accepted ? 1 : 0,
        spawn_threw ? 1 : 0,
        ledger_identity_valid ? 1 : 0,
        events.requested_published.load(std::memory_order_acquire),
        s_exact_retirement_count.load(std::memory_order_acquire),
        target_absent ? 1 : 0,
        same_occurrence_live ? 1 : 0,
        communication_still_live ? 1 : 0,
        later_facts_nonterminal ? 1 : 0,
        release_seen ? 1 : 0,
        child_finalized ? 1 : 0,
        native_exit_confirmed ? 1 : 0,
        survivor_absent ? 1 : 0,
        forced_cleanup ? 1 : 0,
        root_finalized ? 1 : 0,
        reap_record.c_str());
    return 2;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_MANAGED_CHILD_CUSTODY_NAME_RETIREMENT_DIR",
        "managed_child_custody_name_retirement_contract");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared);
}
