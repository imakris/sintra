#include <sintra/detail/ipc/spinlock.h>
#include <sintra/detail/ipc/process_utils.h>
#include <sintra/detail/debug_pause.h>
#include <sintra/detail/logging.h>
#include <sintra/detail/time_utils.h>
#include <sintra/detail/utility.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace {

constexpr std::string_view k_failure_prefix = "spinlock_recovery_test failure: ";
constexpr auto k_child_poll_interval = std::chrono::milliseconds(10);
constexpr auto k_child_cleanup_timeout = std::chrono::seconds(10);
#ifdef _WIN32
// Newer MinGW/UCRT aborts through Windows fail-fast. GetExitCodeProcess exposes
// only its generic outer status; the legacy STATUS_STACK_BUFFER_OVERRUN name
// does not mean this controlled abort path overran a buffer.
constexpr std::uint32_t k_windows_fast_fail_exit_code = 0xC0000409u;
#endif

using sintra::test::Exact_child;
using sintra::test::Exact_child_state;

struct spinlock_layout_t
{
    std::atomic_flag       m_locked;
    std::atomic<uint32_t>  m_owner_pid;
    std::atomic<uint64_t>  m_last_progress_ns;
};

spinlock_layout_t& access_layout(sintra::spinlock& lock)
{
    static_assert(std::is_standard_layout_v<sintra::spinlock>, "spinlock must be standard layout");
    static_assert(sizeof(spinlock_layout_t) == sizeof(sintra::spinlock), "spinlock layout mismatch");
    return *reinterpret_cast<spinlock_layout_t*>(&lock);
}

uint32_t find_dead_pid(uint32_t self_pid)
{
    for (uint32_t candidate = 500000; candidate < 510000; ++candidate) {
        if (candidate == self_pid)                { continue;         }
        if (!sintra::is_process_alive(candidate)) { return candidate; }
    }

    for (uint32_t candidate = self_pid + 1; candidate < self_pid + 10000; ++candidate) {
        if (!sintra::is_process_alive(candidate)) {
            return candidate;
        }
    }

    return 0;
}

[[noreturn]] void fail_after_settling_child(Exact_child& child, std::string message)
{
    std::string cleanup_diagnostic;
    if (!child.terminate_and_settle(cleanup_diagnostic)) {
        message += "; exact-child cleanup failed: ";
        message += cleanup_diagnostic;
    }
    sintra::test::fail(k_failure_prefix, message);
}

bool exited_as_expected_abort(const Exact_child& child) noexcept
{
#ifdef _WIN32
    return child.exited_with_code(3) ||
        child.exited_with_code(k_windows_fast_fail_exit_code);
#else
    return child.exited_from_signal(SIGABRT);
#endif
}

bool publish_ready_marker(
    const std::filesystem::path& marker_path,
    std::string_view             token)
{
    std::filesystem::path temporary_path = marker_path;
    temporary_path += ".tmp." + std::to_string(sintra::test::get_pid());

    FILE* output = std::fopen(temporary_path.string().c_str(), "wb");
    if (!output) {
        return false;
    }

    const bool wrote = std::fwrite(token.data(), 1, token.size(), output) == token.size();
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (!wrote || !flushed || !closed) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, marker_path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return false;
    }
    return true;
}

enum class Marker_state
{
    absent,
    valid,
    invalid,
    error
};

Marker_state probe_ready_marker(
    const std::filesystem::path& marker_path,
    std::string_view             expected_token,
    std::string&                 diagnostic)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(marker_path, exists_error);
    if (exists_error) {
        diagnostic = "marker existence check failed: " + exists_error.message();
        return Marker_state::error;
    }
    if (!exists) {
        return Marker_state::absent;
    }

    std::ifstream input(marker_path, std::ios::binary);
    if (!input) {
        diagnostic = "ready marker exists but could not be opened";
        return Marker_state::error;
    }
    const std::string observed{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        diagnostic = "ready marker could not be read completely";
        return Marker_state::error;
    }
    if (observed != expected_token) {
        diagnostic = "ready marker token mismatch";
        return Marker_state::invalid;
    }
    return Marker_state::valid;
}

