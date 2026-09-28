#include <sintra/sintra.h>

#include "managed_child_test_support.h"
#include "test_utils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr const char* k_child_flag = "--recovery-retirement-child";
constexpr unsigned k_last_occurrence = 12;
constexpr unsigned k_throwing_call = 3;

thread_local uint64_t g_current_worker_id = 0;

struct Worker_events
{
    std::mutex mutex;
    std::condition_variable changed;
    std::array<uint64_t, k_last_occurrence + 1> recovery_ids{};
    std::array<uint64_t, 64> completed_ids{};
    std::array<uint64_t, 64> joined_ids{};
    size_t completed_count = 0;
    size_t joined_count = 0;
};

std::atomic<Worker_events*> g_worker_events{nullptr};

void on_worker_event(const char* stage, uint64_t worker_id) noexcept
{
    if (std::strcmp(stage, "body_started") == 0) {
        g_current_worker_id = worker_id;
    }
    auto* events = g_worker_events.load(std::memory_order_acquire);
    if (!events) {
        return;
    }
    if (std::strcmp(stage, "body_completed") != 0 &&
        std::strcmp(stage, "post_join") != 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(events->mutex);
        if (std::strcmp(stage, "body_completed") == 0) {
            if (events->completed_count < events->completed_ids.size()) {
                events->completed_ids[events->completed_count++] = worker_id;
            }
        }
        else if (events->joined_count < events->joined_ids.size()) {
            events->joined_ids[events->joined_count++] = worker_id;
        }
    }
    events->changed.notify_all();
}

bool contains_id(const std::array<uint64_t, 64>& ids, size_t count, uint64_t id)
{
    return id != 0 && std::find(ids.begin(), ids.begin() + count, id) !=
        ids.begin() + count;
}

fs::path marker(const fs::path& directory, const char* prefix, unsigned occurrence)
{
    return directory / (std::string(prefix) + std::to_string(occurrence));
}

bool check(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return value;
}

#ifdef _WIN32
// A retained std::thread keeps its exited thread object, and so its thread ID,
// alive. The creation time tells a reused ID apart from the original runner.
struct Runner_thread
{
    DWORD    id;
    FILETIME created;
};

FILETIME thread_creation_time(HANDLE thread)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    (void)GetThreadTimes(thread, &created, &exited, &kernel, &user);
    return created;
}

bool runner_thread_retained(const Runner_thread& runner)
{
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, runner.id);
    if (!thread) {
        return false;
    }
    const FILETIME created = thread_creation_time(thread);
    CloseHandle(thread);
    return CompareFileTime(&created, &runner.created) == 0;
}
#endif

int run_child(int argc, char* argv[], const fs::path& directory)
{
    sintra::init(argc, argv);
    sintra::enable_recovery();
    const auto occurrence = sintra::s_recovery_occurrence;
    if (!sintra::test::managed_child::write_child_identity(
            marker(directory, "ready", occurrence)) ||
        !sintra::test::wait_for_file(marker(directory, "go", occurrence), 60s, 5ms))
    {
        return 2;
    }
    if (occurrence == k_last_occurrence) {
        return sintra::detail::finalize() ? 0 : 3;
    }

    sintra::disable_debug_pause_for_current_process();
    sintra::test::prepare_for_intentional_crash("owned recovery worker retirement");
    std::abort();
}

int run_root(int argc, char* argv[], const fs::path& directory)
{
    sintra::init(argc, argv);
    Worker_events events;
    g_worker_events.store(&events, std::memory_order_release);
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        &on_worker_event, std::memory_order_release);
    std::atomic<unsigned> calls{0};
    std::atomic<bool> live_started{false};
    std::atomic<bool> live_cancelled{false};
    std::atomic<bool> coordinator_alive{false};
#ifdef _WIN32
    std::array<Runner_thread, k_last_occurrence + 1> completed_runners{};
