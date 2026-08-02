#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "external_process_invitation_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using sintra::test::managed_child::make_invitation;

constexpr const char* k_role_arg       = "--external_attach_negative_role";
constexpr const char* k_dir_arg        = "--external_attach_negative_dir";
constexpr const char* k_marker_arg     = "--external_attach_negative_marker";
constexpr const char* k_role_helper    = "helper";
constexpr const char* k_role_reject    = "reject";
constexpr const char* k_service_prefix = "e_";
constexpr const char* k_failure_prefix = "external_process_invitation_admission_negative_test: ";

constexpr const char* k_external_attach_rejected_message =
    "Sintra external process invitation was rejected.";

struct External_service : sintra::Derived_transceiver<External_service>
{
    int ping(int value)
    {
        return value + 17;
    }

    SINTRA_RPC(ping)
};

using sintra::test::invitation::Runtime_guard;
using sintra::test::invitation::launch_direct_process;
using sintra::test::invitation::wait_for_control_file;
using sintra::test::invitation::write_control_file;

// The shared fixture takes the failure prefix explicitly; bind this file's constant
// once here rather than at every call.
void write_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               value)
{
    sintra::test::invitation::write_marker(k_failure_prefix, dir, marker, value);
}

std::string service_name_for_marker(const std::string& marker)
{
    return std::string(k_service_prefix) + marker;
}

bool wait_for_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               expected,
    std::chrono::milliseconds      timeout)
{
    return sintra::test::invitation::wait_for_marker(
        k_failure_prefix, dir, marker, expected, timeout);
}

bool wait_for_clean_exit(
    sintra::test::Exact_child& child,
    std::chrono::milliseconds  timeout,
    const char*                message)
{
    return sintra::test::invitation::assert_clean_exit(
        k_failure_prefix, child, timeout, message);
}

bool replace_arg_value(
    std::vector<std::string>&  args,
    const char*                arg_name,
    const std::string&         replacement)
{
    const std::string arg_prefix = std::string(arg_name) + "=";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == arg_name && i + 1 < args.size()) {
            args[i + 1] = replacement;
            return true;
        }
        if (args[i].rfind(arg_prefix, 0) == 0) {
            args[i] = arg_prefix + replacement;
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

int run_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);

    External_service service;
    const auto service_name = service_name_for_marker(marker);
    if (!service.assign_name(service_name.c_str())) {
        write_marker(dir, marker, "assign_failed");
        sintra::leave();
        return 1;
    }
    write_marker(dir, marker, "ready");

    if (!wait_for_control_file(dir, marker, ".release", std::chrono::seconds(10))) {
        write_marker(dir, marker + "_left", "timeout");
        sintra::leave();
        return 1;
    }

    write_marker(dir, marker + "_left", "left");
    const bool left = sintra::leave();
    if (!left) {
        write_marker(dir, marker + "_left", "leave_failed");
    }
    return left ? 0 : 1;
}

int run_reject_helper(int argc, char* argv[])
{
    const auto dir = std::filesystem::path(
        sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        write_marker(
            dir,
            marker,
            std::string(e.what()) == k_external_attach_rejected_message
                ? "rejected"
                : "unexpected_init_failure");
        std::_Exit(0);
    }
    catch (...) {
        write_marker(dir, marker, "unexpected_init_failure");
        std::_Exit(0);
    }

    write_marker(dir, marker, "unexpected_success");
    sintra::leave();
    return 2;
}

bool launch_valid_helper_and_stop(
    const std::string&                         binary_path,
    const std::filesystem::path&               dir,
    const std::string&                         marker,
    const sintra::External_process_invitation& invitation)
{
    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_helper, marker, invitation),
        helper);
    bool ok = sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "valid direct helper launch should succeed");

    if (helper_launched) {
        const auto service_name = service_name_for_marker(marker);
        const auto service_iid  = resolve_until(service_name.c_str(), std::chrono::seconds(8));
        ok &= sintra::test::assert_true(
            service_iid != sintra::invalid_instance_id,
            k_failure_prefix,
            "coordinator should resolve the valid external helper service");

        if (service_iid != sintra::invalid_instance_id) {
            const int reply = External_service::rpc_ping(service_iid, 29);
            ok &= sintra::test::assert_true(
                reply == 46,
                k_failure_prefix,
                "coordinator should call the valid external helper service");
        }

        write_control_file(dir, marker, ".release");
        const bool exited = wait_for_clean_exit(
            helper,
            std::chrono::seconds(8),
            "valid helper process should exit");
        if (exited) {
            ok &= wait_for_marker(dir, marker + "_left", "left", std::chrono::seconds(1));
        }
    }

    return ok;
}

bool wait_for_helper_exit_and_left_marker(
    sintra::test::Exact_child&     child,
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const char*                    exit_message)
{
    const bool exited = wait_for_clean_exit(
        child,
        std::chrono::seconds(8),
        exit_message);
    bool ok = exited;
    if (exited) {
        ok &= wait_for_marker(dir, marker + "_left", "left", std::chrono::seconds(1));
    }
    return ok;
}

