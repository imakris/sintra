//
// Sintra Coordinator Lock Order Test
//
// Regression test for the lock-order inversion between
// Coordinator::unpublish_transceiver() and Coordinator::make_process_group().
//
// unpublish_transceiver() used to hold m_publish_mutex while collecting
// barrier completions, which acquires m_groups_mutex. make_process_group()
// acquires m_groups_mutex and then publishes the group transceiver, which
// acquires m_publish_mutex. Running both concurrently could therefore
// deadlock (ABBA). The same inversion was reachable through
// add_external_process_to_standard_groups() and the join_swarm() rollback
// path, all of which nest m_groups_mutex -> m_publish_mutex.
//
// The test uses the coordinator lock-stage test hook to rendezvous one
// thread inside each critical section, which deadlocks deterministically on
// the inverted ordering. A free-running stress pass follows. A watchdog
// detects the deadlock and exits hard, since deadlocked threads hold
// coordinator mutexes and neither join() nor finalize() can succeed.
//

#include <sintra/sintra.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

using sintra::s_coord;
using sintra::s_coord_id;
using sintra::s_mproc;
using sintra::s_mproc_id;

namespace {

// Stage names are shared with the coordinator call sites, so a rename there
// cannot silently disarm the rendezvous.
constexpr std::string_view k_unpublish_stage =
    sintra::detail::test_hooks::k_stage_unpublish_pre_barrier_collection;
constexpr std::string_view k_make_group_stage =
    sintra::detail::test_hooks::k_stage_make_process_group_groups_locked;

// Pass 1 reproduces the inversion deterministically via the rendezvous hook;
// the free-running pass only adds opportunistic coverage, so a moderate
// iteration count is enough.
constexpr int k_stress_iterations = 50;

std::atomic<bool> g_rendezvous_armed{false};
std::atomic<bool> g_unpublish_stage_reached{false};
std::atomic<bool> g_make_group_stage_reached{false};

void wait_for_peer(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
}

// Holds each thread at its lock stage until the other thread has reached its
// own lock stage (or a deadline passes, so a fixed sintra cannot hang here).
void lock_stage_callback(const char* stage)
{
    if (!g_rendezvous_armed.load(std::memory_order_acquire)) {
        return;
    }

    const std::string_view stage_view(stage);
    if (stage_view == k_unpublish_stage) {
        g_unpublish_stage_reached.store(true, std::memory_order_release);
        wait_for_peer(g_make_group_stage_reached);
    }
    else if (stage_view == k_make_group_stage) {
        g_make_group_stage_reached.store(true, std::memory_order_release);
        wait_for_peer(g_unpublish_stage_reached);
    }
}

// Register a fake process directly in the coordinator registry so that
// unpublish_transceiver() takes the Managed_process cleanup path, which is
// the one that needs m_groups_mutex. The reverse name-map entry is seeded as
// well, so unpublish runs against the same state a real publish produces.
// (publish_transceiver is a strict RPC export and would be ring-routed even
// locally; seeding directly keeps both racing threads under test control.)
void seed_fake_process(sintra::instance_id_type fake_piid)
{
    std::lock_guard publish_lock(s_coord->m_publish_mutex);
    auto& publication = s_coord->m_transceiver_registry[fake_piid][fake_piid];
    publication = sintra::Tn_type{0, "coordinator_lock_order_fake"};
    s_coord->m_member_roles[fake_piid] = {
        publication.reader_identity,
        sintra::detail::Member_lifetime_role::COORDINATOR_BOUND};
    s_mproc->m_instance_id_of_assigned_name.set_value(
        "coordinator_lock_order_fake", fake_piid);
}

bool run_concurrent_unpublish_and_make_group(
    sintra::instance_id_type   fake_piid,
    const std::string&         group_name,
    std::chrono::seconds       budget)
{
    std::atomic<bool> unpublish_done{false};
    std::atomic<bool> unpublish_succeeded{false};
    std::atomic<bool> make_group_done{false};

    // Both RPC wrappers take the local shortcut and run the coordinator
    // member functions directly on these threads.
    std::thread unpublisher([&]() {
        unpublish_succeeded.store(
            sintra::Coordinator::rpc_unpublish_transceiver(
                s_coord_id,
                fake_piid),
            std::memory_order_release);
        unpublish_done.store(true, std::memory_order_release);
    });
    std::thread group_maker([&]() {
        sintra::Coordinator::rpc_make_process_group(
            s_coord_id,
            group_name,
            std::unordered_set<sintra::instance_id_type>{s_mproc_id});
        make_group_done.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + budget;
    while ((!unpublish_done.load(std::memory_order_acquire) ||
            !make_group_done.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!unpublish_done.load(std::memory_order_acquire) ||
        !make_group_done.load(std::memory_order_acquire))
    {
        std::fprintf(stderr,
            "coordinator_lock_order_test: deadlock detected"
            " (unpublish_done=%d, make_group_done=%d)\n",
            unpublish_done.load() ? 1 : 0,
            make_group_done.load() ? 1 : 0);
        std::fflush(stderr);
        // The stuck threads hold m_publish_mutex/m_groups_mutex; they cannot
        // be joined and finalize() would hang on the same mutexes.
        std::_Exit(1);
    }

    unpublisher.join();
    group_maker.join();
    return unpublish_succeeded.load(std::memory_order_acquire);
}

} // namespace

int main(int argc, char* argv[])
{
    sintra::init(argc, argv);

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        &lock_stage_callback, std::memory_order_release);

    const auto fake_piid = sintra::make_process_instance_id();

    // Pass 1: deterministic rendezvous inside the two critical sections.
    seed_fake_process(fake_piid);
    g_rendezvous_armed.store(true, std::memory_order_release);
    const bool deterministic_unpublish_succeeded =
        run_concurrent_unpublish_and_make_group(
            fake_piid,
            "coordinator_lock_order_group_rendezvous",
            std::chrono::seconds(20));
    g_rendezvous_armed.store(false, std::memory_order_release);
    if (!deterministic_unpublish_succeeded ||
        !g_unpublish_stage_reached.load(std::memory_order_acquire) ||
        !g_make_group_stage_reached.load(std::memory_order_acquire))
    {
        std::fprintf(stderr,
            "coordinator_lock_order_test: deterministic pass was disarmed "
            "(unpublish_succeeded=%d, unpublish_stage=%d, make_group_stage=%d)\n",
            deterministic_unpublish_succeeded ? 1 : 0,
            g_unpublish_stage_reached.load(std::memory_order_acquire) ? 1 : 0,
            g_make_group_stage_reached.load(std::memory_order_acquire) ? 1 : 0);
        sintra::detail::test_hooks::s_coordinator_lock_stage.store(
            nullptr, std::memory_order_release);
        sintra::detail::finalize();
        return 1;
    }

    // Pass 2: free-running stress without the rendezvous.
    for (int i = 0; i < k_stress_iterations; ++i) {
        seed_fake_process(fake_piid);
        if (!run_concurrent_unpublish_and_make_group(
                fake_piid,
                "coordinator_lock_order_group_" + std::to_string(i),
                std::chrono::seconds(20)))
        {
            std::fprintf(stderr,
                "coordinator_lock_order_test: unpublish failed in stress iteration %d\n",
                i);
            sintra::detail::test_hooks::s_coordinator_lock_stage.store(
                nullptr, std::memory_order_release);
            sintra::detail::finalize();
            return 1;
        }
    }

    sintra::detail::test_hooks::s_coordinator_lock_stage.store(
        nullptr, std::memory_order_release);

    sintra::detail::finalize();
    std::printf("coordinator_lock_order_test: OK\n");
    return 0;
}
