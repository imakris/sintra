// Destroying a transceiver from inside one of its own RPC handlers is a deterministic
// self-wait, not a race: Transceiver::ensure_rpc_shutdown() waits for the active-call
// count to reach zero, while the only call keeping it above zero is the invocation
// that is doing the waiting. Before the guard existed, the runtime logged six
// five-second warnings and then called std::terminate().
//
// The runtime now detects that the calling thread owns an execution guard on the
// transceiver being shut down and fails the call immediately with a causal
// diagnostic, on both dispatch routes:
//   - the same-process shortcut in Transceiver::rpc_impl (SINTRA_RPC below), and
//   - Transceiver::rpc_handler on a reader thread (SINTRA_RPC_STRICT below, which
//     stays on the request ring even within a single process).
//
// The check runs before any shutdown state is published, so a caller that handles the
// error is left with a transceiver that still works. The last case asserts exactly
// that.

#include <sintra/sintra.h>

#include "test_utils.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* k_failure_prefix = "rpc_self_destruction_guard_test: ";

// The pre-fix stall was 6 x 5 seconds before std::terminate(). Anything near that is
// the defect; the fixed path returns without waiting at all.
constexpr auto k_max_acceptable_call_duration = std::chrono::seconds(3);

struct Self_destroyer : sintra::Derived_transceiver<Self_destroyer>
{
    int destroy_from_direct_call()
    {
        destroy();
        return 1;
    }

    int destroy_from_ring_dispatch()
    {
        destroy();
        return 1;
    }

    int ping(int value)
    {
        return value + 1;
    }

    SINTRA_RPC(destroy_from_direct_call)
    SINTRA_RPC_STRICT(destroy_from_ring_dispatch)
    SINTRA_RPC(ping)
};

template <typename Call>
bool self_destruction_fails_immediately(const char* route, Call&& call)
{
    const auto started = std::chrono::steady_clock::now();

    bool        reported = false;
    std::string diagnostic;
    try {
        call();
    }
    catch (const std::logic_error& e) {
        reported   = true;
        diagnostic = e.what() ? e.what() : "";
    }
    catch (const std::exception& e) {
        std::fprintf(stderr,
            "%s%s: expected std::logic_error, got '%s'\n",
            k_failure_prefix,
            route,
            e.what() ? e.what() : "");
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;

    bool ok = sintra::test::assert_true(
        reported,
        k_failure_prefix,
        std::string(route) + ": self-destruction must be reported as a logic error");

    ok &= sintra::test::assert_true(
        !diagnostic.empty(),
        k_failure_prefix,
        std::string(route) + ": the reported error must carry a diagnostic");

    ok &= sintra::test::assert_true(
        elapsed < k_max_acceptable_call_duration,
        k_failure_prefix,
        std::string(route) + ": self-destruction must fail immediately, not after a wait");

    std::fprintf(stderr,
        "%s%s: elapsed_ms=%lld diagnostic='%s'\n",
        k_failure_prefix,
        route,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
        diagnostic.c_str());

    return ok;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        sintra::init(argc, argv);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%sinit failed: %s\n", k_failure_prefix, e.what());
        return 1;
    }

    bool ok = true;

    {
        Self_destroyer target;
        const auto     iid = target.instance_id();

        ok &= self_destruction_fails_immediately("direct call route", [iid] {
            (void)Self_destroyer::rpc_destroy_from_direct_call(iid);
        });

        ok &= self_destruction_fails_immediately("ring dispatch route", [iid] {
            (void)Self_destroyer::rpc_destroy_from_ring_dispatch(iid);
        });

        // The guard rejects the shutdown before it publishes any of it, so the
        // transceiver must still accept calls.
        int echoed = 0;
        try {
            echoed = Self_destroyer::rpc_ping(iid, 41);
        }
        catch (const std::exception& e) {
            std::fprintf(stderr,
                "%srejected self-destruction left the transceiver unusable: %s\n",
                k_failure_prefix,
                e.what() ? e.what() : "");
        }

        ok &= sintra::test::assert_true(
            echoed == 42,
            k_failure_prefix,
            "a transceiver that refused self-destruction must remain usable");
    }

    sintra::shutdown();

    return ok ? 0 : 1;
}
