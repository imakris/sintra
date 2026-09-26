#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
constexpr const char* k_prefix = "post_handler_fence_order_test: ";
constexpr const char* k_target = "post-handler-fence-target";
constexpr const char* k_group = "post-handler-fence-coordinator";

// Only the coordinator's copies are used by the two incoming RPC handlers.
std::atomic<bool> s_slow_entered{false};
std::atomic<bool> s_release_slow{false};
std::atomic<bool> s_wait_observed{false};
std::atomic<bool> s_handler_active{false};
std::atomic<bool> s_early_callback{false};
std::atomic<unsigned> s_callback_order{0};
std::atomic<bool> s_order_failed{false};
thread_local bool s_observe_this_fence = false;

template <typename Predicate>
bool wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void observe_fence_wait() noexcept
{
    if (s_observe_this_fence) {
        s_wait_observed = true;
    }
}

[[noreturn]] void fail_timeout(const char* message)
{
    std::fprintf(stderr, "%s%s\n", k_prefix, message);
    std::_Exit(1);
}

void record_callback(unsigned expected)
{
    if (s_handler_active) {
        s_early_callback = true;
    }
    if (s_callback_order.fetch_add(1) != expected) {
        s_order_failed = true;
    }
}

struct Fence_target : sintra::Derived_transceiver<Fence_target>
{
    bool hold_peer()
    {
        s_slow_entered = true;
        return wait_until([] { return s_release_slow.load(); });
    }

    bool enter_fence(bool processing)
    {
        if (!wait_until([] { return s_slow_entered.load(); })) {
            return false;
        }
        s_handler_active = true;
        struct Handler_exit
        {
            ~Handler_exit() { s_handler_active = false; }
        } handler_exit;

        sintra::s_mproc->run_after_current_handler([] { record_callback(0); });
        sintra::s_mproc->run_after_current_handler([] {
            // A post-handler can still enter a fence after its originating
            // handler returned. It must not depend on its own reader stream.
            sintra::s_mproc->wait_for_delivery_fence();
            record_callback(1);
        });

        s_observe_this_fence = true;
        const auto sequence = processing
            ? sintra::barrier<sintra::processing_fence_t>("ordered-processing", k_group)
            : sintra::barrier<sintra::delivery_fence_t>("ordered-delivery", k_group);
        s_observe_this_fence = false;
        return sequence != sintra::invalid_sequence;
    }

    SINTRA_RPC(hold_peer)
    SINTRA_RPC(enter_fence)
};

int run_peer(bool slow)
{
    bool ok = true;
    for (unsigned round = 0; round != 2; ++round) {
        sintra::barrier<sintra::rendezvous_t>("post-handler-round-ready", "_sintra_all_processes");
        const auto target = sintra::Coordinator::rpc_resolve_instance(sintra::s_coord_id, k_target);
        if (target == sintra::invalid_instance_id) {
            fail_timeout("peer could not resolve target");
        }
        ok &= slow ? Fence_target::rpc_hold_peer(target)
                   : Fence_target::rpc_enter_fence(target, round == 1);
        sintra::barrier<sintra::rendezvous_t>("post-handler-round-done", "_sintra_all_processes");
    }
    return ok ? 0 : 1;
}

int slow_peer() { return run_peer(true); }
int fence_peer() { return run_peer(false); }

int coordinator(const std::filesystem::path&)
{
    Fence_target target;
    if (!target.assign_name(k_target)) {
        return 1;
    }
    sintra::Coordinator::rpc_make_process_group(sintra::s_coord_id, k_group,
        std::unordered_set<sintra::instance_id_type>{sintra::s_mproc_id});
    sintra::detail::test_hooks::s_delivery_fence_wait = &observe_fence_wait;

    bool ok = true;
    for (unsigned round = 0; round != 2; ++round) {
        s_slow_entered = false;
        s_release_slow = false;
        s_wait_observed = false;
        s_early_callback = false;
        s_callback_order = 0;
        s_order_failed = false;

        sintra::barrier<sintra::rendezvous_t>("post-handler-round-ready", "_sintra_all_processes");
        // The observer runs only after the fence is known to be unsatisfied
        // and after the old early-draining path, so releasing this peer cannot
        // accidentally bypass the ordering interleaving being tested.
        if (!wait_until([] { return s_wait_observed.load(); })) {
            fail_timeout("handler did not reach an unsatisfied fence wait");
        }
        s_release_slow = true;
        if (!wait_until([] { return s_callback_order.load() == 2; })) {
            fail_timeout("deferred callbacks did not complete");
        }
        ok &= sintra::test::assert_true(!s_early_callback && !s_order_failed,
            k_prefix, "deferred callbacks must run in order after the RPC handler returns");
        sintra::barrier<sintra::rendezvous_t>("post-handler-round-done", "_sintra_all_processes");
    }
    sintra::detail::test_hooks::s_delivery_fence_wait = nullptr;
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(argc, argv,
        "SINTRA_POST_HANDLER_FENCE_DIR", "post_handler_fence_order",
        {slow_peer, fence_peer}, coordinator,
        [](const std::filesystem::path&) { return 0; });
}
