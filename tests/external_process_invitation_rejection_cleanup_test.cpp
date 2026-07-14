#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* k_role_arg       = "--external_attach_cleanup_role";
constexpr const char* k_dir_arg        = "--external_attach_cleanup_dir";
constexpr const char* k_marker_arg     = "--external_attach_cleanup_marker";
constexpr const char* k_role_valid     = "valid";
constexpr const char* k_role_status    = "status";
constexpr const char* k_role_cleanup   = "cleanup";
constexpr const char* k_role_attack    = "attack";
constexpr const char* k_service_name   = "external_attach_cleanup_service";
constexpr const char* k_failure_prefix = "external_process_invitation_rejection_cleanup_test: ";

constexpr const char* k_external_attach_rejected_message =
    "Sintra external process invitation was rejected.";

struct done_signal_t {};

struct Cleanup_probe_service : sintra::Derived_transceiver<Cleanup_probe_service>
{
    int ping(int value)
    {
        return value + 17;
    }

    SINTRA_RPC(ping)
};

struct Runtime_guard
{
    bool active = false;

    ~Runtime_guard()
    {
        if (!active || !sintra::s_mproc) {
            return;
        }

        try {
            sintra::detail::finalize();
        }
        catch (...) {
        }
    }

    bool shutdown()
    {
        if (!active) {
            return true;
        }

        active = false;
        return sintra::shutdown();
    }
};

std::filesystem::path marker_path(
    const std::filesystem::path&   dir,
    const std::string&             marker)
{
    return dir / (marker + ".txt");
}

void write_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const std::string&             value)
{
    std::ofstream out(marker_path(dir, marker), std::ios::binary | std::ios::trunc);
    out << value << '\n';
    out.close();
}

bool wait_for_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const std::string&             expected,
    std::chrono::milliseconds      timeout)
{
    const auto path = marker_path(dir, marker);
    std::string actual;

    if (sintra::test::wait_for_first_line(
            path,
            expected,
            actual,
            timeout,
            std::chrono::milliseconds(20)))
    {
        return true;
    }

    if (actual.empty()) {
        std::fprintf(stderr, "%smarker '%s' was not written\n", k_failure_prefix, marker.c_str());
    }
    else {
        std::fprintf(stderr,
            "%smarker '%s' mismatch: expected '%s', actual '%s'\n",
            k_failure_prefix,
            marker.c_str(),
            expected.c_str(),
            actual.c_str());
    }
    return false;
}

bool launch_direct_process(
    const std::string&                 binary_path,
    const std::vector<std::string>&    args,
    sintra::test::Exact_child&         child)
{
    std::vector<std::string> all_args;
    all_args.reserve(args.size() + 1);
    all_args.push_back(binary_path);
    all_args.insert(all_args.end(), args.begin(), args.end());

    sintra::C_string_vector cargs(all_args);
    return child.spawn(binary_path.c_str(), cargs.v());
}

bool assert_clean_exit(
    sintra::test::Exact_child& child,
    std::chrono::milliseconds  timeout,
    const char*                message)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = child.poll();
        if (state == sintra::test::Exact_child_state::exited) {
            const bool clean_exit = child.exited_with_code(0);
            const auto status     = child.describe_status();
            std::string settle_diagnostic;
            const bool settled = child.settle_observed_exit(settle_diagnostic);
            if (!clean_exit || !settled) {
                std::fprintf(
                    stderr,
                    "%s%s: %s%s%s\n",
                    k_failure_prefix,
                    message,
                    status.c_str(),
                    settled ? "" : "; settlement failed: ",
                    settled ? "" : settle_diagnostic.c_str());
            }
            return sintra::test::assert_true(
                clean_exit && settled,
                k_failure_prefix,
                message);
        }
        if (state == sintra::test::Exact_child_state::error) {
            const auto observation = child.describe_status();
            std::string cleanup_diagnostic;
            const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
            std::fprintf(
                stderr,
                "%s%s: observation failed: %s; cleanup %s%s%s\n",
                k_failure_prefix,
                message,
                observation.c_str(),
                cleaned ? "settled" : "failed",
                cleanup_diagnostic.empty() ? "" : ": ",
                cleanup_diagnostic.c_str());
            return sintra::test::assert_true(false, k_failure_prefix, message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string cleanup_diagnostic;
    const bool cleaned = child.terminate_and_settle(cleanup_diagnostic);
    std::fprintf(
        stderr,
        "%s%s: timed out; cleanup %s%s%s\n",
        k_failure_prefix,
        message,
        cleaned ? "settled" : "failed",
        cleanup_diagnostic.empty() ? "" : ": ",
        cleanup_diagnostic.c_str());
    return sintra::test::assert_true(false, k_failure_prefix, message);
}

bool replace_external_attach_token(
    std::vector<std::string>&  args,
    const std::string&         replacement)
{
    constexpr const char* token_arg    = "--external_attach_token";
    const std::string     token_prefix = std::string(token_arg) + "=";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == token_arg && i + 1 < args.size()) {
            args[i + 1] = replacement;
            return true;
        }
        if (args[i].rfind(token_prefix, 0) == 0) {
            args[i] = token_prefix + replacement;
            return true;
        }
    }
    return false;
}

