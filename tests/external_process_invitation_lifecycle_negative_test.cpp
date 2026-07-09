#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr const char* k_role_arg     = "--external_attach_negative_role";
constexpr const char* k_dir_arg      = "--external_attach_negative_dir";
constexpr const char* k_marker_arg   = "--external_attach_negative_marker";
constexpr const char* k_role_delayed = "delayed_init";
constexpr const char* k_role_alive   = "admitted_alive";
constexpr const char* k_role_crash   = "crash_after_init";
constexpr const char* k_role_recover = "enable_recovery_after_init";
constexpr const char* k_role_reuse   = "reuse_attach";

constexpr const char* k_failure_prefix = "external_process_invitation_lifecycle_negative_test: ";
constexpr const char* k_external_attach_rejected_message =
    "Sintra external process invitation was rejected.";

struct launched_process_t
{
    bool   launched = false;
    int    pid      = -1;
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

class Shutdown_watchdog
{
public:
    Shutdown_watchdog(const char* name, std::chrono::milliseconds timeout)
    :
        m_name(name),
        m_thread([this, timeout] { run(timeout); })
    {}

    ~Shutdown_watchdog()
    {
        m_done.store(true, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void run(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "%sshutdown watchdog fired in %s\n", k_failure_prefix, m_name);
                std::fflush(stderr);
                std::_Exit(1);
            }
            std::this_thread::sleep_for(20ms);
        }
    }

    const char*        m_name = "";
    std::atomic<bool>  m_done{false};
    std::thread        m_thread;
};

std::filesystem::path marker_path(
    const std::filesystem::path&   dir,
    const std::string&             marker)
{
    return dir / (marker + ".txt");
}

std::filesystem::path control_path(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    const char*                    suffix)
{
    return dir / (marker + suffix);
}

void write_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               value)
{
    std::ofstream out(marker_path(dir, marker), std::ios::binary | std::ios::trunc);
    out << value << '\n';
}

bool wait_for_marker(
    const std::filesystem::path&   dir,
    const std::string&             marker,
    std::string_view               expected,
    std::chrono::milliseconds      timeout)
{
    const auto path = marker_path(dir, marker);
    std::string actual;

    if (sintra::test::wait_for_first_line(path, expected, actual, timeout, 20ms)) {
        return true;
    }

    if (actual.empty()) {
        std::fprintf(stderr, "%smarker '%s' was not written\n", k_failure_prefix, marker.c_str());
    }
    else {
        std::fprintf(stderr,
            "%smarker '%s' mismatch: expected '%.*s', actual '%s'\n",
            k_failure_prefix,
            marker.c_str(),
            static_cast<int>(expected.size()),
            expected.data(),
            actual.c_str());
    }
    return false;
}

bool wait_for_marker_in(
    const std::filesystem::path&           dir,
    const std::string&                     marker,
    const std::vector<std::string_view>&   expected_values,
    std::chrono::milliseconds              timeout)
{
    const auto path = marker_path(dir, marker);
    std::string actual;

    if (sintra::test::wait_for_first_line_until(
            path,
            expected_values,
            actual,
            std::chrono::steady_clock::now() + timeout,
            20ms))
    {
        return true;
    }

    std::fprintf(stderr,
        "%smarker '%s' did not reach an accepted value, actual '%s'\n",
        k_failure_prefix,
        marker.c_str(),
        actual.empty() ? "<missing>" : actual.c_str());
    return false;
}

void write_control_file(
    const std::filesystem::path&       dir,
    const std::string&                 marker,
    const char*                        suffix)
{
    std::ofstream out(control_path(dir, marker, suffix), std::ios::binary | std::ios::trunc);
    out << "go\n";
}

bool wait_for_control_file(
    const std::filesystem::path&       dir,
    const std::string&                 marker,
    const char*                        suffix,
    std::chrono::milliseconds          timeout)
{
    return sintra::test::wait_for_file(control_path(dir, marker, suffix), timeout, 20ms);
}

launched_process_t launch_direct_process(
    const std::string&                 binary_path,
    const std::vector<std::string>&    args)
{
    std::vector<std::string> all_args;
    all_args.reserve(args.size() + 1);
    all_args.push_back(binary_path);
    all_args.insert(all_args.end(), args.begin(), args.end());

    sintra::C_string_vector cargs(all_args);
    sintra::Spawn_detached_options options;
    int child_pid = -1;
    options.prog          = binary_path.c_str();
    options.argv          = cargs.v();
    options.child_pid_out = &child_pid;
    return {sintra::spawn_detached(options), child_pid};
}

bool wait_for_process_exit(int pid, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return true;
    }

