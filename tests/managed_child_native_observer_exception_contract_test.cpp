// Licensed under the BSD 2-Clause License, see LICENSE.md file for details.

#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using sintra::test::managed_child::process_identity_t;
using sintra::test::managed_child::read_process_identity;
using sintra::test::managed_child::Scoped_test_hook;
using sintra::test::managed_child::exact_process_is_live;
using sintra::test::managed_child::write_complete_file;
using sintra::test::managed_child::write_process_identity;

constexpr std::string_view k_child_flag =
    "--managed-child-native-observer-exception-child";
constexpr std::string_view k_case_flag = "--observer-case";
constexpr std::string_view k_nonce_flag = "--observer-nonce";
constexpr auto k_timeout = 10s;
constexpr std::uint32_t k_high_bit_exit_status = 0xc0000005u;

struct Ready_target : sintra::Derived_transceiver<Ready_target>
{};

struct Failure_plan
{
    const char*              stage = nullptr;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    std::atomic<unsigned>    hits{0};
};

Failure_plan* s_failure_plan = nullptr;

bool inject_observer_failure(
    const char*              stage,
    sintra::instance_id_type process_iid,
    uint32_t                 occurrence) noexcept
{
    auto* plan = s_failure_plan;
    if (!plan || !stage || occurrence != 0 ||
        plan->hits.load(std::memory_order_relaxed) != 0 ||
        process_iid != plan->process_iid ||
        std::string_view(stage) != plan->stage)
    {
        return false;
    }
    plan->hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

struct Observer_gate
{
    std::mutex               mutex;
    std::condition_variable  changed;
    sintra::instance_id_type process_iid = sintra::invalid_instance_id;
    unsigned                 before_wait = 0;
    unsigned                 fallback_available = 0;
    unsigned                 handle_closed = 0;
    unsigned                 observer_registered = 0;
    bool                     cancel_before_registration = false;
    bool                     observer_cancelled = false;
    bool                     registration_complete = false;
    bool                     release_observer = false;
};

Observer_gate* s_observer_gate = nullptr;

void observe_observer(
    const char*              stage,
    sintra::instance_id_type process_iid,
    uint32_t                 occurrence)
{
    auto* gate = s_observer_gate;
    if (!gate || !stage || occurrence != 0 ||
        process_iid != gate->process_iid)
    {
        return;
    }

    std::unique_lock<std::mutex> lock(gate->mutex);
    const std::string_view observed(stage);
    if (observed ==
        sintra::detail::test_hooks::k_managed_child_native_observer_before_wait)
    {
        ++gate->before_wait;
        gate->changed.notify_all();
        if (!gate->cancel_before_registration) {
            gate->changed.wait(lock, [&]() { return gate->release_observer; });
        }
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_after_cancel)
    {
        gate->observer_cancelled = true;
        gate->changed.notify_all();
        if (gate->cancel_before_registration) {
            gate->changed.wait(lock, [&]() {
                return gate->registration_complete;
            });
        }
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_before_registration)
    {
        if (gate->cancel_before_registration) {
            gate->changed.wait(lock, [&]() {
                return gate->observer_cancelled;
            });
        }
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_registered)
    {
        ++gate->observer_registered;
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_registration_complete)
    {
        gate->registration_complete = true;
        gate->changed.notify_all();
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_native_observer_fallback_available)
    {
        ++gate->fallback_available;
        gate->changed.notify_all();
    }
    else if (observed == sintra::detail::test_hooks::
            k_managed_child_windows_fallback_handle_closed ||
        observed == sintra::detail::test_hooks::
            k_managed_child_cleanup_native_exit_confirmed)
    {
        ++gate->handle_closed;
        gate->changed.notify_all();
    }
}

fs::path case_file(
    const fs::path& directory,
    std::string_view stem,
    unsigned case_number)
{
    return directory /
        (std::string(stem) + '_' + std::to_string(case_number) + ".complete");
}

process_identity_t read_identity(const fs::path& path)
{
    return read_process_identity(path).value_or(process_identity_t{});
}

int run_child(
    int argc,
    char* argv[],
    const fs::path& shared_directory)
{
    const auto case_text = sintra::test::get_argv_value(argc, argv, k_case_flag);
    const auto nonce = sintra::test::get_argv_value(argc, argv, k_nonce_flag);
    if (case_text.empty() || nonce.empty()) {
        return 2;
    }
    const unsigned case_number = static_cast<unsigned>(std::stoul(case_text));

    sintra::init(argc, argv);
    Ready_target ready;
    if (!ready.assign_name("native_observer_ready_" + nonce)) {
        return 2;
    }
    if (!write_process_identity(
            case_file(shared_directory, "identity", case_number)))
    {
        return 2;
    }
    if (!sintra::test::wait_for_file(
            case_file(shared_directory, "exit", case_number), k_timeout, 10ms))
    {
        return 3;
    }
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), k_high_bit_exit_status);
    return 0;