bool launch_rejected_helper(
    const std::string&                 binary_path,
    const std::filesystem::path&       dir,
    const std::string&                 marker,
    const std::vector<std::string>&    args)
{
    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(binary_path, args, helper);
    bool ok = sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "rejected direct helper launch should succeed");
    if (helper_launched) {
        ok &= wait_for_marker(dir, marker, "rejected", std::chrono::seconds(8));
        ok &= wait_for_clean_exit(
            helper,
            std::chrono::seconds(8),
            "rejected helper process should exit");
    }
    return ok;
}

std::filesystem::path missing_executable_path(const std::filesystem::path& dir)
{
#ifdef _WIN32
    return dir / "missing_external_attach_spawn_target.exe";
#else
    return dir / "missing_external_attach_spawn_target";
#endif
}

bool run_spawn_collision_keeps_invitation_claimable_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    const auto explicit_iid = sintra::make_process_instance_id();
    auto       invitation   = make_invitation(explicit_iid, std::chrono::seconds(8));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "spawn-collision case should create an explicit-id invitation");

    sintra::Spawn_options spawn_options;
    spawn_options.binary_path         = missing_executable_path(dir).string();
    spawn_options.process_instance_id = explicit_iid;
    const auto custody = sintra::spawn_swarm_process(spawn_options);
    ok &= sintra::test::assert_true(
        !custody,
        k_failure_prefix,
        "spawn with an invited explicit process id and missing executable should fail");

    ok &= launch_valid_helper_and_stop(binary_path, dir, "scv", invitation);
    return guard.shutdown() && ok;
}

bool run_rejected_attempts_do_not_poison_valid_attach_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(12));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "poisoning case should create an invitation");

    auto       wrong_instance_args = helper_args(dir, k_role_reject, "wi", invitation);
    const auto wrong_iid           = sintra::make_process_instance_id();
    ok &= sintra::test::assert_true(
        replace_arg_value(wrong_instance_args, "--instance_id", std::to_string(wrong_iid)),
        k_failure_prefix,
        "wrong-instance case should replace the invitation instance id");
    ok &= launch_rejected_helper(binary_path, dir, "wi", wrong_instance_args);

    auto wrong_token_args = helper_args(dir, k_role_reject, "wt", invitation);
    ok &= sintra::test::assert_true(
        replace_arg_value(
            wrong_token_args,
            "--external_attach_token",
            "wrong-token-for-admission-negative-test"),
        k_failure_prefix,
        "wrong-token case should replace the invitation token argument");
    ok &= launch_rejected_helper(binary_path, dir, "wt", wrong_token_args);

    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(invitation),
        k_failure_prefix,
        "wrong-token claim should retire the original invitation");

    auto fresh_invitation = wait_for_invitation_reuse(
        invitation.process_instance_id,
        std::chrono::seconds(12),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(fresh_invitation),
        k_failure_prefix,
        "wrong-token retirement should allow a fresh invitation for the same process id");
    if (fresh_invitation) {
        ok &= launch_valid_helper_and_stop(
            binary_path,
            dir,
            "arv",
            fresh_invitation);
    }
    return guard.shutdown() && ok;
}

bool run_duplicate_explicit_ids_rejected_while_pending_and_admitted_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    const auto explicit_iid = sintra::make_process_instance_id();
    auto       invitation   = make_invitation(explicit_iid, std::chrono::seconds(8));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "duplicate-id case should create an explicit-id invitation");

    auto duplicate_pending = make_invitation(explicit_iid, std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        !duplicate_pending,
        k_failure_prefix,
        "duplicate explicit invitation id should be rejected while pending");

    sintra::test::Exact_child helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_helper, "div", invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "duplicate-id valid helper launch should succeed");

    if (helper_launched) {
        const auto service_name = service_name_for_marker("div");
        const auto service_iid  = resolve_until(service_name.c_str(), std::chrono::seconds(8));
        ok &= sintra::test::assert_true(
            service_iid != sintra::invalid_instance_id,
            k_failure_prefix,
            "duplicate-id case should admit the valid helper");

        auto duplicate_admitted = make_invitation(explicit_iid, std::chrono::seconds(8));
        ok &= sintra::test::assert_true(
            !duplicate_admitted,
            k_failure_prefix,
            "duplicate explicit invitation id should be rejected while admitted");

        write_control_file(dir, "div", ".release");
        ok &= wait_for_helper_exit_and_left_marker(
            helper,
            dir,
            "div",
            "duplicate-id helper process should exit");
    }

    return guard.shutdown() && ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_role_helper) { return run_helper(       argc, argv); }
    if (role == k_role_reject) { return run_reject_helper(argc, argv); }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    const auto dir = sintra::test::unique_scratch_directory(
        "eia_neg");

    bool ok = true;
    ok &= run_spawn_collision_keeps_invitation_claimable_case(argc, argv, binary_path, dir);
    ok &= run_rejected_attempts_do_not_poison_valid_attach_case(argc, argv, binary_path, dir);
    ok &= run_duplicate_explicit_ids_rejected_while_pending_and_admitted_case(
        argc,
        argv,
        binary_path,
        dir);

    std::error_code ec;
    if (ok) {
        std::filesystem::remove_all(dir, ec);
    }

    return ok ? 0 : 1;
}
