// Stress test for the race between Transceiver destruction and a concurrent
// RPC dispatch.
//
// Race shape (pre-fix):
//   1. RPC caller looks up `obj` in the spinlocked instance-to-object map.
//   2. Caller releases the spinlock.
//   3. Owner thread runs ~Transceiver: ~Instantiator erases the map entry,
//      ensure_rpc_shutdown observes m_active_rpc_calls == 0 and proceeds,
//      m_rpc_lifecycle_mutex is destroyed.
//   4. Caller dereferences the now-dangling `obj` to call
//      try_acquire_rpc_execution() and locks the dead mutex.
//
// The fix extends the spinlock through try_acquire_rpc_execution() so the
// owner thread's ~Instantiator (which acquires the same spinlock to erase the
// entry) cannot make progress until the caller has either incremented
// m_active_rpc_calls (blocking ensure_rpc_shutdown) or observed
// m_accepting_rpc_calls=false and bailed.
//
// This test exercises the same-process direct-call path (rpc_impl) by
// repeatedly creating and destroying a target while N threads pound it with
// RPC calls. Without the fix, sustained pressure eventually hits the race;
// with the fix, the test should run cleanly to completion. Under TSan/ASan
// the failure mode of the unfixed code is deterministic (UAF on
// m_rpc_lifecycle_mutex); without sanitizers it is best-effort smoke.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct Stress_target : sintra::Derived_transceiver<Stress_target>
{
    int noop() { return 1; }
    SINTRA_RPC(noop)
};

constexpr int k_iterations       = 400;
constexpr int k_caller_threads   = 4;
constexpr auto k_target_lifetime = std::chrono::microseconds(50);

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        std::cerr << "rpc_destruction_race_test: init failed: " << e.what() << "\n";
        return 1;
    }

    std::atomic<sintra::instance_id_type> current{0};
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> rpc_returned{0};
    std::atomic<unsigned long long> rpc_unavailable{0};
    std::atomic<unsigned long long> rpc_other{0};

    auto caller = [&]() {
        while (!stop.load(std::memory_order_acquire)) {
            const auto iid = current.load(std::memory_order_acquire);
            if (iid == 0) {
                std::this_thread::yield();
                continue;
            }
            try {
                (void)Stress_target::rpc_noop(iid);
                rpc_returned.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const sintra::rpc_unavailable&) {
                rpc_unavailable.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception&) {
                rpc_other.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> callers;
    callers.reserve(k_caller_threads);
    for (int i = 0; i < k_caller_threads; ++i) {
        callers.emplace_back(caller);
    }

    for (int it = 0; it < k_iterations; ++it) {
        auto* target = new Stress_target;
        current.store(target->instance_id(), std::memory_order_release);
        std::this_thread::sleep_for(k_target_lifetime);
        current.store(0, std::memory_order_release);
        delete target;
    }

    stop.store(true, std::memory_order_release);
    for (auto& t : callers) {
        t.join();
    }

    sintra::shutdown();

    // The test passes if it reaches this point without crashing or
    // deadlocking. Print counters so a CI run records evidence that the
    // contention actually happened (i.e. that some calls reached the
    // unavailable path and were rejected cleanly).
    std::cerr << "rpc_destruction_race_test:"
              << " returned=" << rpc_returned.load()
              << " unavailable=" << rpc_unavailable.load()
              << " other=" << rpc_other.load() << "\n";

    if (rpc_other.load() != 0) {
        return 1;
    }
    return 0;
}
