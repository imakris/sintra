#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "managed_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sintra/detail/sintra_windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

using sintra::test::managed_child::make_invitation;

using namespace std::chrono_literals;

constexpr const char* k_role_arg                       = "--external_attach_negative_role";
constexpr const char* k_dir_arg                        = "--external_attach_negative_dir";
constexpr const char* k_marker_arg                     = "--external_attach_negative_marker";
constexpr const char* k_root_case_arg                  = "--external_attach_negative_root_case";
constexpr const char* k_root_case_reader_retirement    = "reader_retirement";
constexpr const char* k_root_case_reader_start_failure = "reader_retirement_worker_start_failure";
constexpr const char* k_root_case_reader_teardown_race =
    "reader_retirement_worker_teardown_race";
constexpr const char* k_root_case_stale_generation     = "stale_generation";
constexpr const char* k_reader_retirement_worker_start_stage =
    "external_process_reader_retirement_worker_start";
constexpr const char* k_role_delayed                   = "delayed_init";
constexpr const char* k_role_alive                     = "admitted_alive";
constexpr const char* k_role_crash                     = "crash_after_init";
constexpr const char* k_role_recover                   = "enable_recovery_after_init";
constexpr const char* k_role_reuse                     = "reuse_attach";

constexpr int           k_stale_external_crash_status     = 71;
constexpr std::uint32_t k_canceled_reuse_process_index    = 95;
constexpr std::uint32_t k_expired_reuse_process_index     = 96;
constexpr std::uint32_t k_stale_generation_process_index  = 97;
constexpr std::uint32_t k_reader_retirement_process_index = 98;

constexpr const char* k_failure_prefix = "external_process_invitation_lifecycle_negative_test: ";
constexpr const char* k_external_attach_rejected_message =
    "Sintra external process invitation was rejected.";

struct Reader_retirement_gate;

std::mutex              g_create_invitation_hook_mutex;
std::condition_variable g_create_invitation_hook_cv;
bool                    g_create_invitation_hook_armed    = false;
bool                    g_create_invitation_hook_reached  = false;
bool                    g_create_invitation_hook_released = false;
bool                    g_create_invitation_reserve_seen  = false;
std::atomic<sintra::instance_id_type>
                        g_reader_retirement_process_iid{sintra::invalid_instance_id};
std::atomic<std::uint32_t>
                        g_reader_retirement_occurrence{0};
std::atomic<unsigned>   g_reader_retirement_stop_calls{0};
std::atomic<unsigned>   g_reader_retirement_start_failures{0};
std::atomic<Reader_retirement_gate*>
                        g_reader_retirement_gate{nullptr};

#ifdef _WIN32
HANDLE g_abort_ack_handle = INVALID_HANDLE_VALUE;
using Abort_signal_handler = void (*)(int);
Abort_signal_handler g_previous_abort_handler = SIG_DFL;
#else
int              g_abort_ack_fd = -1;
struct sigaction g_previous_abort_action {};
#endif

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

struct Reader_retirement_gate
{
    ~Reader_retirement_gate()
    {
        release();
        auto* expected = this;
        (void)g_reader_retirement_gate.compare_exchange_strong(
            expected,
            nullptr,
            std::memory_order_acq_rel);
    }

    void arm(std::shared_ptr<sintra::Process_message_reader> exact_reader)
    {
        reader = std::move(exact_reader);
        g_reader_retirement_gate.store(this, std::memory_order_release);
    }

    void block()
    {
        std::unique_lock lock(mutex);
        reader_running = reader && reader->running_for_test();
        reached = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }

    bool wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return reached; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }

    std::mutex              mutex;
    std::condition_variable changed;
    bool                    reached        = false;
    bool                    released       = false;
    bool                    reader_running = false;
    std::shared_ptr<sintra::Process_message_reader> reader;
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

#ifdef _WIN32
void acknowledge_abort_signal(int signal_number)
{
    constexpr char acknowledgement[] = "caught\n";
    DWORD written = 0;
    (void)WriteFile(
        g_abort_ack_handle,
        acknowledgement,
        static_cast<DWORD>(sizeof(acknowledgement) - 1),
        &written,
        nullptr);
    g_previous_abort_handler(signal_number);
}
#else
void acknowledge_abort_signal(int signal_number, siginfo_t* info, void* context)
{
    constexpr char acknowledgement[] = "caught\n";
    (void)::write(g_abort_ack_fd, acknowledgement, sizeof(acknowledgement) - 1);

    const auto previous = g_previous_abort_action;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal_number, info, context);
    }
    else {
        previous.sa_handler(signal_number);
    }
}
#endif

