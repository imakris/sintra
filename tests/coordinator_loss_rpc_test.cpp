#include <sintra/sintra.h>

#include "external_process_invitation_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
constexpr const char* k_prefix = "coordinator_loss_rpc_test: ";
constexpr const char* k_role_arg = "--rpc_loss_role";
constexpr const char* k_directory_arg = "--rpc_loss_directory";
constexpr const char* k_owner_name = "coordinator loss RPC owner";
constexpr const char* k_unrelated_name = "coordinator loss unrelated owner";

bool check(bool value, const char* message)
{
    return sintra::test::assert_true(value, k_prefix, message);
}

void mark(const std::filesystem::path& directory, const std::string& name,
    const std::string& value = "ready")
{
    sintra::test::invitation::write_marker(k_prefix, directory, name, value);
}

bool await(const std::filesystem::path& directory, const std::string& name,
    const std::string& value = "ready")
{
    return sintra::test::invitation::wait_for_marker(
        k_prefix, directory, name, value, 8s);
}

struct Service : sintra::Derived_transceiver<Service>
{
    int echo(int value) { return value; }
    int strict_echo(int value) { return value; }

    int held(int value, std::string directory, std::string tag)
    {
        mark(directory, tag + "-entered");
        if (!await(directory, tag + "-release")) {
            throw std::runtime_error("test did not release held RPC");
        }
        mark(directory, tag + "-finished");
        return value;
    }

    SINTRA_RPC(echo)
    SINTRA_RPC_STRICT(strict_echo)
    SINTRA_RPC_STRICT(held)
};

// Inspect the existing outstanding registry without consuming the handle: the
// public state()/wait/get operations perform cleanup of a completed control.
bool outstanding_state(sintra::instance_id_type target, bool waiting)
{
    std::lock_guard outstanding_lock(sintra::s_outstanding_rpcs_mutex());
    for (auto* control : sintra::s_outstanding_rpcs()) {
        std::lock_guard state_lock(control->keep_waiting_mutex);
        if (control->remote_instance == target &&
            control->keep_waiting == waiting && !control->cleaned_up)
        {
            return true;
        }
    }
    return false;
}

template <typename Predicate>
bool until(Predicate predicate, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

bool expect_cancelled(sintra::Rpc_handle<int>& handle, const char* message)
{
    try {
        (void)handle.get_until(std::chrono::steady_clock::now() + 500ms);
    }
    catch (const sintra::rpc_cancelled&) {
        return true;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s%s: %s\n", k_prefix, message, error.what());
        return false;
    }
    return check(false, message);
}

struct Registration_gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool timed_out = false;
} registration_gate;

thread_local bool hold_registration = false;
thread_local bool observe_sync_wait = false;
std::atomic<bool> sync_wait_entered{false};

void before_registration(sintra::instance_id_type)
{
    if (!hold_registration) {
        return;
    }
    std::unique_lock lock(registration_gate.mutex);
    registration_gate.entered = true;
    registration_gate.changed.notify_all();
    registration_gate.timed_out = !registration_gate.changed.wait_for(
        lock, 8s, [] { return registration_gate.released; });
}

void on_sync_wait()
{
    if (observe_sync_wait) {
        sync_wait_entered.store(true, std::memory_order_release);
    }
}

