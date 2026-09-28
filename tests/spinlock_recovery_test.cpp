#include <sintra/detail/ipc/spinlock.h>
#include <sintra/detail/debug_pause.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using sintra::detail::test_hooks::spinlock_event;
using sintra::detail::spinlock_cpu_sample;
using sintra::test::Exact_child;
using sintra::test::Exact_child_state;

constexpr std::string_view k_failure_prefix = "spinlock_recovery_test: ";
constexpr uint64_t k_fake_step_ns = 30'000'000;
constexpr uint64_t k_ns_per_second = 1'000'000'000;
constexpr auto k_watchdog = std::chrono::seconds(30);

enum class Schedule {
    short_holds, late_waiter, clock_failure, clock_regression,
    clock_conversion_overflow, below_threshold, exact_boundary,
    single_jump, multiple_samples, debug_pause, handoff_at_end, owner_zero,
    final_recheck, losing_cas, stale_winner, losing_takeover,
    acquisition_gap, release_gap, still_cpu, takeover_aba,
    generation_wrap_boundary
};

struct Scenario {
    sintra::spinlock* lock = nullptr;
    Schedule mode = Schedule::short_holds;
    std::atomic<unsigned> polls{0};
    std::atomic<unsigned> samples{0};
    std::atomic<uint64_t> seen_events{0};
    std::atomic<bool> entered_gap{false};
    std::atomic<bool> leave_gap{false};
    std::atomic<bool> did_action{false};
    std::atomic<bool> did_aba{false};
    std::atomic<bool> wrapped_generation{false};
    std::atomic<unsigned> witness_starts{0};
    std::atomic<unsigned> short_handoffs{0};
    unsigned release_poll = 130;
};

Scenario* g_scenario = nullptr;
thread_local bool g_in_hook = false;
std::atomic<uint64_t> g_observed_hooks{0};

uint64_t owner_instance(uint32_t pid, uint32_t token = 0)
{
    return (uint64_t(pid) << 32) | token;
}

uint32_t find_dead_pid(uint32_t self_pid)
{
    for (uint32_t pid = 500000; pid < 510000; ++pid) {
        if (pid != self_pid && !sintra::is_process_alive(pid)) {
            return pid;
        }
    }
    return 0;
}

