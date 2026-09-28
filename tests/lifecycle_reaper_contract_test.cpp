#include <sintra/sintra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

#include "test_environment.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

struct Events
{
    std::mutex mutex;
    std::condition_variable changed;
    std::array<uint64_t, 32> completed{};
    std::array<uint64_t, 32> joined{};
    size_t completed_count = 0;
    size_t joined_count = 0;
    bool hold_publication = false;
    bool publication_entered = false;
    bool publication_released = false;
    bool hold_construction = false;
    bool construction_entered = false;
    bool construction_released = false;
    bool drain_requested = false;
    uint64_t pending_id = 0;
};

std::atomic<Events*> g_events{nullptr};
std::atomic<const char*> g_failure_stage{nullptr};
thread_local uint64_t g_worker_id = 0;

bool inject_failure(const char* stage, sintra::instance_id_type, uint32_t) noexcept
{
    const auto* selected = g_failure_stage.load(std::memory_order_acquire);
    return selected && std::strcmp(stage, selected) == 0;
}

void on_event(const char* stage, uint64_t id) noexcept
{
    if (std::strcmp(stage, "body_started") == 0) {
        g_worker_id = id;
    }
    auto* events = g_events.load(std::memory_order_acquire);
    if (!events) {
        return;
    }
    std::unique_lock<std::mutex> lock(events->mutex);
    if (std::strcmp(stage, "body_completed") == 0) {
        if (events->completed_count < events->completed.size()) {
            events->completed[events->completed_count++] = id;
        }
    }
    else if (std::strcmp(stage, "post_join") == 0) {
        if (events->joined_count < events->joined.size()) {
            events->joined[events->joined_count++] = id;
        }
    }
    else if (std::strcmp(stage, "before_publish") == 0 && events->hold_publication) {
        events->pending_id = id;
        events->publication_entered = true;
        events->changed.notify_all();
        events->changed.wait(lock, [&] { return events->publication_released; });
    }
    else if (std::strcmp(stage, "before_construct") == 0 && events->hold_construction) {
        events->construction_entered = true;
        events->changed.notify_all();
        events->changed.wait(lock, [&] { return events->construction_released; });
    }
    else if (std::strcmp(stage, "drain_requested") == 0) {
        events->drain_requested = true;
    }
    lock.unlock();
    events->changed.notify_all();
}

bool check(bool result, const char* message)
{
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return result;
}

bool has_id(const std::array<uint64_t, 32>& ids, size_t count, uint64_t id)
{
    return id != 0 && std::find(ids.begin(), ids.begin() + count, id) !=
        ids.begin() + count;
}

bool wait_for_join(Events& events, uint64_t id)
{
    std::unique_lock<std::mutex> lock(events.mutex);
    return events.changed.wait_for(lock, 10s, [&] {
        return has_id(events.joined, events.joined_count, id);
    });
}

bool init_failure_rolls_back(int argc, char* argv[], const char* stage)
{
    g_failure_stage.store(stage, std::memory_order_release);
    bool failed = false;
    try {
        sintra::init(argc, argv);
    }
    catch (const std::runtime_error&) {
        failed = true;
    }
    return check(failed && !sintra::s_mproc && !sintra::s_init_once,
        "failed eager startup restores uninitialized runtime");
}

