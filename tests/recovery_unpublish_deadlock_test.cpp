// A managed-child unpublish invokes lifecycle and recovery callbacks. Both
// callbacks must be able to finish a public coordinator API call before they
// return; eventual completion after callback return still exposes the lock.

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

constexpr std::string_view k_child_flag =
    "--recovery-unpublish-callback-lock-child";
constexpr std::string_view k_directory_arg  = "--recovery-unpublish-directory";
constexpr std::string_view k_go_marker      = "occurrence_0.go";
constexpr std::string_view k_release_marker = "occurrence_1.release";

constexpr auto k_callback_stage_timeout      = 2s;
constexpr auto k_callback_completion_timeout = 2s;
constexpr auto k_step_timeout                = 15s;
constexpr auto k_child_timeout               = 45s;
constexpr auto k_watchdog_timeout            = 55s;
constexpr auto k_poll_interval               = 10ms;

constexpr const char* k_failure_prefix =
    "recovery_unpublish_deadlock_test: ";

struct Child_identity
{
    sintra::instance_id_type   process_iid = sintra::invalid_instance_id;
    std::uint32_t              occurrence  = 0;
    int                        pid         = -1;
    std::uint64_t              start_stamp = 0;
};

struct Exit_capture
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    std::optional<sintra::Managed_child_exit>
                               event;
    unsigned                   deliveries  = 0;
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
                        options.timeout             = 30s;
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
            std::lock_guard<std::mutex> lock(m_mutex);
            m_threw = true;
            m_completed = true;
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
    const auto start_stamp = sintra::current_process_start_stamp();
    if (!start_stamp) {
        return false;
    }

    std::ostringstream contents;
    contents
        << static_cast<unsigned long long>(sintra::s_mproc_id) << ' '
        << sintra::s_recovery_occurrence << ' '
        << sintra::test::get_pid() << ' '
        << static_cast<unsigned long long>(*start_stamp) << '\n';
    return write_complete_file(
        identity_path(directory, sintra::s_recovery_occurrence),
        contents.str());
}

std::optional<Child_identity> read_child_identity(const fs::path& path)
{
    Child_identity identity;
    unsigned long long process_iid = 0;
    unsigned long long start_stamp = 0;
    std::ifstream input(path, std::ios::binary);
    if (!(input
        >> process_iid
        >> identity.occurrence
        >> identity.pid
        >> start_stamp))
    {
        return std::nullopt;
    }
    identity.process_iid =
        static_cast<sintra::instance_id_type>(process_iid);
    identity.start_stamp = static_cast<std::uint64_t>(start_stamp);
    return identity;
}

