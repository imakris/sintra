//
// Sintra Mass Join Test
//
// Many workers join one swarm at the same moment. Every worker attaches to
// the coordinator's rings while the others do, then exchanges RPCs and
// broadcasts with all of them. All workers must complete initialization,
// receive correct replies, and observe every peer broadcast.
//
// Reader admission, wakeup registration and publication share spinlocks in
// the coordinator's rings. When the waiters outnumber the processors, a
// preempted holder must still get to run and release its lock.
//

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int k_workers = 40;
constexpr int k_rounds = 10;
constexpr const char* k_named_barrier = "mass-join-named";
constexpr const char* k_delivered_barrier = "mass-join-delivered";

struct Mass_join_echo : sintra::Derived_transceiver<Mass_join_echo>
{
    int echo(int value) { return value + 1; }
    SINTRA_RPC(echo);
};

struct note_t
{
    int sender;
    int round;
};

std::atomic<int> s_peer_notes{0};

std::string echo_name(int index)
{
    return "mass_join_echo_" + std::to_string(index);
}

std::filesystem::path result_path(const std::filesystem::path& dir, int index)
{
    return dir / ("worker_" + std::to_string(index) + ".txt");
}

int worker()
{
    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "mass_join");
    const int index = sintra::process_index();
    std::string failure;

    Mass_join_echo echo;
    if (!echo.assign_name(echo_name(index))) {
        failure = "assign_name failed";
    }
    sintra::activate_slot([index](note_t note) {
        if (note.sender != index) {
            s_peer_notes.fetch_add(1);
        }
    });
    sintra::barrier(k_named_barrier, "_sintra_all_processes");

    const std::string peer = echo_name(index % k_workers + 1);
    for (int round = 0; round < k_rounds && failure.empty(); ++round) {
        const int reply = Mass_join_echo::rpc_echo(peer, round);
        if (reply != round + 1) {
            failure = "peer " + peer + " replied " + std::to_string(reply) +
                " to " + std::to_string(round);
        }
        sintra::world() << note_t{index, round};
    }

    sintra::barrier<sintra::processing_fence_t>(k_delivered_barrier, "_sintra_all_processes");

    const int expected = (k_workers - 1) * k_rounds;
    const int received = s_peer_notes.load();
    if (failure.empty() && received != expected) {
        failure = "received " + std::to_string(received) + " of " +
            std::to_string(expected) + " peer notes";
    }
    sintra::test::write_lines(result_path(shared.path(), index),
        {failure.empty() ? std::string("ok") : failure});
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<sintra::Process_descriptor> workers(
        k_workers, sintra::Process_descriptor(worker));

    return sintra::test::run_multi_process_shutdown_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "mass_join",
        std::move(workers),
        [start](const std::filesystem::path&) {
            sintra::barrier(k_named_barrier, "_sintra_all_processes");
            sintra::barrier<sintra::processing_fence_t>(
                k_delivered_barrier, "_sintra_all_processes");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            std::fprintf(stderr, "mass_join_test: %d workers joined and exchanged traffic in %lld ms\n",
                k_workers, static_cast<long long>(elapsed.count()));
            return 0;
        },
        [](const std::filesystem::path& shared_dir) {
            int failures = 0;
            for (int index = 1; index <= k_workers; ++index) {
                const auto lines = sintra::test::read_lines(result_path(shared_dir, index));
                if (lines.size() != 1 || lines[0] != "ok") {
                    std::fprintf(stderr, "mass_join_test: worker %d: %s\n", index,
                        lines.empty() ? "no result" : lines[0].c_str());
                    ++failures;
                }
            }
            return failures == 0 ? 0 : 1;
        });
}