std::vector<std::string> helper_args(
    const std::filesystem::path&               dir,
    const std::string&                         role,
    const std::string&                         marker,
    const sintra::External_process_invitation& invitation)
{
    std::vector<std::string> args = {
        k_role_arg,   role,
        k_dir_arg,    dir.string(),
        k_marker_arg, marker,
    };

    auto sintra_args = invitation.sintra_args();
    args.insert(args.end(), sintra_args.begin(), sintra_args.end());
    return args;
}

sintra::External_process_invitation make_invitation(
    sintra::instance_id_type   process_iid,
    std::chrono::milliseconds  timeout)
{
    sintra::External_process_invitation_options options;
    options.process_instance_id = process_iid;
    options.timeout             = timeout;
    return sintra::create_external_process_invitation(options);
}

sintra::External_process_invitation wait_for_invitation_reuse(
    sintra::instance_id_type   process_iid,
    std::chrono::milliseconds  invitation_timeout,
    std::chrono::milliseconds  wait_timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto invitation = make_invitation(process_iid, invitation_timeout);
        if (invitation) {
            return invitation;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return {};
}

bool retired_invitation_allows_reuse(
    const sintra::External_process_invitation& invitation,
    const char*                                case_name)
{
    bool ok = sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(invitation),
        k_failure_prefix,
        "retired wrong-token invitation should not remain cancellable");

    auto reuse_invitation = wait_for_invitation_reuse(
        invitation.process_instance_id,
        std::chrono::seconds(8),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(reuse_invitation),
        k_failure_prefix,
        case_name);
    if (reuse_invitation) {
        ok &= sintra::test::assert_true(
            sintra::cancel_external_process_invitation(reuse_invitation),
            k_failure_prefix,
            "fresh invitation after wrong-token retirement should be cancellable");
    }
    return ok;
}

sintra::instance_id_type resolve_until(
    const char*                name,
    std::chrono::milliseconds  timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto resolved = sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, name);
        if (resolved != sintra::invalid_instance_id) {
            return resolved;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return sintra::invalid_instance_id;
}

void cleanup_partial_runtime()
{
    if (!sintra::s_mproc) {
        return;
    }

    try {
        sintra::detail::finalize();
    }
    catch (...) {
    }
}

bool failed_init_state_is_clean()
{
    return !sintra::s_mproc && !sintra::s_init_once;
}

bool transceiver_construction_is_blocked()
{
    try {
        Cleanup_probe_service service;
        (void)service.assign_name("rejected_cleanup_probe_transceiver");
        return false;
    }
    catch (...) {
        return true;
    }
}

bool second_init_gets_specific_rejection(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        const bool exact_rejection = std::string(e.what()) == k_external_attach_rejected_message;
        cleanup_partial_runtime();
        return exact_rejection && failed_init_state_is_clean();
    }
    catch (...) {
        cleanup_partial_runtime();
        return false;
    }

    cleanup_partial_runtime();
    return false;
}