bool install_abort_signal_acknowledgement(
    const std::filesystem::path& dir,
    const std::string&           marker)
{
    const auto path = marker_path(dir, marker);

#ifdef _WIN32
    g_abort_ack_handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (g_abort_ack_handle == INVALID_HANDLE_VALUE) {
        std::fprintf(
            stderr,
            "%scould not create abort acknowledgement marker (error %lu)\n",
            k_failure_prefix,
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    // Keep Sintra's signal dispatcher (and its optional debug-pause predecessor)
    // in the chain while recording that abort actually raised SIGABRT.
    const auto previous = std::signal(SIGABRT, acknowledge_abort_signal);
    if (previous == SIG_ERR || previous == SIG_DFL || previous == SIG_IGN) {
        std::signal(SIGABRT, previous == SIG_ERR ? SIG_DFL : previous);
        CloseHandle(g_abort_ack_handle);
        g_abort_ack_handle = INVALID_HANDLE_VALUE;
        std::fprintf(stderr, "%scould not chain Sintra's abort handler\n", k_failure_prefix);
        return false;
    }
    g_previous_abort_handler = previous;
#else
    g_abort_ack_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g_abort_ack_fd == -1) {
        std::fprintf(
            stderr,
            "%scould not create abort acknowledgement marker (errno %d)\n",
            k_failure_prefix,
            errno);
        return false;
    }

    struct sigaction previous {};
    if (sigaction(SIGABRT, nullptr, &previous) != 0) {
        std::fprintf(
            stderr,
            "%scould not read the installed abort handler (errno %d)\n",
            k_failure_prefix,
            errno);
        ::close(g_abort_ack_fd);
        g_abort_ack_fd = -1;
        return false;
    }
    const bool has_sintra_handler =
        (previous.sa_flags & SA_SIGINFO)
            ? previous.sa_sigaction != nullptr
            : previous.sa_handler != nullptr &&
                previous.sa_handler != SIG_DFL &&
                previous.sa_handler != SIG_IGN;
    if (!has_sintra_handler) {
        std::fprintf(stderr, "%scould not chain Sintra's abort handler\n", k_failure_prefix);
        ::close(g_abort_ack_fd);
        g_abort_ack_fd = -1;
        return false;
    }

    g_previous_abort_action = previous;
    auto acknowledgement_action = previous;
    acknowledgement_action.sa_sigaction = acknowledge_abort_signal;
    acknowledgement_action.sa_flags |= SA_SIGINFO;
    if (sigaction(SIGABRT, &acknowledgement_action, nullptr) != 0) {
        std::fprintf(
            stderr,
            "%scould not install abort acknowledgement handler (errno %d)\n",
            k_failure_prefix,
            errno);
        ::close(g_abort_ack_fd);
        g_abort_ack_fd = -1;
        return false;
    }
#endif
    return true;
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

enum class Expected_child_exit
{
    clean,
    intentional_crash
};

bool wait_for_expected_exit(
    sintra::test::Exact_child& child,
    Expected_child_exit       expected,
    std::chrono::milliseconds timeout,
    const char*               message)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = child.poll();
        if (state == sintra::test::Exact_child_state::exited) {
            bool expected_status = false;
            if (expected == Expected_child_exit::clean) {
                expected_status = child.exited_with_code(0);
            }
            else {
#ifdef _WIN32
                // Sintra terminates with 1 when it owns the end of the chain;
                // the harness's disabled debug-pause handler re-raises to UCRT's 3.
                expected_status =
                    child.exited_with_code(1) || child.exited_with_code(3) ||
                    child.exited_with_code(EXCEPTION_ACCESS_VIOLATION);
#else
                expected_status = child.exited_from_signal(SIGABRT);
#endif
            }

            const auto status = child.describe_status();
            std::string settle_diagnostic;
            const bool settled = child.settle_observed_exit(settle_diagnostic);
            if (!expected_status || !settled) {
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
                expected_status && settled,
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
        std::this_thread::sleep_for(20ms);
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

bool has_argv_flag_or_value(int argc, char* argv[], std::string_view flag)
{
    return sintra::test::has_argv_flag(argc, argv, flag);
}

void create_invitation_stage_hook(const char* stage)
{
    if (std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_create_invitation_pre_admission_lock)
    {
        return;
    }

    std::unique_lock lock(g_create_invitation_hook_mutex);
    if (!g_create_invitation_hook_armed) {
        return;
    }

    g_create_invitation_hook_reached = true;
    g_create_invitation_hook_cv.notify_all();
    g_create_invitation_hook_cv.wait(lock, [] {
        return g_create_invitation_hook_released;
    });
}

void reset_create_invitation_stage_hook()
{
    std::lock_guard lock(g_create_invitation_hook_mutex);
    g_create_invitation_hook_armed    = false;
    g_create_invitation_hook_reached  = false;
    g_create_invitation_hook_released = false;
    g_create_invitation_reserve_seen  = false;
}

bool wait_for_create_invitation_stage(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(g_create_invitation_hook_mutex);
    return g_create_invitation_hook_cv.wait_for(lock, timeout, [] {
        return g_create_invitation_hook_reached;
    });
}

void release_create_invitation_stage_hook()
{
    {
        std::lock_guard lock(g_create_invitation_hook_mutex);
        g_create_invitation_hook_released = true;
    }
    g_create_invitation_hook_cv.notify_all();
}

void reserve_invitation_stage_hook(const char* stage)
{
    if (std::string_view(stage) !=
        sintra::detail::test_hooks::k_stage_reserve_external_invitation_entered)
    {
        return;
    }

    std::lock_guard lock(g_create_invitation_hook_mutex);
    if (g_create_invitation_hook_armed) {
        g_create_invitation_reserve_seen = true;
    }
}

void reader_retirement_stop_hook(
    const char*               stage,
    sintra::instance_id_type  process_iid,
    std::uint32_t             occurrence)
{
    if (std::string_view(stage) ==
            sintra::detail::test_hooks::k_process_reader_rpc_unblock_entered &&
        process_iid == g_reader_retirement_process_iid.load(
            std::memory_order_acquire) &&
        occurrence == g_reader_retirement_occurrence.load(
            std::memory_order_acquire))
    {
        const auto call_number =
            g_reader_retirement_stop_calls.fetch_add(
                1,
                std::memory_order_acq_rel) + 1;
        if (call_number == 2) {
            if (auto* gate = g_reader_retirement_gate.load(
                    std::memory_order_acquire))
            {
                gate->block();
            }
        }
    }
}

bool reader_retirement_worker_start_failure_hook(
    const char*               stage,
    sintra::instance_id_type  process_iid,
    std::uint32_t             occurrence) noexcept
{
    if (std::string_view(stage) != k_reader_retirement_worker_start_stage ||
        process_iid != g_reader_retirement_process_iid.load(
            std::memory_order_acquire) ||
        occurrence != g_reader_retirement_occurrence.load(
            std::memory_order_acquire))
    {
        return false;
    }

    return g_reader_retirement_start_failures.fetch_add(
        1,
        std::memory_order_acq_rel) == 0;
}

bool external_process_state_absent(sintra::instance_id_type process_iid)
{
    std::lock_guard lock(sintra::s_coord->m_publish_mutex);
    return
        sintra::s_coord->m_transceiver_registry.count(process_iid) == 0 &&
        sintra::s_coord->m_external_attached_processes.count(process_iid) == 0;
}

bool external_process_state_present(sintra::instance_id_type process_iid)
{
    std::lock_guard lock(sintra::s_coord->m_publish_mutex);
    return
        sintra::s_coord->m_transceiver_registry.count(process_iid) == 1 &&
        sintra::s_coord->m_external_attached_processes.count(process_iid) == 1;
}

bool remove_exact_external_reader_for_reuse(
    sintra::instance_id_type process_iid,
    std::uint32_t            occurrence)
{
    {
        std::shared_lock lock(sintra::s_mproc->m_readers_mutex);
        const auto found = sintra::s_mproc->m_readers.find(process_iid);
        if (found == sintra::s_mproc->m_readers.end()) {
            return true;
        }
        if (!found->second ||
            found->second->get_process_instance_id() != process_iid ||
            found->second->get_occurrence() != occurrence ||
            found->second->get_managed_child_custody_identity() != 0)
        {
            return false;
        }
    }

    sintra::s_mproc->remove_process_reader(process_iid, 1.0);
    return !sintra::s_mproc->has_process_reader(process_iid);
}

bool exact_process_is_live(int pid, std::uint64_t start_stamp)
{
    if (pid <= 0 || !sintra::is_process_alive(static_cast<std::uint32_t>(pid))) {
        return false;
    }
    const auto observed = sintra::query_process_start_stamp(
        static_cast<std::uint32_t>(pid));
    return observed && *observed == start_stamp;
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
#ifdef _WIN32
    sintra::test::install_test_host_terminal_exception_filter();
#else
    if (!install_abort_signal_acknowledgement(dir, marker + "_abort_signal")) {
        sintra::detail::finalize();
        return 2;
    }
#endif
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
    write_marker(dir, marker + "_abort_reached", "reached");
#ifdef _WIN32
    void* page = VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (!page) {
        return 2;
    }
    std::thread([page]() {
        *static_cast<volatile unsigned char*>(page) = 1;
    }).join();
#else
    std::abort();
#endif
    return 2;
}

int run_enable_recovery_after_init_helper(int argc, char* argv[])
{
    const auto dir    = std::filesystem::path(sintra::test::get_argv_value(argc, argv, k_dir_arg));
    const auto marker = sintra::test::get_argv_value(argc, argv, k_marker_arg);

    sintra::init(argc, argv);
    if (!install_abort_signal_acknowledgement(dir, marker + "_abort_signal")) {
        sintra::detail::finalize();
        return 2;
    }
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
    write_marker(dir, marker + "_abort_reached", "reached");
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

    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_delayed, "delayed", invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
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
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::clean,
        5s,
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
    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_delayed, marker, invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
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
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::clean,
        5s,
        "shutdown-hook delayed helper should exit after rejected init");

    return ok;
}

bool run_create_invitation_after_finalize_reopens_case(int argc, char* argv[])
{
    sintra::init(argc, argv);

    bool invitation_valid = true;
    bool creator_returned = false;
    bool ok = true;

    reset_create_invitation_stage_hook();
    {
        std::lock_guard lock(g_create_invitation_hook_mutex);
        g_create_invitation_hook_armed = true;
    }
    sintra::detail::test_hooks::s_runtime_stage.store(
        &create_invitation_stage_hook,
        std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &reserve_invitation_stage_hook,
        std::memory_order_release);

    Shutdown_watchdog watchdog("create-invitation-after-finalize-reopens", 8s);
    std::thread creator([&] {
        auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
        invitation_valid = static_cast<bool>(invitation);
        creator_returned = true;
    });

    ok &= sintra::test::assert_true(
        wait_for_create_invitation_stage(3s),
        k_failure_prefix,
        "create-invitation stale-runtime gate should reach the pre-admission hook");

    sintra::detail::finalize();
    release_create_invitation_stage_hook();

    if (creator.joinable()) {
        creator.join();
    }

    sintra::detail::test_hooks::s_runtime_stage.store(
        nullptr,
        std::memory_order_release);
    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr,
        std::memory_order_release);

    bool reserve_seen = false;
    {
        std::lock_guard lock(g_create_invitation_hook_mutex);
        reserve_seen = g_create_invitation_reserve_seen;
    }
    reset_create_invitation_stage_hook();

    ok &= sintra::test::assert_true(
        creator_returned,
        k_failure_prefix,
        "create-invitation stale-runtime caller should return after admission reopens");
    ok &= sintra::test::assert_true(
        !invitation_valid,
        k_failure_prefix,
        "create-invitation stale-runtime caller should receive an invalid invitation");
    ok &= sintra::test::assert_true(
        !reserve_seen,
        k_failure_prefix,
        "create-invitation stale-runtime caller must not reach coordinator reservation");

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

    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_alive, "alive", invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
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
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::clean,
        12s,
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
    Shutdown_watchdog watchdog("crash-after-init-provenance", 25s);

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;
    std::vector<sintra::process_lifecycle_event> lifecycle_events;
    std::atomic<unsigned> recovery_policy_calls{0};

    auto invitation = make_invitation(sintra::invalid_instance_id, 30s);
    bool ok = sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "crash-after-init should create an invitation");

    sintra::set_lifecycle_handler(
        [&](const sintra::process_lifecycle_event& event) {
            if (event.process_iid != invitation.process_instance_id) {
                return;
            }
            {
                std::lock_guard lock(lifecycle_mutex);
                lifecycle_events.push_back(event);
            }
            lifecycle_changed.notify_all();
        });
    sintra::set_recovery_policy([&](const sintra::Crash_info&) {
        recovery_policy_calls.fetch_add(1, std::memory_order_release);
        return false;
    });

    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_crash, "crash", invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "crash-after-init helper should launch");
    ok &= wait_for_marker(dir, "crash_entry_0", "ready", 5s);

    write_control_file(dir, "crash", ".crash");
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::intentional_crash,
        5s,
        "external helper should exit after intentional crash");
    ok &= wait_for_marker(dir, "crash_abort_reached", "reached", 1s);