bool check_body_teardown_rejection(Events& events)
{
    std::atomic<unsigned> rejected{0};
    std::atomic<bool> unchanged{true};
    std::atomic<uint64_t> worker_id{0};
    std::atomic<uint64_t> concurrent_id{0};
    std::mutex body_gate_mutex;
    std::condition_variable body_gate_changed;
    bool release_body = false;
    auto* process = sintra::s_mproc;
    process->start_owned_lifecycle_worker([&] {
        worker_id.store(g_worker_id, std::memory_order_release);
        {
            std::unique_lock<std::mutex> lock(body_gate_mutex);
            body_gate_changed.wait(lock, [&] { return release_body; });
        }
        const auto initial_state = sintra::detail::s_shutdown_state.load();
        const auto initial_closed = sintra::detail::s_teardown_admission_closed.load();
        const std::array<std::function<void()>, 6> entries{{
            [] { (void)sintra::detail::finalize(); },
            [] { (void)sintra::shutdown(); },
            [] { (void)sintra::shutdown(sintra::shutdown_options{}); },
            [] { (void)sintra::leave(); },
            [] { (void)sintra::detail::finalize_impl(); },
            [process] { process->join_owned_lifecycle_workers(); }
        }};
        for (const auto& entry : entries) {
            try {
                entry();
            }
            catch (const std::logic_error&) {
                ++rejected;
            }
            unchanged.store(unchanged.load() &&
                sintra::detail::s_shutdown_state.load() == initial_state &&
                sintra::detail::s_teardown_admission_closed.load() == initial_closed);
        }
    });
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        events.hold_construction = true;
    }
    std::thread admission([&] {
        process->start_owned_lifecycle_worker([&] {
            concurrent_id.store(g_worker_id, std::memory_order_release);
        });
    });
    bool construction_ready = false;
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        construction_ready = events.changed.wait_for(lock, 10s, [&] {
            return events.construction_entered;
        });
    }
    {
        std::lock_guard<std::mutex> lock(body_gate_mutex);
        release_body = true;
    }
    body_gate_changed.notify_all();
    const auto id = [&] {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.changed.wait_for(lock, 10s, [&] {
            return worker_id.load(std::memory_order_acquire) != 0;
        });
        return worker_id.load(std::memory_order_acquire);
    }();
    bool valid = check(wait_for_join(events, id), "body-guard worker joined");
    valid &= check(rejected.load() == 6 && unchanged.load(),
        "every body teardown entry rejects without changing state");
    valid &= check(construction_ready,
        "body teardown runs during a separate accepted admission");
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        events.construction_released = true;
        events.hold_construction = false;
    }
    events.changed.notify_all();
    admission.join();
    const auto other = [&] {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.changed.wait_for(lock, 10s, [&] {
            return concurrent_id.load(std::memory_order_acquire) != 0;
        });
        return concurrent_id.load(std::memory_order_acquire);
    }();
    valid &= check(wait_for_join(events, other),
        "concurrent admission finishes after body teardown rejection");

    std::atomic<bool> destructor_rejected{false};
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool release = false;
    auto probe = std::shared_ptr<int>(new int(1), [&](int* value) {
        try {
            (void)sintra::detail::finalize_impl();
        }
        catch (const std::logic_error&) {
            destructor_rejected.store(true);
        }
        delete value;
    });
    std::atomic<uint64_t> capture_id{0};
    process->start_owned_lifecycle_worker([&, probe] {
        capture_id.store(g_worker_id, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_changed.wait(lock, [&] { return release; });
    });
    probe.reset();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate_changed.notify_all();
    const auto captured = [&] {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.changed.wait_for(lock, 10s, [&] {
            return capture_id.load(std::memory_order_acquire) != 0;
        });
        return capture_id.load(std::memory_order_acquire);
    }();
    valid &= check(wait_for_join(events, captured) && destructor_rejected.load(),
        "body-owned capture destruction retains teardown guard");
    return valid;
}

bool check_retirement_and_drain(Events& events)
{
    auto* process = sintra::s_mproc;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool a_started = false;
    bool release_a = false;
    std::atomic<uint64_t> a_id{0};
    std::atomic<uint64_t> b_id{0};
    process->start_owned_lifecycle_worker([&] {
        a_id.store(g_worker_id, std::memory_order_release);
        std::unique_lock<std::mutex> lock(gate_mutex);
        a_started = true;
        gate_changed.notify_all();
        gate_changed.wait(lock, [&] { return release_a; });
    });
    bool valid = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        valid = check(gate_changed.wait_for(lock, 10s, [&] { return a_started; }),
            "A enters its body before B starts");
    }
    process->start_owned_lifecycle_worker([&] {
        b_id.store(g_worker_id, std::memory_order_release);
    });
    const auto b = [&] {
        std::unique_lock<std::mutex> lock(events.mutex);
        events.changed.wait_for(lock, 10s, [&] {
            return b_id.load(std::memory_order_acquire) != 0;
        });
        return b_id.load(std::memory_order_acquire);
    }();
    valid &= check(wait_for_join(events, b),
        "B joined promptly without another admission while A remains live");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        valid &= check(!release_a, "A is still in its body when B is joined");
        release_a = true;
    }
    gate_changed.notify_all();
    valid &= check(wait_for_join(events, a_id.load()), "A joined after release");

    std::atomic<bool> pending_ran{false};
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        events.hold_publication = true;
    }
    std::thread admission([&] {
        process->start_owned_lifecycle_worker([&] {
            pending_ran.store(true, std::memory_order_release);
        });
    });
    uint64_t pending_id = 0;
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        valid &= check(events.changed.wait_for(lock, 10s, [&] {
            return events.publication_entered &&
                has_id(events.completed, events.completed_count, events.pending_id);
        }), "worker body completes before publication");
        pending_id = events.pending_id;
    }
    std::thread drainer([&] { process->join_owned_lifecycle_workers(); });
    {
        std::unique_lock<std::mutex> lock(events.mutex);
        valid &= check(events.changed.wait_for(lock, 10s, [&] {
            return events.drain_requested;
        }), "drain closes admission while publication is pending");
    }
    bool closed = false;
    try {
        process->start_owned_lifecycle_worker([] {});
    }
    catch (const sintra::detail::Lifecycle_worker_admission_closed&) {
        closed = true;
    }
    valid &= check(closed, "closed registry rejects a late reservation distinctly");
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        events.publication_released = true;
    }
    events.changed.notify_all();
    admission.join();
    drainer.join();
    process->join_owned_lifecycle_workers();
    valid &= check(pending_ran.load() && wait_for_join(events, pending_id),
        "drain retains and joins accepted pending worker and is idempotent");
    return valid;
}

