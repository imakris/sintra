//
// Recovery authority belongs to one logical custody, not to a reusable
// process-instance id. A delayed recovery decision must retain that authority.
//

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
using sintra::test::managed_child::wait_for_child_identity;
using sintra::test::managed_child::wait_for_exact_process_absence;
using sintra::test::managed_child::write_complete_file;
using sintra::test::managed_child::write_child_identity;

constexpr std::string_view k_opt_in_a_flag  = "--recovery-authority-opt-in-a";
constexpr std::string_view k_opt_in_b_flag  = "--recovery-authority-opt-in-b";
constexpr std::string_view k_delayed_a_flag = "--recovery-authority-delayed-a";
constexpr std::string_view k_delayed_b_flag = "--recovery-authority-delayed-b";

constexpr std::string_view k_opt_in_a_marker           = "opt_in_a.complete";
constexpr std::string_view k_opt_in_a_exit             = "opt_in_a_exit.complete";
constexpr std::string_view k_opt_in_b_marker           = "opt_in_b_0.complete";
constexpr std::string_view k_opt_in_b_recovery_marker  = "opt_in_b_1.complete";
constexpr std::string_view k_opt_in_b_crash            = "opt_in_b_crash.complete";
constexpr std::string_view k_delayed_a_marker          = "delayed_a.complete";
constexpr std::string_view k_delayed_a_crash           = "delayed_a_crash.complete";
constexpr std::string_view k_delayed_b_marker          = "delayed_b_0.complete";
constexpr std::string_view k_delayed_b_recovery_marker = "delayed_b_1.complete";
constexpr std::string_view k_delayed_b_stop            = "delayed_b_stop.complete";

constexpr auto k_step_timeout     = 5s;
constexpr auto k_child_timeout    = 90s;
constexpr auto k_watchdog_timeout = 120s;
constexpr auto k_poll_interval    = 10ms;

constexpr sintra::instance_id_type k_opt_in_iid =
    sintra::compose_instance(35u, 1u);
constexpr sintra::instance_id_type k_delayed_iid =
    sintra::compose_instance(36u, 1u);

struct Retirement_fence
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    std::uint64_t              custody_identity = 0;
    bool                       complete         = false;
};

struct Recovery_decision_fence
{
    std::mutex                 mutex;
    std::condition_variable    changed;
    sintra::instance_id_type   process_iid = sintra::invalid_instance_id;
    bool                       armed       = false;
    bool                       observed    = false;
    bool                       scheduled   = false;
};

Retirement_fence* s_retirement_fence = nullptr;
Recovery_decision_fence s_recovery_decision_fence;

void custody_retirement_stage(const char* stage, std::uint64_t custody_identity) noexcept
{
    auto* fence = s_retirement_fence;
    if (!fence || custody_identity != fence->custody_identity ||
        std::string_view(stage) != sintra::detail::test_hooks::
            k_managed_child_custody_retirement_complete)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(fence->mutex);
        fence->complete = true;
    }
    fence->changed.notify_all();
}

void recovery_decision(
    sintra::instance_id_type   process_iid,
    bool                       scheduled) noexcept
{
    std::lock_guard<std::mutex> lock(s_recovery_decision_fence.mutex);
    if (!s_recovery_decision_fence.armed ||
        s_recovery_decision_fence.observed ||
        process_iid != s_recovery_decision_fence.process_iid)
    {
        return;
    }
    s_recovery_decision_fence.observed  = true;
    s_recovery_decision_fence.scheduled = scheduled;
    s_recovery_decision_fence.changed.notify_all();
}

fs::path marker(const fs::path& directory, std::string_view name)
{
    return directory / std::string(name);
}

std::optional<child_identity_t> wait_for_identity(const fs::path& path)
{
    return wait_for_child_identity(path, k_step_timeout, k_poll_interval);
}

bool identity_is_live_occurrence(
    const std::optional<child_identity_t>& identity,
    sintra::instance_id_type               expected_iid,
    std::uint32_t                          expected_occurrence)
{
    return
        identity                                    &&
        identity->process_iid == expected_iid       &&
        identity->occurrence == expected_occurrence &&
        sintra::test::managed_child::exact_process_is_live(*identity);
}