#ifndef _WIN32
    ok &= wait_for_marker(dir, "crash_abort_signal", "caught", 1s);
#endif

    bool lifecycle_delivered = false;
    {
        std::unique_lock lock(lifecycle_mutex);
        lifecycle_delivered = lifecycle_changed.wait_for(
            lock,
            5s,
            [&] { return !lifecycle_events.empty(); });
    }
    sintra::s_mproc->wait_for_delivery_fence();

    std::vector<sintra::process_lifecycle_event> lifecycle_snapshot;
    {
        std::lock_guard lock(lifecycle_mutex);
        lifecycle_snapshot = lifecycle_events;
    }
    const bool exact_crash_lifecycle =
        lifecycle_snapshot.size() == 1 &&
        lifecycle_snapshot.front().process_iid == invitation.process_instance_id &&
        lifecycle_snapshot.front().why ==
            sintra::process_lifecycle_event::reason::crash &&
        lifecycle_snapshot.front().status != 0;

    ok &= sintra::test::assert_true(
        lifecycle_delivered && exact_crash_lifecycle,
        k_failure_prefix,
        "external crash should emit exactly one exact-IID nonzero-status crash lifecycle event");
    ok &= sintra::test::assert_true(
        external_process_state_absent(invitation.process_instance_id),
        k_failure_prefix,
        "external crash lifecycle should retire the process registry and attachment atomically");
    ok &= sintra::test::assert_true(
        recovery_policy_calls.load(std::memory_order_acquire) == 0,
        k_failure_prefix,
        "external crash must not enter managed-child recovery policy");
    ok &= sintra::test::assert_true(
        !std::filesystem::exists(marker_path(dir, "crash_respawned")) &&
            !std::filesystem::exists(marker_path(dir, "crash_entry_1")),
        k_failure_prefix,
        "external crash should not trigger automatic recovery");

    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    sintra::set_recovery_policy(sintra::Recovery_policy{});
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

    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_recover, "recover", invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "external enable-recovery helper should launch");
    ok &= wait_for_marker(dir, "recover_entry_0", "ready", 5s);
    ok &= wait_for_marker_in(dir, "recover_enable_recovery", {"returned", "threw"}, 5s);

    write_control_file(dir, "recover", ".crash");
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::intentional_crash,
        5s,
        "external enable-recovery helper should exit after intentional crash");
    ok &= wait_for_marker(dir, "recover_abort_reached", "reached", 1s);
    ok &= wait_for_marker(dir, "recover_abort_signal", "caught", 1s);

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