#else
    return 0;
#endif
}

struct Case_result
{
    bool setup = false;
    bool failure_injected = false;
    bool typed_failure = false;
    bool fallback_available = false;
    bool first_finalize_incomplete = false;
    bool release_complete = false;
    bool exact_exit_once = false;
    bool handle_closed_once = false;
    bool survivor_absent = false;
    bool registration_ordered = false;
    bool failure_ordered = false;
    bool exit_observation = false;
    bool callback_reentry = false;

    bool passed() const noexcept
    {
        return setup && failure_injected && typed_failure &&
            fallback_available && release_complete && exact_exit_once &&
            handle_closed_once && survivor_absent && registration_ordered &&
            failure_ordered && exit_observation && callback_reentry;
    }
};

Case_result run_case(
    const std::string& binary_path,
    const fs::path&   shared_directory,
    unsigned          case_number,
    const char*       failure_stage,
    bool              verify_finalize_retry,
    bool              cancel_before_registration = false,
    bool              reenter_release = false,
    bool              reenter_terminate = false,
    bool              exit_before_failure = false)
{
    Case_result result;
    const auto process_iid = sintra::compose_instance(61u + case_number, 1ull);
    const std::string nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto identity_path =
        case_file(shared_directory, "identity", case_number);
    const auto exit_path = case_file(shared_directory, "exit", case_number);
    std::error_code error;
    fs::remove(identity_path, error);
    fs::remove(exit_path, error);

    Observer_gate gate;
    gate.process_iid = process_iid;
    gate.cancel_before_registration = cancel_before_registration;
    s_observer_gate = &gate;
    Scoped_test_hook cleanup_hook(
        sintra::detail::test_hooks::s_managed_child_cleanup,
        &observe_observer);

    Failure_plan failure_plan;
    failure_plan.stage = failure_stage;
    failure_plan.process_iid = process_iid;
    s_failure_plan = &failure_plan;
    Scoped_test_hook failure_hook(
        sintra::detail::test_hooks::s_managed_child_failure,
        &inject_observer_failure);

    sintra::Spawn_options options;
    options.binary_path = binary_path;
    options.args = {
        std::string(k_child_flag),
        std::string(k_case_flag),
        std::to_string(case_number),
        std::string(k_nonce_flag),
        nonce};
    options.process_instance_id = process_iid;
    options.readiness_instance_name = "native_observer_ready_" + nonce;
    options.lifetime.enable_lifeline = false;

    auto custody = sintra::spawn_swarm_process(options);
    const auto ready = custody.wait_for_readiness_until(
        std::chrono::steady_clock::now() + 5s);
    const bool identity_seen = sintra::test::wait_for_file(
        identity_path, 5s, 10ms);
    const auto identity = read_identity(identity_path);
#ifdef _WIN32
    HANDLE child_handle = identity.pid > 0
        ? OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(identity.pid))
        : nullptr;
#endif
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.setup = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.before_wait == 1 &&
                (!cancel_before_registration || gate.registration_complete);
        });
        result.registration_ordered = !cancel_before_registration ||
            gate.observer_registered == 0;
        if (exit_before_failure) {
            result.registration_ordered = gate.registration_complete &&
                gate.observer_registered == 1;
        }
    }
    result.setup = result.setup && custody && identity_seen &&
        identity.pid > 0 && identity.start_stamp != 0 &&
        ready.readiness_state == sintra::Managed_child_readiness_state::reached &&
        exact_process_is_live(identity.pid, identity.start_stamp)
#ifdef _WIN32
        && child_handle && WaitForSingleObject(child_handle, 0) == WAIT_TIMEOUT
