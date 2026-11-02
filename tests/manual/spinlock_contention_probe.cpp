// Optional native Linux measurement. Poll hooks count liveness-check
// opportunities, not syscalls. No timing or CPU threshold is a test oracle.
#include <sintra/detail/ipc/spinlock.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using Event = sintra::detail::test_hooks::spinlock_event;
constexpr auto deadline_interval = std::chrono::seconds(2);
constexpr auto hold_interval = std::chrono::milliseconds(30);
constexpr uint64_t protected_value = 0x67a13cb9;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
bool await(Predicate predicate)
{
    const auto deadline = Clock::now() + deadline_interval;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

struct Shared_state
{
    sintra::spinlock lock;
    std::atomic<uint32_t> held{0};
    std::atomic<uint32_t> waiter_polled{0};
    uint64_t value = 0;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

struct Mapping
{
    Mapping()
    {
        void* memory = ::mmap(nullptr, sizeof(Shared_state), PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        require(memory != MAP_FAILED, "shared mapping failed");
        state = new (memory) Shared_state;
    }
    ~Mapping()
    {
        state->~Shared_state();
        ::munmap(state, sizeof(Shared_state));
    }
    Shared_state* state = nullptr;
};

int hold(Shared_state& state)
{
    sintra::spinlock::locker locked(state.lock);
    state.held.store(1, std::memory_order_release);
    if (!await([&] { return state.waiter_polled.load(std::memory_order_acquire) != 0; })) {
        return 2;
    }
    std::this_thread::sleep_for(hold_interval);
    state.value = protected_value;
    return 0;
}

// Only the waiter thread records hooks. The same-process holder's unlock hooks
// therefore never race the counters, and the forked holder has its own state.
thread_local Shared_state* observed_state = nullptr;
thread_local uint64_t polls = 0;

void observe_poll(const void* lock, Event event)
{
    if (observed_state && lock == &observed_state->lock && event == Event::poll) {
        ++polls;
        observed_state->waiter_polled.store(1, std::memory_order_release);
    }
}

struct Holder
{
    explicit Holder(Shared_state& state) : state(state) {}
    ~Holder()
    {
        // Also permits normal thread cleanup if setup fails before measuring.
        state.waiter_polled.store(1, std::memory_order_release);
        if (thread.joinable()) {
            thread.join();
        }
        if (pid > 0) {
            int status = 0;
            pid_t result;
            do { result = ::waitpid(pid, &status, WNOHANG); }
            while (result < 0 && errno == EINTR);
            if (result == 0) {
                // This exact child has not been reaped, so its PID is retained.
                ::kill(pid, SIGKILL);
                do { result = ::waitpid(pid, &status, 0); }
                while (result < 0 && errno == EINTR);
            }
        }
    }
    Shared_state& state;
    std::thread thread;
    pid_t pid = -1;
    int thread_result = -1;
};

void trial(bool foreign, unsigned round)
{
    Mapping mapping;
    auto& state = *mapping.state;
    Holder holder(state);
    if (foreign) {
        holder.pid = ::fork();
        require(holder.pid >= 0, "fork failed");
        if (holder.pid == 0) {
            ::_exit(hold(state));
        }
    }
    else {
        holder.thread = std::thread([&] { holder.thread_result = hold(state); });
    }
    require(await([&] { return state.held.load(std::memory_order_acquire) != 0; }),
        "holder did not acquire the lock");

    struct Hook_scope
    {
        explicit Hook_scope(Shared_state& state)
        {
            observed_state = &state;
            polls = 0;
            previous = sintra::detail::test_hooks::s_spinlock_event.exchange(&observe_poll);
        }
        ~Hook_scope()
        {
            sintra::detail::test_hooks::s_spinlock_event = previous;
            observed_state = nullptr;
        }
        sintra::detail::test_hooks::spinlock_event_callback previous = nullptr;
    } hooks(state);

    sintra::detail::spinlock_cpu_sample cpu_before{}, cpu_after{};
    const bool cpu_before_available = sintra::detail::read_native_thread_cpu(cpu_before);
    const auto before = Clock::now();
    state.lock.lock();
    const auto acquired = Clock::now();
    const bool cpu_available = sintra::detail::read_native_thread_cpu(cpu_after) &&
        cpu_before_available && cpu_after.ns >= cpu_before.ns;
    const auto value = state.value;
    state.lock.unlock();
    require(value == protected_value, "lock acquired before the holder published its value");

    if (foreign) {
        int status = 0;
        pid_t result = 0;
        require(await([&] {
            do { result = ::waitpid(holder.pid, &status, WNOHANG); }
            while (result < 0 && errno == EINTR);
            return result != 0;
        }), "holder child did not finish");
        require(result == holder.pid, "waitpid lost the exact child");
        holder.pid = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "holder child failed");
    }
    else {
        holder.thread.join();
        require(holder.thread_result == 0, "holder thread failed");
    }

    std::cout << "mode=" << (foreign ? "foreign_process" : "same_process")
              << " round=" << round << " requested_hold_ms=" << hold_interval.count()
              << " poll_opportunities=" << polls
              << " acquisition_wall_ms="
              << std::chrono::duration<double, std::milli>(acquired - before).count()
              << " waiter_cpu_ms=";
    if (cpu_available) {
        std::cout << (cpu_after.ns - cpu_before.ns) / 1e6;
    }
    else {
        std::cout << "unavailable";
    }
    std::cout << " protected_value=passed\n";
}

} // namespace
#endif

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "--measure") {
        std::cerr << "Usage: spinlock_contention_probe --measure\n";
        return 2;
    }
#if defined(__linux__)
    try {
        unsigned round = 0;
        // Reverse the order on the second pair; native liveness/backoff remain
        // unchanged, and no process is intentionally killed during a trial.
        for (const bool foreign : {false, true, true, false}) {
            for (unsigned repetition = 0; repetition != 3; ++repetition) {
                trial(foreign, ++round);
            }
        }
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
#else
    std::cerr << "UNAVAILABLE: this optional measurement requires native Linux\n";
    return 77;
#endif
}
