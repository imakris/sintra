//
// A crash fact belongs to one exact managed-child occurrence. Reusing its
// process-instance id must not let a delayed predecessor crash poison the
// replacement's lifecycle, publication, or recovery decision.
//

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::current_child_identity;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::Managed_child_exit_capture;
using sintra::test::managed_child::wait_for_exact_process_absence;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_a_flag   = "--crash-provenance-child-a";
constexpr std::string_view k_child_b_flag   = "--crash-provenance-child-b";
constexpr std::string_view k_child_a_marker = "child_a.complete";
constexpr std::string_view k_child_b_marker = "child_b.complete";
constexpr std::string_view k_child_b_recovery_marker =
    "child_b_recovery.complete";
constexpr std::string_view k_child_a_crash  = "child_a_crash.complete";
constexpr std::string_view k_child_b_finish = "child_b_finish.complete";
constexpr auto             k_step_timeout   = 8s;
constexpr auto             k_child_timeout  = 50s;
// The predecessor can serialize six step bounds (48s), and the replacement
// can serialize nine (72s). The remaining 30s cover startup and fences.
constexpr auto k_watchdog_timeout = 150s;
constexpr auto k_poll_interval    = 10ms;
constexpr int  k_stale_status     = 173;

constexpr sintra::instance_id_type k_reused_process_iid =
    sintra::compose_instance(37u, 1u);

struct Witness : sintra::Derived_transceiver<Witness>
{};

struct witnessed_child_identity_t
{
    sintra::instance_id_type   process_iid = sintra::invalid_instance_id;
    std::uint32_t              occurrence  = 0;
    int                        pid         = -1;
    std::uint64_t              start_stamp = 0;
    sintra::instance_id_type   witness_iid = sintra::invalid_instance_id;
};

struct relay_fact_t
{
    sintra::instance_id_type   sender           = sintra::invalid_instance_id;
    int                        status           = 0;
    std::uint64_t              custody_identity = 0;
    std::uint32_t              occurrence       = 0;
    bool                       tls_matches      = false;
};

class Relay_log
{
public:
    void record(const sintra::Managed_process::terminated_abnormally& message)
    {
        relay_fact_t fact;
        fact.sender           = message.sender_instance_id;
        fact.status           = message.status;
        fact.custody_identity = message.managed_child_custody_identity;
        fact.occurrence       = message.managed_child_occurrence;
        fact.tls_matches      = sintra::s_tl_current_message == &message;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_facts.push_back(fact);
        }
        m_changed.notify_all();
    }

    bool wait_for_size(std::size_t size)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(
            lock, k_step_timeout, [&] { return m_facts.size() >= size; });
    }

    std::vector<relay_fact_t> snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_facts;
    }

private:
    mutable std::mutex         m_mutex;
    std::condition_variable    m_changed;
    std::vector<relay_fact_t>  m_facts;
};

class Lifecycle_log
{
public:
    void record(const sintra::process_lifecycle_event& event)
    {
        if (event.process_iid != k_reused_process_iid) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(event);
        }
    }

    std::size_t count(sintra::process_lifecycle_event::reason reason) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return count_reason_locked(reason);
    }

    bool has_status(
        sintra::process_lifecycle_event::reason    reason,
        int                                        status) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& event : m_events) {
            if (event.why == reason && event.status == status) {
                return true;
            }
        }
        return false;
    }

private:
    std::size_t count_reason_locked(
        sintra::process_lifecycle_event::reason reason) const
    {
        std::size_t count = 0;
        for (const auto& event : m_events) {
            count += event.why == reason ? 1u : 0u;
        }
        return count;
    }

    mutable std::mutex m_mutex;
    std::vector<sintra::process_lifecycle_event> m_events;
};

fs::path marker(const fs::path& directory, std::string_view name)
{
    return directory / std::string(name);
}

bool write_identity(
    const fs::path&            path,
    sintra::instance_id_type   witness_iid)
{
    const auto identity = current_child_identity();
    if (!identity) {
        return false;
    }
    std::ostringstream contents;
    sintra::test::managed_child::write_child_identity(contents, *identity);
    contents << ' ' << static_cast<unsigned long long>(witness_iid) << '\n';
    return write_complete_file(path, contents.str());
}

std::optional<witnessed_child_identity_t> read_identity(const fs::path& path)
{
    witnessed_child_identity_t identity;
    unsigned long long witness_iid = 0;
    std::ifstream input(path, std::ios::binary);
    const auto base = sintra::test::managed_child::read_child_identity(input);
    if (!base || !(input >> witness_iid)) {
        return std::nullopt;
    }
    identity.process_iid = base->process_iid;
    identity.occurrence  = base->occurrence;
    identity.pid         = base->pid;
    identity.start_stamp = base->start_stamp;
    identity.witness_iid =
        static_cast<sintra::instance_id_type>(witness_iid);
    return identity;
}