bool run_stale_external_generation_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};
    Shutdown_watchdog watchdog("stale-external-generation", 35s);
    bool ok = true;

    const auto process_iid = sintra::make_process_instance_id(
        k_stale_generation_process_index);
    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_changed;
    std::vector<sintra::process_lifecycle_event> lifecycle_events;
    std::atomic<unsigned> recovery_policy_calls{0};
    sintra::set_lifecycle_handler(
        [&](const sintra::process_lifecycle_event& event) {
            if (event.process_iid != process_iid) {
                return;
            }
            {
                std::lock_guard lock(lifecycle_mutex);
                lifecycle_events.push_back(event);
            }
            lifecycle_changed.notify_all();
        });
    sintra::set_recovery_policy([&](const sintra::Crash_info&) {
        recovery_policy_calls.fetch_add(1, std::memory_order_release);
        return false;
    });

    auto invitation_a = make_invitation(process_iid, 30s);
    const bool invitation_a_exact =
        invitation_a && invitation_a.occurrence == 1;
    ok &= sintra::test::assert_true(
        invitation_a_exact,
        k_failure_prefix,
        "stale-generation A should receive exact external occurrence 1");
    if (!invitation_a_exact) {
        sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
        sintra::set_recovery_policy(sintra::Recovery_policy{});
        (void)shutdown_with_watchdog(guard, "stale-external-generation-setup");
        return false;
    }

    sintra::test::Exact_child helper_a(2s);
    ok &= sintra::test::assert_true(
        launch_direct_process(
            binary_path,
            helper_args(dir, k_role_reuse, "stale_generation_a", invitation_a),
            helper_a),
        k_failure_prefix,
        "stale-generation A helper should launch");
    ok &= wait_for_marker(dir, "stale_generation_a", "ready", 5s);
    const auto a_start_stamp = sintra::query_process_start_stamp(
        static_cast<std::uint32_t>(helper_a.pid()));
    const bool a_exact_live = a_start_stamp &&
        exact_process_is_live(helper_a.pid(), *a_start_stamp);

    write_control_file(dir, "stale_generation_a", ".release");
    ok &= wait_for_marker(dir, "stale_generation_a_done", "left", 5s);
    const bool a_exact_exit = wait_for_expected_exit(
        helper_a,
        Expected_child_exit::clean,
        5s,
        "stale-generation A helper should leave cleanly");
    ok &= a_exact_exit;
    {
        std::unique_lock lock(lifecycle_mutex);
        ok &= sintra::test::assert_true(
            lifecycle_changed.wait_for(
                lock,
                5s,
                [&] { return !lifecycle_events.empty(); }),
            k_failure_prefix,
            "stale-generation A should emit a lifecycle event");
    }
    sintra::s_mproc->wait_for_delivery_fence();

    bool a_lifecycle_exact = false;
    {
        std::lock_guard lock(lifecycle_mutex);
        a_lifecycle_exact =
            lifecycle_events.size() == 1 &&
            lifecycle_events.front().process_iid == process_iid &&
            lifecycle_events.front().why ==
                sintra::process_lifecycle_event::reason::normal_exit &&
            lifecycle_events.front().status == 0;
        lifecycle_events.clear();
    }
    ok &= sintra::test::assert_true(
        a_exact_live,
        k_failure_prefix,
        "stale-generation A should expose an exact live PID/start identity");
    ok &= sintra::test::assert_true(
        a_lifecycle_exact,
        k_failure_prefix,
        "stale-generation A should emit one exact normal lifecycle event");
    ok &= sintra::test::assert_true(
        external_process_state_absent(process_iid),
        k_failure_prefix,
        "stale-generation A should retire its registry and attachment");
    ok &= sintra::test::assert_true(
        a_start_stamp && a_exact_exit &&
            !exact_process_is_live(helper_a.pid(), *a_start_stamp),
        k_failure_prefix,
        "stale-generation A should settle its exact native exit without a survivor");
    ok &= sintra::test::assert_true(
        remove_exact_external_reader_for_reuse(
            process_iid, invitation_a.occurrence),
        k_failure_prefix,
        "stale-generation fixture should remove only A's stopped exact reader");

    auto invitation_b = make_invitation(process_iid, 30s);
    const bool invitation_b_exact =
        invitation_b && invitation_b.occurrence == invitation_a.occurrence + 1;
    ok &= sintra::test::assert_true(
        invitation_b_exact,
        k_failure_prefix,
        "stale-generation B should reuse the IID at the next exact occurrence");
    if (!invitation_b_exact) {
        sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
        sintra::set_recovery_policy(sintra::Recovery_policy{});
        (void)shutdown_with_watchdog(guard, "stale-external-generation-reuse");
        return false;
    }

    sintra::test::Exact_child helper_b(2s);
    ok &= sintra::test::assert_true(
        launch_direct_process(
            binary_path,
            helper_args(dir, k_role_reuse, "stale_generation_b", invitation_b),
            helper_b),
        k_failure_prefix,
        "stale-generation B helper should launch");
    ok &= wait_for_marker(dir, "stale_generation_b", "ready", 5s);
    const auto b_start_stamp = sintra::query_process_start_stamp(
        static_cast<std::uint32_t>(helper_b.pid()));
    const bool b_exact_live_before = a_start_stamp && b_start_stamp &&
        exact_process_is_live(helper_b.pid(), *b_start_stamp) &&
        (helper_b.pid() != helper_a.pid() || *b_start_stamp != *a_start_stamp);
    ok &= sintra::test::assert_true(
        b_exact_live_before && external_process_state_present(process_iid),
        k_failure_prefix,
        "stale-generation B should be exactly live and published before injection");

    std::mutex relay_mutex;
    std::condition_variable relay_changed;
    bool relay_seen = false;
    std::uint64_t relayed_custody_identity = 1;
    std::uint32_t relayed_occurrence = 0;
    auto deactivate_relay = sintra::s_mproc->activate<sintra::Managed_process>(
        [&](const sintra::Managed_process::terminated_abnormally& message) {
            if (message.sender_instance_id != process_iid ||
                message.status != k_stale_external_crash_status)
            {
                return;
            }
            {
                std::lock_guard lock(relay_mutex);
                relay_seen = true;
                relayed_custody_identity =
                    message.managed_child_custody_identity;
                relayed_occurrence = message.managed_child_occurrence;
            }
            relay_changed.notify_all();
        },
        sintra::Typed_instance_id<sintra::Managed_process>(sintra::any_remote));

    sintra::Managed_process::terminated_abnormally stale(
        k_stale_external_crash_status);
    stale.sender_instance_id   = process_iid;
    stale.receiver_instance_id = sintra::any_remote;
    sintra::s_mproc->m_out_req_c->relay(
        stale, 0, invitation_a.occurrence);
    {
        std::unique_lock lock(relay_mutex);
        ok &= sintra::test::assert_true(
            relay_changed.wait_for(lock, 5s, [&] { return relay_seen; }),
            k_failure_prefix,
            "stale external crash should cross the coordinator relay");
    }
    sintra::s_mproc->wait_for_delivery_fence();

    std::vector<sintra::process_lifecycle_event> after_stale;
    {
        std::lock_guard lock(lifecycle_mutex);
        after_stale = lifecycle_events;
    }
    const bool stale_relay_exact =
        relay_seen &&
        relayed_custody_identity == 0 &&
        relayed_occurrence == invitation_a.occurrence;
    const bool b_exact_live_after = b_start_stamp &&
        exact_process_is_live(helper_b.pid(), *b_start_stamp);
    ok &= sintra::test::assert_true(
        stale_relay_exact && after_stale.empty() &&
            recovery_policy_calls.load(std::memory_order_acquire) == 0 &&
            b_exact_live_after && external_process_state_present(process_iid),
        k_failure_prefix,
        "stale external generation must not retire or recover the live replacement");

    write_control_file(dir, "stale_generation_b", ".release");
    ok &= wait_for_marker_in(
        dir,
        "stale_generation_b_done",
        {"left", "leave_failed", "timeout"},
        5s);
    const bool b_exact_exit = wait_for_expected_exit(
        helper_b,
        Expected_child_exit::clean,
        5s,
        "stale-generation B helper should leave cleanly");
    ok &= b_exact_exit;
    {
        std::unique_lock lock(lifecycle_mutex);
        (void)lifecycle_changed.wait_for(
            lock,
            5s,
            [&] { return !lifecycle_events.empty(); });
    }
    sintra::s_mproc->wait_for_delivery_fence();

    std::vector<sintra::process_lifecycle_event> final_events;
    {
        std::lock_guard lock(lifecycle_mutex);
        final_events = lifecycle_events;
    }
    const bool b_lifecycle_exact =
        final_events.size() == 1 &&
        final_events.front().process_iid == process_iid &&
        final_events.front().why ==
            sintra::process_lifecycle_event::reason::normal_exit &&
        final_events.front().status == 0;
    const bool survivors_absent =
        a_start_stamp && b_start_stamp && a_exact_exit && b_exact_exit &&
        !exact_process_is_live(helper_a.pid(), *a_start_stamp) &&
        !exact_process_is_live(helper_b.pid(), *b_start_stamp);
    ok &= sintra::test::assert_true(
        b_lifecycle_exact && external_process_state_absent(process_iid) &&
            recovery_policy_calls.load(std::memory_order_acquire) == 0 &&
            survivors_absent,
        k_failure_prefix,
        "stale-generation B should terminate normally with complete cleanup");

    deactivate_relay();
    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    sintra::set_recovery_policy(sintra::Recovery_policy{});
    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "stale-external-generation"),
        k_failure_prefix,
        "shutdown after stale external generation should complete");
    return ok;
}