constexpr const char* k_worker_exit_flag = "--owned-worker-exit";

int run_worker_exit_child(int argc, char* argv[])
{
    sintra::test::prepare_for_intentional_crash("owned worker std::exit");
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    const auto endpoint = reinterpret_cast<HANDLE>(
        static_cast<uintptr_t>(std::strtoull(argv[2], nullptr, 10)));
#else
    const int endpoint = std::atoi(argv[2]);
#endif
    sintra::init(argc, argv);
    sintra::s_mproc->start_owned_lifecycle_worker([endpoint] {
        const char marker = 'X';
#ifdef _WIN32
        DWORD written = 0;
        (void)WriteFile(endpoint, &marker, 1, &written, nullptr);
#else
        (void)write(endpoint, &marker, 1);
#endif
        std::exit(0);
    });
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [] { return false; });
    return 4;
}

bool check_worker_exit_shared_fate(const char* executable)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!check(CreatePipe(&read_end, &write_end, &security, 0) != 0,
            "worker-exit marker pipe created"))
    {
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::string command = std::string("\"") + executable + "\" " +
        k_worker_exit_flag + " " +
        std::to_string(reinterpret_cast<uintptr_t>(write_end));
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessA(
        executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != 0;
    CloseHandle(write_end);
    if (!check(started, "worker-exit subprocess started")) {
        CloseHandle(read_end);
        return false;
    }
    const auto wait_result = WaitForSingleObject(process.hProcess, 5000);
    const bool timed_out = wait_result == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    char marker = 0;
    DWORD read = 0;
    const bool marked = ReadFile(read_end, &marker, 1, &read, nullptr) != 0 &&
        read == 1 && marker == 'X';
    CloseHandle(read_end);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return check(marked && (timed_out || exit_code != 0),
        "worker std::exit reached body and remained shared fate");
#else
    int endpoints[2]{};
    if (!check(pipe(endpoints) == 0, "worker-exit marker pipe created")) {
        return false;
    }
    const auto descriptor = std::to_string(endpoints[1]);
    const pid_t child = fork();
    if (child == 0) {
        close(endpoints[0]);
        execl(executable, executable, k_worker_exit_flag, descriptor.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    close(endpoints[1]);
    if (!check(child > 0, "worker-exit subprocess started")) {
        close(endpoints[0]);
        return false;
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false;
    bool timed_out = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 5s, [&] { return finished; })) {
            timed_out = true;
            kill(child, SIGKILL);
        }
    });
    int status = 0;
    (void)waitpid(child, &status, 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    changed.notify_all();
    watchdog.join();
    char marker = 0;
    const bool marked = read(endpoints[0], &marker, 1) == 1 && marker == 'X';
    close(endpoints[0]);
    return check(marked && (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0),
        "worker std::exit reached body and remained shared fate");
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 2 && std::strcmp(argv[1], k_worker_exit_flag) == 0) {
        return run_worker_exit_child(argc, argv);
    }
    sintra::detail::test_hooks::s_managed_child_failure.store(
        &inject_failure, std::memory_order_release);
    bool valid = init_failure_rolls_back(
        argc, argv, "owned_lifecycle_reaper_construct");
    valid &= init_failure_rolls_back(
        argc, argv, "owned_lifecycle_after_reaper_publication");
    g_failure_stage.store(nullptr, std::memory_order_release);
    sintra::init(argc, argv);
    Events events;
    g_events.store(&events, std::memory_order_release);
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        &on_event, std::memory_order_release);

    g_failure_stage.store("owned_lifecycle_worker_construct", std::memory_order_release);
    bool construction_failed = false;
    try {
        sintra::s_mproc->start_owned_lifecycle_worker(
            [] {}, "owned_lifecycle_worker_construct");
    }
    catch (const std::runtime_error&) {
        construction_failed = true;
    }
    g_failure_stage.store(nullptr, std::memory_order_release);
    valid &= check(construction_failed, "worker construction failure rolls back reservation");
    g_failure_stage.store("owned_lifecycle_worker_allocation", std::memory_order_release);
    bool allocation_failed = false;
    try {
        sintra::s_mproc->start_owned_lifecycle_worker([] {});
    }
    catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    g_failure_stage.store(nullptr, std::memory_order_release);
    valid &= check(allocation_failed, "registry allocation failure admits no worker");
    valid &= check_body_teardown_rejection(events);
    valid &= check_retirement_and_drain(events);
    valid &= check(sintra::detail::finalize(), "external finalization succeeds after drain");
    valid &= check_worker_exit_shared_fate(argv[0]);
    sintra::detail::test_hooks::s_owned_lifecycle_worker_event.store(
        nullptr, std::memory_order_release);
    g_events.store(nullptr, std::memory_order_release);
    sintra::detail::test_hooks::s_managed_child_failure.store(
        nullptr, std::memory_order_release);
    return valid ? 0 : 1;
}
