// Managed-child unpublish invokes lifecycle and recovery callbacks. Each
// callback must be able to finish a public coordinator API call before it
// returns; eventual completion after callback return still exposes the lock.

#include <sintra/sintra.h>
#include <sintra/detail/runtime.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::child_identity_t;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::Managed_child_exit_capture;
using sintra::test::managed_child::write_complete_file;

constexpr std::string_view k_child_flag =
    "--recovery-unpublish-callback-lock-child";
constexpr std::string_view k_directory_arg  = "--recovery-unpublish-directory";
constexpr std::string_view k_go_marker      = "occurrence_0.go";
constexpr std::string_view k_release_marker = "occurrence_1.release";

constexpr auto k_callback_stage_timeout      = 2s;
constexpr auto k_callback_completion_timeout = 2s;
constexpr auto k_scenario_timeout            = 9s;
constexpr auto k_child_timeout               = 20s;
constexpr auto k_watchdog_timeout            = 25s;
constexpr auto k_poll_interval               = 10ms;

constexpr const char* k_failure_prefix =
    "recovery_unpublish_deadlock_test: ";

enum class Probed_callback
{
    LIFECYCLE,
    RECOVERY,
};

struct scenario_spec_t
{
    Probed_callback            callback;
    sintra::instance_id_type   child_iid;
    sintra::instance_id_type   probe_iid;
};

class Public_api_probe
{
public:
    explicit Public_api_probe(sintra::instance_id_type process_iid)
    :
        m_process_iid(process_iid)
    {}

    bool run_inside_callback()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_callback_running = true;
        }

        try {
            m_worker = std::thread(
                [
                    this
                ]()
                {
                    s_active_probe = this;
                    sintra::External_process_invitation invitation;
                    bool threw = false;
                    try {
                        sintra::External_process_invitation_options options;
                        options.process_instance_id = m_process_iid;
                        options.timeout             = k_child_timeout;
                        invitation =
                            sintra::create_external_process_invitation(options);
                    }
                    catch (...) {
                        threw = true;
                    }
                    s_active_probe = nullptr;

                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_invitation               = std::move(invitation);
                        m_threw                    = threw;
                        m_completed                = true;
                        m_completed_while_callback = m_callback_running;
                    }
                    m_changed.notify_all();
                });
        }
        catch (...) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_threw     = true;
                m_completed = true;
            }
            m_changed.notify_all();
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        const bool entered = m_changed.wait_for(
            lock,
            k_callback_stage_timeout,
            [this] { return m_coordinator_entered; });
        const bool completed = m_changed.wait_for(
            lock,
            k_callback_completion_timeout,
            [this] { return m_completed; });

        m_callback_running = false;
        m_callback_observed_entry =
            entered && m_coordinator_entered_while_callback;
        m_callback_observed_completion =
            completed && m_completed_while_callback;
        lock.unlock();
        m_changed.notify_all();
        return
            m_callback_observed_entry &&
            m_callback_observed_completion;
    }

    bool wait_until_completed(
        std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_until(lock, deadline, [this] { return m_completed; });
    }

    void join()
    {
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    bool invitation_is_valid() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<bool>(m_invitation);
    }

    bool cancel_invitation()
    {
        sintra::External_process_invitation invitation;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            invitation = m_invitation;
        }
        return sintra::cancel_external_process_invitation(invitation);
    }

    bool callback_observed_entry() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_callback_observed_entry;
    }

    bool callback_observed_completion() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_callback_observed_completion;
    }

    bool threw() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_threw;
    }

    static void coordinator_lock_stage(const char* stage)
    {
        if (!s_active_probe ||
            std::string_view(stage) != sintra::detail::test_hooks::
                k_stage_reserve_external_invitation_entered)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(s_active_probe->m_mutex);
            s_active_probe->m_coordinator_entered = true;
            s_active_probe->m_coordinator_entered_while_callback =
                s_active_probe->m_callback_running;
        }
        s_active_probe->m_changed.notify_all();
    }

