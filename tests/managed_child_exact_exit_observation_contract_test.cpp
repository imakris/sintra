//
// Exact-occurrence managed-child native-exit observation contract evidence.
//

#include <sintra/sintra.h>

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

namespace fs = std::filesystem;

using namespace std::chrono_literals;

constexpr const char* k_child_flag =
    "--managed_child_exact_exit_observation_child";
constexpr const char* k_reentrant_child_flag =
    "--managed_child_exact_exit_observation_reentrant_child";
constexpr const char* k_pid_0_file =
    "managed_child_exact_exit_observation_pid_0.txt";
constexpr const char* k_pid_1_file =
    "managed_child_exact_exit_observation_pid_1.txt";
constexpr const char* k_crash_0_file =
    "managed_child_exact_exit_observation_crash_0.txt";
constexpr std::uint32_t k_exit_code = 0xc0000005u;
constexpr sintra::instance_id_type k_child_process_iid =
    sintra::compose_instance(31u, 1ull);
constexpr sintra::instance_id_type k_missing_process_iid =
    sintra::compose_instance(32u, 1ull);
constexpr sintra::instance_id_type k_reentrant_process_iid =
    sintra::compose_instance(33u, 1ull);

std::atomic_bool s_fail_exit_dispatcher_start{false};
std::atomic_int s_forced_unavailable_hits{0};

bool fail_exit_dispatcher_start(
    const char* stage,
    sintra::instance_id_type,
    uint32_t) noexcept
{
    return stage &&
        std::string_view(stage) == "managed_child_exit_dispatcher_start" &&
        s_fail_exit_dispatcher_start.exchange(false, std::memory_order_acq_rel);
}

static_assert(std::is_same_v<
    decltype(sintra::Managed_child_exit{}.status), std::uint32_t>);
static_assert(std::is_same_v<
    decltype(sintra::Managed_child_exit{}.native_status), std::uint32_t>);

struct Callback_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    started = false;
    bool                    release = false;
    bool                    unsubscribe_started = false;
    bool                    unsubscribe_returned = false;
};

struct Registration_exit_gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    armed = false;
    bool                    registration_selected = false;
    bool                    native_exit_ready = false;
    bool                    release_registration = false;
};

Registration_exit_gate s_registration_exit_gate;

void registration_stage_callback(const char* stage)
{
    if (std::string_view(stage) !=
        sintra::detail::test_hooks::
            k_stage_observe_managed_child_exit_selected)
    {
        return;
    }

    std::unique_lock<std::mutex> lock(s_registration_exit_gate.mutex);
    if (!s_registration_exit_gate.armed) {
        return;
    }
    s_registration_exit_gate.registration_selected = true;
    s_registration_exit_gate.changed.notify_all();
    s_registration_exit_gate.changed.wait(lock, []() {
        return s_registration_exit_gate.release_registration;
    });
}

void native_exit_stage_callback(
    const char*                  stage,
    sintra::instance_id_type    process_instance_id,
    uint32_t                    occurrence)
{
    if (std::string_view(stage) !=
            sintra::detail::test_hooks::
                k_managed_child_native_exit_before_publication ||
        process_instance_id != k_child_process_iid || occurrence != 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(s_registration_exit_gate.mutex);
    s_registration_exit_gate.native_exit_ready = true;
    s_registration_exit_gate.changed.notify_all();
}

fs::path pid_path(const fs::path& shared_path, uint32_t occurrence)
{
    return shared_path / (occurrence == 0 ? k_pid_0_file : k_pid_1_file);
}

bool write_pid(const fs::path& path, int pid)
{
    return sintra::test::managed_child::write_complete_file(
        path,
        std::to_string(pid) + '\n');
}

bool force_reentrant_exit_status_unavailable(
    const char* stage,
    sintra::instance_id_type process_instance_id,
    uint32_t occurrence) noexcept
{
    if (!stage ||
        std::string_view(stage) != sintra::detail::test_hooks::
            k_managed_child_force_exit_status_unavailable ||
        process_instance_id != k_reentrant_process_iid || occurrence != 0)
    {
        return false;
    }
    s_forced_unavailable_hits.fetch_add(1, std::memory_order_release);
    return true;
}

int read_pid(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    int pid = -1;
    in >> pid;
    return pid;
}

bool terminate_child(int pid)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!process) {
        return false;
    }
    const bool terminated = TerminateProcess(process, k_exit_code) != 0;
    if (terminated) {
        (void)WaitForSingleObject(process, 5000);
    }
    CloseHandle(process);
    return terminated;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

