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
constexpr unsigned k_exit_admission_call = 4;

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

// Admitting lifecycle work joins every completed worker before it returns.
void reap_completed_workers()
{
    sintra::s_mproc->start_owned_lifecycle_worker([] {});
}

std::atomic<bool> g_exit_admission_done{false};

// Thread-exit cleanup of a recovery runner that admits lifecycle work, as a
// destructor that releases a custody does. It starts after the main thread's
// reaping admission, which must not wait for this thread meanwhile.
struct Exit_admission
{
    bool armed = false;

    ~Exit_admission()
    {
        if (!armed) {
            return;
        }
        std::this_thread::sleep_for(100ms);
        reap_completed_workers();
        g_exit_admission_done.store(true, std::memory_order_release);
    }
};

int run_root(int argc, char* argv[], const fs::path& directory)
{
    sintra::init(argc, argv);
    std::mutex mutex;
    std::condition_variable changed;
    std::array<bool, k_last_occurrence + 1> exited{};
    std::atomic<unsigned> calls{0};
    std::atomic<bool> live_started{false};
    std::atomic<bool> live_cancelled{false};
    std::atomic<bool> coordinator_alive{false};
#ifdef _WIN32
    std::vector<Runner_thread> completed_runners;
#endif

    sintra::set_recovery_runner([&](const sintra::Crash_info&, const sintra::Recovery_control& control) {
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
        if (call == k_exit_admission_call) {
            // Constructed before the exit notification below is registered.
            thread_local Exit_admission exit_admission;
            exit_admission.armed = true;
        }
        if (call <= k_last_occurrence) {
            std::unique_lock<std::mutex> lock(mutex);
#ifdef _WIN32
            completed_runners.push_back(
                {GetCurrentThreadId(), thread_creation_time(GetCurrentThread())});
#endif
            exited[call] = true;
            std::notify_all_at_thread_exit(changed, std::move(lock));
        }
        if (call == k_throwing_call) {
            throw std::runtime_error("owned recovery runner fixture");
        }
    });

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
        if (occurrence > 1) {
            std::unique_lock<std::mutex> lock(mutex);
            valid &= check(changed.wait_for(lock, 10s, [&] { return exited[occurrence]; }),
                "completed recovery runner reached native thread exit");
            lock.unlock();
            // Every runner so far except the deliberately live first one has
            // exited. The next lifecycle admission must join each of them.
            reap_completed_workers();
#ifdef _WIN32
            // A runner is reaped once its exit cleanup has finished, and the
            // kernel may drop its last reference to a joined thread shortly
            // after the join. Keep admitting until then; a retained
            // std::thread is never released.
            const auto count_retained = [&] {
                std::lock_guard<std::mutex> runners_lock(mutex);
                unsigned count = 0;
                for (const auto& runner : completed_runners) {
                    count += runner_thread_retained(runner) ? 1 : 0;
                }
                return count;
            };
            const auto release_deadline = std::chrono::steady_clock::now() + 2s;
            unsigned retained = count_retained();
            while (retained != 0 && std::chrono::steady_clock::now() < release_deadline) {
                std::this_thread::sleep_for(5ms);
                reap_completed_workers();
                retained = count_retained();
            }
            if (retained != 0) {
                std::fprintf(stderr, "occurrence %u: %u of %zu completed runner threads retained\n",
                    occurrence, retained, completed_runners.size());
            }
            valid &= check(retained == 0,
                "a later admission releases every completed recovery runner thread");
#endif
        }
        valid &= check(sintra::test::managed_child::write_complete_file(
            marker(directory, "go", occurrence), "go"), "release this owned child occurrence");
    }

    valid &= check(calls.load() == k_last_occurrence, "one runner per recovery occurrence");
    valid &= check(live_started.load() && !live_cancelled.load(),
        "admissions preserve the still-live recovery worker");
    valid &= check(g_exit_admission_done.load(std::memory_order_acquire),
        "a runner's thread-exit cleanup admits lifecycle work while another admission reaps");
    const auto final_child = sintra::test::managed_child::wait_for_child_identity(
        marker(directory, "ready", k_last_occurrence), 1s, 5ms);
    valid &= check(final_child && sintra::test::managed_child::wait_for_exact_process_absence(
        *final_child, 10s, 5ms), "last replacement finishes normal shutdown");
    const auto released = custody.terminate_until(std::chrono::steady_clock::now() + 10s);
    valid &= check(released.release_state == sintra::Managed_child_release_state::complete,
        "owned child cleanup completes");
    valid &= check(sintra::detail::finalize(), "runtime shutdown joins recovery workers");
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