#endif
        ;

    std::mutex exit_mutex;
    std::condition_variable exit_changed;
    unsigned exit_callback_count = 0;
    sintra::Managed_child_exit exit_event;
    auto exit_observation = custody.observe_latest_created_exit(
        [&](const sintra::Managed_child_exit& event) {
            sintra::Managed_child_status callback_status;
            // Fallback delivery must not run on the release worker awaited here.
            if (reenter_terminate) {
                callback_status = custody.terminate_until(
                    std::chrono::steady_clock::now() + 1500ms);
            }
            else if (reenter_release) {
                callback_status = custody.release_until(
                    std::chrono::steady_clock::now() + 1500ms);
            }
            std::lock_guard<std::mutex> lock(exit_mutex);
            exit_event = event;
            ++exit_callback_count;
            result.callback_reentry =
                (!reenter_release && !reenter_terminate) ||
                callback_status.release_state ==
                    sintra::Managed_child_release_state::complete;
            exit_changed.notify_all();
        });
    result.setup = result.setup && static_cast<bool>(exit_observation);

    sintra::Managed_child_status passive;
    std::thread passive_caller([&]() {
        passive = custody.release_until(
            std::chrono::steady_clock::now() + 12s);
    });
    if (verify_finalize_retry) {
        const auto begin = std::chrono::steady_clock::now();
        const bool first_finalize = sintra::detail::finalize();
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        result.first_finalize_incomplete = !first_finalize && custody &&
            elapsed < 2s;
    }
    bool exit_requested = false;
    bool child_exited = false;
#ifdef _WIN32
    DWORD exit_code = STILL_ACTIVE;
    if (exit_before_failure) {
        exit_requested = write_complete_file(exit_path, "exit\n");
        child_exited = child_handle &&
            WaitForSingleObject(child_handle, 5000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(child_handle, &exit_code) != 0 &&
            exit_code != STILL_ACTIVE;
        result.failure_ordered = exit_requested && child_exited &&
            exit_code == k_high_bit_exit_status;
    }
#else
    result.failure_ordered = !exit_before_failure;
#endif
    if (!exit_before_failure) {
        result.failure_ordered = true;
    }
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release_observer = true;
        gate.changed.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.fallback_available = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.fallback_available == 1;
        });
    }
    const auto failed = custody.status();
    result.failure_injected =
        failure_plan.hits.load(std::memory_order_relaxed) == 1;
    result.typed_failure = failed.last_failure.kind ==
        sintra::Managed_child_failure_kind::native_observer &&
        failed.created_occurrences == 1 &&
        (exit_before_failure || failed.exited_occurrences == 0);

    if (!exit_before_failure) {
        exit_requested = write_complete_file(exit_path, "exit\n");
    }
    const auto terminated = custody.terminate_until(
        std::chrono::steady_clock::now() + k_timeout);
    passive_caller.join();
#ifdef _WIN32
    if (!child_exited) {
        child_exited = child_handle &&
            WaitForSingleObject(child_handle, 5000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(child_handle, &exit_code) != 0 &&
            exit_code != STILL_ACTIVE;
    }
#endif
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        result.handle_closed_once = gate.changed.wait_for(lock, 2s, [&]() {
            return gate.handle_closed == 1;
        });
        result.handle_closed_once = result.handle_closed_once &&
            gate.handle_closed == 1;
        result.fallback_available = result.fallback_available &&
            gate.fallback_available == 1;
    }
    const auto complete = custody.status();
    result.release_complete = exit_requested && child_exited &&
        terminated.release_state == sintra::Managed_child_release_state::complete &&
        passive.release_state == sintra::Managed_child_release_state::complete;
    if (!result.release_complete) {
        std::fprintf(
            stderr,
            "NATIVE_OBSERVER_RELEASE_INVALID case=%u requested=%d child=%d "
            "terminated=%d passive=%d\n",
            case_number,
            exit_requested ? 1 : 0,
            child_exited ? 1 : 0,
            terminated.release_state ==
                    sintra::Managed_child_release_state::complete
                ? 1
                : 0,
            passive.release_state ==
                    sintra::Managed_child_release_state::complete
                ? 1
                : 0);
    }
    result.exact_exit_once = complete.created_occurrences == 1 &&
        complete.exited_occurrences == 1;
    {
        std::unique_lock<std::mutex> lock(exit_mutex);
        (void)exit_changed.wait_for(lock, 2s, [&]() {
            return exit_callback_count != 0;
        });
        bool exit_status_valid = false;
#ifdef _WIN32
        exit_status_valid = exit_event.status_kind ==
                sintra::Managed_child_exit_status_kind::exited &&
            exit_event.native_status_available &&
            exit_event.status == exit_code &&
            exit_event.native_status == exit_code &&
            exit_code == k_high_bit_exit_status;
#endif
        result.exit_observation = exit_callback_count == 1 &&
            exit_event.occurrence == exit_observation.occurrence &&
            exit_event.occurrence.process_instance_id == process_iid &&
            exit_event.occurrence.occurrence == 0 &&
            exit_status_valid;
    }
    exit_observation.subscription.unsubscribe();
    result.survivor_absent = !exact_process_is_live(
        identity.pid, identity.start_stamp);