bool run_caller(const std::filesystem::path& directory)
{
    bool ok = true;
    Service local;
    mark(directory, "caller-ready");
    if (!await(directory, "caller-begin")) {
        return false;
    }

    const auto target = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, k_owner_name);
    ok &= check(target != sintra::invalid_instance_id, "peer target must resolve");
    ok &= check(Service::rpc_echo(target, 11) == 11,
        "peer RPC must still succeed after unrelated process retirement");
    ok &= check(Service::rpc_strict_echo(local.instance_id(), 12) == 12,
        "strict local RPC must work before coordinator departure");

    auto completed = Service::rpc_async_echo(target, 37);
    ok &= check(until([&] { return outstanding_state(target, false); }),
        "completed reply must remain registered and unconsumed before stop");

    auto pending = Service::rpc_async_held(target, 41, directory.string(), "loss");
    ok &= await(directory, "loss-entered");

    std::atomic<bool> sync_done{false};
    bool sync_cancelled = false;
    sintra::detail::test_hooks::s_rpc_wait_pending.store(&on_sync_wait);
    std::thread synchronous([&] {
        observe_sync_wait = true;
        try {
            (void)Service::rpc_held(target, 43, directory.string(), "loss-sync");
        }
        catch (const sintra::rpc_cancelled&) {
            sync_cancelled = true;
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "%ssynchronous RPC: %s\n", k_prefix, error.what());
        }
        sync_done.store(true, std::memory_order_release);
    });
    ok &= check(until([] { return sync_wait_entered.load(std::memory_order_acquire); }),
        "synchronous peer caller must be registered and waiting before stop");

    bool late_cancelled = false;
    sintra::detail::test_hooks::s_rpc_outbound_request.store(&before_registration);
    std::thread late([&] {
        hold_registration = true;
        try {
            auto handle = Service::rpc_async_echo(target, 47);
            (void)handle; // Scope destruction abandons an unexpected registration.
        }
        catch (const sintra::rpc_cancelled&) {
            late_cancelled = true;
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "%slate RPC registration: %s\n", k_prefix, error.what());
        }
    });
    {
        std::unique_lock lock(registration_gate.mutex);
        ok &= check(registration_gate.changed.wait_for(
                lock, 3s, [] { return registration_gate.entered; }),
            "late caller must stop at the existing pre-registration hook");
    }

    mark(directory, "caller-pending");
    ok &= check(until([] {
        return sintra::s_mproc->communication_state() ==
            sintra::Managed_process::COMMUNICATION_STOPPED;
    }), "coordinator unpublication must stop the leaf transport");
    ok &= expect_cancelled(pending,
        "peer-targeted async wait must cancel before its deadline");
    const bool sync_cancelled_before_rescue = until(
        [&] { return sync_done.load(std::memory_order_acquire); }, 500ms);
    ok &= check(sync_cancelled_before_rescue,
        "synchronous peer wait must return while its remote handler is held");
    if (!sync_cancelled_before_rescue) {
        // Baseline failure must finish cleanly. This rescue happens only after
        // recording that the real stop path failed to cancel the synchronous RPC.
        sintra::s_mproc->unblock_rpc();
    }
    synchronous.join();
    sintra::detail::test_hooks::s_rpc_wait_pending.store(nullptr);
    ok &= check(sync_cancelled, "synchronous peer wait must report rpc_cancelled");
    ok &= check(completed.get() == 37,
        "an unconsumed completed reply must survive route cancellation");
    ok &= check(Service::rpc_echo(local.instance_id(), 53) == 53,
        "direct non-strict local RPC must remain satisfiable after stop");

    {
        std::lock_guard lock(registration_gate.mutex);
        registration_gate.released = true;
        registration_gate.changed.notify_all();
    }
    late.join();
    sintra::detail::test_hooks::s_rpc_outbound_request.store(nullptr);
    ok &= check(!registration_gate.timed_out && late_cancelled,
        "a caller crossing stop before registration must reject with rpc_cancelled");

    bool new_peer_cancelled = false;
    try {
        auto handle = Service::rpc_async_echo(target, 59);
        (void)handle;
    }
    catch (const sintra::rpc_cancelled&) {
        new_peer_cancelled = true;
    }
    ok &= check(new_peer_cancelled, "new peer RPC must reject at invocation after stop");

    bool strict_local_cancelled = false;
    try {
        auto handle = Service::rpc_async_strict_echo(local.instance_id(), 61);
        (void)handle;
    }
    catch (const sintra::rpc_cancelled&) {
        strict_local_cancelled = true;
    }
    ok &= check(strict_local_cancelled,
        "strict local RPC still needs the stopped coordinator transport");
    return ok;
}

int run_leaf(int argc, char* argv[], const std::string& role,
    const std::filesystem::path& directory)
{
    sintra::init(argc, argv);
    sintra::test::invitation::Runtime_guard runtime{true};
    bool ok = check(sintra::process_lifetime_detail::s_lifeline_handle_value.empty(),
        "external fixture must have no lifeline masking RPC cancellation");
    {
        if (role == "caller") {
            ok &= run_caller(directory);
            mark(directory, "caller-result", ok ? "passed" : "failed");
        }
        else {
            Service service;
            service.assign_name(role == "owner" ? k_owner_name : k_unrelated_name);
            mark(directory, role + "-ready");
            ok &= await(directory, role + "-exit");
            // The unrelated leaf was individually unpublished. Its own route
            // is explicitly stopped only for test-owned local cleanup.
            sintra::s_mproc->stop();
        }
    }
    const bool finalized = sintra::detail::finalize();
    runtime.active = !finalized;
    ok &= check(finalized && !sintra::s_mproc,
        "external leaf finalization must complete after transport stop");
    mark(directory, role + "-finalized", finalized ? "passed" : "failed");
    return ok ? 0 : 1;
}

