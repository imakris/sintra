//
// Exact managed-child detach is committed only by the one-byte lifeline
// message.  A definite non-delivery restores owner authority; a committed
// detach closes recovery and native observation without fabricating OS exit.
//

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::Managed_child_exit_capture;
using sintra::test::managed_child::Scoped_test_hook;
using sintra::test::managed_child::child_identity_t;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::wait_for_child_identity;
using sintra::test::managed_child::wait_for_exact_process_absence;
using sintra::test::managed_child::write_child_identity;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_failure_child_flag =
    "--detach-transaction-failure-child";
constexpr std::string_view k_success_child_flag =
    "--detach-transaction-success-child";
constexpr std::string_view k_failure_ready = "failure_ready.complete";
constexpr std::string_view k_success_ready = "success_ready.complete";
constexpr std::string_view k_child_detached = "child_detached.complete";
constexpr std::string_view k_child_leave = "child_leave.complete";
constexpr std::string_view k_child_dormant = "child_dormant.complete";
constexpr std::string_view k_child_finish = "child_finish.complete";
constexpr std::string_view k_child_finished = "child_finished.complete";
constexpr auto k_step_timeout = 10s;
constexpr auto k_child_timeout = 60s;
constexpr auto k_watchdog_timeout = 80s;
constexpr auto k_poll_interval = 10ms;
constexpr std::string_view k_failure_prefix =
    "MANAGED_CHILD_DETACH_TRANSACTION_INVALID ";

std::atomic<sintra::instance_id_type> s_failure_iid{
    sintra::invalid_instance_id};
std::atomic<sintra::instance_id_type> s_hook_iid{
    sintra::invalid_instance_id};
std::atomic<unsigned> s_before_commit{0};
std::atomic<unsigned> s_after_commit{0};

fs::path marker(const fs::path& directory, std::string_view name)
{
    return directory / std::string(name);
}

bool inject_detach_write_failure(
    const char* stage,
    sintra::instance_id_type process_iid,
    std::uint32_t occurrence) noexcept
{
    return stage && occurrence == 0 &&
        std::string_view(stage) ==
            sintra::detail::test_hooks::k_managed_child_fail_detach_write &&
        process_iid == s_failure_iid.load(std::memory_order_acquire);
}

void count_detach_stage(
    const char* stage,
    sintra::instance_id_type process_iid,
    std::uint32_t occurrence)
{
    if (!stage || occurrence != 0 ||
        process_iid != s_hook_iid.load(std::memory_order_acquire))
    {
        return;
    }
    if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_detach_before_commit)
    {
        s_before_commit.fetch_add(1, std::memory_order_release);
    }
    else if (std::string_view(stage) ==
        sintra::detail::test_hooks::k_managed_child_detach_after_commit)
    {
        s_after_commit.fetch_add(1, std::memory_order_release);
    }
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }
    return true;
}

std::shared_ptr<sintra::detail::Managed_child_custody_record>
custody_record_for(sintra::instance_id_type process_iid)
{
    std::lock_guard<std::mutex> lock(sintra::s_mproc->m_child_custody_mutex);
    const auto found =
        sintra::s_mproc->m_child_custody_by_process.find(process_iid);
    return found == sintra::s_mproc->m_child_custody_by_process.end()
        ? nullptr
        : found->second.custody.lock();
}

sintra::Managed_child_custody spawn_child(
    const std::string& binary_path,
    sintra::instance_id_type process_iid,
    std::string_view child_flag)
{
    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {std::string(child_flag)};
    options.process_instance_id = process_iid;
    options.lifetime.enable_lifeline = true;
    options.lifetime.hard_exit_timeout_ms = 100;
    options.readiness_instance_name =
        "managed_child_detach_target_that_never_publishes";
    return sintra::spawn_swarm_process(options);
}