bool rejected_helper_can_create_group()
{
    if (!sintra::s_mproc) {
        return false;
    }

    std::unordered_set<sintra::instance_id_type> members{sintra::s_mproc_id};
    auto handle = sintra::Coordinator::rpc_async_make_process_group(
        sintra::s_coord_id,
        "rejected_cleanup_group",
        members);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    try {
        return handle.get_until(deadline) != sintra::invalid_instance_id;
    }
    catch (const sintra::rpc_timeout&) {
        sintra::s_mproc->unblock_rpc(sintra::process_of(sintra::s_coord_id));
        return false;
    }
}

bool rejected_helper_can_publish_process()
{
    if (!sintra::s_mproc) {
        return false;
    }

    auto handle = sintra::Coordinator::rpc_async_publish_transceiver(
        sintra::s_coord_id,
        sintra::make_user_type_id(1001),
        sintra::s_mproc_id,
        "rejected_cleanup_process");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool published = false;
    try {
        published = handle.get_until(deadline) != sintra::invalid_instance_id;
    }
    catch (const sintra::rpc_timeout&) {
        sintra::s_mproc->unblock_rpc(sintra::process_of(sintra::s_coord_id));
        return false;
    }

    if (published) {
        try {
            (void)sintra::Coordinator::rpc_unpublish_transceiver(sintra::s_coord_id, sintra::s_mproc_id);
        }
        catch (...) {
        }
    }
    return published;
}

int run_valid_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);

    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;

    sintra::activate_slot([&](const done_signal_t&) {
        std::lock_guard<std::mutex> lock(done_mutex);
        done = true;
        done_cv.notify_all();
    });

    Cleanup_probe_service service;
    if (!service.assign_name(k_service_name)) {
        write_marker(dir, marker, "assign_failed");
        sintra::leave();
        return 1;
    }
    write_marker(dir, marker, "ready");

    {
        std::unique_lock<std::mutex> lock(done_mutex);
        if (!done_cv.wait_for(lock, std::chrono::seconds(10), [&] { return done; })) {
            write_marker(dir, marker + "_left", "timeout");
            sintra::leave();
            return 1;
        }
    }

    sintra::leave();
    write_marker(dir, marker + "_left", "left");
    return 0;
}

int run_status_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        const bool exact_rejection = std::string(e.what()) == k_external_attach_rejected_message;
        write_marker(dir, marker, exact_rejection ? "rejected" : "unexpected_init_failure");
        cleanup_partial_runtime();
        return exact_rejection ? 0 : 2;
    }
    catch (...) {
        write_marker(dir, marker, "unexpected_init_failure");
        cleanup_partial_runtime();
        return 2;
    }

    write_marker(dir, marker, "unexpected_success");
    sintra::leave();
    return 2;
}

int run_cleanup_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    bool exact_rejection = false;
    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        exact_rejection = std::string(e.what()) == k_external_attach_rejected_message;
    }
    catch (...) {
    }

    const bool first_state_clean      = failed_init_state_is_clean();
    const bool transceiver_blocked    = transceiver_construction_is_blocked();
    const bool second_specific_reject = second_init_gets_specific_rejection(argc, argv);
    const bool final_state_clean      = failed_init_state_is_clean();
    const bool clean_after_rejection  =
        exact_rejection        &&
        first_state_clean      &&
        transceiver_blocked    &&
        second_specific_reject &&
        final_state_clean;

    if (clean_after_rejection) {
        write_marker(dir, marker, "rejected_clean");
        return 0;
    }

    write_marker(
        dir,
        marker,
        exact_rejection ? "partial_runtime_after_rejection" : "unexpected_init_failure");
    cleanup_partial_runtime();
    return 2;
}