bool run_stale_external_generation_isolated(const std::string& binary_path)
{
    sintra::test::Exact_child root(2s);
    const bool launched = launch_direct_process(
        binary_path,
        {k_root_case_arg, k_root_case_stale_generation},
        root);
    bool ok = sintra::test::assert_true(
        launched,
        k_failure_prefix,
        "stale-generation root should launch in an isolated process");
    if (!launched) {
        return false;
    }

    const auto root_start_stamp = sintra::query_process_start_stamp(
        static_cast<std::uint32_t>(root.pid()));
    ok &= sintra::test::assert_true(
        root_start_stamp && exact_process_is_live(root.pid(), *root_start_stamp),
        k_failure_prefix,
        "stale-generation root should expose an exact live PID/start identity");
    ok &= wait_for_expected_exit(
        root,
        Expected_child_exit::clean,
        45s,
        "isolated stale-generation root should complete cleanly");
    ok &= sintra::test::assert_true(
        root_start_stamp &&
            !exact_process_is_live(root.pid(), *root_start_stamp),
        k_failure_prefix,
        "stale-generation root should leave no exact native survivor");
    return ok;
}

bool attach_and_release_for_reuse(
    const std::string&                         binary_path,
    const std::filesystem::path&               dir,
    const std::string&                         marker,
    const sintra::External_process_invitation& invitation)
{
    bool ok = true;
    sintra::test::Exact_child helper(2s);
    const bool helper_launched = launch_direct_process(
        binary_path,
        helper_args(dir, k_role_reuse, marker, invitation),
        helper);
    ok &= sintra::test::assert_true(
        helper_launched,
        k_failure_prefix,
        "reuse helper should launch");
    ok &= wait_for_marker(dir, marker, "ready", 5s);
    write_control_file(dir, marker, ".release");
    ok &= wait_for_marker(dir, marker + "_done", "left", 5s);
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::clean,
        5s,
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

bool run_reader_retirement_case(
    int                            argc,
    char*                          argv[],
    const std::string&             binary_path,
    const std::filesystem::path&   dir,
    bool                           inject_worker_start_failure = false,
    bool                           close_worker_admission_before_retry = false)
{
    sintra::init(argc, argv);
    Runtime_guard guard{true};
    Shutdown_watchdog watchdog("external-reader-retirement", 35s);
    bool ok = true;

    const auto process_iid = sintra::make_process_instance_id(
        k_reader_retirement_process_index);
    auto invitation = make_invitation(process_iid, 30s);
    ok &= sintra::test::assert_true(
        static_cast<bool>(invitation),
        k_failure_prefix,
        "reader-retirement should create the initial invitation");

    std::shared_ptr<sintra::Process_message_reader> old_reader;
    {
        std::shared_lock lock(sintra::s_mproc->m_readers_mutex);
        const auto found = sintra::s_mproc->m_readers.find(process_iid);
        if (found != sintra::s_mproc->m_readers.end()) {
            old_reader = found->second;
        }
    }
    const bool old_reader_exact =
        old_reader &&
        old_reader->get_process_instance_id() == process_iid &&
        old_reader->get_occurrence() == invitation.occurrence &&
        old_reader->get_managed_child_custody_identity() == 0;
    ok &= sintra::test::assert_true(
        old_reader_exact,
        k_failure_prefix,
        "reader-retirement should capture the exact external reader");

    g_reader_retirement_process_iid.store(process_iid, std::memory_order_release);
    g_reader_retirement_occurrence.store(
        invitation.occurrence,
        std::memory_order_release);
    g_reader_retirement_stop_calls.store(0, std::memory_order_release);
    g_reader_retirement_start_failures.store(0, std::memory_order_release);
    Reader_retirement_gate gate;
    Reader_retirement_gate teardown_gate;
    gate.arm(old_reader);
    sintra::detail::test_hooks::s_process_reader_rpc_unblock.store(
        &reader_retirement_stop_hook,
        std::memory_order_release);
    if (inject_worker_start_failure) {
        sintra::detail::test_hooks::s_managed_child_failure.store(
            &reader_retirement_worker_start_failure_hook,
            std::memory_order_release);
    }
    if (close_worker_admission_before_retry) {
        sintra::set_lifecycle_handler(
            [process_iid, &teardown_gate](
                const sintra::process_lifecycle_event& event)
            {
                if (event.process_iid == process_iid) {
                    teardown_gate.block();
                }
            });
    }

    sintra::test::Exact_child helper(2s);
    ok &= sintra::test::assert_true(
        launch_direct_process(
            binary_path,
            helper_args(dir, k_role_crash, "reader_retirement_a", invitation),
            helper),
        k_failure_prefix,
        "reader-retirement crash helper should launch");
    ok &= wait_for_marker(
        dir,
        "reader_retirement_a_entry_0",
        "ready",
        5s);
    write_control_file(dir, "reader_retirement_a", ".crash");
    ok &= wait_for_expected_exit(
        helper,
        Expected_child_exit::intentional_crash,
        5s,
        "reader-retirement helper should exit after intentional crash");

    bool teardown_gate_reached = false;
    bool workers_empty_at_join_close = false;
    if (close_worker_admission_before_retry) {
        teardown_gate_reached = teardown_gate.wait(5s);
        if (teardown_gate_reached) {
            sintra::s_mproc->join_owned_lifecycle_workers();
            {
                std::lock_guard lock(
                    sintra::s_mproc->m_owned_lifecycle_workers_mutex);
                workers_empty_at_join_close =
                    sintra::s_mproc->m_owned_lifecycle_workers.empty();
            }
        }
        teardown_gate.release();
    }

    const bool gate_reached = gate.wait(5s);
    bool old_running_while_blocked = false;
    {
        std::lock_guard lock(gate.mutex);
        old_running_while_blocked = gate.reader_running;
    }
    const auto exact_stop_calls = g_reader_retirement_stop_calls.load(
        std::memory_order_acquire);
    const auto start_failures = g_reader_retirement_start_failures.load(
        std::memory_order_acquire);
    bool worker_admitted_after_join_close = false;
    if (close_worker_admission_before_retry) {
        std::lock_guard lock(
            sintra::s_mproc->m_owned_lifecycle_workers_mutex);
        worker_admitted_after_join_close =
            !sintra::s_mproc->m_owned_lifecycle_workers.empty();
    }
    if (close_worker_admission_before_retry) {
        ok &= sintra::test::assert_true(
            teardown_gate_reached && workers_empty_at_join_close &&
                !gate_reached && exact_stop_calls == 1 &&
                !worker_admitted_after_join_close,
            k_failure_prefix,
            "deferred reader retirement must not admit a lifecycle worker after join closure");
    }
    else {
        ok &= sintra::test::assert_true(
            gate_reached && exact_stop_calls >= 2,
            k_failure_prefix,
            "crash retirement should enter the exact old reader stop worker");
    }
    ok &= sintra::test::assert_true(
        !inject_worker_start_failure || start_failures == 1,
        k_failure_prefix,
        "reader retirement should retry after the injected worker-start failure");

    sintra::External_process_invitation blocked_invitation;
    bool replacement_transport_while_blocked = false;
    if (gate_reached) {
        blocked_invitation = make_invitation(process_iid, 30s);
        std::shared_lock lock(sintra::s_mproc->m_readers_mutex);
        const auto found = sintra::s_mproc->m_readers.find(process_iid);
        replacement_transport_while_blocked =
            found != sintra::s_mproc->m_readers.end() &&
            found->second &&
            found->second != old_reader &&
            found->second->get_occurrence() == blocked_invitation.occurrence;
    }
    sintra::detail::test_hooks::s_process_reader_rpc_unblock.store(
        nullptr,
        std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr,
        std::memory_order_release);
    g_reader_retirement_process_iid.store(
        sintra::invalid_instance_id,
        std::memory_order_release);
    g_reader_retirement_occurrence.store(0, std::memory_order_release);
    sintra::set_lifecycle_handler(sintra::Lifecycle_handler{});
    const bool blocked_rejected =
        !blocked_invitation &&
        !replacement_transport_while_blocked;
    ok &= sintra::test::assert_true(
        blocked_rejected,
        k_failure_prefix,
        "same-IID replacement must not reserve rings before the old reader stops");

    if (blocked_invitation) {
        (void)sintra::cancel_external_process_invitation(blocked_invitation);
    }
    gate.release();
    if (close_worker_admission_before_retry) {
        sintra::s_mproc->join_owned_lifecycle_workers();
    }

    bool old_reader_stopped = false;
    if (old_reader) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (old_reader->running_for_test() &&
            std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(20ms);
        }
        old_reader_stopped = !old_reader->running_for_test();
    }
    ok &= sintra::test::assert_true(
        old_reader_stopped,
        k_failure_prefix,
        "exact old external reader should stop after the gate releases");

    if (close_worker_admission_before_retry) {
        ok &= sintra::test::assert_true(
            shutdown_with_watchdog(guard, "external-reader-retirement-teardown"),
            k_failure_prefix,
            "shutdown after closing lifecycle worker admission should complete");
        std::printf(
            "EXTERNAL_READER_RETIREMENT_TEARDOWN lifecycle_gate=%d "
            "workers_empty_at_close=%d admitted_after_close=%d "
            "exact_stop_calls=%u start_failures=%u old_stopped=%d\n",
            teardown_gate_reached ? 1 : 0,
            workers_empty_at_join_close ? 1 : 0,
            worker_admitted_after_join_close ? 1 : 0,
            exact_stop_calls,
            start_failures,
            old_reader_stopped ? 1 : 0);
        return ok;
    }

    sintra::External_process_invitation replacement_invitation;
    const bool replacement_reserved = wait_for_reusable_invitation(
        process_iid,
        replacement_invitation);
    ok &= sintra::test::assert_true(
        replacement_reserved &&
            replacement_invitation.occurrence > invitation.occurrence,
        k_failure_prefix,
        "same-IID replacement should be reservable after old-reader quiescence");
    if (replacement_reserved) {
        ok &= attach_and_release_for_reuse(
            binary_path,
            dir,
            "reader_retirement_b",
            replacement_invitation);
    }

    ok &= sintra::test::assert_true(
        shutdown_with_watchdog(guard, "external-reader-retirement"),
        k_failure_prefix,
        "shutdown after exact external reader retirement should complete");

    std::printf(
        "EXTERNAL_READER_RETIREMENT gate=%d exact_stop_calls=%u "
        "start_failures=%u "
        "old_running=%d blocked_rejected=%d replacement_transport=%d "
        "old_stopped=%d replacement_reserved=%d\n",
        gate_reached ? 1 : 0,
        exact_stop_calls,
        start_failures,
        old_running_while_blocked ? 1 : 0,
        blocked_rejected ? 1 : 0,
        replacement_transport_while_blocked ? 1 : 0,
        old_reader_stopped ? 1 : 0,
        replacement_reserved ? 1 : 0);
    return ok;
}