private:
    static thread_local Public_api_probe* s_active_probe;

    sintra::instance_id_type   m_process_iid;
    mutable std::mutex         m_mutex;
    std::condition_variable    m_changed;
    std::thread                m_worker;
    sintra::External_process_invitation
                               m_invitation;
    bool                       m_callback_running                   = false;
    bool                       m_coordinator_entered                = false;
    bool                       m_coordinator_entered_while_callback = false;
    bool                       m_completed                          = false;
    bool                       m_completed_while_callback           = false;
    bool                       m_callback_observed_entry            = false;
    bool                       m_callback_observed_completion       = false;
    bool                       m_threw                              = false;
};

thread_local Public_api_probe* Public_api_probe::s_active_probe = nullptr;

fs::path identity_path(const fs::path& directory, std::uint32_t occurrence)
{
    return directory /
        ("occurrence_" + std::to_string(occurrence) + ".identity");
}

fs::path marker_path(const fs::path& directory, std::string_view marker)
{
    return directory / std::string(marker);
}

bool write_child_identity(const fs::path& directory)
{
    return sintra::test::managed_child::write_child_identity(
        identity_path(directory, sintra::s_recovery_occurrence));
}

std::optional<child_identity_t> wait_for_child_identity(
    const fs::path&                        directory,
    std::uint32_t                          occurrence,
    std::chrono::steady_clock::time_point  deadline)
{
    return
        sintra::test::managed_child::wait_for_child_identity(
            identity_path(directory, occurrence),
            deadline,
            k_poll_interval);
}

template <typename Predicate>
bool wait_until(
    Predicate&&                            predicate,
    std::chrono::steady_clock::time_point  deadline)
{
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(k_poll_interval);
    }
    while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

bool child_identity_is_exact(
    const std::optional<child_identity_t>& identity,
    sintra::instance_id_type               process_iid,
    std::uint32_t                          occurrence)
{
    return
        identity                             &&
        identity->process_iid == process_iid &&
        identity->occurrence  == occurrence  &&
        identity->pid > 0                    &&
        identity->start_stamp != 0;
}

bool exact_identity_is_absent(
    const std::optional<child_identity_t>& identity)
{
    return
        identity &&
        !exact_process_is_live(identity->pid, identity->start_stamp);
}

bool write_marker(const fs::path& directory, std::string_view marker)
{
    return write_complete_file(marker_path(directory, marker), "go\n");
}

const char* callback_name(Probed_callback callback)
{
    return callback == Probed_callback::LIFECYCLE
        ? "lifecycle"
        : "recovery";
}