void event_hook(const void* address, spinlock_event event)
{
    auto* scenario = g_scenario;
    if (!scenario || address != scenario->lock || g_in_hook) {
        return;
    }
    g_in_hook = true;
    const auto ordinal = static_cast<unsigned>(event);
    const uint64_t bit = uint64_t(1) << ordinal;
    g_observed_hooks.fetch_or(bit);
    const auto prior_events = scenario->seen_events.fetch_or(bit);
    if ((prior_events & bit) == 0) {
        // Park once at each protocol boundary. The fake thread clock only
        // advances when its provider callback says that waiter CPU ran.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (event == spinlock_event::before_start_generation) {
        ++scenario->witness_starts;
    }
    if (event == spinlock_event::before_liveness &&
        ((scenario->mode == Schedule::exact_boundary && scenario->samples.load() < 3) ||
         (scenario->mode == Schedule::multiple_samples && scenario->samples.load() < 4)))
    {
        std::exit(2); // A below-threshold sample must not enter the abort decision.
    }

    if (event == spinlock_event::poll) {
        const unsigned poll = ++scenario->polls;
        if (scenario->mode == Schedule::short_holds && poll % 20 == 0 &&
            poll < scenario->release_poll)
        {
            scenario->lock->unlock();
            scenario->lock->lock();
            ++scenario->short_handoffs;
        }
        if (scenario->release_poll != 0 && poll == scenario->release_poll &&
            scenario->mode != Schedule::acquisition_gap &&
            scenario->mode != Schedule::release_gap)
        {
            scenario->lock->unlock();
        }
    }
    if (event == spinlock_event::after_end_owner &&
        scenario->mode == Schedule::handoff_at_end && !scenario->did_action.exchange(true))
    {
        // End O has already read the old owner. The zero store and the
        // acquire-only winning CAS happen before the end G load.
        scenario->lock->unlock();
        scenario->lock->lock();
    }
    if (event == spinlock_event::after_end_owner &&
        scenario->mode == Schedule::owner_zero && !scenario->did_action.exchange(true))
    {
        scenario->lock->unlock();
    }
    if (event == spinlock_event::after_end_cpu &&
        scenario->mode == Schedule::takeover_aba && !scenario->did_aba.exchange(true))
    {
        const auto a = scenario->lock->test_owner();
        const auto g = scenario->lock->test_generation();
        // Two death-qualified winners have pre-CAS bumped G, but neither
        // has published its post-CAS odd mark. O returns to its old bits.
        scenario->lock->test_install_owner(owner_instance(500001, 1), g + 2);
        scenario->lock->test_install_owner(a, g + 4);
    }
    if (event == spinlock_event::after_end_cpu &&
        scenario->mode == Schedule::generation_wrap_boundary &&
        !scenario->did_action.exchange(true))
    {
        scenario->lock->unlock();
        scenario->lock->lock();
        scenario->lock->unlock();
        scenario->lock->lock();
        scenario->wrapped_generation = scenario->lock->test_generation() == 1;
    }
    if (event == spinlock_event::before_final_generation &&
        scenario->mode == Schedule::final_recheck && !scenario->did_action.exchange(true))
    {
        scenario->lock->unlock();
        scenario->lock->lock();
        scenario->release_poll = scenario->polls.load() + 2;
    }
    if (event == spinlock_event::before_acquire_cas &&
        scenario->mode == Schedule::stale_winner && !scenario->did_action.exchange(true))
    {
        // This contender wins only after a whole intervening marked hold.
        scenario->lock->lock();
        scenario->lock->unlock();
    }
    if (event == spinlock_event::before_acquire_cas &&
        scenario->mode == Schedule::losing_cas && !scenario->did_action.exchange(true))
    {
        scenario->lock->lock();
    }
    if (event == spinlock_event::before_takeover_cas &&
        scenario->mode == Schedule::losing_takeover && !scenario->did_action.exchange(true))
    {
        // Another death-qualified taker wins after this attempt's +2 bump.
        scenario->lock->lock();
        scenario->release_poll = scenario->polls.load() + 3;
    }
    if ((event == spinlock_event::after_acquire_cas &&
            scenario->mode == Schedule::acquisition_gap) ||
        (event == spinlock_event::after_even_mark &&
            scenario->mode == Schedule::release_gap))
    {
        if (!scenario->did_action.exchange(true)) {
            scenario->entered_gap = true;
            while (!scenario->leave_gap.load()) {
                std::this_thread::yield();
            }
        }
    }
    g_in_hook = false;
}