int run_child(
    int argc,
    char* argv[],
    const fs::path& directory,
    bool success_child)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    sintra::Coordinator::rpc_mark_initialization_complete(
        sintra::s_coord_id, sintra::s_mproc_id);
    sintra::enable_recovery();
    const auto ready = success_child ? k_success_ready : k_failure_ready;
    if (!write_child_identity(marker(directory, ready))) {
        return 3;
    }

    if (!success_child) {
        std::this_thread::sleep_for(k_child_timeout);
        return 4;
    }

    const bool detached = wait_until([] {
        return sintra::s_mproc &&
            sintra::s_mproc->m_member_lifetime_role.load(
                std::memory_order_acquire) ==
                sintra::detail::Member_lifetime_role::DETACHED;
    }, k_step_timeout);
    if (!detached || !write_complete_file(
            marker(directory, k_child_detached), "detached=1\n") ||
        !sintra::test::wait_for_file(
            marker(directory, k_child_leave), k_child_timeout, k_poll_interval) ||
        !sintra::leave() || !write_complete_file(
            marker(directory, k_child_dormant), "dormant=1\n") ||
        !sintra::test::wait_for_file(
            marker(directory, k_child_finish), k_child_timeout, k_poll_interval))
    {
        return 5;
    }

    return write_complete_file(
        marker(directory, k_child_finished), "finished=1\n") ? 0 : 6;
}

bool check_failure_rollback(
    const std::string& binary_path,
    const fs::path& directory)
{
    const auto process_iid = sintra::make_process_instance_id();
    s_failure_iid.store(process_iid, std::memory_order_release);
    s_hook_iid.store(process_iid, std::memory_order_release);
    s_before_commit.store(0, std::memory_order_release);
    s_after_commit.store(0, std::memory_order_release);

    auto custody = spawn_child(binary_path, process_iid, k_failure_child_flag);
    const auto child = wait_for_child_identity(
        marker(directory, k_failure_ready), k_step_timeout, k_poll_interval);
    const auto record = custody_record_for(process_iid);
    Managed_child_exit_capture exit;
    auto observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exit.record(event); });
    const bool observation_registered = static_cast<bool>(observation);

    Scoped_test_hook failure_hook(
        sintra::detail::test_hooks::s_managed_child_failure,
        &inject_detach_write_failure);
    const auto result = record
        ? sintra::s_mproc->detach_child_custody_until(
            record, std::chrono::steady_clock::now() + k_step_timeout)
        : sintra::detail::Managed_child_detach_result::NOT_STARTED;
    failure_hook.restore();

    bool owner_bound = false;
    bool recipe_restored = false;
    bool active_provenance = false;
    bool recovery_restored = false;
    bool role_restored = false;
    bool readiness_preserved = false;
    if (record) {
        std::scoped_lock lock(
            record->mutex,
            sintra::s_mproc->m_cached_spawns_mutex,
            sintra::s_mproc->m_child_custody_mutex);
        const auto* exact = record->find_occurrence_locked(process_iid, 0);
        owner_bound = exact && !exact->native.detaching() &&
            !exact->native.disowned();
        const auto recipe = sintra::s_mproc->m_cached_spawns.find(process_iid);
        recipe_restored = recipe != sintra::s_mproc->m_cached_spawns.end() &&
            recipe->second.custody == record;
        const auto active =
            sintra::s_mproc->m_child_custodies.find(record->identity);
        active_provenance =
            active != sintra::s_mproc->m_child_custodies.end() &&
            active->second == record &&
            sintra::s_mproc->m_disowned_child_custodies.find(record->identity) ==
                sintra::s_mproc->m_disowned_child_custodies.end();
        recovery_restored = record->recovery_requested;
        readiness_preserved =
            record->readiness == sintra::detail::Readiness_phase::pending &&
            !record->readiness_cancelled.load(std::memory_order_acquire);
    }
    if (record) {
        std::lock_guard lock(sintra::s_coord->m_publish_mutex);
        const auto role = sintra::s_coord->m_member_roles.find(process_iid);
        role_restored = role != sintra::s_coord->m_member_roles.end() &&
            role->second.identity.matches(record->identity, process_iid, 0) &&
            role->second.role ==
                sintra::detail::Member_lifetime_role::COORDINATOR_BOUND;
    }

    const bool child_survived = child &&
        exact_process_is_live(child->pid, child->start_stamp);
    const auto release = custody.terminate_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    const bool child_absent = child && wait_for_exact_process_absence(
        *child, k_step_timeout, k_poll_interval);
    const bool real_exit = exit.wait_for_one(k_step_timeout);
    const auto exit_state = exit.snapshot();
    observation.subscription.unsubscribe();

    return child && record && observation_registered &&
        result == sintra::detail::Managed_child_detach_result::
            DEFINITE_NON_DELIVERY &&
        s_before_commit.load(std::memory_order_acquire) == 1 &&
        s_after_commit.load(std::memory_order_acquire) == 0 &&
        owner_bound && recipe_restored && active_provenance && recovery_restored &&
        role_restored && readiness_preserved && child_survived && child_absent &&
        real_exit &&
        exit_state.deliveries == 1 && exit_state.event &&
        exit_state.event->status_kind !=
            sintra::Managed_child_exit_status_kind::observation_ended_by_detach &&
        release.release_state == sintra::Managed_child_release_state::complete;
}