bool run_reader_retirement_isolated(const std::string& binary_path)
{
    sintra::test::Exact_child root(2s);
    const bool launched = launch_direct_process(
        binary_path,
        {k_root_case_arg, k_root_case_reader_retirement},
        root);
    bool ok = sintra::test::assert_true(
        launched,
        k_failure_prefix,
        "reader-retirement root should launch in an isolated process");
    if (!launched) {
        return false;
    }

    ok &= wait_for_expected_exit(
        root,
        Expected_child_exit::clean,
        45s,
        "isolated reader-retirement root should complete cleanly");
    return ok;
}

bool run_reader_retirement_start_failure_isolated(
    const std::string& binary_path)
{
    sintra::test::Exact_child root(2s);
    const bool launched = launch_direct_process(
        binary_path,
        {k_root_case_arg, k_root_case_reader_start_failure},
        root);
    bool ok = sintra::test::assert_true(
        launched,
        k_failure_prefix,
        "reader-retirement start-failure root should launch in an isolated process");
    if (!launched) {
        return false;
    }

    ok &= wait_for_expected_exit(
        root,
        Expected_child_exit::clean,
        45s,
        "isolated reader-retirement start-failure root should complete cleanly");
    return ok;
}

bool run_reader_retirement_teardown_race_isolated(
    const std::string& binary_path)
{
    sintra::test::Exact_child root(2s);
    const bool launched = launch_direct_process(
        binary_path,
        {k_root_case_arg, k_root_case_reader_teardown_race},
        root);
    bool ok = sintra::test::assert_true(
        launched,
        k_failure_prefix,
        "reader-retirement teardown-race root should launch in an isolated process");
    if (!launched) {
        return false;
    }

    ok &= wait_for_expected_exit(
        root,
        Expected_child_exit::clean,
        45s,
        "isolated reader-retirement teardown-race root should complete cleanly");
    return ok;
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

    const auto canceled_iid = sintra::make_process_instance_id(
        k_canceled_reuse_process_index);
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

    const auto expired_iid = sintra::make_process_instance_id(
        k_expired_reuse_process_index);
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
    const auto        root_case   = sintra::test::get_argv_value(argc, argv, k_root_case_arg);

    if (root_case == k_root_case_stale_generation) {
        const bool ok = run_stale_external_generation_case(
            argc, argv, binary_path, dir);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ok ? 0 : 1;
    }
    if (root_case == k_root_case_reader_retirement) {
        const bool ok = run_reader_retirement_case(
            argc, argv, binary_path, dir);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ok ? 0 : 1;
    }
    if (root_case == k_root_case_reader_start_failure) {
        const bool ok = run_reader_retirement_case(
            argc, argv, binary_path, dir, true);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ok ? 0 : 1;
    }
    if (root_case == k_root_case_reader_teardown_race) {
        const bool ok = run_reader_retirement_case(
            argc, argv, binary_path, dir, true, true);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return ok ? 0 : 1;
    }

    bool ok = true;
    ok &= run_shutdown_before_claim_case(argc, argv, binary_path, dir);
    ok &= run_shutdown_with_admitted_alive_case(argc, argv, binary_path, dir);
    ok &= run_crash_after_init_case(argc, argv, binary_path, dir);
    ok &= run_enable_recovery_after_admission_case(argc, argv, binary_path, dir);
    ok &= run_stale_external_generation_isolated(binary_path);
    ok &= run_reader_retirement_isolated(binary_path);
    ok &= run_reader_retirement_start_failure_isolated(binary_path);
    ok &= run_reader_retirement_teardown_race_isolated(binary_path);
    ok &= run_cancel_expire_reuse_case(argc, argv, binary_path, dir);
    ok &= run_shutdown_hook_claim_rejection_preserves_invitation_case(argc, argv, binary_path, dir);
    ok &= run_create_invitation_after_finalize_reopens_case(argc, argv);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    return ok ? 0 : 1;
}