std::optional<Child_identity> wait_for_child_identity(
    const fs::path&    directory,
    std::uint32_t      occurrence)
{
    const auto path = identity_path(directory, occurrence);
    if (!sintra::test::wait_for_file(path, k_child_timeout, k_poll_interval)) {
        return std::nullopt;
    }
    return read_child_identity(path);
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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
    const std::optional<Child_identity>&   identity,
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

bool wait_for_exact_absence(
    const std::optional<Child_identity>&   identity,
    std::chrono::milliseconds              timeout)
{
    if (!identity) {
        return false;
    }
    return wait_until(
        [&identity] {
            return !exact_process_is_live(
                identity->pid,
                identity->start_stamp);
        },
        timeout);
}

bool write_marker(const fs::path& directory, std::string_view marker)
{
    return write_complete_file(marker_path(directory, marker), "go\n");
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

int run_root(int argc, char* argv[])
{
    const auto binary_path = sintra::test::get_binary_path(argc, argv);
    if (binary_path.empty()) {
        return 2;
    }

    const auto directory =
        sintra::test::unique_scratch_directory("recovery_unpublish_callback_lock");

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

    bool       ok        = true;
    const auto child_iid = sintra::make_process_instance_id();

    const auto lifecycle_probe_iid = sintra::make_process_instance_id();
    const auto recovery_probe_iid  = sintra::make_process_instance_id();
    ok &= sintra::test::assert_true(
        child_iid           != sintra::invalid_instance_id &&
        lifecycle_probe_iid != sintra::invalid_instance_id &&
        recovery_probe_iid  != sintra::invalid_instance_id &&
        child_iid           != lifecycle_probe_iid         &&
        child_iid           != recovery_probe_iid          &&
        lifecycle_probe_iid != recovery_probe_iid,
        k_failure_prefix,
        "preallocated process instance ids must be valid and distinct");

    Public_api_probe lifecycle_probe(lifecycle_probe_iid);
    Public_api_probe recovery_probe(recovery_probe_iid);
    std::atomic<unsigned> lifecycle_callbacks{0};
    std::atomic<unsigned> recovery_callbacks{0};
    std::atomic<bool> lifecycle_callback_done{false};
    std::atomic<bool> recovery_callback_done{false};
    std::atomic<bool> lifecycle_callback_contract{false};
    std::atomic<bool> recovery_callback_contract{false};

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &Public_api_probe::coordinator_lock_stage,
        std::memory_order_release);
    sintra::set_lifecycle_handler([
        &lifecycle_probe,
        &lifecycle_callbacks,
        &lifecycle_callback_done,
        &lifecycle_callback_contract,
        child_iid
        ](
        const sintra::process_lifecycle_event& event)
        {
            if (event.process_iid != child_iid ||
                event.why !=
                    sintra::process_lifecycle_event::reason::unpublished)
            {
                return;
            }
            if (lifecycle_callbacks.fetch_add(
                    1, std::memory_order_acq_rel) != 0)
            {
                return;
            }
            lifecycle_callback_contract.store(
                lifecycle_probe.run_inside_callback(),
                std::memory_order_release);
            lifecycle_callback_done.store(true, std::memory_order_release);
        });
    sintra::set_recovery_policy([
        &recovery_probe,
        &recovery_callbacks,
        &recovery_callback_done,
        &recovery_callback_contract,
        child_iid
        ](
        const sintra::Crash_info& info)
        {
            if (info.process_iid                  == child_iid &&
                recovery_callbacks.fetch_add(
                    1, std::memory_order_acq_rel) == 0)
            {
                recovery_callback_contract.store(
                    recovery_probe.run_inside_callback(),
                    std::memory_order_release);
                recovery_callback_done.store(true, std::memory_order_release);
            }
            return true;
        });

    sintra::Spawn_options options;
    options.binary_path              = binary_path;
    options.args                     = {
        std::string(k_child_flag),
        std::string(k_directory_arg),
        directory.string()
    };
    options.process_instance_id      = child_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    ok &= sintra::test::assert_true(
        static_cast<bool>(custody),
        k_failure_prefix,
        "public managed-child spawn should return custody");

    Exit_capture exit_capture;
    sintra::Managed_child_exit_observation exit_observation;
    if (custody) {
        exit_observation = custody.observe_latest_created_exit([
            &exit_capture
            ](
            const sintra::Managed_child_exit& event)
            {
                {
                    std::lock_guard<std::mutex> lock(exit_capture.mutex);
                    exit_capture.event = event;
                    ++exit_capture.deliveries;
                }
                exit_capture.changed.notify_all();
            });
    }
    ok &= sintra::test::assert_true(
        static_cast<bool>(exit_observation),
        k_failure_prefix,
        "initial child should provide an exact exit observation");

    const auto initial = wait_for_child_identity(directory, 0);
    ok &= sintra::test::assert_true(
        child_identity_is_exact(initial, child_iid, 0) &&
            exact_process_is_live(initial->pid, initial->start_stamp),
        k_failure_prefix,
        "initial child identity must be exact and live");
    ok &= sintra::test::assert_true(
        write_marker(directory, k_go_marker),
        k_failure_prefix,
        "initial child go marker should be written");

    const bool callbacks_finished = wait_until(
        [&] {
            return
                lifecycle_callback_done.load(std::memory_order_acquire) &&
                recovery_callback_done.load(std::memory_order_acquire);
        },
        k_step_timeout);
    ok &= sintra::test::assert_true(
        callbacks_finished,
        k_failure_prefix,
        "unpublished lifecycle and recovery callbacks must both finish");

    const auto probe_deadline =
        std::chrono::steady_clock::now() + k_step_timeout;
    const bool lifecycle_eventual =
        lifecycle_probe.wait_until_completed(probe_deadline);
    const bool recovery_eventual =
        recovery_probe.wait_until_completed(probe_deadline);
    lifecycle_probe.join();
    recovery_probe.join();

    ok &= sintra::test::assert_true(
        lifecycle_callback_contract.load(std::memory_order_acquire) &&
            lifecycle_probe.callback_observed_entry() &&
            lifecycle_probe.callback_observed_completion(),
        k_failure_prefix,
        "lifecycle callback public API must enter and complete before callback return");
    ok &= sintra::test::assert_true(
        recovery_callback_contract.load(std::memory_order_acquire) &&
            recovery_probe.callback_observed_entry() &&
            recovery_probe.callback_observed_completion(),
        k_failure_prefix,
        "recovery policy public API must enter and complete before callback return");
    ok &= sintra::test::assert_true(
        lifecycle_eventual && recovery_eventual &&
            !lifecycle_probe.threw() && !recovery_probe.threw(),
        k_failure_prefix,
        "both public API probes must eventually complete without throwing");
    ok &= sintra::test::assert_true(
        lifecycle_probe.invitation_is_valid() &&
            recovery_probe.invitation_is_valid(),
        k_failure_prefix,
        "both eventual public API probes must return valid invitations");
    const bool lifecycle_invitation_cancelled =
        lifecycle_probe.cancel_invitation();
    const bool recovery_invitation_cancelled =
        recovery_probe.cancel_invitation();
    ok &= sintra::test::assert_true(
        lifecycle_invitation_cancelled && recovery_invitation_cancelled,
        k_failure_prefix,
        "both eventual invitations must remain cancellable");

    sintra::Managed_child_exit initial_exit;
    bool     initial_exit_delivered  = false;
    unsigned initial_exit_deliveries = 0;
    {
        std::unique_lock<std::mutex> lock(exit_capture.mutex);
        initial_exit_delivered = exit_capture.changed.wait_for(
            lock,
            k_step_timeout,
            [&exit_capture] { return exit_capture.event.has_value(); });
        if (exit_capture.event) {
            initial_exit = *exit_capture.event;
        }
        initial_exit_deliveries = exit_capture.deliveries;
    }
    ok &= sintra::test::assert_true(
        initial_exit_delivered                           &&
        initial_exit_deliveries == 1                    &&
        initial_exit.occurrence == exit_observation.occurrence &&
        initial_exit.occurrence.process_instance_id == child_iid &&
        initial_exit.occurrence.occurrence == 0          &&
        initial_exit.occurrence.custody_identity != 0,
        k_failure_prefix,
        "initial native exit must deliver one exact occurrence identity");

    const auto recovered = wait_for_child_identity(directory, 1);
    ok &= sintra::test::assert_true(
        child_identity_is_exact(recovered, child_iid, 1) &&
            exact_process_is_live(recovered->pid, recovered->start_stamp),
        k_failure_prefix,
        "recovery must create exact occurrence 1");
    ok &= sintra::test::assert_true(
        write_marker(directory, k_release_marker),
        k_failure_prefix,
        "recovered child release marker should be written");

    const bool initial_absent   = wait_for_exact_absence(initial,   k_step_timeout);
    const bool recovered_absent = wait_for_exact_absence(recovered, k_step_timeout);
    ok &= sintra::test::assert_true(
        initial_absent && recovered_absent,
        k_failure_prefix,
        "both exact child PID/start identities must be absent");

    const bool two_exits = wait_until(
        [&custody] {
            const auto status = custody.status();
            return
                status.admitted_occurrences == 2 &&
                status.created_occurrences  == 2 &&
                status.exited_occurrences   == 2;
        },
        k_step_timeout);
    const auto status_before_release = custody.status();
    ok &= sintra::test::assert_true(
        two_exits &&
            status_before_release.admitted_occurrences == 2 &&
            status_before_release.created_occurrences  == 2 &&
            status_before_release.exited_occurrences   == 2,
        k_failure_prefix,
        "custody status must report admitted=created=exited=2");

    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + k_step_timeout);
    ok &= sintra::test::assert_true(
        released.release_state == sintra::Managed_child_release_state::complete,
        k_failure_prefix,
        "managed-child custody release must complete");

    exit_observation.subscription.unsubscribe();
    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    sintra::set_recovery_policy(sintra::Recovery_policy{});
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr,
        std::memory_order_release);
    ok &= sintra::test::assert_true(
        sintra::shutdown(),
        k_failure_prefix,
        "coordinator shutdown must complete");

    std::fprintf(stderr,
        "RECOVERY_UNPUBLISH_CALLBACK_LOCK_RESULT "
        "lifecycle_entry=%d lifecycle_completion=%d "
        "recovery_entry=%d recovery_completion=%d "
        "lifecycle_eventual=%d recovery_eventual=%d "
        "admitted=%zu created=%zu exited=%zu\n",
        lifecycle_probe.callback_observed_entry() ? 1 : 0,
        lifecycle_probe.callback_observed_completion() ? 1 : 0,
        recovery_probe.callback_observed_entry() ? 1 : 0,
        recovery_probe.callback_observed_completion() ? 1 : 0,
        lifecycle_eventual ? 1 : 0,
        recovery_eventual ? 1 : 0,
        status_before_release.admitted_occurrences,
        status_before_release.created_occurrences,
        status_before_release.exited_occurrences);

    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();
    std::error_code error;
    fs::remove_all(directory, error);
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