std::optional<witnessed_child_identity_t> wait_for_identity(const fs::path& path)
{
    if (!sintra::test::wait_for_file(path, k_step_timeout, k_poll_interval)) {
        return std::nullopt;
    }
    auto identity = read_identity(path);
    if (!identity || identity->process_iid != k_reused_process_iid ||
        identity->pid <= 0 || identity->start_stamp == 0 ||
        !exact_process_is_live(identity->pid, identity->start_stamp))
    {
        return std::nullopt;
    }
    return identity;
}

bool wait_for_absence(const std::optional<witnessed_child_identity_t>& identity)
{
    return
        identity &&
        wait_for_exact_process_absence(identity->pid, identity->start_stamp, k_step_timeout, k_poll_interval);
}

sintra::instance_id_type resolve_name(const std::string& name)
{
    try {
        return sintra::Coordinator::rpc_resolve_instance(
            sintra::s_coord_id, name);
    }
    catch (...) {
        return sintra::invalid_instance_id;
    }
}

sintra::Managed_child_custody spawn_child(
    const std::string& binary_path,
    std::string_view   flag)
{
    sintra::Spawn_options options;
    options.binary_path              = binary_path;
    options.args                     = {std::string(flag)};
    options.process_instance_id      = k_reused_process_iid;
    options.lifetime.enable_lifeline = false;
    return sintra::spawn_swarm_process(options);
}

void inject_stale_crash(
    const sintra::Managed_child_occurrence_identity& identity)
{
    sintra::Managed_process::terminated_abnormally stale(k_stale_status);
    stale.sender_instance_id   = k_reused_process_iid;
    stale.receiver_instance_id = sintra::any_remote;
    sintra::s_mproc->m_out_req_c->relay(
        stale, identity.custody_identity, identity.occurrence);
}

int run_child(
    int                argc,
    char*              argv[],
    const fs::path&    shared_path,
    const std::string& witness_name)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    if (sintra::test::has_argv_flag(argc, argv, k_child_a_flag)) {
        sintra::enable_recovery();
        if (!write_identity( marker(shared_path, k_child_a_marker), sintra::invalid_instance_id) ||
            !sintra::test::wait_for_file(
                marker(shared_path, k_child_a_crash), k_child_timeout, k_poll_interval))
        {
            return 3;
        }
        sintra::disable_debug_pause_for_current_process();
        sintra::test::prepare_for_intentional_crash(
            "managed-child crash provenance predecessor");
        std::abort();
    }

    if (!sintra::test::has_argv_flag(argc, argv, k_child_b_flag)) {
        return 4;
    }
    sintra::enable_recovery();
    bool child_valid = true;
    if (sintra::s_recovery_occurrence == 0) {
        Witness witness;
        child_valid = witness.assign_name(witness_name) &&
            write_identity(
                marker(shared_path, k_child_b_marker),
                witness.instance_id()) &&
            sintra::test::wait_for_file(
                marker(shared_path, k_child_b_finish),
                k_child_timeout,
                k_poll_interval);
    }
    else {
        child_valid = sintra::s_recovery_occurrence == 1 &&
            write_identity(
                marker(shared_path, k_child_b_recovery_marker),
                sintra::invalid_instance_id) &&
            sintra::test::wait_for_file(
                marker(shared_path, k_child_b_finish),
                k_child_timeout,
                k_poll_interval);
    }
    return child_valid && sintra::detail::finalize() ? 0 : 5;
}

