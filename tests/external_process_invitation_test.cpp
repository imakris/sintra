#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using sintra::test::managed_child::make_invitation;

constexpr const char* k_role_arg       = "--external_attach_role";
constexpr const char* k_dir_arg        = "--external_attach_dir";
constexpr const char* k_marker_arg     = "--external_attach_marker";
constexpr const char* k_role_helper    = "helper";
constexpr const char* k_role_reject    = "reject";
constexpr const char* k_service_name   = "external_attach_service";
constexpr const char* k_failure_prefix = "external_process_invitation_test: ";

constexpr const char* k_external_attach_rejected_message =
    "Sintra external process invitation was rejected.";

struct done_signal_t {};

struct External_service : sintra::Derived_transceiver<External_service>
{
    int ping(int value)
    {
        return value + 13;
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

    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;

    sintra::activate_slot([&](const done_signal_t&) {
        std::lock_guard<std::mutex> lock(done_mutex);
        done = true;
        done_cv.notify_all();
    });

    External_service service;
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

bool run_valid_attach_case(
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
        "coordinator should create an external-process invitation");

    auto duplicate_pending = make_invitation(
        invitation.process_instance_id,
        std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        !duplicate_pending,
        k_failure_prefix,
        "duplicate pending process id should be rejected");

    const std::string marker = "valid";
    sintra::test::Exact_child valid_helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_helper, marker, invitation),
        valid_helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "direct helper launch should succeed");
    if (!ok) {
        std::string cleanup_diagnostic;
        const bool cleaned = valid_helper.terminate_and_settle(cleanup_diagnostic);
        if (!cleaned) {
            std::fprintf(
                stderr,
                "%svalid helper early-return cleanup failed%s%s\n",
                k_failure_prefix,
                cleanup_diagnostic.empty() ? "" : ": ",
                cleanup_diagnostic.c_str());
        }
        return false;
    }

    const auto service_iid = resolve_until(k_service_name, std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        service_iid != sintra::invalid_instance_id,
        k_failure_prefix,
        "coordinator should resolve the externally attached helper service");

    if (service_iid != sintra::invalid_instance_id) {
        const int reply = External_service::rpc_ping(service_iid, 31);
        ok &= sintra::test::assert_true(
            reply == 44,
            k_failure_prefix,
            "coordinator should call the externally attached helper service");
    }

    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(invitation),
        k_failure_prefix,
        "claimed invitation should no longer be cancellable");

    auto duplicate_active = make_invitation(
        invitation.process_instance_id,
        std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        !duplicate_active,
        k_failure_prefix,
        "duplicate admitted process id should be rejected");

    sintra::world() << done_signal_t{};
    ok &= wait_for_marker(dir, marker + "_left", "left", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        valid_helper,
        std::chrono::seconds(8),
        "valid helper process should exit after receiving done");

    sintra::test::Exact_child replay_helper(std::chrono::seconds(2));
    const bool replay_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_reject, "replay", invitation),
        replay_helper);
    ok &= sintra::test::assert_true(
        replay_launched,
        k_failure_prefix,
        "replay helper launch should succeed");
    ok &= wait_for_marker(dir, "replay", "rejected", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        replay_helper,
        std::chrono::seconds(8),
        "replay helper process should exit after rejection");

    return guard.shutdown() && ok;
}

bool run_wrong_token_case(
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
        "wrong-token case should create an invitation");

    auto args = helper_args(dir, k_role_reject, "wrong_token", invitation);
    ok &= sintra::test::assert_true(
        replace_external_attach_token(args, "wrong-token-for-external-attach-test"),
        k_failure_prefix,
        "wrong-token case should replace the invitation token argument");

    sintra::test::Exact_child wrong_token_helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(binary_path, args, wrong_token_helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "wrong-token helper launch should succeed");
    ok &= wait_for_marker(dir, "wrong_token", "rejected", std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(invitation),
        k_failure_prefix,
        "wrong-token attempt should retire the invitation");
    auto fresh_invitation = wait_for_invitation_reuse(
        invitation.process_instance_id,
        std::chrono::seconds(8),
        std::chrono::seconds(6));
    ok &= sintra::test::assert_true(
        static_cast<bool>(fresh_invitation),
        k_failure_prefix,
        "wrong-token attempt should allow a fresh invitation after cleanup");
    if (fresh_invitation) {
        ok &= sintra::test::assert_true(
            sintra::cancel_external_process_invitation(fresh_invitation),
            k_failure_prefix,
            "fresh invitation after wrong-token retirement should be cancellable");
    }
    ok &= assert_clean_exit(
        wrong_token_helper,
        std::chrono::seconds(8),
        "wrong-token helper process should exit after rejection");

    return guard.shutdown() && ok;
}

bool run_canceled_case(
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
        "canceled case should create an invitation");

    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(invitation.process_instance_id),
        k_failure_prefix,
        "id-based cancel_external_process_invitation should cancel a pending invitation");

    sintra::test::Exact_child canceled_helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_reject, "canceled", invitation),
        canceled_helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "canceled helper launch should succeed");
    ok &= wait_for_marker(dir, "canceled", "rejected", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        canceled_helper,
        std::chrono::seconds(8),
        "canceled helper process should exit after rejection");

    return guard.shutdown() && ok;
}