#ifdef _WIN32
    HANDLE handle = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE,
        static_cast<DWORD>(pid));
    if (!handle) {
        return true;
    }

    const DWORD result = WaitForSingleObject(handle, static_cast<DWORD>(timeout.count()));
    if (result == WAIT_OBJECT_0) {
        CloseHandle(handle);
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        TerminateProcess(handle, 1);
        WaitForSingleObject(handle, 2000);
        CloseHandle(handle);
        return false;
    }

    CloseHandle(handle);
    return false;
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int         status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (result == static_cast<pid_t>(pid)) {
            return true;
        }
        if (result == 0) {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        if (errno != EINTR) {
            return true;
        }
    }

    const auto child_pid = static_cast<pid_t>(pid);
    if (::kill(child_pid, SIGTERM) == -1 && errno == ESRCH) {
        return false;
    }

    const auto reap_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < reap_deadline) {
        int         status = 0;
        const pid_t result = ::waitpid(child_pid, &status, WNOHANG);
        if (result == child_pid)            { return false; }
        if (result == -1 && errno != EINTR) { return false; }
        std::this_thread::sleep_for(20ms);
    }

    if (::kill(child_pid, SIGKILL) == 0) {
        int status = 0;
        while (::waitpid(child_pid, &status, 0) == -1 && errno == EINTR) {
            continue;
        }
    }
    return false;
#endif
}

bool has_argv_flag_or_value(int argc, char* argv[], std::string_view flag)
{
    return sintra::test::has_argv_flag(argc, argv, flag);
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

bool shutdown_with_watchdog(
    Runtime_guard&             guard,
    const char*                case_name,
    std::chrono::milliseconds* elapsed_out = nullptr)
{
    const auto start = std::chrono::steady_clock::now();
    Shutdown_watchdog watchdog(case_name, 6s);
    const bool shutdown_ok = guard.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (elapsed_out) {
        *elapsed_out = elapsed;
    }
    return shutdown_ok;
}

int run_delayed_init_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    write_marker(dir, marker, "before_init");
    if (!wait_for_control_file(dir, marker, ".go", 10s)) {
        write_marker(dir, marker, "timeout_waiting_for_go");
        return 1;
    }

    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error& e) {
        write_marker(
            dir,
            marker,
            std::string(e.what()) == k_external_attach_rejected_message
                ? "rejected"
                : "init_failed");
        return 0;
    }
    catch (...) {
        write_marker(dir, marker, "init_failed");
        return 0;
    }

    write_marker(dir, marker, "unexpected_success");
    sintra::detail::finalize();
    return 2;
}

int run_admitted_alive_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    const bool has_lifeline_arg = has_argv_flag_or_value(argc, argv, "--lifeline_handle");

    sintra::init(argc, argv);
    write_marker(dir, marker, has_lifeline_arg ? "unexpected_lifeline_arg" : "ready");

    if (!wait_for_control_file(dir, marker, ".release", 10s)) {
        write_marker(dir, marker + "_done", "timeout");
        sintra::leave();
        return 1;
    }

    write_marker(dir, marker + "_done", "released");
    sintra::leave();
    return 0;
}

int run_crash_after_init_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);
    const auto occurrence = sintra::s_recovery_occurrence;
    write_marker(dir, marker + "_entry_" + std::to_string(occurrence), "ready");

    if (occurrence > 0) {
        write_marker(dir, marker + "_respawned", "respawned");
        sintra::detail::finalize();
        return 0;
    }

    if (!wait_for_control_file(dir, marker, ".crash", 10s)) {
        write_marker(dir, marker + "_done", "timeout");
        sintra::detail::finalize();
        return 1;
    }

    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash("external-negative-crash");
    std::abort();
}

int run_enable_recovery_after_init_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);
    const auto occurrence = sintra::s_recovery_occurrence;
    write_marker(dir, marker + "_entry_" + std::to_string(occurrence), "ready");

    if (occurrence > 0) {
        write_marker(dir, marker + "_respawned", "respawned");
        sintra::detail::finalize();
        return 0;
    }

    try {
        sintra::enable_recovery();
        write_marker(dir, marker + "_enable_recovery", "returned");
    }
    catch (...) {
        write_marker(dir, marker + "_enable_recovery", "threw");
    }

    if (!wait_for_control_file(dir, marker, ".crash", 10s)) {
        write_marker(dir, marker + "_done", "timeout");
        sintra::detail::finalize();
        return 1;
    }

    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash("external-negative-recovery");
    std::abort();
}

int run_reuse_attach_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);
    write_marker(dir, marker, "ready");

    if (!wait_for_control_file(dir, marker, ".release", 10s)) {
        write_marker(dir, marker + "_done", "timeout");
        sintra::leave();
        return 1;
    }

    const bool left = sintra::leave();
    write_marker(dir, marker + "_done", left ? "left" : "leave_failed");
    return left ? 0 : 1;
}