bool check_committed_detach(
    const std::string& binary_path,
    const fs::path& directory,
    bool& owner_finalized)
{
    const auto process_iid = sintra::make_process_instance_id();
    s_failure_iid.store(sintra::invalid_instance_id, std::memory_order_release);
    s_hook_iid.store(process_iid, std::memory_order_release);
    s_before_commit.store(0, std::memory_order_release);
    s_after_commit.store(0, std::memory_order_release);

    auto custody = spawn_child(binary_path, process_iid, k_success_child_flag);
    const auto child = wait_for_child_identity(
        marker(directory, k_success_ready), k_step_timeout, k_poll_interval);
    const auto record = custody_record_for(process_iid);
    Managed_child_exit_capture exit;
    auto observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exit.record(event); });
    const bool observation_registered = static_cast<bool>(observation);

    const auto result = record
        ? sintra::s_mproc->detach_child_custody_until(
            record, std::chrono::steady_clock::now() + k_step_timeout)
        : sintra::detail::Managed_child_detach_result::NOT_STARTED;
    const auto duplicate = record
        ? sintra::s_mproc->detach_child_custody_until(
            record, std::chrono::steady_clock::now() + k_step_timeout)
        : sintra::detail::Managed_child_detach_result::NOT_STARTED;
    const bool detach_exit = exit.wait_for_one(k_step_timeout);
    const auto exit_state = exit.snapshot();
    const auto status = custody.status();

    bool custody_facts = false;
    if (record) {
        std::scoped_lock lock(
            record->mutex,
            sintra::s_mproc->m_cached_spawns_mutex,
            sintra::s_mproc->m_child_custody_mutex);
        const auto* exact = record->find_occurrence_locked(process_iid, 0);
        const auto disowned =
            sintra::s_mproc->m_disowned_child_custodies.find(record->identity);
        custody_facts = exact && exact->native.disowned() &&
            !record->recovery_requested &&
            sintra::s_mproc->m_cached_spawns.find(process_iid) ==
                sintra::s_mproc->m_cached_spawns.end() &&
            sintra::s_mproc->m_child_custodies.find(record->identity) ==
                sintra::s_mproc->m_child_custodies.end() &&
            disowned != sintra::s_mproc->m_disowned_child_custodies.end() &&
            disowned->second == record;
#ifdef _WIN32
        custody_facts = custody_facts &&
            !exact->native.process_handle_owned() &&
            !exact->native.exit_observer_registered() &&
            exact->native.exit_observer_cancellation_handle() == 0;
#endif
    }
    bool lifecycle_workers_settled = false;
    {
        std::lock_guard<std::mutex> lock(
            sintra::s_mproc->m_owned_lifecycle_workers_mutex);
        lifecycle_workers_settled =
            sintra::s_mproc->m_owned_lifecycle_workers.empty();
    }

    const bool child_observed = sintra::test::wait_for_file(
        marker(directory, k_child_detached), k_step_timeout, k_poll_interval);
    const bool leave_sent = write_complete_file(
        marker(directory, k_child_leave), "leave=1\n");
    const bool child_dormant = sintra::test::wait_for_file(
        marker(directory, k_child_dormant), k_step_timeout, k_poll_interval);
    const bool child_survived = child &&
        exact_process_is_live(child->pid, child->start_stamp);
    owner_finalized = sintra::detail::finalize();
    const bool finish_sent = write_complete_file(
        marker(directory, k_child_finish), "finish=1\n");
    const bool child_finished = sintra::test::wait_for_file(
        marker(directory, k_child_finished), k_step_timeout, k_poll_interval);
    const bool child_absent = child && wait_for_exact_process_absence(
        *child, k_step_timeout, k_poll_interval);