bool expected_recovery_exit(const sintra::Managed_child_exit& event)
{
    if (!event.native_status_available) {
        return false;
    }
#ifdef _WIN32
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::exited &&
        event.status != 0 && event.native_status == event.status;
#else
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::signaled &&
        event.status == SIGABRT &&
        WIFSIGNALED(static_cast<int>(event.native_status)) &&
        WTERMSIG(static_cast<int>(event.native_status)) == SIGABRT;
#endif
}

bool expected_terminated_exit(const sintra::Managed_child_exit& event)
{
    if (!event.native_status_available) {
        return false;
    }
#ifdef _WIN32
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::exited &&
        event.status == k_exit_code && event.native_status == k_exit_code;
#else
    return event.status_kind ==
            sintra::Managed_child_exit_status_kind::signaled &&
        event.status == SIGKILL &&
        WIFSIGNALED(static_cast<int>(event.native_status)) &&
        WTERMSIG(static_cast<int>(event.native_status)) == SIGKILL;
#endif
}

bool unexpected_native_status_is_unavailable()
{
#ifdef _WIN32
    return true;
#else
    const auto native_status = static_cast<std::uint32_t>(
        (SIGSTOP << 8) | 0x7f);
    const auto event = sintra::detail::make_managed_child_exit(
        {k_child_process_iid, 99}, native_status, true);
    return WIFSTOPPED(static_cast<int>(native_status)) &&
        event.status_kind ==
            sintra::Managed_child_exit_status_kind::unavailable &&
        event.status == 0 && event.native_status_available &&
        event.native_status == native_status;
#endif
}

int run_child(
    int argc,
    char* argv[],
    const fs::path& shared_path)
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }

    const uint32_t occurrence = sintra::s_recovery_occurrence;
    if (occurrence == 0) {
        sintra::enable_recovery();
    }
    if (occurrence > 1 ||
        !write_pid(pid_path(shared_path, occurrence), sintra::test::get_pid()))
    {
        return 2;
    }
    if (occurrence == 0) {
        if (!sintra::test::wait_for_file(
                shared_path / k_crash_0_file, 30s, 10ms))
        {
            return 2;
        }
        sintra::disable_debug_pause_for_current_process();
        sintra::test::prepare_for_intentional_crash(
            "managed child exact exit occurrence 0");
        std::abort();
    }
    std::this_thread::sleep_for(30s);
    return 2;
}

int run_reentrant_child(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (...) {
        return 2;
    }
    return 0;
}