bool run_shutdown_before_claim_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "shutdown-before-claim should create an invitation");

    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_delayed, "delayed", invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "shutdown-before-claim helper should launch");
    ok &= wait_for_marker(dir, "delayed", "before_init", 5s);

    std::chrono::milliseconds elapsed{};
    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "shutdown-before-claim", &elapsed),
        k_failure_prefix,
        "shutdown-before-claim should complete shutdown");
    ok &= sintra::test::assert_true(
        elapsed < 3s,
        k_failure_prefix,
        "shutdown-before-claim should not wait for an unclaimed helper");

    write_control_file(dir, "delayed", ".go");
    ok &= wait_for_marker_in(dir, "delayed", {"init_failed", "rejected"}, 5s);
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 5s),
        k_failure_prefix,
        "delayed helper should exit after coordinator shutdown rejects init");

    return ok;
}

bool run_shutdown_hook_claim_rejection_preserves_invitation_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "shutdown-hook claim-rejection should create an invitation");

    constexpr const char* marker = "hook_delayed";
    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_delayed, marker, invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "shutdown-hook claim-rejection helper should launch");
    ok &= wait_for_marker(dir, marker, "before_init", 5s);

    bool helper_rejected = false;
    bool invitation_preserved = false;
    Shutdown_watchdog watchdog("shutdown-hook-claim-rejection", 8s);
    guard.active = false;
    ok &= sintra::test::assert_true(
        sintra::shutdown(sintra::shutdown_options{
            .coordinator_shutdown_hook = [&] {
                write_control_file(dir, marker, ".go");
                if (!wait_for_marker_in(
                        dir,
                        marker,
                        {"rejected", "init_failed", "unexpected_success"},
                        5s))
                {
                    return;
                }

                const auto lines = sintra::test::read_lines(marker_path(dir, marker));
                helper_rejected = !lines.empty() && lines.front() == "rejected";
                invitation_preserved =
                    sintra::cancel_external_process_invitation(invitation);
            }
        }),
        k_failure_prefix,
        "shutdown-hook claim-rejection shutdown should complete");
    ok &= sintra::test::assert_true(
        helper_rejected,
        k_failure_prefix,
        "shutdown-hook helper init should be rejected while teardown admission is closed");
    ok &= sintra::test::assert_true(
        invitation_preserved,
        k_failure_prefix,
        "shutdown-hook rejected claim should leave the original invitation cancelable");
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 5s),
        k_failure_prefix,
        "shutdown-hook delayed helper should exit after rejected init");

    return ok;
}

bool run_shutdown_with_admitted_alive_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "admitted-alive should create an invitation");

    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_alive, "alive", invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "admitted-alive helper should launch");
    ok &= wait_for_marker(dir, "alive", "ready", 5s);

    std::chrono::milliseconds elapsed{};
    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "admitted-alive", &elapsed),
        k_failure_prefix,
        "shutdown with an admitted live external helper should complete");
    ok &= sintra::test::assert_true(
        elapsed < 3s,
        k_failure_prefix,
        "admitted live external helper should not block coordinator shutdown");

    write_control_file(dir, "alive", ".release");
    ok &= wait_for_marker(dir, "alive_done", "released", 5s);
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 12s),
        k_failure_prefix,
        "admitted-alive helper should exit after cooperative release");

    return ok;
}

bool run_crash_after_init_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "crash-after-init should create an invitation");

    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_crash, "crash", invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "crash-after-init helper should launch");
    ok &= wait_for_marker(dir, "crash_entry_0", "ready", 5s);

    write_control_file(dir, "crash", ".crash");
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 5s),
        k_failure_prefix,
        "external helper should exit after intentional crash");

    std::this_thread::sleep_for(500ms);
    ok &= sintra::test::assert_true(
        !std::filesystem::exists(marker_path(dir, "crash_respawned")) &&
            !std::filesystem::exists(marker_path(dir, "crash_entry_1")),
        k_failure_prefix,
        "external crash should not trigger automatic recovery");
    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "crash-after-init"),
        k_failure_prefix,
        "shutdown after an externally attached helper crash should complete");

    return ok;
}

bool run_enable_recovery_after_admission_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};

    sintra::set_recovery_policy([&](const sintra::Crash_info&) {
        write_marker(dir, "recovery_policy_called", "called");
        return true;
    });

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "external enable-recovery should create an invitation");

    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_recover, "recover", invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "external enable-recovery helper should launch");
    ok &= wait_for_marker(dir, "recover_entry_0", "ready", 5s);
    ok &= wait_for_marker_in(dir, "recover_enable_recovery", {"returned", "threw"}, 5s);

    write_control_file(dir, "recover", ".crash");
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 5s),
        k_failure_prefix,
        "external enable-recovery helper should exit after intentional crash");

    std::this_thread::sleep_for(700ms);
    ok &= sintra::test::assert_true(
        !std::filesystem::exists(marker_path(dir, "recovery_policy_called")),
        k_failure_prefix,
        "external enable_recovery() must not enroll the helper in recovery");
    ok &= sintra::test::assert_true(
        !std::filesystem::exists(marker_path(dir, "recover_respawned")) &&
            !std::filesystem::exists(marker_path(dir, "recover_entry_1")),
        k_failure_prefix,
        "external enable_recovery() must not respawn an attached helper");
    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "external-enable-recovery"),
        k_failure_prefix,
        "shutdown after rejected external recovery should complete");

    return ok;
}

