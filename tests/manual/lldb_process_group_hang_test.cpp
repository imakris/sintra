//
// Sintra LLDB Process Group Hang Test
//
// This manual test intentionally orchestrates a multi-process ping-pong
// exchange that stalls after a few iterations. It exists to validate the
// debugger integration path that captures LLDB stacks for every process and
// thread in the group when the harness times out.
//

#include <sintra/sintra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Ping {};
struct Pong {};

constexpr std::string_view kBarrierName = "lldb-hang-test-ready";

void run_background_heartbeat(const std::shared_ptr<std::atomic<bool>>& running)
{
    std::thread([running] {
        while (running->load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }).detach();
}

int ping_responder_process()
{
    auto running = std::make_shared<std::atomic<bool>>(true);
    run_background_heartbeat(running);

    sintra::activate_slot([](Ping) {
        sintra::world() << Pong();
    });

    sintra::barrier(std::string(kBarrierName), "_sintra_all_processes");

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

int pong_responder_process()
{
    auto running = std::make_shared<std::atomic<bool>>(true);
    run_background_heartbeat(running);

    auto hang_mutex = std::make_shared<std::mutex>();
    auto hang_cv = std::make_shared<std::condition_variable>();
    auto hang_triggered = std::make_shared<std::atomic<bool>>(false);
    auto exchange_count = std::make_shared<std::atomic<int>>(0);

    sintra::activate_slot([=](Pong) {
        int count = exchange_count->fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 6) {
            sintra::world() << Ping();
            return;
        }

        if (!hang_triggered->exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "[  FAILED  ] pong_responder intentionally stalled after %d exchanges\n",
                count);
            std::fflush(stderr);
        }

        std::unique_lock<std::mutex> lock(*hang_mutex);
        hang_cv->wait(lock, [] { return false; });
    });

    sintra::barrier(std::string(kBarrierName), "_sintra_all_processes");

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const bool is_spawned = std::any_of(argv, argv + argc, [](const char* arg) {
        return std::string_view(arg) == "--branch_index";
    });

    std::vector<sintra::Process_descriptor> processes;
    processes.emplace_back(ping_responder_process);
    processes.emplace_back(pong_responder_process);

    sintra::init(argc, argv, processes);

    if (!is_spawned) {
        auto running = std::make_shared<std::atomic<bool>>(true);
        run_background_heartbeat(running);

        sintra::barrier(std::string(kBarrierName), "_sintra_all_processes");

        sintra::world() << Ping();

        std::fprintf(stderr, "Coordinator waiting for workers to complete (expected hang).\n");
        std::fflush(stderr);

        std::mutex wait_mutex;
        std::unique_lock<std::mutex> lock(wait_mutex);
        std::condition_variable wait_cv;
        wait_cv.wait(lock, [] { return false; });
    }

    sintra::finalize();
    return 0;
}