#endif

    sintra::Recovery_runner custom_runner =
        [&](const sintra::Crash_info&, const sintra::Recovery_control& control) {
        const auto call = ++calls;
        control.spawn();
        control.spawn();
        if (call == 1) {
            live_started.store(true, std::memory_order_release);
            while (!control.should_cancel()) {
                std::this_thread::sleep_for(2ms);
            }
            coordinator_alive.store(sintra::s_coord != nullptr, std::memory_order_release);
            control.spawn();
            live_cancelled.store(true, std::memory_order_release);
            return;
        }
        if (call <= k_last_occurrence) {
            std::lock_guard<std::mutex> lock(events.mutex);
            events.recovery_ids[call] = g_current_worker_id;
#ifdef _WIN32
            completed_runners[call] =
                {GetCurrentThreadId(), thread_creation_time(GetCurrentThread())};
#endif
            events.changed.notify_all();
        }
        if (call == k_throwing_call) {
            throw std::runtime_error("owned recovery runner fixture");
        }
    };
    sintra::set_recovery_runner(custom_runner);

    sintra::Spawn_options options;
    options.binary_path              = sintra::test::get_binary_path(argc, argv);
    options.args                     = {k_child_flag};
    options.lifetime.enable_lifeline = false;
    auto custody = sintra::spawn_swarm_process(options);
    bool valid = check(static_cast<bool>(custody), "owned child admission");
    for (unsigned occurrence = 0; valid && occurrence <= k_last_occurrence; ++occurrence) {
        valid &= check(sintra::test::wait_for_file(
            marker(directory, "ready", occurrence), 10s, 5ms), "replacement child readiness");
        if (!valid) {
            break;
        }
        if (occurrence > 2) {
            const unsigned custom_call = occurrence - 1;
            std::unique_lock<std::mutex> lock(events.mutex);
            valid &= check(events.changed.wait_for(lock, 10s, [&] {
                const auto id = events.recovery_ids[custom_call];
                return contains_id(events.joined_ids, events.joined_count, id);
            }), "finished recovery runner is joined without later admission");
            const auto id = events.recovery_ids[custom_call];
            valid &= check(contains_id(events.completed_ids, events.completed_count, id),
                "body completion precedes post-join publication");
            lock.unlock();
#ifdef _WIN32
            valid &= check(!runner_thread_retained(completed_runners[custom_call]),
                "post-join publication releases the runner kernel object");
#endif
        }
        // Keep custom runner A live while the next crash uses default recovery.
        // Restore custom routing before that replacement is crashed again.
        if (occurrence == 1) {
            sintra::set_recovery_runner({});
        }
        else if (occurrence == 2) {
            valid &= check(live_started.load() && !live_cancelled.load(),
                "default recovery overlaps the live custom runner");
            sintra::set_recovery_runner(custom_runner);
        }
        valid &= check(sintra::test::managed_child::write_complete_file(
            marker(directory, "go", occurrence), "go"), "release this owned child occurrence");
    }

    valid &= check(calls.load() == k_last_occurrence - 1,
        "custom decisions and one default recovery each occur once");
    valid &= check(live_started.load() && !live_cancelled.load(),
        "admissions preserve the still-live recovery worker");
    const auto final_child = sintra::test::managed_child::wait_for_child_identity(
        marker(directory, "ready", k_last_occurrence), 1s, 5ms);
    valid &= check(final_child && sintra::test::managed_child::wait_for_exact_process_absence(
        *final_child, 10s, 5ms), "last replacement finishes normal shutdown");
    const auto released = custody.terminate_until(std::chrono::steady_clock::now() + 10s);
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete,
        "owned child cleanup completes");
    valid &= check(sintra::detail::finalize(), "runtime shutdown joins recovery workers");
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        nullptr, std::memory_order_release);
    g_worker_events.store(nullptr, std::memory_order_release);
    valid &= check(live_cancelled.load() && coordinator_alive.load(),
        "live worker sees cancellation before coordinator destruction");
    return valid ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::test::Shared_directory shared("SINTRA_RECOVERY_RETIREMENT_DIR", "recovery_retirement");
    if (sintra::test::has_argv_flag(argc, argv, k_child_flag)) {
        return run_child(argc, argv, shared.path());
    }
    return run_root(argc, argv, shared.path());
}