bool identity_is_occurrence(
    const std::optional<child_identity_t>& identity,
    sintra::instance_id_type               expected_iid,
    std::uint32_t                          expected_occurrence)
{
    return
        identity                                    &&
        identity->process_iid == expected_iid       &&
        identity->occurrence == expected_occurrence &&
        identity->pid > 0                           &&
        identity->start_stamp != 0;
}

bool wait_for_absence(const std::optional<child_identity_t>& identity)
{
    return
        identity &&
        wait_for_exact_process_absence(*identity, k_step_timeout, k_poll_interval);
}

std::optional<std::uint64_t> capture_custody_identity(
    const sintra::Managed_child_custody& custody)
{
    const auto observation = custody.observe_latest_created_exit(
        [](const sintra::Managed_child_exit&) {});
    if (!observation) {
        return std::nullopt;
    }
    return observation.occurrence.custody_identity;
}

void arm_retirement_fence(
    Retirement_fence&  fence,
    std::uint64_t      custody_identity)
{
    std::lock_guard<std::mutex> lock(fence.mutex);
    fence.custody_identity = custody_identity;
    fence.complete         = false;
    s_retirement_fence     = &fence;
}

bool wait_for_retirement(
    Retirement_fence&                      fence,
    std::chrono::steady_clock::time_point  deadline)
{
    std::unique_lock<std::mutex> lock(fence.mutex);
    return fence.changed.wait_until(lock, deadline, [&] { return fence.complete; });
}

void arm_recovery_decision(sintra::instance_id_type process_iid)
{
    std::lock_guard<std::mutex> lock(s_recovery_decision_fence.mutex);
    s_recovery_decision_fence.process_iid = process_iid;
    s_recovery_decision_fence.armed       = true;
    s_recovery_decision_fence.observed    = false;
    s_recovery_decision_fence.scheduled   = false;
}

std::optional<bool> wait_for_recovery_decision(
    std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(s_recovery_decision_fence.mutex);
    if (!s_recovery_decision_fence.changed.wait_until(
            lock, deadline, [] { return s_recovery_decision_fence.observed; }))
    {
        return std::nullopt;
    }
    s_recovery_decision_fence.armed = false;
    return s_recovery_decision_fence.scheduled;
}

sintra::Managed_child_custody spawn_child(
    const std::string&         binary_path,
    std::string_view           flag,
    sintra::instance_id_type   process_iid)
{
    sintra::Spawn_options options;
    options.binary_path              = binary_path;
    options.args                     = {std::string(flag)};
    options.process_instance_id      = process_iid;
    options.lifetime.enable_lifeline = false;
    return sintra::spawn_swarm_process(options);
}

int run_child(int argc, char* argv[], const fs::path& shared_path)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    const bool opt_in_a  = sintra::test::has_argv_flag(argc, argv, k_opt_in_a_flag);
    const bool opt_in_b  = sintra::test::has_argv_flag(argc, argv, k_opt_in_b_flag);
    const bool delayed_a = sintra::test::has_argv_flag(argc, argv, k_delayed_a_flag);
    const bool delayed_b = sintra::test::has_argv_flag(argc, argv, k_delayed_b_flag);
    if (opt_in_a || delayed_a) {
        sintra::enable_recovery();
    }

    fs::path identity_marker;
    fs::path action_marker;
    if (opt_in_a) {
        identity_marker = marker(shared_path, k_opt_in_a_marker);
        action_marker   = marker(shared_path, k_opt_in_a_exit);
    }
    else
    if (opt_in_b) {
        identity_marker = marker(
            shared_path,
            sintra::s_recovery_occurrence == 0
                ? k_opt_in_b_marker
                : k_opt_in_b_recovery_marker);
        action_marker   = marker(shared_path, k_opt_in_b_crash);
    }
    else
    if (delayed_a) {
        identity_marker = marker(shared_path, k_delayed_a_marker);
        action_marker   = marker(shared_path, k_delayed_a_crash);
    }
    else
    if (delayed_b) {
        identity_marker = marker(
            shared_path,
            sintra::s_recovery_occurrence == 0
                ? k_delayed_b_marker
                : k_delayed_b_recovery_marker);
        action_marker   = marker(shared_path, k_delayed_b_stop);
    }
    else {
        return 3;
    }

    if (!write_child_identity(identity_marker)) {
        return 4;
    }
    if (!sintra::test::wait_for_file(
            action_marker, k_child_timeout, k_poll_interval))
    {
        return 4;
    }

    if (opt_in_a || delayed_b) {
        return sintra::detail::finalize() ? 0 : 5;
    }

    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash(
        opt_in_b
            ? "recovery authority must not cross custodies"
            : "delayed recovery authority must retain its custody");
    std::abort();
}