int run_attack_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    bool exact_rejection = false;
    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        exact_rejection = std::string(e.what()) == k_external_attach_rejected_message;
    }
    catch (...) {
    }

    if (!exact_rejection) {
        write_marker(dir, marker, "unexpected_init_failure");
        cleanup_partial_runtime();
        return 2;
    }

    if (failed_init_state_is_clean()) {
        write_marker(dir, marker, "rejected_isolated");
        return 0;
    }

    const bool constructed_transceiver = !transceiver_construction_is_blocked();
    const bool created_group           = rejected_helper_can_create_group();
    const bool published_process       = rejected_helper_can_publish_process();

    if (created_group) {
        write_marker(dir, marker, "created_group_after_rejection");
        cleanup_partial_runtime();
        return 2;
    }

    if (published_process) {
        write_marker(dir, marker, "published_process_after_rejection");
        cleanup_partial_runtime();
        return 2;
    }

    if (constructed_transceiver) {
        write_marker(dir, marker, "constructed_transceiver_after_rejection");
        cleanup_partial_runtime();
        return 2;
    }

    write_marker(dir, marker, "rejected_isolated");
    cleanup_partial_runtime();
    return 0;
}

bool launch_rejected_status_helper(
    const std::string&     binary_path,
    const std::filesystem::path&
                           dir,
    const std::string&     marker,
    const sintra::External_process_invitation&
                           invitation,
    bool                   corrupt_token)
{
    auto args = helper_args(dir, k_role_status, marker, invitation);
    bool ok   = true;
    if (corrupt_token) {
        ok &= sintra::test::assert_true(
            replace_external_attach_token(args, "wrong-token-for-rejection-cleanup-test"),
            k_failure_prefix,
            "wrong-token helper should replace the invitation token argument");
    }

    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(binary_path, args, helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "rejected helper launch should succeed");
    ok &= wait_for_marker(dir, marker, "rejected", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        helper,
        std::chrono::seconds(8),
        "rejected helper should exit normally after the expected rejection");
    return ok;
}

bool admit_valid_helper(
    const std::string&                         binary_path,
    const std::filesystem::path&               dir,
    const sintra::External_process_invitation& invitation)
{
    bool ok = true;

    const std::string marker = "valid_after_rejections";
    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_valid, marker, invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "valid helper launch should succeed after rejected attempts");
    ok &= wait_for_marker(dir, marker, "ready", std::chrono::seconds(8));

    const auto service_iid = resolve_until(k_service_name, std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        service_iid != sintra::invalid_instance_id,
        k_failure_prefix,
        "coordinator should resolve the valid helper after rejected attempts");

    if (service_iid != sintra::invalid_instance_id) {
        const int reply = Cleanup_probe_service::rpc_ping(service_iid, 25);
        ok &= sintra::test::assert_true(
            reply == 42,
            k_failure_prefix,
            "coordinator should call the valid helper after rejected attempts");
    }

    sintra::world() << done_signal_t{};
    ok &= wait_for_marker(dir, marker + "_left", "left", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        helper,
        std::chrono::seconds(8),
        "valid helper should exit cleanly after receiving done");
    return ok;
}

bool run_wrong_token_cleanup_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(8));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "wrong-token cleanup case should create an invitation");

    auto args = helper_args(dir, k_role_cleanup, "wrong_token_cleanup", invitation);
    ok &= sintra::test::assert_true(
        replace_external_attach_token(args, "wrong-token-for-cleanup-probe"),
        k_failure_prefix,
        "wrong-token cleanup case should replace the invitation token argument");

    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(binary_path, args, helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "wrong-token cleanup helper launch should succeed");
    ok &= wait_for_marker(
        dir,
        "wrong_token_cleanup",
        "rejected_clean",
        std::chrono::seconds(8));
    ok &= assert_clean_exit(
        helper,
        std::chrono::seconds(8),
        "wrong-token cleanup helper should exit normally");

    ok &= retired_invitation_allows_reuse(
        invitation,
        "wrong-token cleanup case should allow a fresh invitation after cleanup");

    return guard.shutdown() && ok;
}