bool launch(const std::string& binary, const std::filesystem::path& directory,
    const char* role, sintra::External_process_invitation& invitation,
    sintra::test::Exact_child& child)
{
    invitation = sintra::test::managed_child::make_invitation(
        sintra::invalid_instance_id, 20s);
    if (!check(bool(invitation), "external invitation must be created")) {
        return false;
    }
    std::vector<std::string> args{
        k_role_arg, role, k_directory_arg, directory.string()};
    const auto sintra_args = invitation.sintra_args();
    args.insert(args.end(), sintra_args.begin(), sintra_args.end());
    return check(sintra::test::invitation::launch_direct_process(binary, args, child),
        "exact external helper must launch");
}

bool run_coordinator(int argc, char* argv[])
{
    sintra::test::Shared_directory shared("SINTRA_RPC_LOSS_DIRECTORY", "coordinator_loss_rpc");
    const auto& directory = shared.path();
    sintra::init(argc, argv);
    sintra::test::invitation::Runtime_guard runtime{true};
    const std::string binary = sintra::test::get_binary_path(argc, argv);
    sintra::test::Exact_child owner(2s), caller(2s), unrelated(2s);
    sintra::External_process_invitation owner_invitation, caller_invitation, unrelated_invitation;
    if (!launch(binary, directory, "owner", owner_invitation, owner) ||
        !launch(binary, directory, "caller", caller_invitation, caller) ||
        !launch(binary, directory, "unrelated", unrelated_invitation, unrelated))
    {
        return false;
    }
    bool ok = await(directory, "owner-ready");
    ok &= await(directory, "caller-ready");
    ok &= await(directory, "unrelated-ready");

    const auto healthy_target = sintra::Coordinator::rpc_resolve_instance(
        sintra::s_coord_id, k_owner_name);
    auto healthy = Service::rpc_async_held(healthy_target, 17,
        directory.string(), "isolation");
    auto retiring = Service::rpc_async_held(k_unrelated_name, 19,
        directory.string(), "retiring");
    ok &= await(directory, "isolation-entered");
    ok &= await(directory, "retiring-entered");
    ok &= check(sintra::Coordinator::rpc_unpublish_transceiver(
        sintra::s_coord_id, unrelated_invitation.process_instance_id),
        "unrelated process must retire through its real coordinator reader");
    ok &= expect_cancelled(retiring, "retired leaf RPC must cancel");
    ok &= check(outstanding_state(healthy_target, true),
        "retiring one peer must preserve the other pending peer RPC");
    mark(directory, "isolation-release");
    ok &= check(healthy.get_until(std::chrono::steady_clock::now() + 3s) == 17,
        "unaffected peer reply must still be delivered");
    mark(directory, "retiring-release");
    mark(directory, "unrelated-exit");
    ok &= sintra::test::invitation::assert_clean_exit(
        k_prefix, unrelated, 5s, "unrelated external leaf must settle cleanly");

    mark(directory, "caller-begin");
    ok &= await(directory, "caller-pending");
    // Exercise the actual coordinator-unpublication notification and deferred
    // leaf stop. The coordinator stays alive only as the isolated test supervisor;
    // this is not an abrupt native-death detection test.
    ok &= check(sintra::Coordinator::rpc_unpublish_transceiver(
        sintra::s_coord_id, sintra::s_mproc_id),
        "coordinator process must unpublish through the existing notification path");
    const auto result_path = sintra::test::invitation::marker_path(directory, "caller-result");
    ok &= check(sintra::test::wait_for_file(result_path, 4s),
        "caller must report its result before the held handler is released");
    ok &= check(sintra::test::read_lines(result_path) ==
        std::vector<std::string>{"passed"}, "caller must pass every route-loss assertion");
    mark(directory, "loss-release");
    mark(directory, "loss-sync-release");
    mark(directory, "owner-exit");
    ok &= sintra::test::invitation::assert_clean_exit(
        k_prefix, caller, 5s, "calling external leaf must finalize and settle cleanly");
    ok &= sintra::test::invitation::assert_clean_exit(
        k_prefix, owner, 5s, "owner external leaf must finalize and settle cleanly");
    ok &= await(directory, "caller-finalized", "passed");
    ok &= await(directory, "owner-finalized", "passed");

    const bool finalized = sintra::detail::finalize();
    runtime.active = !finalized;
    return check(finalized, "coordinator finalization must complete") && ok;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);
        if (!role.empty()) {
            return run_leaf(argc, argv, role,
                sintra::test::get_argv_value(argc, argv, k_directory_arg));
        }
        return run_coordinator(argc, argv) ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s%s\n", k_prefix, error.what());
        return 1;
    }
}
