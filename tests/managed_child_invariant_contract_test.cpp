// Fail-first contract for managed-child exact-record and native-roster lookups.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr std::string_view k_child_flag = "--f03-invariant-child";

enum class Target { none, exact_occurrence, posix_reap_reservation };

struct Child_identity
{
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    uint32_t occurrence = 0;
    int pid = -1;
    uint64_t start_stamp = 0;
};

std::atomic<Target> s_target{Target::none};
std::atomic<sintra::instance_id_type> s_expected_iid{
    sintra::invalid_instance_id};
std::atomic<unsigned> s_exact_hits{0};
std::atomic<unsigned> s_roster_hits{0};
std::atomic<uint64_t> s_reservation{0};
std::atomic<int> s_expected_pid{-1};
std::atomic<unsigned> s_reap_count{0};
std::atomic<int> s_reap_status{0};
std::atomic<bool> s_cleanup_entered{false};
std::atomic<bool> s_cleanup_release{false};
fs::path s_marker;

std::optional<Child_identity> read_identity(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    unsigned long long process_iid = 0;
    unsigned long long start_stamp = 0;
    Child_identity identity;
    if (!(in >> process_iid >> identity.occurrence >> identity.pid >> start_stamp)) {
        return std::nullopt;
    }
    identity.process_iid = static_cast<sintra::instance_id_type>(process_iid);
    identity.start_stamp = static_cast<uint64_t>(start_stamp);
    return identity;
}

bool force_invariant_miss(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence,
    uint64_t reservation) noexcept
{
    if (!stage || occurrence != 0 ||
        process_iid != s_expected_iid.load(std::memory_order_acquire))
    {
        return false;
    }
    const std::string_view observed(stage);
    if (s_target.load(std::memory_order_acquire) == Target::exact_occurrence &&
        observed == sintra::detail::test_hooks::
            k_managed_child_exact_occurrence_lookup)
    {
        s_exact_hits.fetch_add(1, std::memory_order_release);
        return true;
    }
#ifndef _WIN32
    if (s_target.load(std::memory_order_acquire) ==
            Target::posix_reap_reservation &&
        observed == sintra::detail::test_hooks::
            k_managed_child_posix_reap_reservation_lookup)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        std::error_code error;
        while (!fs::exists(s_marker, error) &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(2ms);
        }
        try {
            if (const auto identity = read_identity(s_marker)) {
                s_expected_pid.store(identity->pid, std::memory_order_release);
            }
        }
        catch (...) {
        }
        s_reservation.store(reservation, std::memory_order_release);
        s_roster_hits.fetch_add(1, std::memory_order_release);
        return true;
    }
#else
    (void)reservation;
#endif
    return false;
}

#ifndef _WIN32
void observe_reap(pid_t pid, int status) noexcept
{
    if (pid == s_expected_pid.load(std::memory_order_acquire)) {
        s_reap_status.store(status, std::memory_order_relaxed);
        s_reap_count.fetch_add(1, std::memory_order_release);
    }
}
#endif

void gate_automatic_cleanup(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence)
{
    if (!stage ||
        s_target.load(std::memory_order_acquire) !=
            Target::posix_reap_reservation ||
        process_iid != s_expected_iid.load(std::memory_order_acquire) ||
        occurrence != 0 ||
        std::string_view(stage) != sintra::detail::test_hooks::
            k_managed_child_cleanup_before_actions)
    {
        return;
    }
    s_cleanup_entered.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!s_cleanup_release.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
}

int child_main(const fs::path& marker)
{
    const auto stamp = sintra::current_process_start_stamp();
    if (!stamp) return 2;
    const std::string contents =
        std::to_string(static_cast<unsigned long long>(sintra::s_mproc_id)) + " " +
        std::to_string(sintra::s_recovery_occurrence) + " " +
        std::to_string(sintra::test::get_pid()) + " " +
        std::to_string(static_cast<unsigned long long>(*stamp)) + "\n";
    if (!sintra::test::managed_child::write_complete_file(marker, contents)) {
        return 3;
    }
    std::this_thread::sleep_for(15s);
    return 0;
}

enum class Case_result { green, red, invalid };

