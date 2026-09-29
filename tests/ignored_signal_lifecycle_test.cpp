#include <sintra/sintra.h>

#include "exact_child_test_support.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#endif

namespace {

constexpr const char* k_child_arg = "--ignored-signal-child";

struct Ping { int value; };

bool disposition_is_ignored(int sig)
{
#ifdef _WIN32
    return std::signal(sig, SIG_GET) == SIG_IGN;
#else
    struct sigaction current {};
    return ::sigaction(sig, nullptr, &current) == 0 && current.sa_handler == SIG_IGN;
#endif
}

int run_child(int sig, const char* binary)
{
    if (std::signal(sig, SIG_IGN) == SIG_ERR) {
        return 2;
    }
    const char* args[] = {binary, nullptr};
    sintra::init(1, args);
    std::atomic<int> received{0};
    sintra::activate_slot([&](const Ping& ping) {
        received.store(ping.value, std::memory_order_release);
    });
    if (!disposition_is_ignored(sig)) {
        return 3;
    }
    if (std::raise(sig) != 0) {
        return 4;
    }
    // A false crash dispatch stops readers; this local event must still be
    // delivered after the ignored signal.
    sintra::local() << Ping{sig};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load(std::memory_order_acquire) != sig &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (received.load(std::memory_order_acquire) != sig) {
        return 6;
    }
    sintra::detail::finalize();
    return 0;
}

bool run_case(const std::string& binary, int sig)
{
    const std::vector<std::string> args = {
        binary, k_child_arg, std::to_string(sig)
    };
    sintra::C_string_vector cargs(args);
    sintra::test::Exact_child child(std::chrono::seconds(5));
    if (!child.spawn(binary.c_str(), cargs.v())) {
        std::fprintf(stderr, "ignored_signal_lifecycle_test: spawn failed: %s\n",
            child.error().c_str());
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto state = child.poll();
    while (state == sintra::test::Exact_child_state::running &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        state = child.poll();
    }
    if (state != sintra::test::Exact_child_state::exited || !child.exited_with_code(0)) {
        std::fprintf(stderr, "ignored_signal_lifecycle_test: signal %d: %s\n",
            sig, child.describe_status().c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string(argv[1]) == k_child_arg) {
        try {
            return run_child(std::stoi(argv[2]), argv[0]);
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "ignored_signal_lifecycle_test child: %s\n", e.what());
            return 5;
        }
    }
    const auto binary = sintra::test::get_binary_path(argc, argv);
    return run_case(binary, SIGINT) && run_case(binary, SIGTERM) ? 0 : 1;
}