bool attach_and_release_for_reuse(
    const std::string&                         binary_path,
    const std::filesystem::path&               dir,
    const std::string&                         marker,
    const sintra::External_process_invitation& invitation)
{
    bool ok = true;
    const auto helper = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_reuse, marker, invitation));
    ok &= sintra::test::assert_true(
        helper.launched,
        k_failure_prefix,
        "reuse helper should launch");
    ok &= wait_for_marker(dir, marker, "ready", 5s);
    write_control_file(dir, marker, ".release");
    ok &= wait_for_marker(dir, marker + "_done", "left", 5s);
    ok &= sintra::test::assert_true(
        wait_for_process_exit(helper.pid, 5s),
        k_failure_prefix,
        "reuse helper should exit after leave");
    return ok;
}

bool wait_for_reusable_invitation(
    sintra::instance_id_type               process_iid,
    sintra::External_process_invitation&   out_invitation)
{
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline) {
        out_invitation = make_invitation(process_iid, 30s);
        if (out_invitation) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

bool run_cancel_expire_reuse_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};
    bool ok = true;

    const auto canceled_iid = sintra::make_process_instance_id();
    auto       canceled     = make_invitation(canceled_iid, 30s);
    ok &= sintra::test::assert_true(
        static_cast<bool>(canceled),
        k_failure_prefix,
        "canceled explicit-id invitation should be created");
    ok &= sintra::test::assert_true(
        sintra::cancel_external_process_invitation(canceled),
        k_failure_prefix,
        "explicit-id invitation should be cancelable before claim");

    sintra::External_process_invitation canceled_reuse;
    ok &= sintra::test::assert_true(
        wait_for_reusable_invitation(canceled_iid, canceled_reuse),
        k_failure_prefix,
        "canceled explicit id should be reusable after cleanup grace");
    if (canceled_reuse) {
        ok &= attach_and_release_for_reuse(binary_path, dir, "canceled_reuse", canceled_reuse);
    }

    const auto expired_iid = sintra::make_process_instance_id();
    auto       expired     = make_invitation(expired_iid, 100ms);
    ok &= sintra::test::assert_true(
        static_cast<bool>(expired),
        k_failure_prefix,
        "expiring explicit-id invitation should be created");

    sintra::External_process_invitation expired_reuse;
    ok &= sintra::test::assert_true(
        wait_for_reusable_invitation(expired_iid, expired_reuse),
        k_failure_prefix,
        "expired explicit id should be reusable after cleanup grace");
    if (expired_reuse) {
        ok &= attach_and_release_for_reuse(binary_path, dir, "expired_reuse", expired_reuse);
    }

    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "cancel-expire-reuse"),
        k_failure_prefix,
        "shutdown after canceled/expired reuse should complete");

    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    std::set_terminate(sintra::test::custom_terminate_handler);

    const auto role = sintra::test::get_argv_value(argc, argv, k_role_arg);
    if (role == k_role_delayed) { return run_delayed_init_helper(              argc, argv); }
    if (role == k_role_alive)   { return run_admitted_alive_helper(            argc, argv); }
    if (role == k_role_crash)   { return run_crash_after_init_helper(          argc, argv); }
    if (role == k_role_recover) { return run_enable_recovery_after_init_helper(argc, argv); }
    if (role == k_role_reuse)   { return run_reuse_attach_helper(              argc, argv); }

    const std::string binary_path = sintra::test::get_binary_path(argc, argv);
    const auto        dir         = sintra::test::unique_scratch_directory("ext_attach_life_neg");

    bool ok = true;
    ok &= run_shutdown_before_claim_case(argc, argv, binary_path, dir);
    ok &= run_shutdown_with_admitted_alive_case(argc, argv, binary_path, dir);
    ok &= run_crash_after_init_case(argc, argv, binary_path, dir);
    ok &= run_enable_recovery_after_admission_case(argc, argv, binary_path, dir);
    ok &= run_cancel_expire_reuse_case(argc, argv, binary_path, dir);
    ok &= run_shutdown_hook_claim_rejection_preserves_invitation_case(argc, argv, binary_path, dir);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    return ok ? 0 : 1;
}