Case_result run_case(
    int argc,
    char* argv[],
    const sintra::test::Shared_directory& shared,
    Target target,
    std::string_view name)
{
    sintra::init(argc, argv);
    s_target.store(target, std::memory_order_release);
    s_exact_hits.store(0, std::memory_order_relaxed);
    s_roster_hits.store(0, std::memory_order_relaxed);
    s_reservation.store(0, std::memory_order_relaxed);
    s_expected_pid.store(-1, std::memory_order_relaxed);
    s_reap_count.store(0, std::memory_order_relaxed);
    s_reap_status.store(0, std::memory_order_relaxed);
    s_cleanup_entered.store(false, std::memory_order_relaxed);
    s_cleanup_release.store(false, std::memory_order_relaxed);
    s_marker = shared.path() / (std::string(name) + ".complete");

    const auto process_iid = sintra::make_process_instance_id();
    s_expected_iid.store(process_iid, std::memory_order_release);
    sintra::Managed_child_custody custody;
    {
        sintra::test::managed_child::Scoped_test_hook invariant_hook(
            sintra::detail::test_hooks::s_managed_child_invariant,
            &force_invariant_miss);
        sintra::test::managed_child::Scoped_test_hook cleanup_hook(
            sintra::detail::test_hooks::s_managed_child_cleanup,
            &gate_automatic_cleanup);
#ifndef _WIN32
        sintra::test::managed_child::Scoped_test_hook reap_hook(
            sintra::detail::test_hooks::s_child_reaped,
            &observe_reap);
#endif
        sintra::Spawn_options options;
        options.binary_path = fs::absolute(argv[0]).string();
        options.args = {std::string(k_child_flag), s_marker.string()};
        options.process_instance_id = process_iid;
        options.lifetime.enable_lifeline = true;
        custody = sintra::spawn_swarm_process(options);

        if (target == Target::posix_reap_reservation) {
            const auto cleanup_deadline =
                std::chrono::steady_clock::now() + 3s;
            while (!s_cleanup_entered.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < cleanup_deadline)
            {
                std::this_thread::sleep_for(1ms);
            }
        }

        const auto before = custody.status();
        std::optional<Child_identity> identity;
        if (before.created_occurrences != 0 &&
            sintra::test::wait_for_file(s_marker, 3s, 5ms))
        {
            identity = read_identity(s_marker);
            if (identity) {
                s_expected_pid.store(identity->pid, std::memory_order_release);
            }
        }

        const bool reader_retained_before_cleanup =
            sintra::s_mproc->has_process_reader(process_iid);
        bool lifeline_retained_before_cleanup = false;
        {
            std::lock_guard<std::mutex> lock(sintra::s_mproc->m_lifeline_mutex);
            lifeline_retained_before_cleanup =
                sintra::s_mproc->m_lifeline_writes.count(process_iid) == 1;
        }
        bool init_clear_before_cleanup = false;
        {
            std::lock_guard lock(sintra::s_coord->m_init_tracking_mutex);
            init_clear_before_cleanup =
                sintra::s_coord->m_processes_in_initialization.count(
                    process_iid) == 0;
        }
        bool roster_empty_before_cleanup = true;
        bool exact_roster_authority_before_cleanup = false;
#ifndef _WIN32
        {
            std::lock_guard<std::mutex> lock(
                sintra::s_mproc->m_spawned_child_pids_mutex);
            roster_empty_before_cleanup =
                sintra::s_mproc->m_spawned_child_pids.empty();
            if (identity) {
                const auto reservation =
                    s_reservation.load(std::memory_order_acquire);
                exact_roster_authority_before_cleanup = std::any_of(
                    sintra::s_mproc->m_spawned_child_pids.begin(),
                    sintra::s_mproc->m_spawned_child_pids.end(),
                    [&](const auto& slot) {
                        return slot.reservation_id == reservation &&
                            reservation != 0 && slot.pid == identity->pid &&
                            slot.start_stamp_available &&
                            slot.start_stamp == identity->start_stamp &&
                            slot.occurrence.process_instance_id == process_iid &&
                            slot.occurrence.occurrence == 0;
                    });
            }
        }
#endif

        const auto cleanup_started = std::chrono::steady_clock::now();
        bool automatic_cleanup_requested = false;
        sintra::Managed_child_status released;
        if (target == Target::posix_reap_reservation) {
            const bool automatic_cleanup_entered =
                s_cleanup_entered.load(std::memory_order_acquire);
            s_cleanup_release.store(true, std::memory_order_release);
            released = custody.status();
            automatic_cleanup_requested =
                automatic_cleanup_entered && released.release_state !=
                    sintra::Managed_child_release_state::open;
            const auto automatic_deadline = cleanup_started + 6s;
            while (released.release_state !=
                       sintra::Managed_child_release_state::complete &&
                   std::chrono::steady_clock::now() < automatic_deadline)
            {
                std::this_thread::sleep_for(5ms);
                released = custody.status();
            }
        }
        else {
            released = custody.terminate_until(cleanup_started + 6s);
        }
        const bool cleanup_bounded =
            std::chrono::steady_clock::now() - cleanup_started < 7s;
        const bool exact_absent = !identity ||
            !sintra::test::managed_child::exact_process_is_live(
                identity->pid, identity->start_stamp);
        bool roster_empty = true;
#ifndef _WIN32
        {
            std::lock_guard<std::mutex> lock(
                sintra::s_mproc->m_spawned_child_pids_mutex);
            roster_empty = sintra::s_mproc->m_spawned_child_pids.empty();
        }
#endif
        bool init_clear = false;
        {
            std::lock_guard lock(sintra::s_coord->m_init_tracking_mutex);
            init_clear =
                sintra::s_coord->m_processes_in_initialization.count(process_iid) == 0;
        }

        const bool common = cleanup_bounded && custody &&
            released.release_state == sintra::Managed_child_release_state::complete &&
            exact_absent && roster_empty && init_clear;
        const char* expected_failure_stage =
            target == Target::exact_occurrence
            ? sintra::detail::test_hooks::k_managed_child_exact_occurrence_lookup
            : sintra::detail::test_hooks::
                k_managed_child_posix_reap_reservation_lookup;
        const bool typed_failure =
            released.last_failure.kind ==
                sintra::Managed_child_failure_kind::setup_exception &&
            released.last_failure.occurrence == 0 &&
            released.last_failure.native_error == 0 &&
            released.last_failure.message.find(expected_failure_stage) !=
                std::string::npos;
        const bool no_failure = released.last_failure.kind ==
            sintra::Managed_child_failure_kind::none;
        const bool exact_hit = s_exact_hits.load(std::memory_order_acquire) == 1;
        const bool roster_hit = s_roster_hits.load(std::memory_order_acquire) == 1;
        const bool identity_valid = identity &&
            identity->process_iid == process_iid && identity->occurrence == 0 &&
            identity->pid > 0 && identity->start_stamp != 0;
#ifndef _WIN32
        const int reap_status = s_reap_status.load(std::memory_order_acquire);
        const bool reap_abnormal = WIFSIGNALED(reap_status) ||
            (WIFEXITED(reap_status) && WEXITSTATUS(reap_status) != 0);
        const bool reap_exact = !identity ||
            (s_reap_count.load(std::memory_order_acquire) == 1 &&
             s_expected_pid.load(std::memory_order_acquire) == identity->pid &&
             reap_abnormal);
#else
        const bool reap_exact = true;
#endif

        bool green = false;
        bool red = false;
        if (target == Target::exact_occurrence) {
            green = common && exact_hit && !roster_hit && typed_failure &&
                !reader_retained_before_cleanup &&
                !lifeline_retained_before_cleanup &&
                init_clear_before_cleanup && roster_empty_before_cleanup &&
                released.admitted_occurrences == 1 &&
                released.created_occurrences == 0 &&
                released.exited_occurrences == 0 && !identity;
            red = common && exact_hit && !roster_hit && no_failure &&
                released.admitted_occurrences == 1 &&
                released.created_occurrences == 1 &&
                released.exited_occurrences == 1 && identity_valid && reap_exact;
        }
#ifndef _WIN32
        else {
            green = common && !exact_hit && roster_hit && typed_failure &&
                reader_retained_before_cleanup &&
                lifeline_retained_before_cleanup &&
                init_clear_before_cleanup &&
                exact_roster_authority_before_cleanup &&
                automatic_cleanup_requested &&
                s_reservation.load(std::memory_order_acquire) != 0 &&
                released.admitted_occurrences == 1 &&
                released.created_occurrences == 1 &&
                released.exited_occurrences == 1 && identity_valid && reap_exact;
            red = common && !exact_hit && roster_hit && no_failure &&
                s_reservation.load(std::memory_order_acquire) != 0 &&
                released.admitted_occurrences == 1 &&
                released.created_occurrences == 1 &&
                released.exited_occurrences == 1 && identity_valid && reap_exact;
            if (!green && !red) {
                std::fprintf(stderr,
                    "F03_REAP_INVALID common=%d exact_hit=%d roster_hit=%d "
                    "typed=%d no_failure=%d reader=%d lifeline=%d init=%d "
                    "authority=%d auto=%d reservation=%llu "
                    "admitted=%zu created=%zu exited=%zu identity=%d reap=%d\n",
                    common, exact_hit, roster_hit, typed_failure, no_failure,
                    reader_retained_before_cleanup,
                    lifeline_retained_before_cleanup,
                    init_clear_before_cleanup,
                    exact_roster_authority_before_cleanup,
                    automatic_cleanup_requested,
                    static_cast<unsigned long long>(
                        s_reservation.load(std::memory_order_acquire)),
                    released.admitted_occurrences,
                    released.created_occurrences,
                    released.exited_occurrences,
                    identity_valid,
                    reap_exact);
            }
        }
#endif
        invariant_hook.restore();
        cleanup_hook.restore();
#ifndef _WIN32
        reap_hook.restore();
#endif
        const bool hooks_restored =
            sintra::detail::test_hooks::s_managed_child_invariant.load(
                std::memory_order_acquire) == nullptr
            && sintra::detail::test_hooks::s_managed_child_cleanup.load(
                std::memory_order_acquire) == nullptr
#ifndef _WIN32
            && sintra::detail::test_hooks::s_child_reaped.load(
                std::memory_order_acquire) == nullptr
#endif
            ;
        green = green && hooks_restored;
        red = red && hooks_restored;
        s_target.store(Target::none, std::memory_order_release);

        const auto finalize_started = std::chrono::steady_clock::now();
        const bool finalized = sintra::detail::finalize();
        const bool finalize_bounded =
            std::chrono::steady_clock::now() - finalize_started < 2s;
        const bool runtime_gone = !sintra::s_mproc && !sintra::s_coord;
        std::error_code error;
        fs::remove(s_marker, error);
        if (finalized && finalize_bounded && runtime_gone && green) {
            return Case_result::green;
        }
        if (finalized && finalize_bounded && runtime_gone && red) {
            return Case_result::red;
        }
    }
    if (sintra::s_mproc) (void)sintra::detail::finalize();
    std::error_code error;
    fs::remove(s_marker, error);
    return Case_result::invalid;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_F03_INVARIANT_DIR", "managed_child_f03_invariant");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        sintra::init(argc, argv);
        return child_main(fs::path(sintra::test::get_argv_value(
            argc, argv, k_child_flag)));
    }

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 25s;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) std::_Exit(124);
            std::this_thread::sleep_for(20ms);
        }
    });

    const auto exact = run_case(
        argc, argv, shared, Target::exact_occurrence, "exact_occurrence");