bool run_expired_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::milliseconds(100));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "expired case should create an invitation");

    std::this_thread::sleep_for(std::chrono::milliseconds(160));

    sintra::test::Exact_child expired_helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_reject, "expired", invitation),
        expired_helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "expired helper launch should succeed");
    ok &= wait_for_marker(dir, "expired", "rejected", std::chrono::seconds(8));
    ok &= assert_clean_exit(
        expired_helper,
        std::chrono::seconds(8),
        "expired helper process should exit after rejection");

    return guard.shutdown() && ok;
}

bool run_recovery_occurrence_rejected_case(
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
        "recovery-occurrence case should create an invitation");

    auto args = helper_args(dir, k_role_reject, "recovery_occurrence", invitation);
    args.push_back("--recovery_occurrence");
    args.push_back("1");

    sintra::test::Exact_child recovery_occurrence_helper(std::chrono::seconds(2));
    const bool helper_launched = launch_direct_process(
        binary_path,
        args,
        recovery_occurrence_helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "recovery-occurrence helper launch should succeed");
    ok &= wait_for_marker(dir, "recovery_occurrence", "rejected", std::chrono::seconds(8));
    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(invitation),
        k_failure_prefix,
        "recovery-occurrence rejection should leave the invitation cancellable");
    ok &= assert_clean_exit(
        recovery_occurrence_helper,
        std::chrono::seconds(8),
        "recovery-occurrence helper process should exit after rejection");

    return guard.shutdown() && ok;
}

bool run_invalid_id_case(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    bool ok = true;

    auto current_process = make_invitation(
        sintra::s_mproc_id,
        std::chrono::seconds(1));
    ok &= sintra::test::assert_true(
        !current_process,
        k_failure_prefix,
        "current process id should not be accepted for an external invitation");

    auto coordinator_process = make_invitation(
        sintra::process_of(sintra::s_coord_id),
        std::chrono::seconds(1));
    ok &= sintra::test::assert_true(
        !coordinator_process,
        k_failure_prefix,
        "coordinator process id should not be accepted for an external invitation");

    auto transceiver_id = make_invitation(
        sintra::s_coord_id,
        std::chrono::seconds(1));
    ok &= sintra::test::assert_true(
        !transceiver_id,
        k_failure_prefix,
        "non-process transceiver id should not be accepted for an external invitation");

    return guard.shutdown() && ok;
}

bool run_stale_invitation_cancel_case(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    const auto explicit_iid = sintra::make_process_instance_id();
    auto old_invitation = make_invitation(
        explicit_iid,
        std::chrono::seconds(8));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(old_invitation),
        k_failure_prefix,
        "stale-cancel case should create the first explicit-id invitation");

    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(old_invitation),
        k_failure_prefix,
        "stale-cancel case should cancel the first explicit-id invitation");

    sintra::External_process_invitation new_invitation;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        new_invitation = make_invitation(explicit_iid, std::chrono::seconds(8));
        if (new_invitation) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ok &= sintra::test::assert_true(
        static_cast<bool>(new_invitation),
        k_failure_prefix,
        "stale-cancel case should reuse the explicit id after cleanup");
    ok &= sintra::test::assert_true(
        !sintra::cancel_external_process_invitation(old_invitation),
        k_failure_prefix,
        "stale invitation object should not cancel a newer invitation for the same id");
    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(new_invitation),
        k_failure_prefix,
        "new invitation should remain cancellable after stale-object cancellation attempt");

    return guard.shutdown() && ok;
}

bool run_pending_shutdown_case(int argc, char* argv[])
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(
        sintra::invalid_instance_id,
        std::chrono::seconds(30));
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "pending-shutdown case should create an invitation");

    const auto start       = std::chrono::steady_clock::now();
    const bool shutdown_ok = guard.shutdown();
    const auto elapsed     = std::chrono::steady_clock::now() - start;

    ok &= sintra::test::assert_true(
        shutdown_ok,
        k_failure_prefix,
        "shutdown should succeed with a pending invitation");
    ok &= sintra::test::assert_true(
        elapsed < std::chrono::seconds(3),
        k_failure_prefix,
        "pending invitation should not block shutdown");
    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_role_helper) { return run_helper(       argc, argv); }
    if (role == k_role_reject) { return run_reject_helper(argc, argv); }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    const auto        dir         = sintra::test::unique_scratch_directory("external_process_invitation");

    bool ok = true;
    ok &= run_valid_attach_case(argc, argv, binary_path, dir);
    ok &= run_wrong_token_case(argc, argv, binary_path, dir);
    ok &= run_canceled_case(argc, argv, binary_path, dir);
    ok &= run_expired_case(argc, argv, binary_path, dir);
    ok &= run_recovery_occurrence_rejected_case(argc, argv, binary_path, dir);
    ok &= run_invalid_id_case(argc, argv);
    ok &= run_stale_invitation_cancel_case(argc, argv);
    ok &= run_pending_shutdown_case(argc, argv);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    return ok ? 0 : 1;
}