struct Ready_marker
{
    std::filesystem::path directory;
    std::filesystem::path path;
    std::string           token;
};

Ready_marker make_ready_marker(const std::string& label, uint32_t self_pid)
{
    const auto pid   = std::to_string(self_pid);
    const auto nonce = std::to_string(sintra::monotonic_now_ns());
    Ready_marker marker;
    marker.directory = sintra::test::unique_scratch_directory("spinlock_recovery_" + label);
    marker.path      = marker.directory / (label + "-ready-" + pid + '-' + nonce + ".marker");
    marker.token     = "spinlock-" + label + "-ready:" + pid + ':' + nonce;
    return marker;
}

void remove_ready_marker(const Ready_marker& marker)
{
    std::error_code cleanup_error;
    std::filesystem::remove(marker.path, cleanup_error);
    cleanup_error.clear();
    std::filesystem::remove(marker.directory, cleanup_error);
}

// Two contenders observe the same dead owner. The recovery warning is logged
// after the late contender's observation and before it acts on it, so this
// callback lets the racing contender recover the lock inside that window.
struct Stale_recovery_race
{
    sintra::spinlock*  lock = nullptr;
    std::string        dead_owner_warning;
    std::atomic<int>   dead_owner_warnings{0};
    std::atomic<bool>  racing_contender_acquired{false};
    std::atomic<bool>  racing_contender_holds{false};
    std::atomic<bool>  late_contender_acquired{false};
    bool               recovered_inside_window = false;
    std::thread        racing_contender;
};

// Both contenders share a pid, so a racing-contender hold that the late
// contender observes for the 2 s live-owner timeout aborts the test. These
// bounds keep the whole race well inside that timeout.
constexpr auto k_racing_contender_hold    = std::chrono::milliseconds(200);
constexpr auto k_racing_contender_timeout = std::chrono::seconds(1);