int run_root(
    int argc,
    char* argv[],
    const fs::path& shared_path)
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

    std::atomic_int recovery_requests{0};
    sintra::set_recovery_policy([&](const sintra::Crash_info&) {
        return recovery_requests.fetch_add(1, std::memory_order_acq_rel) == 0;
    });

    std::atomic_int no_occurrence_callback_count{0};
    sintra::Spawn_options missing_options;
    missing_options.binary_path =
        (shared_path / "missing-managed-child.exe").string();
    missing_options.process_instance_id = k_missing_process_iid;
    missing_options.lifetime.enable_lifeline = false;
    auto missing_custody = sintra::spawn_swarm_process(missing_options);
    auto missing_observation = missing_custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            no_occurrence_callback_count.fetch_add(1, std::memory_order_release);
        });
    const auto missing_status = missing_custody.status();
    const auto missing_released = missing_custody.release_until(
        std::chrono::steady_clock::now() + 5s);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {k_child_flag};
    options.process_instance_id = k_child_process_iid;
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);

    std::atomic_int rejected_callback_count{0};
    sintra::Managed_child_exit_observation rejected_observation;
    {
        s_fail_exit_dispatcher_start.store(true, std::memory_order_release);
        sintra::test::managed_child::Scoped_test_hook dispatcher_start_hook(
            sintra::detail::test_hooks::s_managed_child_failure,
            &fail_exit_dispatcher_start);
        rejected_observation = custody.observe_latest_created_exit(
            [&](const sintra::Managed_child_exit&) {
                rejected_callback_count.fetch_add(1, std::memory_order_release);
            });
    }
    s_forced_unavailable_hits.store(0, std::memory_order_release);
    sintra::test::managed_child::Scoped_test_hook unavailable_status_hook(
        sintra::detail::test_hooks::s_managed_child_failure,
        &force_reentrant_exit_status_unavailable);

    std::mutex event_mutex;
    std::condition_variable event_changed;
    sintra::Managed_child_exit observed_event;
    sintra::Managed_child_exit_observation observation;
    std::thread::id registering_thread;
    std::thread::id observed_thread;
    int observed_count = 0;
    sintra::Managed_child_custody reentrant_custody;

    std::atomic_int cancelled_count{0};
    auto cancelled_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            cancelled_count.fetch_add(1, std::memory_order_release);
        });
    const bool cancelled_observation_registered =
        static_cast<bool>(cancelled_observation);
    cancelled_observation.subscription.unsubscribe();

    std::atomic_int self_unsubscribe_count{0};
    sintra::Managed_child_exit_observation self_observation;
    self_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            self_unsubscribe_count.fetch_add(1, std::memory_order_release);
            self_observation.subscription.unsubscribe();
        });

    std::atomic_int throwing_count{0};
    auto throwing_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            throwing_count.fetch_add(1, std::memory_order_release);
            throw std::runtime_error("intentional exit callback failure");
        });

    Callback_gate callback_gate;
    auto blocking_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            std::unique_lock<std::mutex> lock(callback_gate.mutex);
            callback_gate.started = true;
            callback_gate.changed.notify_all();
            callback_gate.changed.wait(lock, [&]() {
                return callback_gate.release;
            });
        });

    const bool self_observation_registered = static_cast<bool>(self_observation);
    const bool throwing_observation_registered =
        static_cast<bool>(throwing_observation);

    const fs::path occurrence_0_pid_path = pid_path(shared_path, 0);
    const bool pid_seen = sintra::test::wait_for_file(
        occurrence_0_pid_path,
        5s,
        10ms);
    const int pid = pid_seen ? read_pid(occurrence_0_pid_path) : -1;

    sintra::test::managed_child::Scoped_test_hook registration_hook(
        sintra::detail::test_hooks::s_runtime_stage,
        &registration_stage_callback);
    sintra::test::managed_child::Scoped_test_hook native_exit_hook(
        sintra::detail::test_hooks::s_managed_child_cleanup,
        &native_exit_stage_callback);
    {
        std::lock_guard<std::mutex> lock(s_registration_exit_gate.mutex);
        s_registration_exit_gate.armed = true;
    }
    std::thread registration_thread([&]() {
        registering_thread = std::this_thread::get_id();
        observation = custody.observe_latest_created_exit(
            [&](const sintra::Managed_child_exit& event) {
                // Live delivery must not inherit native-exit custody locks.
                sintra::Spawn_options reentrant_options;
                reentrant_options.binary_path = binary_path;
                reentrant_options.args = {k_reentrant_child_flag};
                reentrant_options.process_instance_id = k_reentrant_process_iid;
                reentrant_options.lifetime.enable_lifeline = false;
                auto spawned = sintra::spawn_swarm_process(reentrant_options);
                std::lock_guard<std::mutex> lock(event_mutex);
                reentrant_custody = std::move(spawned);
                observed_event = event;
                observed_thread = std::this_thread::get_id();
                ++observed_count;
                event_changed.notify_all();
            });
    });
    bool registration_selected = false;
    {
        std::unique_lock<std::mutex> lock(s_registration_exit_gate.mutex);
        registration_selected = s_registration_exit_gate.changed.wait_for(
            lock,
            5s,
            []() {
                return s_registration_exit_gate.registration_selected;
            });
    }
    const bool terminated = registration_selected && pid > 0 &&
        write_pid(shared_path / k_crash_0_file, 1);
    bool native_exit_ready = false;
    {
        std::unique_lock<std::mutex> lock(s_registration_exit_gate.mutex);
        native_exit_ready = s_registration_exit_gate.changed.wait_for(
            lock,
            5s,
            []() { return s_registration_exit_gate.native_exit_ready; });
        s_registration_exit_gate.release_registration = true;
    }
    s_registration_exit_gate.changed.notify_all();
    registration_thread.join();
    registration_hook.restore();
    native_exit_hook.restore();
    const bool observation_registered = static_cast<bool>(observation);

    bool callback_started = false;
    {
        std::unique_lock<std::mutex> lock(callback_gate.mutex);
        callback_started = callback_gate.changed.wait_for(
            lock,
            5s,
            [&]() { return callback_gate.started; });
    }

    std::thread unsubscribe_thread([&]() {
        {
            std::lock_guard<std::mutex> lock(callback_gate.mutex);
            callback_gate.unsubscribe_started = true;
        }
        callback_gate.changed.notify_all();
        blocking_observation.subscription.unsubscribe();
        {
            std::lock_guard<std::mutex> lock(callback_gate.mutex);
            callback_gate.unsubscribe_returned = true;
        }
        callback_gate.changed.notify_all();
    });
    bool unsubscribe_started = false;
    bool returned_before_release = false;
    {
        std::unique_lock<std::mutex> lock(callback_gate.mutex);
        unsubscribe_started = callback_gate.changed.wait_for(
            lock,
            5s,
            [&]() { return callback_gate.unsubscribe_started; });
        if (unsubscribe_started) {
            returned_before_release = callback_gate.changed.wait_for(
                lock,
                100ms,
                [&]() { return callback_gate.unsubscribe_returned; });
        }
        callback_gate.release = true;
    }
    callback_gate.changed.notify_all();
    unsubscribe_thread.join();
    const bool unsubscribe_waited = unsubscribe_started &&
        !returned_before_release && callback_gate.unsubscribe_returned;

    bool callback_observed = false;
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        callback_observed = event_changed.wait_for(
            lock,
            5s,
            [&]() { return observed_count == 1; });
    }

    std::mutex unavailable_mutex;
    std::condition_variable unavailable_changed;
    sintra::Managed_child_exit unavailable_event;
    int unavailable_count = 0;
    auto unavailable_observation =
        reentrant_custody.observe_latest_created_exit(
            [&](const sintra::Managed_child_exit& event) {
                std::lock_guard<std::mutex> lock(unavailable_mutex);
                unavailable_event = event;
                ++unavailable_count;
                unavailable_changed.notify_all();
            });
    bool unavailable_observed = false;
    {
        std::unique_lock<std::mutex> lock(unavailable_mutex);
        unavailable_observed = unavailable_changed.wait_for(
            lock,
            5s,
            [&]() { return unavailable_count == 1; });
    }
    unavailable_status_hook.restore();
    const bool unavailable_delivery = unavailable_observation &&
        unavailable_observed && unavailable_count == 1 &&
        unavailable_observation.occurrence.process_instance_id ==
            k_reentrant_process_iid &&
        unavailable_event.occurrence == unavailable_observation.occurrence &&
        unavailable_event.status_kind ==
            sintra::Managed_child_exit_status_kind::unavailable &&
        unavailable_event.status == 0 &&
        !unavailable_event.native_status_available &&
        s_forced_unavailable_hits.load(std::memory_order_acquire) == 1;

    const fs::path occurrence_1_pid_path = pid_path(shared_path, 1);
    const bool replacement_pid_seen = sintra::test::wait_for_file(
        occurrence_1_pid_path,
        10s,
        10ms);
    const int replacement_pid = replacement_pid_seen
        ? read_pid(occurrence_1_pid_path)
        : -1;
    std::mutex replacement_mutex;
    std::condition_variable replacement_changed;
    sintra::Managed_child_exit replacement_event;
    int replacement_count = 0;
    auto replacement_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            std::lock_guard<std::mutex> lock(replacement_mutex);
            replacement_event = event;
            ++replacement_count;
            replacement_changed.notify_all();
        });
    const bool replacement_observation_registered =
        static_cast<bool>(replacement_observation);
    const bool replacement_selected_latest = replacement_observation_registered &&
        replacement_observation.occurrence.process_instance_id ==
            k_child_process_iid &&
        replacement_observation.occurrence.occurrence == 1 &&
        replacement_observation.occurrence != observation.occurrence;
    const bool replacement_terminated = replacement_pid > 0 &&
        replacement_pid != pid && terminate_child(replacement_pid);
    bool replacement_observed = false;
    {
        std::unique_lock<std::mutex> lock(replacement_mutex);
        replacement_observed = replacement_changed.wait_for(
            lock,
            5s,
            [&]() { return replacement_count == 1; });
    }
    bool no_child_setup_failed = false;
    std::shared_ptr<sintra::detail::Managed_child_custody_record>
        no_child_custody_record;
    {
        const auto current = sintra::s_mproc->child_custody_occurrence_token(
            k_child_process_iid);
        auto record = current.custody.lock();
        if (record) {
            no_child_custody_record = record;
            sintra::Managed_process::Spawn_swarm_process_args failed_args;
            failed_args.binary_name =
                (shared_path / "missing-newer-occurrence.exe").string();
            failed_args.args = {failed_args.binary_name};
            failed_args.piid = k_child_process_iid;
            failed_args.occurrence = 2;
            failed_args.custody = record;
            failed_args.lifetime.enable_lifeline = false;
            auto failed_attempt =
                sintra::s_mproc->admit_child_custody_occurrence(
                    record, k_child_process_iid, 2);
            const auto failed_result = sintra::s_mproc->spawn_swarm_process(
                failed_args, failed_attempt);
            no_child_setup_failed = !failed_result.success &&
                !failed_result.os_process_created;
        }
    }
    bool newer_no_child_seen = false;
    const auto newer_no_child_deadline =
        std::chrono::steady_clock::now() + 5s;
    do {
        const auto status = custody.status();
        newer_no_child_seen = status.admitted_occurrences == 3 &&
            status.created_occurrences == 2 &&
            status.exited_occurrences == 2;
        if (!newer_no_child_seen) {
            std::this_thread::sleep_for(10ms);
        }
    } while (!newer_no_child_seen &&
        std::chrono::steady_clock::now() < newer_no_child_deadline);
    newer_no_child_seen = no_child_setup_failed && newer_no_child_seen;

    std::mutex replay_mutex;
    std::condition_variable replay_changed;
    sintra::Managed_child_exit replay_event;
    std::thread::id replay_thread;
    const std::thread::id replay_registering_thread =
        std::this_thread::get_id();
    int replay_count = 0;
    auto replay_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            std::lock_guard<std::mutex> lock(replay_mutex);
            replay_event = event;
            replay_thread = std::this_thread::get_id();
            ++replay_count;
            replay_changed.notify_all();
        });
    const bool replay_observation_registered =
        static_cast<bool>(replay_observation);
    bool replay_observed = false;
    {
        std::unique_lock<std::mutex> lock(replay_mutex);
        replay_observed = replay_changed.wait_for(
            lock,
            5s,
            [&]() { return replay_count == 1; });
    }

    // The third occurrence is a test-only admitted no-child state. Retire that
    // synthetic admission after proving selection so normal public cleanup
    // continues from the last real OS-created occurrence.
    if (newer_no_child_seen && no_child_custody_record) {
        {
            std::lock_guard<std::mutex> lock(
                sintra::s_mproc->m_child_custody_mutex);
            auto active = sintra::s_mproc->m_child_custody_by_process.find(
                k_child_process_iid);
            if (active !=
                sintra::s_mproc->m_child_custody_by_process.end())
            {
                active->second.occurrence = 1;
            }
        }
        std::lock_guard<std::mutex> lock(no_child_custody_record->mutex);
        auto& occurrences = no_child_custody_record->occurrences;
        occurrences.erase(
            std::remove_if(
                occurrences.begin(),
                occurrences.end(),
                [](const auto& candidate) {
                    return candidate.process_instance_id ==
                            k_child_process_iid &&
                        candidate.occurrence == 2;
                }),
            occurrences.end());
    }

    observation.subscription.unsubscribe();
    throwing_observation.subscription.unsubscribe();
    unavailable_observation.subscription.unsubscribe();
    replacement_observation.subscription.unsubscribe();
    replay_observation.subscription.unsubscribe();

    const auto reentrant_released = reentrant_custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const bool reentrant_spawn_valid = reentrant_custody &&
        reentrant_released.release_state ==
            sintra::Managed_child_release_state::complete;
    const auto released = custody.release_until(
        std::chrono::steady_clock::now() + 5s);
    const auto completed_status = custody.status();
    bool finalized = false;
    bool teardown_observation_empty = false;
    std::atomic_int teardown_callback_count{0};
    try {
        finalized = sintra::shutdown(sintra::shutdown_options{
            .coordinator_shutdown_hook = [&]() {
                auto during_teardown = custody.observe_latest_created_exit(
                    [&](const sintra::Managed_child_exit&) {
                        teardown_callback_count.fetch_add(
                            1, std::memory_order_release);
                    });
                teardown_observation_empty = !during_teardown;
            }});
    }
    catch (...) {
    }
    auto after_shutdown_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit&) {
            teardown_callback_count.fetch_add(1, std::memory_order_release);
        });

    const bool identity_valid = observation_registered &&
        observation.occurrence.process_instance_id == k_child_process_iid &&
        observation.occurrence.occurrence == 0 &&
        observed_event.occurrence == observation.occurrence &&
        replacement_event.occurrence == replacement_observation.occurrence &&
        replay_observation.occurrence == replacement_observation.occurrence &&
        replay_event.occurrence == replacement_observation.occurrence;
    const bool valid = missing_custody && !missing_observation &&
        unexpected_native_status_is_unavailable() &&
        missing_status.created_occurrences == 0 &&
        missing_released.release_state ==
            sintra::Managed_child_release_state::complete &&
        no_occurrence_callback_count.load(std::memory_order_acquire) == 0 &&
        !rejected_observation &&
        rejected_callback_count.load(std::memory_order_acquire) == 0 &&
        custody && registration_selected && native_exit_ready &&
        observation_registered && cancelled_observation_registered &&
        self_observation_registered &&
        throwing_observation_registered &&
        callback_started && unsubscribe_waited &&
        pid_seen && terminated && callback_observed && observed_count == 1 &&
        expected_recovery_exit(observed_event) && identity_valid &&
        observed_thread != registering_thread &&
        cancelled_count.load(std::memory_order_acquire) == 0 &&
        self_unsubscribe_count.load(std::memory_order_acquire) == 1 &&
        throwing_count.load(std::memory_order_acquire) == 1 &&
        unavailable_delivery &&
        replacement_pid_seen && replacement_selected_latest &&
        replacement_terminated && replacement_observed &&
        replacement_count == 1 && expected_terminated_exit(replacement_event) &&
        observed_count == 1 &&
        replay_observation_registered && replay_observed && replay_count == 1 &&
        expected_terminated_exit(replay_event) &&
        replay_thread != replay_registering_thread &&
        newer_no_child_seen &&
        reentrant_spawn_valid &&
        completed_status.admitted_occurrences == 2 &&
        completed_status.created_occurrences == 2 &&
        completed_status.exited_occurrences == 2 &&
        released.release_state ==
            sintra::Managed_child_release_state::complete &&
        finalized && teardown_observation_empty &&
        !after_shutdown_observation &&
        teardown_callback_count.load(std::memory_order_acquire) == 0;
    if (!valid) {
        std::fprintf(
            stderr,
            "MANAGED_CHILD_EXACT_EXIT_OBSERVATION_INVALID missing=%d "
            "missing_observation=%d missing_created=%zu missing_released=%d "
            "no_occurrence_count=%d rejected=%d rejected_count=%d custody=%d "
            "observation=%d identity=%d "
            "race_selected=%d exit_ready=%d pid_seen=%d terminated=%d "
            "callback_started=%d waited=%d "
            "callback=%d observed_count=%d expected_exit=%d off_thread=%d "
            "cancelled_registered=%d cancelled=%d self=%d throwing=%d "
            "replacement_pid=%d replacement_selected=%d replacement_exit=%d "
            "replacement_count=%d replay=%d replay_count=%d replay_exit=%d "
            "replay_off_thread=%d unavailable=%d newer_no_child=%d "
            "reentrant_spawn=%d admitted=%zu created=%zu exited=%zu "
            "released=%d finalized=%d teardown_empty=%d after_empty=%d "
            "teardown_count=%d\n",
            missing_custody ? 1 : 0,
            missing_observation ? 1 : 0,
            missing_status.created_occurrences,
            missing_released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            no_occurrence_callback_count.load(std::memory_order_acquire),
            rejected_observation ? 1 : 0,
            rejected_callback_count.load(std::memory_order_acquire),
            custody ? 1 : 0,
            observation_registered ? 1 : 0,
            identity_valid ? 1 : 0,
            registration_selected ? 1 : 0,
            native_exit_ready ? 1 : 0,
            pid_seen ? 1 : 0,
            terminated ? 1 : 0,
            callback_started ? 1 : 0,
            unsubscribe_waited ? 1 : 0,
            callback_observed ? 1 : 0,
            observed_count,
            expected_recovery_exit(observed_event) ? 1 : 0,
            observed_thread != registering_thread ? 1 : 0,
            cancelled_observation_registered ? 1 : 0,
            cancelled_count.load(std::memory_order_acquire),
            self_unsubscribe_count.load(std::memory_order_acquire),
            throwing_count.load(std::memory_order_acquire),
            replacement_pid_seen ? 1 : 0,
            replacement_selected_latest ? 1 : 0,
            expected_terminated_exit(replacement_event) ? 1 : 0,
            replacement_count,
            replay_observed ? 1 : 0,
            replay_count,
            expected_terminated_exit(replay_event) ? 1 : 0,
            replay_thread != replay_registering_thread ? 1 : 0,
            unavailable_delivery ? 1 : 0,
            newer_no_child_seen ? 1 : 0,
            reentrant_spawn_valid ? 1 : 0,
            completed_status.admitted_occurrences,
            completed_status.created_occurrences,
            completed_status.exited_occurrences,
            released.release_state ==
                sintra::Managed_child_release_state::complete ? 1 : 0,
            finalized ? 1 : 0,
            teardown_observation_empty ? 1 : 0,
            after_shutdown_observation ? 0 : 1,
            teardown_callback_count.load(std::memory_order_acquire));
    }
    return valid ? 0 : 1;
}

}

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_TEST_SHARED_DIR",
        "managed_child_exact_exit_observation_contract_test");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    if (sintra::test::has_argv_flag(argc, argv, k_reentrant_child_flag)) {
        return run_reentrant_child(argc, argv);
    }
    return run_root(argc, argv, shared.path());
}