bool run_rejected_helpers_exit_with_specific_rejection_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    bool ok = true;

    auto wrong_token_invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        static_cast<bool>(wrong_token_invitation),
        k_failure_prefix,
        "wrong-token status case should create an invitation");
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "wrong_token_status",
        wrong_token_invitation,
        true);
    ok &= retired_invitation_allows_reuse(
        wrong_token_invitation,
        "wrong-token status case should allow a fresh invitation after cleanup");

    auto canceled_invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        static_cast<bool>(canceled_invitation),
        k_failure_prefix,
        "canceled status case should create an invitation");
    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(canceled_invitation),
        k_failure_prefix,
        "canceled status case should cancel the invitation");
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "canceled_status",
        canceled_invitation,
        false);

    auto expired_invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::milliseconds(100));
    ok &= sintra::test::assert_true(
        static_cast<bool>(expired_invitation),
        k_failure_prefix,
        "expired status case should create an invitation");
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "expired_status",
        expired_invitation,
        false);

    return guard.shutdown() && ok;
}

bool run_same_explicit_id_recovery_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    bool ok = true;

    const auto explicit_iid = sintra::make_process_instance_id();

    auto wrong_token_invitation = make_invitation(explicit_iid, std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        static_cast<bool>(wrong_token_invitation),
        k_failure_prefix,
        "explicit-id recovery case should create the wrong-token invitation");
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "explicit_wrong_token",
        wrong_token_invitation,
        true);
    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(wrong_token_invitation),
        k_failure_prefix,
        "explicit-id wrong-token invitation should be retired");

    auto canceled_invitation = wait_for_invitation_reuse(
        explicit_iid,
        std::chrono::seconds(8),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(canceled_invitation),
        k_failure_prefix,
        "explicit id should be reusable after wrong-token cleanup");
    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(canceled_invitation),
        k_failure_prefix,
        "explicit-id recovery case should cancel the canceled invitation");
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "explicit_canceled",
        canceled_invitation,
        false);

    auto expired_invitation = wait_for_invitation_reuse(
        explicit_iid,
        std::chrono::milliseconds(100),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(expired_invitation),
        k_failure_prefix,
        "explicit id should be reusable after canceled-invitation cleanup");
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    ok &= launch_rejected_status_helper(
        binary_path,
        dir,
        "explicit_expired",
        expired_invitation,
        false);

    auto valid_invitation = wait_for_invitation_reuse(
        explicit_iid,
        std::chrono::seconds(8),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(valid_invitation),
        k_failure_prefix,
        "explicit id should be reusable after expired-invitation cleanup");
    if (valid_invitation) {
        ok &= admit_valid_helper(binary_path, dir, valid_invitation);
    }

    return guard.shutdown() && ok;
}

bool run_rejected_helper_cannot_affect_coordinator_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(8));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "coordinator-isolation case should create an invitation");

    auto args = helper_args(dir, k_role_attack, "coordinator_isolation", invitation);
    ok &= sintra::test::assert_true(
        replace_external_attach_token(args, "wrong-token-for-coordinator-isolation"),
        k_failure_prefix,
        "coordinator-isolation case should replace the invitation token argument");

    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(binary_path, args, helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "coordinator-isolation helper launch should succeed");
    ok &= wait_for_marker(
        dir,
        "coordinator_isolation",
        "rejected_isolated",
        std::chrono::seconds(8));
    ok &= assert_clean_exit(
        helper,
        std::chrono::seconds(8),
        "coordinator-isolation helper should exit normally");

    ok &= retired_invitation_allows_reuse(
        invitation,
        "coordinator-isolation case should allow a fresh invitation after cleanup");

    return guard.shutdown() && ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_role_valid)   { return run_valid_helper(  argc, argv); }
    if (role == k_role_status)  { return run_status_helper( argc, argv); }
    if (role == k_role_cleanup) { return run_cleanup_helper(argc, argv); }
    if (role == k_role_attack)  { return run_attack_helper( argc, argv); }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    const auto        dir         = sintra::test::unique_scratch_directory("ext_attach_reject_clean");

    bool ok = true;
    ok &= run_wrong_token_cleanup_case(argc, argv, binary_path, dir);
    ok &= run_rejected_helpers_exit_with_specific_rejection_case(argc, argv, binary_path, dir);
    ok &= run_same_explicit_id_recovery_case(argc, argv, binary_path, dir);
    ok &= run_rejected_helper_cannot_affect_coordinator_case(argc, argv, binary_path, dir);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    return ok ? 0 : 1;
}
