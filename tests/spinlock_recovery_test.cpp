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
    std::atomic<uint64_t>  m_owner;
    std::atomic<uint64_t>  m_last_progress_ns;
};

// A spinlock owner is a process instance: the PID in the upper 32 bits and the
// process's token in the lower 32. A nonzero owner word holds the lock.
uint64_t owner_instance(uint32_t pid, uint32_t token = 0)
{
    return (static_cast<uint64_t>(pid) << 32) | token;
}

void install_owner(spinlock_layout_t& layout, uint64_t owner)
{
    layout.m_owner.store(owner, std::memory_order_release);
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
}

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

// The live-owner timeout is 2 s. Case 4 streams short holds until the waiter
// has kept waiting well past it, and case 5 stops the waiter for longer than it.
// A hold of k_overlong_hold or more counts as one the waiter may legitimately
// time out on; it is shorter than the timeout, so no such hold escapes it.
constexpr auto k_live_owner_timeout    = std::chrono::seconds(2);
constexpr auto k_timeout_margin        = std::chrono::milliseconds(500);
constexpr auto k_overlong_hold         = std::chrono::milliseconds(1500);
constexpr auto k_short_hold            = std::chrono::milliseconds(1);
constexpr auto k_waiter_stop           = std::chrono::seconds(3);
constexpr auto k_interleaving_limit    = std::chrono::seconds(10);
constexpr auto k_timed_child_deadline  = std::chrono::seconds(20);
constexpr int  k_schedule_attempts     = 3;

using Steady_time = std::chrono::steady_clock::time_point;

std::chrono::steady_clock::rep to_ticks(Steady_time time)
{
    return time.time_since_epoch().count();
}

std::atomic<bool> g_debug_pause_timed_out{false};
std::atomic<bool> g_debug_pause_polled_again{false};
const void* g_same_process_poll_lock = nullptr;
std::atomic<bool> g_same_process_contended_poll{false};

void observe_same_process_poll(const void* lock, Steady_time, bool)
{
    if (lock == g_same_process_poll_lock) {
        g_same_process_contended_poll = true;
    }
}

void observe_debug_pause_poll(const void*, Steady_time, bool timed_out)
{
    if (g_debug_pause_timed_out.load()) {
        g_debug_pause_polled_again = true;
    }
    if (timed_out) {
        g_debug_pause_timed_out = true;
    }
}

Steady_time from_ticks(std::chrono::steady_clock::rep ticks)
{
    return Steady_time(std::chrono::steady_clock::duration(ticks));
}

std::filesystem::path inconclusive_marker_path(const std::filesystem::path& marker_path)
{
    return marker_path.string() + ".inconclusive";
}

// What a child learns from the waiter's contended polls through the spinlock
// test hook, which runs on the waiter thread; only that thread writes
// first_poll. Every decision is judged by the time the waiter read for it, not
// by when the hook runs, since the waiter can be descheduled in between. A
// schedule in which one hold really outlasts the timeout proves nothing, since
// the abort is then correct. The hook records such a schedule in the
// inconclusive marker before the abort, and the parent retries it.
struct Waiter_polls
{
    const void*                                   lock = nullptr;
    std::filesystem::path                         inconclusive_path;
    std::string                                   token;
    Steady_time                                   first_poll{};
    std::atomic<bool>                             polled{false};
    std::atomic<std::chrono::steady_clock::rep>   hold_started{0};
    std::atomic<bool>                             overlong_hold{false};
    std::atomic<bool>                             hold_stream_over{false};
    std::atomic<bool>                             outlasted_timeout{false};
    std::atomic<std::chrono::steady_clock::rep>   resume_boundary{0};
    std::atomic<bool>                             resumed_poll_kept_waiting{false};
};

Waiter_polls g_waiter_polls;