void run_racing_contender(Stale_recovery_race& race)
{
    race.lock->lock();
    race.racing_contender_holds = true;
    race.racing_contender_acquired = true;

    // The late contender can acquire only after this release unless it
    // breaks exclusion, so bound the hold instead of waiting for it.
    const auto release_deadline = std::chrono::steady_clock::now() + k_racing_contender_hold;
    while (!race.late_contender_acquired && std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    race.racing_contender_holds = false;
    race.lock->unlock();
}

void recover_inside_late_contender_window(
    sintra::log_level level,
    const char*       message,
    void*             user_data)
{
    auto& race = *static_cast<Stale_recovery_race*>(user_data);
    if (std::string_view(message).find(race.dead_owner_warning) == std::string_view::npos) {
        sintra::detail::default_log_callback(level, message, nullptr);
        return;
    }
    if (race.dead_owner_warnings.fetch_add(1) != 0) {
        return;
    }

    race.racing_contender = std::thread(run_racing_contender, std::ref(race));
    const auto acquire_deadline = std::chrono::steady_clock::now() + k_racing_contender_timeout;
    while (!race.racing_contender_acquired && std::chrono::steady_clock::now() < acquire_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    race.recovered_inside_window = race.racing_contender_acquired;
}

// Children that expect to abort must do so without a dialog or crash report.
bool prepare_abort_expecting_child()
{
    sintra::detail::set_debug_pause_active(false);
    sintra::test::prepare_for_intentional_crash();
    if (std::signal(SIGABRT, SIG_DFL) == SIG_ERR) {
        return false;
    }
#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (_set_error_mode(_OUT_TO_STDERR) == -1) {
        return false;
    }
#endif
    return true;
}

// The live-owner timeout is 2 s. The stream of short holds and the stopped
// waiter both outlast it, while every individual hold stays far below it.
constexpr auto k_short_hold            = std::chrono::milliseconds(1);
constexpr auto k_short_hold_stream     = std::chrono::seconds(3);
constexpr auto k_waiter_stop           = std::chrono::seconds(3);
constexpr auto k_waiter_observation    = std::chrono::milliseconds(100);
constexpr auto k_timed_child_deadline  = std::chrono::seconds(20);

// Ends one short hold and starts the next by the same thread, with the owner
// and stamp writes of unlock() and lock(), as if this thread won every race
// against the waiter. The flag stays set, so the waiter cannot acquire.
void hand_over_to_next_hold(spinlock_layout_t& layout, uint32_t self_pid)
{
    layout.m_owner_pid.store(0, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
    layout.m_owner_pid.store(self_pid, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
}

// A waiter behind a stream of short holds that together outlast the timeout
// must keep waiting. The marker records that it did; the final hold is then
// kept until the waiter's timeout aborts the process.
int run_short_holds_child(
    const std::filesystem::path& marker_path,
    std::string_view             marker_token)
{
    const uint32_t self_pid = static_cast<uint32_t>(sintra::detail::get_current_process_id());
    sintra::spinlock short_hold_lock;
    auto& layout = access_layout(short_hold_lock);
    short_hold_lock.lock();

    std::atomic<bool> waiter_started{false};
    std::thread waiter([&] {
        waiter_started = true;
        short_hold_lock.lock();
        short_hold_lock.unlock();
    });
    while (!waiter_started) {
        std::this_thread::yield();
    }

    const auto stream_end = std::chrono::steady_clock::now() + k_short_hold_stream;
    while (std::chrono::steady_clock::now() < stream_end) {
        std::this_thread::sleep_for(k_short_hold);
        hand_over_to_next_hold(layout, self_pid);
    }
    if (!publish_ready_marker(marker_path, marker_token)) {
        std::_Exit(2);
    }
    waiter.join();
    return 1;
}

#ifndef _WIN32
std::atomic<bool> g_stopped_waiter_resumed{false};

void stop_waiter_thread(int)
{
    timespec remaining{
        static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(k_waiter_stop).count()),
        0};
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {}
    g_stopped_waiter_resumed.store(true);
}
#endif

// A suspend stops the holder and the waiter alike. A waiter that was stopped
// for longer than the timeout must not count that time against a hold that is
// released soon after both run again.
int run_stopped_waiter_child()
{
    sintra::spinlock stopped_lock;
    stopped_lock.lock();

    std::atomic<bool> waiter_started{false};
    std::atomic<bool> waiter_acquired{false};
#ifdef _WIN32
    HANDLE waiter_handle = nullptr;
#endif
    std::thread waiter([&] {
#ifdef _WIN32
        waiter_handle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
#endif
        waiter_started = true;
        stopped_lock.lock();
        waiter_acquired = true;
        stopped_lock.unlock();
    });
    while (!waiter_started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(k_waiter_observation);

#ifdef _WIN32
    if (!waiter_handle || SuspendThread(waiter_handle) == static_cast<DWORD>(-1)) {
        std::_Exit(2);
    }
    std::this_thread::sleep_for(k_waiter_stop);
    if (ResumeThread(waiter_handle) == static_cast<DWORD>(-1)) {
        std::_Exit(2);
    }
    CloseHandle(waiter_handle);
#else
    struct sigaction stop_action {};
    stop_action.sa_handler = stop_waiter_thread;
    sigemptyset(&stop_action.sa_mask);
    if (sigaction(SIGUSR1, &stop_action, nullptr) != 0 ||
        pthread_kill(waiter.native_handle(), SIGUSR1) != 0)
    {
        std::_Exit(2);
    }
    while (!g_stopped_waiter_resumed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif

    // Keep the same hold across the resumed waiter's next polls.
    std::this_thread::sleep_for(k_waiter_observation);
    const bool acquired_while_held = waiter_acquired.load();
    stopped_lock.unlock();
    waiter.join();
    return acquired_while_held ? 1 : 0;
}

// Runs an exact child to an exit it must reach within the deadline.
void run_child_to_exit(
    Exact_child&        child,
    const char*         program,
    const char* const*  args,
    std::string_view    context)
{
    const std::string prefix(context);
    if (!child.spawn(program, args)) {
        fail_after_settling_child(child, prefix + " failed to spawn exact child: " + child.error());
    }

    const auto deadline = std::chrono::steady_clock::now() + k_timed_child_deadline;
    while (true) {
        const auto child_state = child.poll();
        if (child_state == Exact_child_state::exited) {
            break;
        }
        if (child_state == Exact_child_state::error) {
            fail_after_settling_child(
                child,
                prefix + " exact-child observation failed: " + child.error());
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail_after_settling_child(
                child,
                prefix + " child did not terminate within " +
                    std::to_string(k_timed_child_deadline.count()) + " seconds");
        }
        std::this_thread::sleep_for(k_child_poll_interval);
    }

    std::string settle_diagnostic;
    if (!child.settle_observed_exit(settle_diagnostic)) {
        fail_after_settling_child(
            child,
            prefix + " could not settle the exact child exit: " + settle_diagnostic);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "--spinlock-sleeper") {
        int sleep_ms = 4000;
        if (argc >= 3) {
            sleep_ms = std::atoi(argv[2]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return 0;
    }

    if (argc >= 4 && std::string_view(argv[1]) == "--spinlock-short-holds") {
        if (!prepare_abort_expecting_child()) {
            return 2;
        }
        return run_short_holds_child(std::filesystem::path(argv[2]), argv[3]);
    }

    if (argc >= 2 && std::string_view(argv[1]) == "--spinlock-stopped-waiter") {
        if (!prepare_abort_expecting_child()) {
            return 2;
        }
        return run_stopped_waiter_child();
    }

    if (argc >= 5 && std::string_view(argv[1]) == "--spinlock-stall-child") {
        if (!prepare_abort_expecting_child()) {
            return 2;
        }
        const bool same_process = std::string_view(argv[2]) == "self";
        const uint32_t owner_pid = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
        const std::filesystem::path marker_path(argv[3]);
        const std::string_view      marker_token(argv[4]);
        sintra::spinlock stall_lock;
        if (same_process) {
            sintra::spinlock::locker held_lock(stall_lock);
            if (!publish_ready_marker(marker_path, marker_token)) {
                return 2;
            }
            std::thread contender([&] { stall_lock.lock(); });
            contender.join();
            return 1;
        }
        auto& stall_layout = access_layout(stall_lock);
        stall_layout.m_locked.clear(std::memory_order_release);
        stall_layout.m_locked.test_and_set(std::memory_order_acquire);
        stall_layout.m_owner_pid.store(owner_pid, std::memory_order_release);
        stall_layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
        if (!publish_ready_marker(marker_path, marker_token)) {
            return 2;
        }
        stall_lock.lock();
        return 1;
    }

    sintra::spinlock lock;
    auto& layout = access_layout(lock);

    const uint32_t self_pid = static_cast<uint32_t>(sintra::detail::get_current_process_id());

    // Case 1: recover from a dead owner.
    const uint32_t dead_pid = find_dead_pid(self_pid);
    sintra::test::require_true(dead_pid != 0 && dead_pid != self_pid, k_failure_prefix,
        "failed to locate a dead pid");
    sintra::test::require_true(!sintra::is_process_alive(dead_pid), k_failure_prefix,
        "dead pid should not be alive");

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(dead_pid, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    lock.lock();
    lock.unlock();

    // Contenders that observe the same dead owner must not both acquire.
    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(dead_pid, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    Stale_recovery_race race;
    race.lock = &lock;
    race.dead_owner_warning = "Owner PID " + std::to_string(dead_pid) + " disappeared";
    sintra::set_log_callback(&recover_inside_late_contender_window, &race);
    lock.lock();
    const bool exclusion_broken = race.racing_contender_holds.load();
    race.late_contender_acquired = true;
    lock.unlock();
    if (race.racing_contender.joinable()) {
        race.racing_contender.join();
    }
    sintra::set_log_callback(nullptr);

    sintra::test::require_true(race.recovered_inside_window, k_failure_prefix,
        "the racing contender did not recover the dead owner's lock while the late "
        "contender was between observing the owner and acting on it");
    sintra::test::require_true(!exclusion_broken, k_failure_prefix,
        "a contender acting on a stale dead-owner observation acquired the spinlock "
        "while the contender that recovered it first still held it");

    // Case 2: live owner with debug pause active should be taken over.
    const std::string sleep_arg = "30000";
    const std::vector<const char*> sleep_args = {
        argv[0],
        "--spinlock-sleeper",
        sleep_arg.c_str(),
        nullptr
    };
    Exact_child sleep_child(k_child_cleanup_timeout);
    if (!sleep_child.spawn(argv[0], sleep_args.data())) {
        fail_after_settling_child(
            sleep_child,
            "case 2 failed to spawn exact live-owner child: " + sleep_child.error());
    }
    const auto sleep_child_state = sleep_child.poll();
    if (sleep_child_state != Exact_child_state::running) {
        fail_after_settling_child(
            sleep_child,
            "case 2 child was not authoritatively live before owner assignment: " +
                (sleep_child_state == Exact_child_state::exited
                    ? sleep_child.describe_status()
                    : sleep_child.error()));
    }
    const int child_pid = sleep_child.pid();

    layout.m_locked.clear(std::memory_order_release);
    layout.m_locked.test_and_set(std::memory_order_acquire);
    layout.m_owner_pid.store(static_cast<uint32_t>(child_pid), std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);

    sintra::detail::set_debug_pause_active(true);
    lock.lock();
    lock.unlock();
    sintra::detail::set_debug_pause_active(false);

    const auto post_recovery_child_state = sleep_child.poll();
    if (post_recovery_child_state != Exact_child_state::running) {
        fail_after_settling_child(
            sleep_child,
            "case 2 did not take over the lock while the exact owner remained live: " +
                (post_recovery_child_state == Exact_child_state::exited
                    ? sleep_child.describe_status()
                    : sleep_child.error()));
    }

    std::string sleep_cleanup_diagnostic;
    if (!sleep_child.terminate_and_settle(sleep_cleanup_diagnostic)) {
        fail_after_settling_child(
            sleep_child,
            "case 2 exact-child cleanup failed: " + sleep_cleanup_diagnostic);
    }

    // Live foreign and same-process owners must fail closed on a stalled lock.
    for (const std::string owner_arg : {std::to_string(self_pid), std::string("self")}) {
        const Ready_marker marker = make_ready_marker("stall", self_pid);
        const std::string marker_arg = marker.path.string();
        const std::vector<const char*> stall_args = {
            argv[0],
            "--spinlock-stall-child",
            owner_arg.c_str(),
            marker_arg.c_str(),
            marker.token.c_str(),
            nullptr
        };
        Exact_child stall_child(k_child_cleanup_timeout);
        if (!stall_child.spawn(argv[0], stall_args.data())) {
            fail_after_settling_child(
                stall_child,
                "case 3 failed to spawn exact stall child: " + stall_child.error());
        }

        const auto marker_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (true) {
            std::string marker_diagnostic;
            auto marker_state = probe_ready_marker(marker.path, marker.token, marker_diagnostic);
            if (marker_state == Marker_state::valid) {
                break;
            }
            if (marker_state == Marker_state::invalid || marker_state == Marker_state::error) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 readiness-marker failure: " + marker_diagnostic);
            }

            const auto child_state = stall_child.poll();
            if (child_state == Exact_child_state::exited) {
                marker_state = probe_ready_marker(marker.path, marker.token, marker_diagnostic);
                if (marker_state == Marker_state::valid) {
                    break;
                }
                fail_after_settling_child(
                    stall_child,
                    "case 3 child exited before publishing its readiness marker: " +
                        stall_child.describe_status());
            }
            if (child_state == Exact_child_state::error) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 exact-child observation failed before readiness: " +
                        stall_child.error());
            }
            if (std::chrono::steady_clock::now() >= marker_deadline) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 child did not publish its readiness marker within 15 seconds");
            }
            std::this_thread::sleep_for(k_child_poll_interval);
        }

        constexpr int stall_timeout_default_ms = 10000;
        int stall_timeout_ms = sintra::test::read_env_int(
            "SINTRA_SPINLOCK_STALL_TIMEOUT_MS",
            stall_timeout_default_ms);
        if (stall_timeout_ms <= 0) {
            stall_timeout_ms = stall_timeout_default_ms;
        }
        const auto stall_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(stall_timeout_ms);
        while (true) {
            const auto child_state = stall_child.poll();
            if (child_state == Exact_child_state::exited) {
                if (!exited_as_expected_abort(stall_child)) {
                    const auto observed = stall_child.describe_status();
                    fail_after_settling_child(
                        stall_child,
                        "case 3 stall child terminated with unexpected status: " + observed);
                }
                std::string settle_diagnostic;
                if (!stall_child.settle_observed_exit(settle_diagnostic)) {
                    fail_after_settling_child(
                        stall_child,
                        "case 3 could not settle the expected exact child exit: " +
                            settle_diagnostic);
                }
                break;
            }
            if (child_state == Exact_child_state::error) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 exact-child observation failed after readiness: " +
                        stall_child.error());
            }
            if (std::chrono::steady_clock::now() >= stall_deadline) {
                fail_after_settling_child(
                    stall_child,
                    "case 3 ready stall child did not terminate within " +
                        std::to_string(stall_timeout_ms) + " ms");
            }
            std::this_thread::sleep_for(k_child_poll_interval);
        }

        remove_ready_marker(marker);
    }

    // Case 4: a stream of short holds that together outlast the live-owner
    // timeout must not abort a waiting contender, and the single hold that
    // follows it must.
    {
        const Ready_marker marker = make_ready_marker("short_holds", self_pid);
        const std::string marker_arg = marker.path.string();
        const std::vector<const char*> short_hold_args = {
            argv[0],
            "--spinlock-short-holds",
            marker_arg.c_str(),
            marker.token.c_str(),
            nullptr
        };
        Exact_child short_hold_child(k_child_cleanup_timeout);
        run_child_to_exit(short_hold_child, argv[0], short_hold_args.data(), "case 4");

        std::string marker_diagnostic;
        const auto marker_state =
            probe_ready_marker(marker.path, marker.token, marker_diagnostic);
        remove_ready_marker(marker);
        sintra::test::require_true(marker_state == Marker_state::valid, k_failure_prefix,
            "case 4 the waiter did not outlast short holds that each stayed far below the "
            "live-owner timeout (" + short_hold_child.describe_status() + "; " +
            (marker_diagnostic.empty() ? std::string("no marker") : marker_diagnostic) + ")");
        sintra::test::require_true(exited_as_expected_abort(short_hold_child), k_failure_prefix,
            "case 4 a single hold past the live-owner timeout did not abort the waiter: " +
                short_hold_child.describe_status());
    }

    // Case 5: a waiter stopped for longer than the timeout must not abort on a
    // hold that is released soon after it runs again.
    {
        const std::vector<const char*> stopped_waiter_args = {
            argv[0],
            "--spinlock-stopped-waiter",
            nullptr
        };
        Exact_child stopped_waiter_child(k_child_cleanup_timeout);
        run_child_to_exit(
            stopped_waiter_child, argv[0], stopped_waiter_args.data(), "case 5");
        sintra::test::require_true(stopped_waiter_child.exited_with_code(0), k_failure_prefix,
            "case 5 a waiter stopped past the live-owner timeout did not acquire the lock "
            "released after it resumed: " + stopped_waiter_child.describe_status());
    }

    return 0;
}