#ifdef _WIN32
    if (child_handle) {
        CloseHandle(child_handle);
    }
#endif
    s_failure_plan = nullptr;
    s_observer_gate = nullptr;
    fs::remove(identity_path, error);
    fs::remove(exit_path, error);
    return result;
}

int run_root(
    int argc,
    char* argv[],
    const fs::path& shared_directory)
{
#ifndef _WIN32
    (void)argc;
    (void)argv;
    (void)shared_directory;
    std::printf("NATIVE_OBSERVER_EXCEPTION_NOT_APPLICABLE\n");
    return 0;
#else
    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    sintra::init(argc, argv);
    const char* failure_stages[] = {
        sintra::detail::test_hooks::
            k_managed_child_fail_native_observer_after_registration,
        sintra::detail::test_hooks::
            k_managed_child_fail_native_observer_before_wait,
        sintra::detail::test_hooks::
            k_managed_child_fail_native_observer_after_registration,
        sintra::detail::test_hooks::
            k_managed_child_fail_native_observer_after_registration,
        sintra::detail::test_hooks::k_managed_child_fail_native_observer_wait};
    Case_result results[5];
    for (unsigned i = 0; i != 5; ++i) {
        results[i] = run_case(
            binary_path,
            shared_directory,
            i,
            failure_stages[i],
            i == 4,
            i == 2,
            i == 0,
            i == 1,
            i == 3);
        if (!results[i].passed() ||
            (i == 4 && !results[i].first_finalize_incomplete))
        {
            std::fprintf(
                stderr,
                "NATIVE_OBSERVER_EXCEPTION_INVALID case=%u setup=%d "
                "injected=%d typed=%d fallback=%d finalize=%d release=%d "
                "exit=%d close=%d survivor_absent=%d registration=%d "
                "failure_order=%d observation=%d callback_reentry=%d\n",
                i,
                results[i].setup ? 1 : 0,
                results[i].failure_injected ? 1 : 0,
                results[i].typed_failure ? 1 : 0,
                results[i].fallback_available ? 1 : 0,
                results[i].first_finalize_incomplete ? 1 : 0,
                results[i].release_complete ? 1 : 0,
                results[i].exact_exit_once ? 1 : 0,
                results[i].handle_closed_once ? 1 : 0,
                results[i].survivor_absent ? 1 : 0,
                results[i].registration_ordered ? 1 : 0,
                results[i].failure_ordered ? 1 : 0,
                results[i].exit_observation ? 1 : 0,
                results[i].callback_reentry ? 1 : 0);
            return 2;
        }
    }

    sintra::deactivate_all_slots();
    const bool final_retry = sintra::detail::finalize();
    if (!final_retry) {
        std::fprintf(stderr, "NATIVE_OBSERVER_EXCEPTION_FINAL_RETRY_FAILED\n");
        return 2;
    }
    std::printf(
        "NATIVE_OBSERVER_EXCEPTION_GREEN cases=5 typed=1 fallback=1 "
        "terminate_retry=1 finalize_retry=1 close_once=1 survivor_absent=1\n");
    return 0;
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared(
        "SINTRA_NATIVE_OBSERVER_EXCEPTION_DIR",
        "managed_child_native_observer_exception_contract");
    try {
        if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
            return run_child(argc, argv, shared.path());
        }
        return run_root(argc, argv, shared.path());
    }
    catch (const std::exception& exception) {
        std::fprintf(stderr, "NATIVE_OBSERVER_EXCEPTION_THROW: %s\n", exception.what());
        return 2;
    }
}
