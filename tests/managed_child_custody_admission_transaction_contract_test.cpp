//
// Fail-first contract for transactional managed-child occurrence admission.
// The injected recovery admission failure must not leave a pending occurrence.
//

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag = "--admission-transaction-child";
constexpr std::string_view k_ledger_0 = "occurrence_0.complete";
constexpr std::string_view k_ledger_1 = "occurrence_1.complete";
constexpr std::string_view k_crash_0 = "crash_occurrence_0.complete";
constexpr auto k_watchdog_timeout = 20s;

struct Child_ledger
{
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    uint32_t occurrence = 0;
    int pid = -1;
    uint64_t start_stamp = 0;
};

std::atomic<sintra::instance_id_type> s_expected_iid{sintra::invalid_instance_id};
std::atomic<unsigned> s_failure_remaining{0};
std::atomic<unsigned> s_failure_hits{0};

fs::path marker(const fs::path& directory, std::string_view name)
{
    return directory / std::string(name);
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

bool inject_admission_mapping_failure(
    const char* stage,
    sintra::instance_id_type process_iid,
    uint32_t occurrence) noexcept
{
    if (!stage ||
        std::string_view(stage) !=
            sintra::detail::test_hooks::k_managed_child_fail_admission_mapping ||
        process_iid != s_expected_iid.load(std::memory_order_acquire) ||
        occurrence != 1)
    {
        return false;
    }

    unsigned remaining = s_failure_remaining.load(std::memory_order_acquire);
    while (remaining != 0) {
        if (s_failure_remaining.compare_exchange_weak(
                remaining, remaining - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            s_failure_hits.fetch_add(1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

std::optional<Child_ledger> read_ledger(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    Child_ledger ledger;
    unsigned long long process_iid = 0;
    unsigned long long start_stamp = 0;
    if (!(in >> process_iid >> ledger.occurrence >> ledger.pid >> start_stamp)) {
        return std::nullopt;
    }
    ledger.process_iid = static_cast<sintra::instance_id_type>(process_iid);
    ledger.start_stamp = static_cast<uint64_t>(start_stamp);
    return ledger;
}

bool write_ledger(const fs::path& path)
{
    const auto start_stamp = sintra::current_process_start_stamp();
    if (!start_stamp) {
        return false;
    }
    std::ostringstream contents;
    contents << static_cast<unsigned long long>(sintra::s_mproc_id) << ' '
             << sintra::s_recovery_occurrence << ' '
             << sintra::test::get_pid() << ' '
             << static_cast<unsigned long long>(*start_stamp) << '\n';
    return write_complete_file(path, contents.str());
}

struct Recovery_completion
{
    std::atomic<bool>& complete;
    ~Recovery_completion()
    {
        complete.store(true, std::memory_order_release);
    }
};

struct Admission_facts
{
    bool record_found = false;
    bool exact_one_occurrence = false;
    bool exact_two_occurrences = false;
    bool predecessor_owned = false;
    bool predecessor_exited = false;
    bool failed_occurrence_pending = false;
    bool failed_occurrence_no_child = false;
    bool active_map_predecessor = false;
};

Admission_facts observe_admission_facts(
    sintra::instance_id_type process_iid,
    const std::shared_ptr<sintra::detail::Managed_child_custody_record>& expected)
{
    Admission_facts facts;
    std::shared_ptr<sintra::detail::Managed_child_custody_record> mapped;
    uint32_t mapped_occurrence = 0;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        const auto it = sintra::s_mproc->m_child_custody_by_process.find(process_iid);
        if (it != sintra::s_mproc->m_child_custody_by_process.end()) {
            mapped = it->second.custody.lock();
            mapped_occurrence = it->second.occurrence;
        }
    }
    facts.active_map_predecessor = mapped == expected && mapped_occurrence == 0;
    facts.record_found = static_cast<bool>(expected);
    if (!expected) {
        return facts;
    }

    std::lock_guard<std::mutex> lock(expected->mutex);
    facts.exact_one_occurrence = expected->occurrences.size() == 1;
    facts.exact_two_occurrences = expected->occurrences.size() == 2;
    if (expected->occurrences.empty()) {
        return facts;
    }
    const auto& predecessor = expected->occurrences[0];
    facts.predecessor_owned =
        predecessor.occurrence == 0 &&
        predecessor.setup ==
            sintra::detail::Managed_child_occurrence_record::setup_state::ownership_ready &&
        predecessor.native.created();
    facts.predecessor_exited = predecessor.native.exited();
    if (!facts.exact_two_occurrences) {
        return facts;
    }
    const auto& failed = expected->occurrences[1];
    facts.failed_occurrence_pending =
        failed.occurrence == 1 &&
        failed.process_instance_id == process_iid &&
        failed.setup ==
            sintra::detail::Managed_child_occurrence_record::setup_state::pending;
    facts.failed_occurrence_no_child =
        failed.native.confirm_absent();
    return facts;
}

int child_main(
    const sintra::test::Shared_directory& shared)
{
    sintra::enable_recovery();
    const auto occurrence = sintra::s_recovery_occurrence;
    const auto ledger_name = occurrence == 0 ? k_ledger_0 : k_ledger_1;
    if (!write_ledger(marker(shared.path(), ledger_name))) {
        return 3;
    }
    if (occurrence != 0) {
        return 4;
    }
    if (!sintra::test::wait_for_file(
            marker(shared.path(), k_crash_0), k_watchdog_timeout, 10ms))
    {
        return 5;
    }
    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash(
        "managed-child admission transaction occurrence 0");
    std::abort();
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_ADMISSION_TRANSACTION_DIR",
        "managed_child_admission_transaction");
    const bool is_child = sintra::test::has_argv_flag(argc, argv, k_child_flag);

    sintra::init(argc, argv);
    if (is_child) {
        const int result = child_main(shared);
        sintra::detail::finalize();
        return result;
    }

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&]() {
        const auto deadline = std::chrono::steady_clock::now() + k_watchdog_timeout;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                    "F05_ADMISSION_TRANSACTION_INVALID watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    std::atomic<bool> recovery_entered{false};
    std::atomic<bool> recovery_released{false};
    std::atomic<bool> recovery_complete{false};
    std::mutex crash_mutex;
    std::optional<sintra::Crash_info> crash_info;
    sintra::set_recovery_runner([&](
        const sintra::Crash_info& info,
        const sintra::Recovery_control& control)
    {
        Recovery_completion completion{recovery_complete};
        {
            std::lock_guard<std::mutex> lock(crash_mutex);
            crash_info = info;
        }
        recovery_entered.store(true, std::memory_order_release);
        while (!recovery_released.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        control.spawn();
    });

    const auto process_iid = sintra::make_process_instance_id();
    s_expected_iid.store(process_iid, std::memory_order_release);
    s_failure_remaining.store(1, std::memory_order_release);
    s_failure_hits.store(0, std::memory_order_release);
    sintra::test::managed_child::Scoped_test_hook failure_hook(
        sintra::detail::test_hooks::s_managed_child_failure,
        &inject_admission_mapping_failure);

    sintra::Spawn_options options;
    options.binary_path = fs::absolute(argv[0]).string();
    options.args = {std::string(k_child_flag)};
    options.process_instance_id = process_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    const bool ledger_seen = sintra::test::wait_for_file(
        marker(shared.path(), k_ledger_0), 10s, 10ms);
    const auto ledger = ledger_seen
        ? read_ledger(marker(shared.path(), k_ledger_0))
        : std::nullopt;
    const bool ledger_valid = ledger &&
        ledger->process_iid == process_iid && ledger->occurrence == 0 &&
        ledger->pid > 0 && ledger->start_stamp != 0 &&
        exact_process_is_live(ledger->pid, ledger->start_stamp);
    const bool crash_requested = ledger_valid && write_complete_file(
        marker(shared.path(), k_crash_0), "complete=1\n");
    const bool recovery_seen = wait_until([&]() {
        return recovery_entered.load(std::memory_order_acquire);
    }, 10s);

    bool exact_crash = false;
    {
        std::lock_guard<std::mutex> lock(crash_mutex);
        exact_crash = ledger && crash_info &&
            crash_info->process_iid == process_iid && crash_info->status != 0;
    }
    recovery_released.store(true, std::memory_order_release);
    const bool recovery_finished = wait_until([&]() {
        return recovery_complete.load(std::memory_order_acquire);
    }, 5s);
    failure_hook.restore();
    sintra::set_recovery_runner(sintra::Recovery_runner{});

    const bool predecessor_absent = ledger && wait_until([&]() {
        return !exact_process_is_live(ledger->pid, ledger->start_stamp);
    }, 5s);
    const bool replacement_marker_absent =
        !fs::exists(marker(shared.path(), k_ledger_1));

    const auto accepted_status = custody.status();
    std::shared_ptr<sintra::detail::Managed_child_custody_record> custody_record;
    {
        std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
        const auto active = sintra::s_mproc->m_child_custody_by_process.find(process_iid);
        if (active != sintra::s_mproc->m_child_custody_by_process.end()) {
            custody_record = active->second.custody.lock();
        }
    }
    bool predecessor_exit_fact_seen = false;
    if (predecessor_absent && custody_record) {
        std::unique_lock<std::mutex> lock(custody_record->mutex);
        predecessor_exit_fact_seen = custody_record->changed.wait_for(
            lock, 5s, [&]() {
                const auto predecessor = std::find_if(
                    custody_record->occurrences.begin(),
                    custody_record->occurrences.end(),
                    [&](const auto& occurrence) {
                        return occurrence.process_instance_id == process_iid &&
                            occurrence.occurrence == 0;
                    });
                return predecessor != custody_record->occurrences.end() &&
                    predecessor->native.exited();
            });
    }
    const auto facts = observe_admission_facts(process_iid, custody_record);

    const auto terminate_started = std::chrono::steady_clock::now();
    const auto terminated = custody.terminate_until(
        terminate_started + 400ms);
    const auto terminate_elapsed =
        std::chrono::steady_clock::now() - terminate_started;
    const bool terminate_bounded = terminate_elapsed < 2s;

    const auto finalize_started = std::chrono::steady_clock::now();
    const bool finalized = sintra::detail::finalize();
    const auto finalize_elapsed =
        std::chrono::steady_clock::now() - finalize_started;
    const bool finalize_bounded = finalize_elapsed < 2s;

    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    const unsigned hook_hits = s_failure_hits.load(std::memory_order_acquire);
    const bool red =
        hook_hits == 1 && accepted_status.accepted &&
        accepted_status.admitted_occurrences == 2 && ledger_valid &&
        crash_requested && recovery_seen && recovery_finished && exact_crash &&
        facts.record_found && facts.exact_two_occurrences &&
        facts.predecessor_owned && facts.predecessor_exited &&
        facts.failed_occurrence_pending && facts.failed_occurrence_no_child &&
        facts.active_map_predecessor && predecessor_absent &&
        replacement_marker_absent && terminated.release_requested &&
        !terminated.release_complete && terminate_bounded &&
        !finalized && finalize_bounded;

    const bool green =
        hook_hits == 1 && accepted_status.accepted &&
        accepted_status.admitted_occurrences == 1 && ledger_valid &&
        crash_requested && recovery_seen && recovery_finished && exact_crash &&
        facts.record_found && facts.exact_one_occurrence &&
        facts.predecessor_owned && facts.predecessor_exited &&
        facts.active_map_predecessor && predecessor_absent &&
        replacement_marker_absent && terminated.release_requested &&
        terminated.release_complete && terminate_bounded && finalized &&
        finalize_bounded;

    if (green) {
        std::fprintf(stdout,
            "F05_ADMISSION_TRANSACTION_GREEN hook_hits=1 accepted=1 admitted=1 "
            "occurrence_1_rolled_back=1 occurrence_1_os_child=0 "
            "terminate_bounded=1 terminate_complete=1 finalize_bounded=1 "
            "finalized=1 predecessor_absent=1 occurrence_1_marker=0 survivors=0\n");
        std::fflush(stdout);
        return 0;
    }

    if (red) {
        std::fprintf(stderr,
            "F05_ADMISSION_TRANSACTION_RED hook_hits=1 accepted=1 admitted=2 "
            "occurrence_1_pending=1 occurrence_1_os_child=0 active_occurrence=0 "
            "terminate_bounded=1 terminate_complete=0 finalize_bounded=1 "
            "finalized=0 predecessor_absent=1 occurrence_1_marker=0 survivors=0\n");
        std::fflush(stderr);
        return 2;
    }

    std::fprintf(stderr,
        "F05_ADMISSION_TRANSACTION_INVALID hook_hits=%u accepted=%d admitted=%zu "
        "ledger=%d crash_requested=%d recovery_seen=%d recovery_finished=%d "
        "exact_crash=%d record=%d one=%d two=%d predecessor_owned=%d predecessor_exited=%d "
        "exit_fact_seen=%d "
        "pending_1=%d no_child_1=%d active_0=%d predecessor_absent=%d marker_1_absent=%d "
        "release_requested=%d release_complete=%d terminate_bounded=%d finalized=%d "
        "finalize_bounded=%d\n",
        hook_hits,
        accepted_status.accepted ? 1 : 0,
        accepted_status.admitted_occurrences,
        ledger_valid ? 1 : 0,
        crash_requested ? 1 : 0,
        recovery_seen ? 1 : 0,
        recovery_finished ? 1 : 0,
        exact_crash ? 1 : 0,
        facts.record_found ? 1 : 0,
        facts.exact_one_occurrence ? 1 : 0,
        facts.exact_two_occurrences ? 1 : 0,
        facts.predecessor_owned ? 1 : 0,
        facts.predecessor_exited ? 1 : 0,
        predecessor_exit_fact_seen ? 1 : 0,
        facts.failed_occurrence_pending ? 1 : 0,
        facts.failed_occurrence_no_child ? 1 : 0,
        facts.active_map_predecessor ? 1 : 0,
        predecessor_absent ? 1 : 0,
        replacement_marker_absent ? 1 : 0,
        terminated.release_requested ? 1 : 0,
        terminated.release_complete ? 1 : 0,
        terminate_bounded ? 1 : 0,
        finalized ? 1 : 0,
        finalize_bounded ? 1 : 0);
    std::fflush(stderr);
    return 3;
}