#ifndef _WIN32
    const bool reap_retired = child && wait_until([&] {
        std::lock_guard<std::mutex> lock(
            sintra::detail::detached_child_reap_mutex());
        const auto& roster = sintra::detail::detached_child_reap_roster();
        return std::none_of(roster.begin(), roster.end(), [&](const auto& slot) {
            return slot.pid == child->pid &&
                slot.start_stamp == child->start_stamp;
        });
    }, k_step_timeout);
#else
    const bool reap_retired = true;
#endif
    observation.subscription.unsubscribe();

    const bool valid = child && record && observation_registered &&
        result == sintra::detail::Managed_child_detach_result::DISOWNED &&
        duplicate == sintra::detail::Managed_child_detach_result::DISOWNED &&
        s_before_commit.load(std::memory_order_acquire) == 1 &&
        s_after_commit.load(std::memory_order_acquire) == 1 &&
        detach_exit && exit_state.deliveries == 1 && exit_state.event &&
        exit_state.event->status_kind ==
            sintra::Managed_child_exit_status_kind::observation_ended_by_detach &&
        observation.occurrence == exit_state.event->occurrence &&
        status.created_occurrences == 1 && status.exited_occurrences == 0 &&
        status.readiness_state ==
            sintra::Managed_child_readiness_state::observation_stopped &&
        custody_facts && lifecycle_workers_settled && child_observed &&
        leave_sent && child_dormant && child_survived && owner_finalized &&
        finish_sent && child_finished && child_absent && reap_retired;
    if (!valid) {
        std::fprintf(stderr,
            "detach_commit result=%d duplicate=%d pre=%u post=%u exit=%d/%u status=%zu/%zu/%d facts=%d workers=%d observed=%d leave=%d dormant=%d live=%d final=%d finish=%d/%d absent=%d reap=%d\n",
            static_cast<int>(result), static_cast<int>(duplicate),
            s_before_commit.load(), s_after_commit.load(),
            detach_exit ? 1 : 0, exit_state.deliveries,
            status.created_occurrences, status.exited_occurrences,
            static_cast<int>(status.readiness_state),
            custody_facts ? 1 : 0, lifecycle_workers_settled ? 1 : 0,
            child_observed ? 1 : 0, leave_sent ? 1 : 0,
            child_dormant ? 1 : 0, child_survived ? 1 : 0,
            owner_finalized ? 1 : 0, finish_sent ? 1 : 0,
            child_finished ? 1 : 0, child_absent ? 1 : 0,
            reap_retired ? 1 : 0);
    }
    return valid;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_DETACH_TRANSACTION_DIR",
        "managed_child_detach_transaction");
    const bool failure_child =
        sintra::test::has_argv_flag(argc, argv, k_failure_child_flag);
    const bool success_child =
        sintra::test::has_argv_flag(argc, argv, k_success_child_flag);
    if (failure_child || success_child) {
        return run_child(argc, argv, shared.path(), success_child);
    }

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

    std::atomic<bool> watchdog_done{false};
    std::thread watchdog([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + k_watchdog_timeout;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                    "MANAGED_CHILD_DETACH_TRANSACTION_INVALID watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    Scoped_test_hook cleanup_hook(
        sintra::detail::test_hooks::s_managed_child_cleanup,
        &count_detach_stage);
    const bool rollback_valid =
        check_failure_rollback(binary_path, shared.path());
    bool finalized = false;
    const bool commit_valid =
        check_committed_detach(binary_path, shared.path(), finalized);
    cleanup_hook.restore();

    if (!finalized && sintra::s_mproc) {
        finalized = sintra::detail::finalize();
    }
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    if (!rollback_valid || !commit_valid || !finalized) {
        std::fprintf(stderr,
            "%.*srollback=%d commit=%d finalized=%d\n",
            static_cast<int>(k_failure_prefix.size()),
            k_failure_prefix.data(),
            rollback_valid ? 1 : 0,
            commit_valid ? 1 : 0,
            finalized ? 1 : 0);
        return 1;
    }
    return 0;
}