void observe_waiter_polls(
    const void*                                         lock,
    const std::filesystem::path&                        marker_path,
    std::string_view                                    marker_token,
    sintra::detail::test_hooks::Spinlock_poll_callback  callback)
{
    g_waiter_polls.lock              = lock;
    g_waiter_polls.inconclusive_path = inconclusive_marker_path(marker_path);
    g_waiter_polls.token             = std::string(marker_token);
    sintra::detail::test_hooks::s_spinlock_poll.store(callback, std::memory_order_release);
}

[[noreturn]] void exit_inconclusive()
{
    (void)publish_ready_marker(g_waiter_polls.inconclusive_path, g_waiter_polls.token);
    std::_Exit(2);
}

// Waits for a flag that the waiter's polls set, or ends the schedule as
// inconclusive when the waiter does not run within the limit.
void await_waiter_poll(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + k_interleaving_limit;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            exit_inconclusive();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void observe_short_hold_poll(const void* lock, Steady_time poll_time, bool timed_out)
{
    auto& polls = g_waiter_polls;
    if (lock != polls.lock) {
        return;
    }
    if (!polls.polled.load()) {
        polls.first_poll = poll_time;
        polls.polled = true;
    }
    if (!timed_out) {
        if (poll_time - polls.first_poll > k_live_owner_timeout + k_timeout_margin) {
            polls.outlasted_timeout = true;
        }
        return;
    }
    // The hold that timed out is either still current, and started no later
    // than hold_started, or it has ended and left overlong_hold behind before
    // hold_started moved on, which is why hold_started is read first.
    const auto hold_started = from_ticks(polls.hold_started.load());
    const bool overlong =
        poll_time - hold_started >= k_overlong_hold || polls.overlong_hold.load();
    if (!polls.hold_stream_over.load() && overlong) {
        (void)publish_ready_marker(polls.inconclusive_path, polls.token);
    }
}

// Ends one short hold and starts the next by the same thread, as if this
// thread won every race against the waiter. Both holds belong to this process
// instance, so only the progress stamp that unlock() and lock() write changes;
// the owner word stays set, and the waiter cannot acquire. The next hold's
// start is taken before it is published and the previous hold's end after, so
// both bound the holds from outside.
void hand_over_to_next_hold(spinlock_layout_t& layout)
{
    const auto next_started = std::chrono::steady_clock::now();
    layout.m_last_progress_ns.store(sintra::monotonic_now_ns(), std::memory_order_relaxed);
    const auto previous_ended = std::chrono::steady_clock::now();
    if (previous_ended - from_ticks(g_waiter_polls.hold_started.load()) >= k_overlong_hold) {
        g_waiter_polls.overlong_hold = true;
    }
    g_waiter_polls.hold_started = to_ticks(next_started);
}

// A waiter behind a stream of short holds must keep waiting past the timeout.
// The stream starts once the waiter has polled inside lock() and ends once a
// poll has kept waiting well past the timeout, measured from the waiter's first
// poll, which the marker then records. The final hold is kept until the
// waiter's timeout aborts the process.
int run_short_holds_child(
    const std::filesystem::path& marker_path,
    std::string_view             marker_token)
{
    sintra::spinlock short_hold_lock;
    auto& layout = access_layout(short_hold_lock);
    g_waiter_polls.hold_started = to_ticks(std::chrono::steady_clock::now());
    short_hold_lock.lock();
    observe_waiter_polls(&short_hold_lock, marker_path, marker_token, &observe_short_hold_poll);

    std::thread waiter([&] {
        short_hold_lock.lock();
        short_hold_lock.unlock();
    });
    await_waiter_poll(g_waiter_polls.polled);

    const auto stream_limit = std::chrono::steady_clock::now() + k_interleaving_limit;
    while (!g_waiter_polls.outlasted_timeout.load()) {
        if (std::chrono::steady_clock::now() >= stream_limit) {
            exit_inconclusive();
        }
        std::this_thread::sleep_for(k_short_hold);
        hand_over_to_next_hold(layout);
    }
    g_waiter_polls.hold_stream_over = true;
    if (!publish_ready_marker(marker_path, marker_token)) {
        std::_Exit(2);
    }
    waiter.join();
    return 1;
}

void observe_stopped_waiter_poll(const void* lock, Steady_time poll_time, bool timed_out)
{
    auto& polls = g_waiter_polls;
    if (lock != polls.lock) {
        return;
    }
    polls.polled = true;
    const auto resume_boundary = polls.resume_boundary.load();
    const bool after_resume = resume_boundary != 0 && poll_time > from_ticks(resume_boundary);
    if (!timed_out) {
        if (after_resume) {
            polls.resumed_poll_kept_waiting = true;
        }
        return;
    }
    // Before the stop, or after a resumed poll has already kept waiting, a
    // timeout means that the holder itself stayed descheduled past it.
    if (!after_resume || polls.resumed_poll_kept_waiting.load()) {
        (void)publish_ready_marker(polls.inconclusive_path, polls.token);
    }
}

#ifndef _WIN32
void stop_waiter_thread(int)
{
    timespec remaining{
        static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(k_waiter_stop).count()),
        0};
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {}
    g_waiter_polls.resume_boundary.store(to_ticks(std::chrono::steady_clock::now()));
}
#endif