int run_child(int argc, char* argv[])
{
    const fs::path directory(
        sintra::test::get_argv_value(argc, argv, k_directory_arg));
    if (directory.empty()) {
        return 2;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    if (sintra::s_recovery_occurrence == 0) {
        sintra::enable_recovery();
    }

    if (!write_child_identity(directory)) {
        return 3;
    }

    if (sintra::s_recovery_occurrence == 0) {
        if (!sintra::test::wait_for_file(
                marker_path(directory, k_go_marker), k_child_timeout, k_poll_interval))
        {
            return 4;
        }

        try {
            (void)sintra::Coordinator::rpc_unpublish_transceiver(
                sintra::s_coord_id,
                sintra::s_mproc_id);
        }
        catch (...) {
        }
        std::_Exit(0);
    }

    if (sintra::s_recovery_occurrence != 1) {
        return 5;
    }

    if (!sintra::test::wait_for_file(
            marker_path(directory, k_release_marker), k_child_timeout, k_poll_interval))
    {
        return 6;
    }
    return sintra::detail::finalize() ? 0 : 7;
}

bool run_scenario(
    const fs::path&        binary_path,
    const scenario_spec_t& scenario)
{
    const std::string scenario_name = callback_name(scenario.callback);
    const auto directory = sintra::test::unique_scratch_directory(
        "recovery_unpublish_" + scenario_name);
    const auto deadline =
        std::chrono::steady_clock::now() + k_scenario_timeout;

    Public_api_probe probe(scenario.probe_iid);

    std::atomic<bool> lifecycle_seen{false};
    std::atomic<bool> recovery_seen{false};
    std::atomic<bool> probe_done{false};
    std::atomic<bool> probe_contract{false};

    Managed_child_exit_capture exit_capture;
    sintra::Managed_child_custody custody;
    sintra::Managed_child_exit_observation exit_observation;
    std::optional<child_identity_t> initial;
    std::optional<child_identity_t> recovered;
    sintra::Managed_child_status status_before_release;
    bool probe_eventual       = false;
    bool invitation_valid     = false;
    bool invitation_cancelled = false;
    bool release_complete     = false;

    sintra::set_lifecycle_handler([
        &lifecycle_seen,
        &probe,
        &probe_done,
        &probe_contract,
        callback = scenario.callback,
        child_iid = scenario.child_iid
        ](
        const sintra::process_lifecycle_event& event)
        {
            if (event.process_iid != child_iid ||
                event.why !=
                    sintra::process_lifecycle_event::reason::unpublished ||
                lifecycle_seen.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            if (callback == Probed_callback::LIFECYCLE) {
                probe_contract.store(
                    probe.run_inside_callback(),
                    std::memory_order_release);
                probe_done.store(true, std::memory_order_release);
            }
        });
    sintra::set_recovery_policy([
        &recovery_seen,
        &probe,
        &probe_done,
        &probe_contract,
        callback = scenario.callback,
        child_iid = scenario.child_iid
        ](
        const sintra::Crash_info& info)
        {
            if (info.process_iid == child_iid                            &&
                !recovery_seen.exchange(true, std::memory_order_acq_rel) &&
                callback         == Probed_callback::RECOVERY)
            {
                probe_contract.store(
                    probe.run_inside_callback(),
                    std::memory_order_release);
                probe_done.store(true, std::memory_order_release);
            }
            return true;
        });

    bool scenario_ok = [&]() {
        if (!sintra::test::assert_true(
                scenario.child_iid     != sintra::invalid_instance_id &&
                    scenario.probe_iid != sintra::invalid_instance_id &&
                    scenario.child_iid != scenario.probe_iid,
                k_failure_prefix,
                scenario_name + " scenario IDs must be valid and distinct"))
        {
            return false;
        }

        sintra::Spawn_options options;
        options.binary_path              = binary_path.string();
        options.args                     = {
            std::string(k_child_flag),
            std::string(k_directory_arg),
            directory.string()
        };
        options.process_instance_id      = scenario.child_iid;
        options.lifetime.enable_lifeline = false;
        custody                          = sintra::spawn_swarm_process(options);
        if (!sintra::test::assert_true(
                static_cast<bool>(custody), k_failure_prefix, scenario_name + " public spawn must return custody"))
        {
            return false;
        }

        exit_observation = custody.observe_latest_created_exit([
            &exit_capture
            ](
            const sintra::Managed_child_exit& event)
            {
                exit_capture.record(event);
            });
        if (!sintra::test::assert_true(
                static_cast<bool>(exit_observation), k_failure_prefix,
                scenario_name + " initial child must expose exact exit"))
        {
            return false;
        }

        initial = wait_for_child_identity(directory, 0, deadline);
        if (!sintra::test::assert_true(
                child_identity_is_exact(initial, scenario.child_iid, 0) &&
                    exact_process_is_live(initial->pid, initial->start_stamp),
                k_failure_prefix,
                scenario_name + " initial occurrence must be exact and live"))
        {
            return false;
        }
        if (!sintra::test::assert_true(
                write_marker(directory, k_go_marker), k_failure_prefix,
                scenario_name + " initial go marker must be written"))
        {
            return false;
        }

        const bool callback_finished = wait_until(
            [&probe_done] {
                return probe_done.load(std::memory_order_acquire);
            },
            deadline);
        if (!sintra::test::assert_true(
                callback_finished, k_failure_prefix, scenario_name + " selected callback must finish"))
        {
            return false;
        }

        probe_eventual = probe.wait_until_completed(deadline);
        probe.join();
        if (!sintra::test::assert_true(
                probe_eventual && !probe.threw(), k_failure_prefix,
                scenario_name + " public API probe must eventually complete"))
        {
            return false;
        }
        invitation_valid = probe.invitation_is_valid();
        if (!sintra::test::assert_true(
                invitation_valid, k_failure_prefix, scenario_name + " public API probe must return an invitation"))
        {
            return false;
        }
        invitation_cancelled = probe.cancel_invitation();
        if (!sintra::test::assert_true(
                invitation_cancelled, k_failure_prefix, scenario_name + " invitation must remain cancellable"))
        {
            return false;
        }

        sintra::Managed_child_exit initial_exit;
        const bool initial_exit_delivered = exit_capture.wait_for_one_until(deadline);
        const auto initial_exit_capture   = exit_capture.snapshot();
        if (initial_exit_capture.event) {
            initial_exit = *initial_exit_capture.event;
        }
        if (!sintra::test::assert_true(
                initial_exit_delivered &&
                    initial_exit_capture.deliveries          == 1 &&
                    initial_exit.occurrence                  == exit_observation.occurrence &&
                    initial_exit.occurrence.process_instance_id ==
                        scenario.child_iid &&
                    initial_exit.occurrence.occurrence       == 0 &&
                    initial_exit.occurrence.custody_identity != 0,
                k_failure_prefix,
                scenario_name + " native exit must identify occurrence 0"))
        {
            return false;
        }

        recovered = wait_for_child_identity(directory, 1, deadline);
        if (!sintra::test::assert_true(
                child_identity_is_exact(recovered, scenario.child_iid, 1) &&
                    exact_process_is_live(
                        recovered->pid,
                        recovered->start_stamp),
                k_failure_prefix,
                scenario_name + " recovery must create exact occurrence 1"))
        {
            return false;
        }
        if (!sintra::test::assert_true(
                write_marker(directory, k_release_marker), k_failure_prefix,
                scenario_name + " recovery release marker must be written"))
        {
            return false;
        }

        const bool exact_children_absent = wait_until(
            [&initial, &recovered] {
                return
                    exact_identity_is_absent(initial) &&
                    exact_identity_is_absent(recovered);
            },
            deadline);
        if (!sintra::test::assert_true(
                exact_children_absent, k_failure_prefix, scenario_name + " exact PID/start identities must be absent"))
        {
            return false;
        }

        const bool two_exits = wait_until(
            [&custody] {
                const auto status = custody.status();
                return
                    status.admitted_occurrences == 2 &&
                    status.created_occurrences  == 2 &&
                    status.exited_occurrences   == 2;
            },
            deadline);
        status_before_release = custody.status();
        if (!sintra::test::assert_true(
                two_exits &&
                    status_before_release.admitted_occurrences == 2 &&
                    status_before_release.created_occurrences  == 2 &&
                    status_before_release.exited_occurrences   == 2,
                k_failure_prefix,
                scenario_name + " custody must report admitted=created=exited=2"))
        {
            return false;
        }

        const auto released = custody.release_until(deadline);
        release_complete =
            released.release_state ==
                sintra::Managed_child_release_state::complete;
        if (!sintra::test::assert_true(
                release_complete, k_failure_prefix, scenario_name + " custody release must complete"))
        {
            return false;
        }

        bool ok = true;
        ok &= sintra::test::assert_true(
            lifecycle_seen.load(std::memory_order_acquire),
            k_failure_prefix,
            scenario_name + " lifecycle callback must run");
        ok &= sintra::test::assert_true(
            recovery_seen.load(std::memory_order_acquire),
            k_failure_prefix,
            scenario_name + " recovery policy must run");
        ok &= sintra::test::assert_true(
            probe.callback_observed_entry(),
            k_failure_prefix,
            scenario_name + " probe must enter the public coordinator API");
        ok &= sintra::test::assert_true(
            probe.callback_observed_completion() &&
                probe_contract.load(std::memory_order_acquire),
            k_failure_prefix,
            scenario_name +
                " public API must complete before selected callback returns");
        return ok;
    }();

    (void)write_marker(directory, k_go_marker);
    (void)write_marker(directory, k_release_marker);
    if (!probe_eventual) {
        probe_eventual = probe.wait_until_completed(deadline);
        probe.join();
    }
    invitation_valid = probe.invitation_is_valid();
    if (invitation_valid && !invitation_cancelled) {
        invitation_cancelled = probe.cancel_invitation();
    }
    exit_observation.subscription.unsubscribe();

    if (custody && !release_complete) {
        const auto released = custody.terminate_until(deadline);
        release_complete =
            released.release_state ==
                sintra::Managed_child_release_state::complete;
        scenario_ok &= sintra::test::assert_true(
            release_complete,
            k_failure_prefix,
            scenario_name + " cleanup custody termination must complete");
    }
    if (initial) {
        scenario_ok &= sintra::test::assert_true(
            wait_until(
                [&initial] { return exact_identity_is_absent(initial); },
                deadline),
            k_failure_prefix,
            scenario_name + " cleanup must reap exact occurrence 0");
    }
    if (recovered) {
        scenario_ok &= sintra::test::assert_true(
            wait_until(
                [&recovered] { return exact_identity_is_absent(recovered); },
                deadline),
            k_failure_prefix,
            scenario_name + " cleanup must reap exact occurrence 1");
    }

    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    sintra::set_recovery_policy(sintra::Recovery_policy{});

    std::fprintf(stderr,
        "RECOVERY_UNPUBLISH_CALLBACK_LOCK_SCENARIO "
        "callback=%s entry=%d completion_inside=%d eventual=%d "
        "invitation_valid=%d invitation_cancelled=%d "
        "admitted=%zu created=%zu exited=%zu release=%d\n",
        scenario_name.c_str(),
        probe.callback_observed_entry() ? 1 : 0,
        probe.callback_observed_completion() ? 1 : 0,
        probe_eventual ? 1 : 0,
        invitation_valid ? 1 : 0,
        invitation_cancelled ? 1 : 0,
        status_before_release.admitted_occurrences,
        status_before_release.created_occurrences,
        status_before_release.exited_occurrences,
        release_complete ? 1 : 0);

    std::error_code error;
    fs::remove_all(directory, error);
    return scenario_ok;
}

int run_root(int argc, char* argv[])
{
    const auto binary_path = sintra::test::get_binary_path(argc, argv);
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
    std::thread watchdog(
        [
            &watchdog_done
        ]()
        {
            const auto deadline =
                std::chrono::steady_clock::now() + k_watchdog_timeout;
            while (!watchdog_done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::fprintf(stderr,
                        "RECOVERY_UNPUBLISH_CALLBACK_LOCK_INVALID "
                        "watchdog_timeout=1\n");
                    std::fflush(stderr);
                    std::_Exit(124);
                }
                std::this_thread::sleep_for(20ms);
            }
        });

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &Public_api_probe::coordinator_lock_stage,
        std::memory_order_release);

    const scenario_spec_t lifecycle_scenario{
        Probed_callback::LIFECYCLE,
        sintra::make_process_instance_id(),
        sintra::make_process_instance_id(),
    };
    const scenario_spec_t recovery_scenario{
        Probed_callback::RECOVERY,
        sintra::make_process_instance_id(),
        sintra::make_process_instance_id(),
    };

    bool ok = true;
    ok &= sintra::test::assert_true(
        lifecycle_scenario.child_iid != sintra::invalid_instance_id &&
            lifecycle_scenario.probe_iid != sintra::invalid_instance_id &&
            recovery_scenario.child_iid  != sintra::invalid_instance_id &&
            recovery_scenario.probe_iid  != sintra::invalid_instance_id &&
            lifecycle_scenario.child_iid != lifecycle_scenario.probe_iid &&
            lifecycle_scenario.child_iid != recovery_scenario.child_iid  &&
            lifecycle_scenario.child_iid != recovery_scenario.probe_iid  &&
            lifecycle_scenario.probe_iid != recovery_scenario.child_iid  &&
            lifecycle_scenario.probe_iid != recovery_scenario.probe_iid  &&
            recovery_scenario.child_iid  != recovery_scenario.probe_iid,
        k_failure_prefix,
        "scenario child and probe IDs must be valid and pairwise distinct");
    if (ok) {
        ok &= run_scenario(binary_path, lifecycle_scenario);
        ok &= run_scenario(binary_path, recovery_scenario);
    }

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr,
        std::memory_order_release);
    ok &= sintra::test::assert_true(
        sintra::shutdown(),
        k_failure_prefix,
        "coordinator shutdown must complete");

    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv);
    }
    return run_root(argc, argv);
}
