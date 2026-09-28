#include <sintra/detail/ipc/spinlock.h>
#include <sintra/detail/debug_pause.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--marked-holder-child") {
        sintra::detail::set_debug_pause_active(false);
        sintra::test::prepare_for_intentional_crash();
        if (std::signal(SIGABRT, SIG_DFL) == SIG_ERR) {
            return 2;
        }
#ifdef _WIN32
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        if (_set_error_mode(_OUT_TO_STDERR) == -1) {
            return 2;
        }
#endif
        sintra::spinlock lock;
        lock.lock(); // Stay inside the marked critical section while W runs.
        std::thread waiter([&] {
#ifdef _WIN32
            // The suite runs many child tests concurrently. Give this waiter
            // enough CPU to reach its CPU-time threshold under the runner's
            // fixed 30-second per-test limit.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
            lock.lock();
        });
        waiter.join();
        return 1;
    }

    const char* args[] = {argv[0], "--marked-holder-child", nullptr};
    sintra::test::Exact_child child(std::chrono::seconds(10));
    if (!child.spawn(argv[0], args)) {
        std::fprintf(stderr, "spinlock_real_clock_test: child spawn failed: %s\n",
            child.error().c_str());
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (child.poll() == sintra::test::Exact_child_state::running &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (child.poll() != sintra::test::Exact_child_state::exited) {
        std::string diagnostic;
        child.terminate_and_settle(diagnostic);
        std::fprintf(stderr, "spinlock_real_clock_test: child exceeded watchdog: %s\n",
            diagnostic.c_str());
        return 1;
    }
#ifdef _WIN32
    const bool aborted = child.exited_with_code(3) || child.exited_with_code(0xC0000409u);
#else
    const bool aborted = child.exited_from_signal(SIGABRT);
#endif
    std::string diagnostic;
    if (!child.settle_observed_exit(diagnostic) || !aborted) {
        std::fprintf(stderr, "spinlock_real_clock_test: no diagnostic abort: %s\n",
            child.describe_status().c_str());
        return 1;
    }
    return 0;
}