int run_root(int argc, char* argv[], const fs::path& shared_path)
{
    const std::string binary_path =
        sintra::test::get_binary_path(argc, argv);
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
                    "CRASH_PROVENANCE_REUSE_INVALID watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    Relay_log relay_log;
    Lifecycle_log lifecycle_log;
    std::atomic<unsigned> recovery_policy_calls{0};
    std::atomic<unsigned> recovery_runner_calls{0};
    std::mutex              recovery_runner_mutex;
    std::condition_variable recovery_runner_changed;
    bool recovery_runner_complete = false;
    auto deactivate_relay = sintra::s_mproc->activate<sintra::Managed_process>(
        [&](const sintra::Managed_process::terminated_abnormally& message) {
            relay_log.record(message);
        },
        sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));
    sintra::set_lifecycle_handler(
        [&](const sintra::process_lifecycle_event& event) {
            lifecycle_log.record(event);
        });
    sintra::set_recovery_policy([&](const sintra::Crash_info&) {
        return recovery_policy_calls.fetch_add(1, std::memory_order_acq_rel) != 0;
    });
    sintra::set_recovery_runner([&](
        const sintra::Crash_info&,
        const sintra::Recovery_control& control)
    {
        recovery_runner_calls.fetch_add(1, std::memory_order_release);
        control.spawn();
        {
            std::lock_guard<std::mutex> lock(recovery_runner_mutex);
            recovery_runner_complete = true;
        }
        recovery_runner_changed.notify_all();
    });

    const std::string witness_name =
        "managed_child_crash_provenance_replacement_" +
        std::to_string(static_cast<unsigned long long>(k_reused_process_iid));

    auto custody_a = spawn_child(binary_path, k_child_a_flag);
    const auto child_a = wait_for_identity(
        marker(shared_path, k_child_a_marker));
    Managed_child_exit_capture exit_a;
    auto observation_a = custody_a.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exit_a.record(event); });
    const bool a_identity_exact = child_a && observation_a &&
        observation_a.occurrence.custody_identity    != 0                    &&
        observation_a.occurrence.process_instance_id == k_reused_process_iid &&
        observation_a.occurrence.occurrence          == child_a->occurrence;
    const bool a_crash_requested = a_identity_exact && write_complete_file(
        marker(shared_path, k_child_a_crash), "complete=1\n");
    const bool real_relay_fenced = relay_log.wait_for_size(1);
    if (real_relay_fenced) {
        sintra::s_mproc->wait_for_delivery_fence();
    }
    const std::size_t crash_count_after_real = lifecycle_log.count(
        sintra::process_lifecycle_event::reason::crash);
    const bool a_crash_lifecycle = crash_count_after_real == 1;
    const bool a_exit_observed   = exit_a.wait_for_one(k_step_timeout);
    const bool a_absent          = wait_for_absence(child_a);
    auto       a_release         = custody_a.release_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    if (a_release.release_state != sintra::Managed_child_release_state::complete) {
        a_release = custody_a.terminate_until(
            std::chrono::steady_clock::now() + k_step_timeout);
    }

    const auto relays_after_a = relay_log.snapshot();
    const bool real_relay_body_valid = relays_after_a.size() == 1 &&
        relays_after_a[0].sender == k_reused_process_iid &&
        relays_after_a[0].status != 0 && relays_after_a[0].tls_matches;
    const bool real_relay_exact = real_relay_body_valid &&
        relays_after_a[0].custody_identity ==
            observation_a.occurrence.custody_identity &&
        relays_after_a[0].occurrence == observation_a.occurrence.occurrence;
    const bool a_baseline = a_identity_exact && a_crash_requested &&
        real_relay_fenced && real_relay_body_valid && a_crash_lifecycle &&
        recovery_policy_calls.load(std::memory_order_acquire) == 1 &&
        a_exit_observed && exit_a.exact(observation_a.occurrence) && a_absent &&
        a_release.release_state == sintra::Managed_child_release_state::complete;

    auto custody_b = spawn_child(binary_path, k_child_b_flag);
    const auto child_b = wait_for_identity(
        marker(shared_path, k_child_b_marker));
    Managed_child_exit_capture exit_b;
    auto observation_b = custody_b.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) { exit_b.record(event); });
    const bool b_identity_exact = child_b && observation_b &&
        observation_b.occurrence.custody_identity != 0 &&
        observation_b.occurrence.custody_identity !=
            observation_a.occurrence.custody_identity &&
        observation_b.occurrence.process_instance_id == k_reused_process_iid &&
        observation_b.occurrence.occurrence == child_b->occurrence;
    const bool b_witness_before = child_b &&
        child_b->witness_iid != sintra::invalid_instance_id &&
        resolve_name(witness_name) == child_b->witness_iid;

    if (a_identity_exact) {
        inject_stale_crash(observation_a.occurrence);
    }
    const bool stale_relay_fenced = relay_log.wait_for_size(2);
    if (stale_relay_fenced) {
        sintra::s_mproc->wait_for_delivery_fence();
    }
    const auto relays_after_stale = relay_log.snapshot();
    const bool stale_relay_exact = relays_after_stale.size() == 2 &&
        relays_after_stale[1].sender == k_reused_process_iid &&
        relays_after_stale[1].status == k_stale_status &&
        relays_after_stale[1].custody_identity ==
            observation_a.occurrence.custody_identity &&
        relays_after_stale[1].occurrence ==
            observation_a.occurrence.occurrence &&
        relays_after_stale[1].tls_matches;
    const std::size_t crash_count_after_stale = lifecycle_log.count(
        sintra::process_lifecycle_event::reason::crash);
    const unsigned recovery_count_after_stale =
        recovery_policy_calls.load(std::memory_order_acquire);
    bool stale_recovery_fenced = recovery_count_after_stale == 1;
    if (recovery_count_after_stale > 1) {
        std::unique_lock<std::mutex> lock(recovery_runner_mutex);
        stale_recovery_fenced = recovery_runner_changed.wait_for(
            lock,
            k_step_timeout,
            [&] { return recovery_runner_complete; });
    }
    const auto b_status_after_stale = custody_b.status();
    std::optional<witnessed_child_identity_t> child_b_recovery;
    if (b_status_after_stale.created_occurrences > 1) {
        child_b_recovery = wait_for_identity(
            marker(shared_path, k_child_b_recovery_marker));
    }
    const bool b_has_recovery_occurrence =
        b_status_after_stale.admitted_occurrences > 1 ||
        b_status_after_stale.created_occurrences > 1;
    const bool b_recovery_identity_valid =
        b_status_after_stale.created_occurrences <= 1 ||
        (child_b_recovery && child_b_recovery->occurrence == 1);
    const bool b_live_after_stale = child_b && exact_process_is_live(
        child_b->pid, child_b->start_stamp);
    const bool b_witness_after = child_b &&
        resolve_name(witness_name) == child_b->witness_iid;

    const bool b_finish_requested = write_complete_file(
        marker(shared_path, k_child_b_finish), "complete=1\n");
    const bool b_exit_observed = exit_b.wait_for_one(k_step_timeout);
    const bool b_absent        = wait_for_absence(child_b);
    const bool b_recovery_absent = !child_b_recovery ||
        wait_for_absence(child_b_recovery);
    if (b_exit_observed && b_absent && b_recovery_absent) {
        sintra::s_mproc->wait_for_delivery_fence();
        sintra::s_mproc->wait_for_delivery_fence();
    }
    const std::size_t normal_count_after_b = lifecycle_log.count(
        sintra::process_lifecycle_event::reason::normal_exit);
    const bool b_normal_lifecycle = normal_count_after_b == 1;
    auto b_release = custody_b.release_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    if (b_release.release_state != sintra::Managed_child_release_state::complete) {
        b_release = custody_b.terminate_until(
            std::chrono::steady_clock::now() + k_step_timeout);
    }

    observation_a.subscription.unsubscribe();
    observation_b.subscription.unsubscribe();
    deactivate_relay();
    sintra::set_recovery_runner(sintra::Recovery_runner{});
    sintra::set_recovery_policy(sintra::Recovery_policy{});
    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    const bool finalized = sintra::shutdown();
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    const std::size_t final_crash_count = lifecycle_log.count(
        sintra::process_lifecycle_event::reason::crash);
    const std::size_t final_normal_count = lifecycle_log.count(
        sintra::process_lifecycle_event::reason::normal_exit);
    const bool releases_complete =
        a_release.release_state == sintra::Managed_child_release_state::complete &&
        b_release.release_state == sintra::Managed_child_release_state::complete;
    const bool survivors_absent = a_absent && b_absent && b_recovery_absent &&
        child_a && child_b &&
        !exact_process_is_live(child_a->pid, child_a->start_stamp) &&
        !exact_process_is_live(child_b->pid, child_b->start_stamp) &&
        (!child_b_recovery || !exact_process_is_live(
            child_b_recovery->pid, child_b_recovery->start_stamp));
    const bool setup_valid = a_baseline && custody_b && b_identity_exact &&
        b_witness_before && stale_relay_fenced && stale_relay_exact &&
        stale_recovery_fenced && b_recovery_identity_valid &&
        b_finish_requested && b_exit_observed &&
        exit_b.exact(observation_b.occurrence) && exit_b.normal_zero() &&
        releases_complete && survivors_absent && finalized;

    const bool fixed_green = setup_valid && real_relay_exact &&
        child_b->occurrence == 0 && b_status_after_stale.admitted_occurrences == 1 &&
        b_status_after_stale.created_occurrences == 1 &&
        b_status_after_stale.exited_occurrences == 0 &&
        b_live_after_stale && b_witness_after && crash_count_after_stale == 1 &&
        recovery_count_after_stale == 1 &&
        recovery_runner_calls.load(std::memory_order_acquire) == 0 &&
        !b_has_recovery_occurrence && !child_b_recovery && b_normal_lifecycle &&
        final_crash_count == 1 && final_normal_count == 1 &&
        lifecycle_log.has_status(
            sintra::process_lifecycle_event::reason::normal_exit, 0);

    if (fixed_green) {
        std::fprintf(stdout,
            "CRASH_PROVENANCE_REUSE_GREEN real_stamp=1 a_custody=%llu "
            "a_occurrence=%u b_custody=%llu b_occurrence=0 stale_crashes=0 "
            "stale_recovery=0 stale_recipe=0 b_recovery=0 b_live=1 "
            "b_witness=1 b_normal=1 survivors=0\n",
            static_cast<unsigned long long>(
                observation_a.occurrence.custody_identity),
            observation_a.occurrence.occurrence,
            static_cast<unsigned long long>(
                observation_b.occurrence.custody_identity));
        std::fflush(stdout);
        return 0;
    }

    const bool current_red = setup_valid && !real_relay_exact &&
        relays_after_a[0].custody_identity == 0 &&
        child_b->occurrence == 0 &&
        b_status_after_stale.admitted_occurrences == 2 &&
        b_status_after_stale.created_occurrences ==
            (child_b_recovery ? 2u : 1u) &&
        b_status_after_stale.exited_occurrences == 0 &&
        b_live_after_stale && b_witness_after && crash_count_after_stale == 2 &&
        recovery_count_after_stale == 2 &&
        recovery_runner_calls.load(std::memory_order_acquire) == 1 &&
        (child_b_recovery ? b_normal_lifecycle : !b_normal_lifecycle) &&
        final_crash_count == 2 &&
        final_normal_count == (child_b_recovery ? 1u : 0u);

    if (current_red) {
        std::fprintf(stderr,
            "CRASH_PROVENANCE_REUSE_RED real_stamp=0 a_custody=%llu "
            "a_occurrence=%u b_custody=%llu b_occurrence=%u stale_crashes=1 "
            "stale_recovery=1 stale_recipe=1 b_admitted=2 b_created=%zu "
            "b_live=1 b_witness=1 b_normal=%zu survivors=0\n",
            static_cast<unsigned long long>(
                observation_a.occurrence.custody_identity),
            observation_a.occurrence.occurrence,
            static_cast<unsigned long long>(
                observation_b.occurrence.custody_identity),
            child_b->occurrence,
            b_status_after_stale.created_occurrences,
            final_normal_count);
        std::fflush(stderr);
        return 2;
    }

    std::fprintf(stderr,
        "CRASH_PROVENANCE_REUSE_INVALID setup=%d a_baseline=%d real_exact=%d "
        "real_relays=%zu b_identity=%d b_occurrence=%u b_live=%d b_witness=%d "
        "stale_fenced=%d stale_exact=%d stale_crashes=%zu stale_recovery=%u "
        "recipe_fenced=%d runner_calls=%u b_admitted=%zu b_created=%zu "
        "b_recovery=%d "
        "b_exit=%d b_normal=%d final_crashes=%zu final_normals=%zu "
        "releases=%d survivors=%d finalized=%d\n",
        setup_valid ? 1 : 0,
        a_baseline ? 1 : 0,
        real_relay_exact ? 1 : 0,
        relays_after_a.size(),
        b_identity_exact ? 1 : 0,
        child_b ? child_b->occurrence : 0,
        b_live_after_stale ? 1 : 0,
        b_witness_after ? 1 : 0,
        stale_relay_fenced ? 1 : 0,
        stale_relay_exact ? 1 : 0,
        crash_count_after_stale,
        recovery_count_after_stale,
        stale_recovery_fenced ? 1 : 0,
        recovery_runner_calls.load(std::memory_order_acquire),
        b_status_after_stale.admitted_occurrences,
        b_status_after_stale.created_occurrences,
        child_b_recovery ? 1 : 0,
        b_exit_observed ? 1 : 0,
        b_normal_lifecycle ? 1 : 0,
        final_crash_count,
        final_normal_count,
        releases_complete ? 1 : 0,
        survivors_absent ? 1 : 0,
        finalized ? 1 : 0);
    std::fflush(stderr);
    return 3;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_CRASH_PROVENANCE_REUSE_DIR",
        "managed_child_crash_provenance_reuse");
    const std::string witness_name =
        "managed_child_crash_provenance_replacement_" +
        std::to_string(static_cast<unsigned long long>(k_reused_process_iid));
    if (sintra::test::has_argv_flag(argc, argv, k_child_a_flag) ||
        sintra::test::has_argv_flag(argc, argv, k_child_b_flag))
    {
        return run_child(argc, argv, shared.path(), witness_name);
    }
    return run_root(argc, argv, shared.path());
}