// A suspend stops the holder and the waiter alike. A waiter that was stopped
// for longer than the timeout must not count that time against a hold that is
// released soon after both run again. The waiter is stopped only once it has
// polled inside lock(), and the lock is released only once a poll that the
// waiter read the time for after resuming has decided to keep waiting.
int run_stopped_waiter_child(
    const std::filesystem::path& marker_path,
    std::string_view             marker_token)
{
    sintra::spinlock stopped_lock;
    stopped_lock.lock();
    observe_waiter_polls(&stopped_lock, marker_path, marker_token, &observe_stopped_waiter_poll);

    std::atomic<bool> waiter_acquired{false};
#ifdef _WIN32
    std::atomic<HANDLE> waiter_handle{nullptr};
#endif
    std::thread waiter([&] {
#ifdef _WIN32
        waiter_handle = OpenThread(THREAD_SUSPEND_RESUME, FALSE, GetCurrentThreadId());
#endif
        stopped_lock.lock();
        waiter_acquired = true;
        stopped_lock.unlock();
    });
    await_waiter_poll(g_waiter_polls.polled);

#ifdef _WIN32
    const HANDLE handle = waiter_handle.load();
    if (!handle || SuspendThread(handle) == static_cast<DWORD>(-1)) {
        std::_Exit(2);
    }
    std::this_thread::sleep_for(k_waiter_stop);
    g_waiter_polls.resume_boundary = to_ticks(std::chrono::steady_clock::now());
    if (ResumeThread(handle) == static_cast<DWORD>(-1)) {
        std::_Exit(2);
    }
    CloseHandle(handle);
#else
    struct sigaction stop_action {};
    stop_action.sa_handler = stop_waiter_thread;
    sigemptyset(&stop_action.sa_mask);
    if (sigaction(SIGUSR1, &stop_action, nullptr) != 0 ||
        pthread_kill(waiter.native_handle(), SIGUSR1) != 0)
    {
        std::_Exit(2);
    }
#endif
    await_waiter_poll(g_waiter_polls.resumed_poll_kept_waiting);

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

struct Schedule_outcome
{
    bool          inconclusive   = false;
    Marker_state  marker         = Marker_state::absent;
    std::string   marker_diagnostic;
    std::string   status;
    bool          aborted        = false;
    bool          exited_cleanly = false;
};

// Runs one schedule of a timed child, which reports through its markers
// whether it survived the stream and whether a single hold really outlasted
// the timeout, so that the schedule proved nothing.
Schedule_outcome run_timed_schedule(
    const char*         program,
    const char*         mode,
    const std::string&  label,
    uint32_t            self_pid,
    std::string_view    context)
{
    const Ready_marker marker = make_ready_marker(label, self_pid);
    const std::string marker_arg = marker.path.string();
    const std::vector<const char*> args = {
        program,
        mode,
        marker_arg.c_str(),
        marker.token.c_str(),
        nullptr
    };
    Exact_child child(k_child_cleanup_timeout);
    run_child_to_exit(child, program, args.data(), context);

    Schedule_outcome outcome;
    outcome.marker = probe_ready_marker(marker.path, marker.token, outcome.marker_diagnostic);
    const auto inconclusive_path = inconclusive_marker_path(marker.path);
    std::string inconclusive_diagnostic;
    outcome.inconclusive =
        probe_ready_marker(inconclusive_path, marker.token, inconclusive_diagnostic) ==
        Marker_state::valid;
    outcome.status         = child.describe_status();
    outcome.aborted        = exited_as_expected_abort(child);
    outcome.exited_cleanly = child.exited_with_code(0);

    std::error_code cleanup_error;
    std::filesystem::remove(inconclusive_path, cleanup_error);
    remove_ready_marker(marker);
    return outcome;
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

    if (argc >= 4 && std::string_view(argv[1]) == "--spinlock-stopped-waiter") {
        if (!prepare_abort_expecting_child()) {
            return 2;
        }
        return run_stopped_waiter_child(std::filesystem::path(argv[2]), argv[3]);
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
        install_owner(access_layout(stall_lock), owner_instance(owner_pid));
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

    install_owner(layout, owner_instance(dead_pid));

    lock.lock();
    lock.unlock();

    // Contenders that observe the same dead owner must not both acquire.
    install_owner(layout, owner_instance(dead_pid));

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

    // Case 1b: an earlier process with this PID died holding the lock. Its
    // token differs from this process's, so the lock is taken over at the
    // first liveness poll instead of being diagnosed as a 2 s self stall.
    const uint64_t self_instance = sintra::detail::current_process_instance();
    sintra::test::require_true(
        sintra::detail::process_instance_pid(self_instance) == self_pid, k_failure_prefix,
        "the process instance must carry this process's PID");
    install_owner(layout, owner_instance(self_pid, static_cast<uint32_t>(self_instance) + 1));
    lock.lock();
    const uint64_t takeover_owner = layout.m_owner.load();
    lock.unlock();
    sintra::test::require_true(
        takeover_owner == self_instance, k_failure_prefix,
        "a lock left by an earlier process with this PID must be taken over");

    // Case 1c: another thread of this process holds the lock. It keeps
    // excluding and is never taken over.
    {
        std::atomic<bool> holder_locked{false};
        std::atomic<bool> owner_observed{false};
        std::atomic<bool> holder_release_started{false};
        std::atomic<bool> waiter_acquired_before_release{false};
        uint64_t waiter_owner = 0;
        std::thread holder([&] {
            lock.lock();
            holder_locked = true;
            while (!owner_observed || !g_same_process_contended_poll) {
                std::this_thread::yield();
            }
            holder_release_started = true;
            lock.unlock();
        });
        while (!holder_locked) {
            std::this_thread::yield();
        }
        const uint64_t held_owner = layout.m_owner.load();
        owner_observed = true;
        g_same_process_poll_lock = &lock;
        g_same_process_contended_poll = false;
        sintra::detail::test_hooks::s_spinlock_poll.store(
            &observe_same_process_poll, std::memory_order_release);
        sintra::detail::set_debug_pause_active(true);
        std::thread waiter([&] {
            lock.lock();
            waiter_acquired_before_release = !holder_release_started.load();
            waiter_owner = layout.m_owner.load();
            lock.unlock();
        });
        holder.join();
        waiter.join();
        sintra::detail::set_debug_pause_active(false);
        sintra::detail::test_hooks::s_spinlock_poll.store(nullptr, std::memory_order_release);
        g_same_process_poll_lock = nullptr;
        sintra::test::require_true(held_owner == self_instance &&
            g_same_process_contended_poll && !waiter_acquired_before_release &&
            waiter_owner == self_instance,
            k_failure_prefix,
            "a lock held by another thread of this process must exclude and not be taken over");
    }

    // Case 2: debug pause must keep waiting behind a live owner, even after
    // the timeout, then recover once that owner is proven dead.
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

    const uint64_t live_owner = owner_instance(static_cast<uint32_t>(child_pid));
    install_owner(layout, live_owner);

    g_debug_pause_timed_out = false;
    g_debug_pause_polled_again = false;
    sintra::detail::test_hooks::s_spinlock_poll.store(
        &observe_debug_pause_poll, std::memory_order_release);
    sintra::detail::set_debug_pause_active(true);
    std::atomic<bool> waiter_acquired{false};
    std::thread waiter([&] {
        lock.lock();
        waiter_acquired = true;
        lock.unlock();
    });

    const auto debug_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!g_debug_pause_polled_again && !waiter_acquired &&
        std::chrono::steady_clock::now() < debug_wait_deadline)
    {
        std::this_thread::sleep_for(k_child_poll_interval);
    }
    const bool excluded = g_debug_pause_timed_out && g_debug_pause_polled_again &&
        !waiter_acquired && layout.m_owner.load() == live_owner;
    const auto live_child_state = sleep_child.poll();

    std::string sleep_cleanup_diagnostic;
    if (!sleep_child.terminate_and_settle(sleep_cleanup_diagnostic)) {
        fail_after_settling_child(
            sleep_child,
            "case 2 exact-child cleanup failed: " + sleep_cleanup_diagnostic);
    }
    waiter.join();
    sintra::detail::set_debug_pause_active(false);
    sintra::detail::test_hooks::s_spinlock_poll.store(nullptr, std::memory_order_release);
    sintra::test::require_true(excluded && live_child_state == Exact_child_state::running &&
        waiter_acquired, k_failure_prefix,
        "case 2 debug pause must exclude a live owner through timeout and recover after death");

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
    for (int attempt = 1;; ++attempt) {
        const auto outcome = run_timed_schedule(
            argv[0], "--spinlock-short-holds", "short_holds", self_pid, "case 4");
        if (outcome.inconclusive && outcome.marker != Marker_state::valid) {
            sintra::test::require_true(attempt < k_schedule_attempts, k_failure_prefix,
                "case 4 every schedule had a single hold that really outlasted the "
                "live-owner timeout");
            std::fprintf(stderr,
                "spinlock_recovery_test: case 4 schedule %d had a single hold that really "
                "outlasted the live-owner timeout; retrying\n",
                attempt);
            continue;
        }
        sintra::test::require_true(outcome.marker == Marker_state::valid, k_failure_prefix,
            "case 4 the waiter did not keep waiting past the live-owner timeout behind "
            "short holds (" + outcome.status + "; " +
            (outcome.marker_diagnostic.empty() ? std::string("no marker") : outcome.marker_diagnostic) +
            ")");
        sintra::test::require_true(outcome.aborted, k_failure_prefix,
            "case 4 a single hold past the live-owner timeout did not abort the waiter: " +
                outcome.status);
        break;
    }

    // Case 5: a waiter stopped for longer than the timeout must not abort on a
    // hold that is released soon after it runs again.
    for (int attempt = 1;; ++attempt) {
        const auto outcome = run_timed_schedule(
            argv[0], "--spinlock-stopped-waiter", "stopped_waiter", self_pid, "case 5");
        if (outcome.inconclusive && !outcome.exited_cleanly) {
            sintra::test::require_true(attempt < k_schedule_attempts, k_failure_prefix,
                "case 5 every schedule had a single hold that really outlasted the "
                "live-owner timeout");
            std::fprintf(stderr,
                "spinlock_recovery_test: case 5 schedule %d had a single hold that really "
                "outlasted the live-owner timeout; retrying\n",
                attempt);
            continue;
        }
        sintra::test::require_true(outcome.exited_cleanly, k_failure_prefix,
            "case 5 a waiter stopped past the live-owner timeout did not acquire the lock "
            "released after it resumed: " + outcome.status);
        break;
    }

    return 0;
}