int run_root(int argc, char* argv[], const fs::path& shared_path)
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
    std::thread watchdog([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + k_watchdog_timeout;
        while (!watchdog_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                    "RECOVERY_AUTHORITY_REUSE_INVALID watchdog_timeout=1\n");
                std::fflush(stderr);
                std::_Exit(124);
            }
            std::this_thread::sleep_for(20ms);
        }
    });

    std::atomic<unsigned> inherited_callbacks{0};
    std::atomic<unsigned> delayed_callbacks{0};
    std::mutex delayed_mutex;
    std::condition_variable delayed_changed;
    bool delayed_entered  = false;
    bool delayed_released = false;
    bool delayed_complete = false;
    Retirement_fence retirement_fence;
    sintra::test::managed_child::Scoped_test_hook retirement_hook(
        sintra::detail::test_hooks::s_managed_child_custody_retirement,
        &custody_retirement_stage);
    sintra::test::managed_child::Scoped_test_hook recovery_decision_hook(
        sintra::detail::test_hooks::s_recovery_decision,
        &recovery_decision);
    sintra::set_recovery_runner([&](
        const sintra::Crash_info& info,
        const sintra::Recovery_control& control)
    {
        if (info.process_iid == k_opt_in_iid) {
            inherited_callbacks.fetch_add(1, std::memory_order_release);
            return;
        }
        if (info.process_iid                                          != k_delayed_iid ||
            delayed_callbacks.fetch_add(1, std::memory_order_acq_rel) != 0)
        {
            return;
        }

        std::unique_lock<std::mutex> lock(delayed_mutex);
        delayed_entered = true;
        delayed_changed.notify_all();
        delayed_changed.wait(lock, [&] { return delayed_released; });
        lock.unlock();
        control.spawn();
        lock.lock();
        delayed_complete = true;
        delayed_changed.notify_all();
    });

    const auto settle_custody = [](
        const sintra::Managed_child_custody& custody,
        bool setup_valid)
    {
        if (!custody) {
            return sintra::Managed_child_status{};
        }
        const auto deadline = std::chrono::steady_clock::now() + k_step_timeout;
        return setup_valid
            ? custody.release_until(deadline)
            : custody.terminate_until(deadline);
    };

    auto opt_in_a = spawn_child(binary_path, k_opt_in_a_flag, k_opt_in_iid);
    const auto opt_in_a_identity = opt_in_a
        ? wait_for_identity(marker(shared_path, k_opt_in_a_marker))
        : std::nullopt;
    const bool opt_in_a_identity_valid = identity_is_live_occurrence(
        opt_in_a_identity, k_opt_in_iid, 0);
    const auto opt_in_a_custody_identity = opt_in_a
        ? capture_custody_identity(opt_in_a)
        : std::nullopt;
    if (opt_in_a_custody_identity) {
        arm_retirement_fence(
            retirement_fence, *opt_in_a_custody_identity);
    }
    const bool opt_in_a_exit_requested = write_complete_file(
        marker(shared_path, k_opt_in_a_exit), "complete=1\n");
    const bool opt_in_a_absent = opt_in_a_identity_valid &&
        wait_for_absence(opt_in_a_identity);
    const auto opt_in_a_release = settle_custody(
        opt_in_a,
        opt_in_a_identity_valid && opt_in_a_custody_identity && opt_in_a_exit_requested && opt_in_a_absent);
    const bool opt_in_a_retired = opt_in_a_custody_identity &&
        wait_for_retirement(
            retirement_fence,
            std::chrono::steady_clock::now() + k_step_timeout);

    const bool opt_in_a_ready_for_reuse = opt_in_a_identity_valid &&
        opt_in_a_custody_identity && opt_in_a_exit_requested &&
        opt_in_a_absent &&
        opt_in_a_release.release_state ==
            sintra::Managed_child_release_state::complete &&
        opt_in_a_retired;
    auto opt_in_b = opt_in_a_ready_for_reuse
        ? spawn_child(binary_path, k_opt_in_b_flag, k_opt_in_iid)
        : sintra::Managed_child_custody{};
    const auto opt_in_b_identity = opt_in_b
        ? wait_for_identity(marker(shared_path, k_opt_in_b_marker))
        : std::nullopt;
    const bool opt_in_b_identity_valid = identity_is_live_occurrence(
        opt_in_b_identity, k_opt_in_iid, 0);
    if (opt_in_b) {
        arm_recovery_decision(k_opt_in_iid);
    }
    const bool opt_in_b_crash_requested = write_complete_file(
        marker(shared_path, k_opt_in_b_crash), "complete=1\n");
    const auto opt_in_b_recovery_decision = opt_in_b
        ? wait_for_recovery_decision(
            std::chrono::steady_clock::now() + k_step_timeout)
        : std::nullopt;
    const bool opt_in_b_absent = opt_in_b_identity_valid &&
        wait_for_absence(opt_in_b_identity);
    const auto opt_in_b_status = opt_in_b.status();
    const bool opt_in_b_recovery_absent =
        !fs::exists(marker(shared_path, k_opt_in_b_recovery_marker));
    const auto opt_in_b_release = settle_custody(
        opt_in_b,
        opt_in_b_identity_valid && opt_in_b_crash_requested && opt_in_b_recovery_decision.has_value() && opt_in_b_absent
    );

    auto delayed_a = spawn_child(binary_path, k_delayed_a_flag, k_delayed_iid);
    const auto delayed_a_identity = delayed_a
        ? wait_for_identity(marker(shared_path, k_delayed_a_marker))
        : std::nullopt;
    const bool delayed_a_identity_valid = identity_is_live_occurrence(
        delayed_a_identity, k_delayed_iid, 0);
    const auto delayed_a_custody_identity = delayed_a
        ? capture_custody_identity(delayed_a)
        : std::nullopt;
    if (delayed_a_custody_identity) {
        arm_retirement_fence(
            retirement_fence, *delayed_a_custody_identity);
    }
    const bool delayed_a_crash_requested = write_complete_file(
        marker(shared_path, k_delayed_a_crash), "complete=1\n");
    const bool delayed_a_absent = delayed_a_identity_valid &&
        wait_for_absence(delayed_a_identity);
    bool recovery_parked = false;
    {
        std::unique_lock<std::mutex> lock(delayed_mutex);
        recovery_parked = delayed_changed.wait_for(
            lock, k_step_timeout, [&] { return delayed_entered; });
    }
    const auto delayed_a_release = settle_custody(
        delayed_a,
        delayed_a_identity_valid && delayed_a_custody_identity &&
            delayed_a_crash_requested && delayed_a_absent && recovery_parked);
    const bool delayed_a_retired = delayed_a_custody_identity &&
        wait_for_retirement(
            retirement_fence,
            std::chrono::steady_clock::now() + k_step_timeout);

    const bool delayed_a_ready_for_reuse = delayed_a_identity_valid &&
        delayed_a_custody_identity && delayed_a_crash_requested &&
        delayed_a_absent && recovery_parked &&
        delayed_a_release.release_state ==
            sintra::Managed_child_release_state::complete &&
        delayed_a_retired;
    auto delayed_b = delayed_a_ready_for_reuse
        ? spawn_child(binary_path, k_delayed_b_flag, k_delayed_iid)
        : sintra::Managed_child_custody{};
    const auto delayed_b_identity = delayed_b
        ? wait_for_identity(marker(shared_path, k_delayed_b_marker))
        : std::nullopt;
    const bool delayed_b_identity_valid = identity_is_live_occurrence(
        delayed_b_identity, k_delayed_iid, 0);
    const bool delayed_b_stop_written = write_complete_file(
        marker(shared_path, k_delayed_b_stop), "complete=1\n");
    const bool delayed_b_absent = delayed_b_identity_valid &&
        wait_for_absence(delayed_b_identity);
    if (delayed_b &&
        (!delayed_b_identity_valid || !delayed_b_stop_written ||
         !delayed_b_absent))
    {
        (void)delayed_b.terminate_until(
            std::chrono::steady_clock::now() + k_step_timeout);
    }
    {
        std::lock_guard<std::mutex> lock(delayed_mutex);
        delayed_released = true;
        delayed_changed.notify_all();
    }
    bool old_control_completed = false;
    {
        std::unique_lock<std::mutex> lock(delayed_mutex);
        old_control_completed = delayed_changed.wait_for(
            lock, k_step_timeout, [&] { return delayed_complete; });
    }

    const auto delayed_b_status = delayed_b.status();
    const bool delayed_b_has_extra_occurrence =
        delayed_b_status.admitted_occurrences > 1 ||
        delayed_b_status.created_occurrences > 1;
    std::optional<child_identity_t> delayed_b_recovery_identity;
    if (delayed_b_has_extra_occurrence) {
        delayed_b_recovery_identity = wait_for_identity(
            marker(shared_path, k_delayed_b_recovery_marker));
    }
    const bool delayed_b_recovery_identity_valid =
        !delayed_b_has_extra_occurrence ||
        identity_is_occurrence(
            delayed_b_recovery_identity, k_delayed_iid, 1);
    const bool delayed_b_recovery_absent =
        !delayed_b_has_extra_occurrence ||
        (delayed_b_recovery_identity_valid &&
         wait_for_absence(delayed_b_recovery_identity));
    const auto delayed_b_release = settle_custody(
        delayed_b,
        delayed_b_identity_valid && delayed_b_stop_written &&
            old_control_completed && delayed_b_absent &&
            delayed_b_recovery_identity_valid && delayed_b_recovery_absent);

    sintra::set_recovery_runner(sintra::Recovery_runner{});
    const bool finalized = sintra::shutdown();
    watchdog_done.store(true, std::memory_order_release);
    watchdog.join();

    const bool releases_complete =
        opt_in_a_release.release_state ==
            sintra::Managed_child_release_state::complete &&
        opt_in_b_release.release_state ==
            sintra::Managed_child_release_state::complete &&
        delayed_a_release.release_state ==
            sintra::Managed_child_release_state::complete &&
        delayed_b_release.release_state ==
            sintra::Managed_child_release_state::complete;
    const bool exact_survivors_absent = opt_in_a_absent && opt_in_b_absent &&
        delayed_a_absent && delayed_b_absent && delayed_b_recovery_absent;
    const bool setup_valid = opt_in_a && opt_in_b && delayed_a && delayed_b &&
        opt_in_a_identity_valid && opt_in_b_identity_valid &&
        delayed_a_identity_valid && delayed_b_identity_valid &&
        opt_in_a_custody_identity && delayed_a_custody_identity &&
        opt_in_a_retired && delayed_a_retired &&
        opt_in_b_recovery_decision.has_value() &&
        delayed_b_recovery_identity_valid && opt_in_a_exit_requested &&
        opt_in_b_crash_requested && delayed_a_crash_requested &&
        delayed_b_stop_written && recovery_parked && old_control_completed &&
        delayed_callbacks.load(std::memory_order_acquire) == 1 &&
        opt_in_b_recovery_absent && releases_complete &&
        exact_survivors_absent && finalized;
    const bool opt_in_isolated =
        opt_in_b_recovery_decision.has_value()                   &&
        !*opt_in_b_recovery_decision                             &&
        inherited_callbacks.load(std::memory_order_acquire) == 0 &&
        opt_in_b_status.admitted_occurrences == 1;
    const bool stale_control_isolated =
        delayed_b_status.admitted_occurrences == 1                   &&
        delayed_b_status.created_occurrences == 1                    &&
        !delayed_b_has_extra_occurrence                              &&
        !delayed_b_recovery_identity                                 &&
        !fs::exists(marker(shared_path, k_delayed_b_recovery_marker));

    if (setup_valid && opt_in_isolated && stale_control_isolated) {
        std::fprintf(stdout,
            "RECOVERY_AUTHORITY_REUSE_GREEN opt_in_scheduled=0 "
            "opt_in_callbacks=0 "
            "opt_in_b_occurrences=1 delayed_callbacks=1 "
            "delayed_b_occurrences=1 delayed_recovery=0 survivors=0\n");
        std::fflush(stdout);
        return 0;
    }

    const bool opt_in_red =
        opt_in_b_recovery_decision.has_value()    &&
        *opt_in_b_recovery_decision               &&
        opt_in_b_status.admitted_occurrences == 1;
    const bool stale_control_red =
        delayed_b_has_extra_occurrence    &&
        delayed_b_recovery_identity_valid &&
        delayed_b_recovery_absent;
    if (setup_valid && opt_in_red && stale_control_red) {
        std::fprintf(stderr,
            "RECOVERY_AUTHORITY_REUSE_RED opt_in_scheduled=1 "
            "opt_in_callbacks=%u "
            "opt_in_b_occurrences=%zu delayed_callbacks=%u "
            "delayed_b_occurrences=%zu delayed_b_created=%zu "
            "delayed_recovery_marker=%d survivors=0\n",
            inherited_callbacks.load(std::memory_order_relaxed),
            opt_in_b_status.admitted_occurrences,
            delayed_callbacks.load(std::memory_order_relaxed),
            delayed_b_status.admitted_occurrences,
            delayed_b_status.created_occurrences,
            delayed_b_recovery_identity ? 1 : 0);
        std::fflush(stderr);
        return 2;
    }

    std::fprintf(stderr,
        "RECOVERY_AUTHORITY_REUSE_INVALID setup=%d opt_in_a=%d opt_in_b=%d "
        "delayed_a=%d delayed_b=%d opt_in_a_exit=%d opt_in_b_crash=%d "
        "delayed_a_crash=%d delayed_b_stop=%d recovery_parked=%d "
        "old_control_complete=%d opt_in_decision=%d opt_in_scheduled=%d "
        "opt_in_callbacks=%u opt_in_occurrences=%zu "
        "opt_in_recovery_absent=%d delayed_callbacks=%u "
        "delayed_occurrences=%zu delayed_created=%zu delayed_extra=%d "
        "delayed_recovery_identity=%d delayed_recovery_identity_valid=%d "
        "releases=%d survivors_absent=%d opt_in_retired=%d "
        "delayed_retired=%d finalized=%d\n",
        setup_valid ? 1 : 0,
        opt_in_a_identity_valid ? 1 : 0,
        opt_in_b_identity_valid ? 1 : 0,
        delayed_a_identity_valid ? 1 : 0,
        delayed_b_identity_valid ? 1 : 0,
        opt_in_a_exit_requested ? 1 : 0,
        opt_in_b_crash_requested ? 1 : 0,
        delayed_a_crash_requested ? 1 : 0,
        delayed_b_stop_written ? 1 : 0,
        recovery_parked ? 1 : 0,
        old_control_completed ? 1 : 0,
        opt_in_b_recovery_decision.has_value() ? 1 : 0,
        opt_in_b_recovery_decision.value_or(false) ? 1 : 0,
        inherited_callbacks.load(std::memory_order_relaxed),
        opt_in_b_status.admitted_occurrences,
        opt_in_b_recovery_absent ? 1 : 0,
        delayed_callbacks.load(std::memory_order_relaxed),
        delayed_b_status.admitted_occurrences,
        delayed_b_status.created_occurrences,
        delayed_b_has_extra_occurrence ? 1 : 0,
        delayed_b_recovery_identity ? 1 : 0,
        delayed_b_recovery_identity_valid ? 1 : 0,
        releases_complete ? 1 : 0,
        exact_survivors_absent ? 1 : 0,
        opt_in_a_retired ? 1 : 0,
        delayed_a_retired ? 1 : 0,
        finalized ? 1 : 0);
    std::fflush(stderr);
    return 3;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_RECOVERY_AUTHORITY_REUSE_DIR",
        "managed_child_recovery_authority_reuse");
    if (sintra::test::has_argv_flag(argc, argv, k_opt_in_a_flag) ||
        sintra::test::has_argv_flag(argc, argv, k_opt_in_b_flag) ||
        sintra::test::has_argv_flag(argc, argv, k_delayed_a_flag) ||
        sintra::test::has_argv_flag(argc, argv, k_delayed_b_flag))
    {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared.path());
}