#ifndef _WIN32
    const auto roster = run_case(
        argc, argv, shared, Target::posix_reap_reservation, "reap_reservation");
#else
    const auto roster = Case_result::green;
#endif
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    if (exact == Case_result::green && roster == Case_result::green) {
        std::fprintf(stdout,
            "F03_INVARIANT_GREEN exact_lookup=fail_closed "
#ifndef _WIN32
            "reap_lookup=fail_closed finalized=1 survivors=0 hooks_restored=1\n");
#else
            "reap_lookup=not_applicable finalized=1 survivors=0 hooks_restored=1\n");
#endif
        return 0;
    }
    if (exact == Case_result::red &&
#ifndef _WIN32
        roster == Case_result::red
#else
        roster == Case_result::green
#endif
        )
    {
        std::fprintf(stderr,
            "F03_INVARIANT_RED exact_lookup=ignored "
#ifndef _WIN32
            "reap_lookup=ignored "
#else
            "reap_lookup=not_applicable "
#endif
            "cleanup_bounded=1 finalized=1 survivors=0 hooks_restored=1\n");
        return 88;
    }
    std::fprintf(stderr, "F03_INVARIANT_INVALID exact=%d roster=%d\n",
        static_cast<int>(exact), static_cast<int>(roster));
    return 3;
}