bool fake_cpu(spinlock_cpu_sample& sample)
{
    auto* scenario = g_scenario;
    const unsigned n = ++scenario->samples;
    if ((scenario->mode == Schedule::clock_failure ||
         scenario->mode == Schedule::clock_conversion_overflow) && n == 3)
    {
        // A native conversion overflow reports an invalid sample like an API failure.
        return false;
    }
    if (scenario->mode == Schedule::clock_failure ||
        scenario->mode == Schedule::clock_regression ||
        scenario->mode == Schedule::clock_conversion_overflow)
    {
        sample = {n == 1 ? 0ull : n == 2 ? 1'500'000'000ull :
            n == 3 ? 1'000'000'000ull : n == 4 ? 1'600'000'000ull : 2'100'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::still_cpu) {
        sample = {0};
        return true;
    }
    if (scenario->mode == Schedule::below_threshold) {
        sample = {n == 1 ? 0ull : 1'999'999'999ull};
        return true;
    }
    if (scenario->mode == Schedule::exact_boundary) {
        sample = {n == 1 ? 0ull : n == 2 ? 1'999'999'999ull : 2'000'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::single_jump) {
        sample = {n == 1 ? 0ull : 3'000'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::multiple_samples) {
        sample = {n == 1 ? 0ull : n == 2 ? 800'000'000ull :
            n == 3 ? 1'500'000'000ull : 2'000'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::handoff_at_end ||
        scenario->mode == Schedule::takeover_aba)
    {
        sample = {n == 1 ? 0ull : n == 2 ? 1'500'000'000ull :
            n == 3 ? 2'500'000'000ull : 3'000'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::owner_zero ||
        scenario->mode == Schedule::final_recheck ||
        scenario->mode == Schedule::generation_wrap_boundary)
    {
        sample = {n == 1 ? 0ull : 2'100'000'000ull};
        return true;
    }
    if (scenario->mode == Schedule::short_holds ||
        scenario->mode == Schedule::debug_pause ||
        scenario->mode == Schedule::acquisition_gap ||
        scenario->mode == Schedule::release_gap)
    {
        sample = {n * k_fake_step_ns};
        return true;
    }
    if (scenario->mode == Schedule::late_waiter) {
        sample = {5 * k_ns_per_second + n * 10'000'000};
        return true;
    }
    sample = {n * 10'000'000};
    return true;
}

struct Hook_scope {
    explicit Hook_scope(Scenario& scenario)
    {
        g_scenario = &scenario;
        sintra::detail::test_hooks::s_spinlock_cpu.store(&fake_cpu);
        sintra::detail::test_hooks::s_spinlock_event.store(&event_hook);
    }
    ~Hook_scope()
    {
        sintra::detail::test_hooks::s_spinlock_event.store(nullptr);
        sintra::detail::test_hooks::s_spinlock_cpu.store(nullptr);
        g_scenario = nullptr;
    }
};

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "spinlock_recovery_test: %s\n", message);
    }
    return condition;
}

bool run_surviving_schedule(Schedule mode, unsigned release_poll)
{
    sintra::spinlock lock;
    lock.lock();
    if (mode == Schedule::generation_wrap_boundary) {
        lock.test_install_owner(sintra::detail::current_process_instance(), UINT64_MAX - 2);
    }
    Scenario scenario;
    scenario.lock = &lock;
    scenario.mode = mode;
    scenario.release_poll = release_poll;
    Hook_scope hooks(scenario);
    if (mode == Schedule::debug_pause) {
        sintra::detail::set_debug_pause_active(true);
    }
    lock.lock();
    const bool reached = scenario.polls.load() >=
        (mode == Schedule::owner_zero ? 1u : scenario.release_poll);
    const bool reset_clock =
        mode == Schedule::clock_failure ||
        mode == Schedule::clock_regression ||
        mode == Schedule::clock_conversion_overflow;
    const bool exercised = (!reset_clock || scenario.witness_starts.load() >= 2) &&
        (mode != Schedule::short_holds || scenario.short_handoffs.load() >= 5) &&
        (mode != Schedule::handoff_at_end || scenario.did_action.load()) &&
        (mode != Schedule::owner_zero || scenario.did_action.load()) &&
        (mode != Schedule::final_recheck || scenario.did_action.load()) &&
        (mode != Schedule::takeover_aba || scenario.did_aba.load()) &&
        (mode != Schedule::generation_wrap_boundary ||
            (scenario.did_action.load() && scenario.wrapped_generation.load()));
    lock.unlock();
    sintra::detail::set_debug_pause_active(false);
    return expect(reached && exercised, "surviving schedule missed its required action");
}

bool run_gap_schedule(Schedule mode)
{
    sintra::spinlock lock;
    Scenario scenario;
    scenario.lock = &lock;
    scenario.mode = mode;
    scenario.release_poll = 100;
    Hook_scope hooks(scenario);
    std::thread holder([&] {
        lock.lock();
        lock.unlock();
    });
    const auto deadline = std::chrono::steady_clock::now() + k_watchdog;
    while (!scenario.entered_gap && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!scenario.entered_gap) {
        scenario.leave_gap = true;
        holder.join();
        return expect(false, "holder did not reach its publication gap");
    }
    // The fake clock advances by more than two seconds during this unmarked
    // or already-even hold. A release from the polling hook ends the wait.
    std::thread waiter([&] { lock.lock(); lock.unlock(); });
    while (scenario.polls < 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    scenario.leave_gap = true;
    holder.join();
    waiter.join();
    return expect(scenario.polls >= 100 && scenario.samples >= 68,
        "waiter did not sample publication gap past the CPU timeout");
}

bool run_dead_owner_recovery(uint64_t owner, const char* message)
{
    sintra::spinlock lock;
    lock.test_install_owner(owner, 1);
    Scenario scenario;
    scenario.lock = &lock;
    scenario.mode = Schedule::still_cpu;
    scenario.release_poll = 0;
    Hook_scope hooks(scenario);
    lock.lock();
    const uint64_t takeover_mark = uint64_t(1) <<
        static_cast<unsigned>(spinlock_event::after_takeover_mark);
    const bool recovered = (scenario.seen_events.load() & takeover_mark) != 0 &&
        lock.test_owner() == sintra::detail::current_process_instance() &&
        lock.test_generation() == 5;
    lock.unlock();
    return expect(recovered, message);
}

bool prepare_abort_child()
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

bool expected_abort(const Exact_child& child)
{
#ifdef _WIN32
    return child.exited_with_code(3) || child.exited_with_code(0xC0000409u);
#else
    return child.exited_from_signal(SIGABRT);
#endif
}

bool run_abort_child(const char* program, const char* mode, const char* owner)
{
    const char* args[] = {program, mode, owner, nullptr};
    Exact_child child(std::chrono::seconds(10));
    if (!child.spawn(program, args)) {
        return expect(false, "could not spawn abort child");
    }
    const auto deadline = std::chrono::steady_clock::now() + k_watchdog;
    while (child.poll() == Exact_child_state::running &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (child.poll() != Exact_child_state::exited) {
        std::string diagnostic;
        child.terminate_and_settle(diagnostic);
        return expect(false, "abort child exceeded watchdog");
    }
    const bool aborted = expected_abort(child);
    std::string diagnostic;
    const bool settled = child.settle_observed_exit(diagnostic);
    return expect(aborted && settled, "child did not abort on continuous live hold");
}

int fake_abort_mode(std::string_view owner_arg, Schedule mode)
{
    if (!prepare_abort_child()) {
        return 2;
    }
    sintra::spinlock lock;
    const auto owner = owner_arg == "self"
        ? sintra::detail::current_process_instance()
        : owner_instance(uint32_t(std::strtoul(owner_arg.data(), nullptr, 10)));
    lock.test_install_owner(owner, 1);
    Scenario scenario;
    scenario.lock = &lock;
    scenario.mode = mode;
    scenario.release_poll = 0; // An unchanged live hold must cause abort.
    Hook_scope hooks(scenario);
    lock.lock();
    return 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string_view(argv[1]) == "--fake-abort-exact") {
        return fake_abort_mode(argv[2], Schedule::exact_boundary);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--fake-abort-jump") {
        return fake_abort_mode(argv[2], Schedule::single_jump);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--fake-abort-multiple") {
        return fake_abort_mode(argv[2], Schedule::multiple_samples);
    }

    const uint32_t self_pid = uint32_t(sintra::detail::get_current_process_id());
    const uint32_t dead_pid = find_dead_pid(self_pid);
    if (!expect(dead_pid != 0, "could not find an absent PID")) {
        return 1;
    }
    if (!run_dead_owner_recovery(owner_instance(dead_pid),
            "absent PID owner was not recovered and freshly marked")) {
        return 1;
    }
    const uint32_t other_token = uint32_t(sintra::detail::current_process_instance()) + 1;
    if (!run_dead_owner_recovery(owner_instance(self_pid, other_token),
            "same-PID different-token owner was not recovered and freshly marked")) {
        return 1;
    }
    {
        sintra::spinlock lock;
        lock.test_install_owner(owner_instance(dead_pid), 1);
        sintra::detail::set_debug_pause_active(true);
        lock.lock();
        const bool recovered = lock.test_owner() == sintra::detail::current_process_instance();
        lock.unlock();
        sintra::detail::set_debug_pause_active(false);
        if (!expect(recovered, "debug pause prevented death-proven recovery")) {
            return 1;
        }
    }
    {
        sintra::spinlock lock;
        lock.test_install_owner(owner_instance(dead_pid), 1);
        Scenario scenario;
        scenario.lock = &lock;
        scenario.mode = Schedule::losing_takeover;
        Hook_scope hooks(scenario);
        lock.lock();
        const bool two_bumps = lock.test_generation() >= 7;
        lock.unlock();
        if (!expect(two_bumps && scenario.did_action,
                "losing takeover did not preserve both pre-CAS bumps")) {
            return 1;
        }
    }
    if (!run_surviving_schedule(Schedule::short_holds, 140) ||
        !run_surviving_schedule(Schedule::late_waiter, 10) ||
        !run_surviving_schedule(Schedule::clock_failure, 8) ||
        !run_surviving_schedule(Schedule::clock_regression, 8) ||
        !run_surviving_schedule(Schedule::clock_conversion_overflow, 8) ||
        !run_surviving_schedule(Schedule::below_threshold, 8) ||
        !run_surviving_schedule(Schedule::debug_pause, 140) ||
        !run_surviving_schedule(Schedule::handoff_at_end, 10) ||
        !run_surviving_schedule(Schedule::owner_zero, 10) ||
        !run_surviving_schedule(Schedule::final_recheck, 1000) ||
        !run_surviving_schedule(Schedule::takeover_aba, 10) ||
        !run_surviving_schedule(Schedule::generation_wrap_boundary, 10) ||
        !run_surviving_schedule(Schedule::still_cpu, 100) ||
        !run_gap_schedule(Schedule::acquisition_gap) ||
        !run_gap_schedule(Schedule::release_gap))
    {
        return 1;
    }
    {
        sintra::spinlock lock;
        Scenario scenario;
        scenario.lock = &lock;
        scenario.mode = Schedule::stale_winner;
        Hook_scope hooks(scenario);
        lock.lock();
        const bool fresh = scenario.did_action && lock.test_generation() == 3;
        lock.unlock();
        if (!expect(fresh, "stale winning contender did not get a fresh odd mark")) {
            return 1;
        }
    }
    {
        sintra::spinlock lock;
        Scenario scenario;
        scenario.lock = &lock;
        scenario.mode = Schedule::losing_cas;
        scenario.release_poll = 5;
        Hook_scope hooks(scenario);
        lock.lock();
        const bool lost = scenario.did_action && scenario.polls >= 5;
        lock.unlock();
        if (!expect(lost, "ordinary CAS race did not lose and retry")) {
            return 1;
        }
    }
    const auto self = std::to_string(self_pid);
    if (!run_abort_child(argv[0], "--fake-abort-exact", "self") ||
        !run_abort_child(argv[0], "--fake-abort-exact", self.c_str()) ||
        !run_abort_child(argv[0], "--fake-abort-jump", "self") ||
        !run_abort_child(argv[0], "--fake-abort-jump", self.c_str()) ||
        !run_abort_child(argv[0], "--fake-abort-multiple", "self") ||
        !run_abort_child(argv[0], "--fake-abort-multiple", self.c_str()))
    {
        return 1;
    }
    constexpr auto hook_count = static_cast<unsigned>(spinlock_event::poll) + 1;
    constexpr uint64_t all_hooks = (uint64_t(1) << hook_count) - 1;
    if (!expect((g_observed_hooks.load() & all_hooks) == all_hooks,
            "some protocol handshake hooks were not exercised")) {
        return 1;
    }
    return 0;
}
