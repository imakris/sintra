//
// Sintra delivery-fence reader deadlock test
//
// A process keeps one Process_message_reader per peer, each with its own request
// reader thread, and a targeted RPC is dispatched on the reader thread of the ring it
// arrived on. Two RPC handlers coming from two different peers therefore run on two
// different request reader threads of the same process.
//
// Managed_process::wait_for_delivery_fence() waits for every local reader's captured
// request and reply sequences to be reached, and excludes only the calling thread's
// own request stream, because that thread cannot serve its own ring while it waits.
// The same is true of every other request reader thread parked in the fence, and that
// case was not excluded: handler A waited for reader B's request stream while handler
// B waited for reader A's, neither reader could read while parked, and neither stream
// carried a stopped flag. The result was a permanent hang of both threads.
//
// This test reproduces exactly that shape. The coordinator process publishes one
// transceiver; both children call it at the same time; each handler waits until both
// have arrived (so that both request streams are provably behind their leading
// sequence) and then enters a delivery fence. If the fence honours only the caller's
// own exclusion, both handlers hang and the completion count never reaches two.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr const char* k_failure_prefix = "delivery_fence_reader_deadlock_test: ";
constexpr const char* k_target_name    = "delivery_fence_reader_deadlock_target";

constexpr int  k_peer_count       = 2;
constexpr auto k_arrival_timeout  = 5s;
constexpr auto k_fence_deadline   = 20s;

// Coordinator-process state. The children never touch their own copies.
std::atomic<int> g_arrived{0};
std::atomic<int> g_completed{0};

struct Fence_target : sintra::Derived_transceiver<Fence_target>
{
    // Runs on the request reader thread of the calling peer's ring.
    int enter_fence()
    {
        g_arrived.fetch_add(1);

        // Park both handlers here first. Each thread has fetched, but not yet
        // published, the request message it is handling, so at the moment the fences
        // capture their targets both request streams are behind their leading
        // sequence and each fence really does need the other thread to make progress.
        const auto arrival_deadline = std::chrono::steady_clock::now() + k_arrival_timeout;
        while (g_arrived.load() < k_peer_count &&
            std::chrono::steady_clock::now() < arrival_deadline)
        {
            std::this_thread::sleep_for(1ms);
        }

        if (g_arrived.load() < k_peer_count) {
            // The peer never arrived, so this is not the interleaving under test.
            // Report it rather than entering a fence that proves nothing.
            std::fprintf(stderr, "%speer handler did not arrive\n", k_failure_prefix);
            return 0;
        }

        sintra::s_mproc->wait_for_delivery_fence();

        g_completed.fetch_add(1);
        return 1;
    }

    SINTRA_RPC(enter_fence)
};

int call_from_peer()
{
    sintra::barrier("delivery-fence-deadlock-setup", "_sintra_all_processes");

    const auto target_iid =
        sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, k_target_name);
    if (target_iid == sintra::invalid_instance_id) {
        std::fprintf(stderr, "%speer could not resolve the fence target\n", k_failure_prefix);
        sintra::barrier("delivery-fence-deadlock-done", "_sintra_all_processes");
        return 1;
    }

    int accepted = 0;
    try {
        accepted = Fence_target::rpc_enter_fence(target_iid);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%speer rpc failed: %s\n", k_failure_prefix, e.what());
    }

    sintra::barrier("delivery-fence-deadlock-done", "_sintra_all_processes");
    return accepted == 1 ? 0 : 1;
}

int first_peer()  { return call_from_peer(); }
int second_peer() { return call_from_peer(); }

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_DELIVERY_FENCE_DEADLOCK_DIR",
        "delivery_fence_reader_deadlock",
        {first_peer, second_peer},
        [](const std::filesystem::path& shared_dir) {
            std::filesystem::remove(shared_dir / "result.txt");
        },
        [](const std::filesystem::path& shared_dir) {
            Fence_target target;
            if (!target.assign_name(k_target_name)) {
                std::fprintf(stderr, "%sfailed to publish the fence target\n", k_failure_prefix);
                return 1;
            }

            sintra::barrier("delivery-fence-deadlock-setup", "_sintra_all_processes");

            const auto deadline = std::chrono::steady_clock::now() + k_fence_deadline;
            while (g_completed.load() < k_peer_count &&
                std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(10ms);
            }

            const int  completed = g_completed.load();
            const bool ok        = sintra::test::assert_true(
                completed == k_peer_count,
                k_failure_prefix,
                "both handler-invoked delivery fences must complete");

            std::ofstream out(shared_dir / "result.txt", std::ios::binary | std::ios::trunc);
            out << (ok ? "ok" : "fail") << '\n';
            out << completed << '\n';

            sintra::barrier("delivery-fence-deadlock-done", "_sintra_all_processes");
            return ok ? 0 : 1;
        },
        [](const std::filesystem::path& shared_dir) {
            std::ifstream in(shared_dir / "result.txt", std::ios::binary);
            if (!in) {
                return 1;
            }

            std::string status;
            std::getline(in, status);
            return status == "ok" ? 0 : 1;
        });
}
