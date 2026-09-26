#include <sintra/sintra.h>

#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>

namespace {

constexpr int k_reply = 17;
constexpr const char* k_coordinator_name = "rpc_wait_guard_coordinator";

class Guard_service : public sintra::Derived_transceiver<Guard_service>
{
public:
    ~Guard_service() { destroy(); }

    int echo() { ++m_echo_calls; return k_reply; }
    int direct_echo() { return k_reply; }
    unsigned echo_calls() const { return m_echo_calls.load(); }

    // Define deduced-return RPC helpers before the methods that call them.
    SINTRA_RPC_STRICT(echo)
    SINTRA_RPC(direct_echo)

    void prepare_completed()
    {
        m_completed.emplace(rpc_async_echo(instance_id()));
        (void)m_completed->get_until(std::chrono::steady_clock::now() + std::chrono::seconds(3));
    }

    int exercise(int mode)
    {
        try {
            switch (mode) {
            case 0:
            case 6:
                return rpc_echo(instance_id());
            case 1:
                return rpc_async_echo(instance_id()).get();
            case 2:
                return rpc_async_echo(instance_id()).get_until(
                    std::chrono::steady_clock::now() + std::chrono::seconds(3));
            case 3:
                return rpc_direct_echo(instance_id());
            case 4:
                return m_completed->get();
            case 5:
                m_pending.emplace(rpc_async_echo(instance_id()));
                try {
                    (void)m_pending->get();
                }
                catch (const std::logic_error&) {
                    return k_reply;
                }
                return 0;
            case 7:
                try {
                    (void)rpc_async_echo(instance_id()).get_until(std::chrono::steady_clock::now());
                }
                catch (const sintra::rpc_timeout&) {
                    return k_reply;
                }
                return 0;
            }
        }
        catch (const std::logic_error&) {
            return 1;
        }
        return 0;
    }

    bool finish_pending()
    {
        return m_pending->get_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(3)) == k_reply;
    }

    SINTRA_RPC_STRICT(exercise)

private:
    std::atomic<unsigned> m_echo_calls{0};
    std::optional<sintra::Rpc_handle<int>> m_completed;
    std::optional<sintra::Rpc_handle<int>> m_pending;
};

bool check_local_reader(Guard_service& service)
{
    service.prepare_completed();
    for (int mode = 0; mode != 6; ++mode) {
        try {
            const auto previous_calls = service.echo_calls();
            const int actual = Guard_service::rpc_async_exercise(service.instance_id(), mode).get_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
            const int expected = mode < 3 ? 1 : k_reply;
            if (actual != expected) {
                std::fprintf(stderr, "RPC wait guard mode %d returned %d, expected %d\n",
                    mode, actual, expected);
                return false;
            }
            if (mode == 0) {
                // A later request proves the rejected synchronous call did
                // not leave a transported invocation queued ahead of it.
                (void)Guard_service::rpc_async_echo(service.instance_id()).get_until(
                    std::chrono::steady_clock::now() + std::chrono::seconds(3));
                if (service.echo_calls() != previous_calls + 1) {
                    std::fprintf(stderr, "Rejected synchronous RPC was still submitted\n");
                    return false;
                }
            }
        }
        catch (const sintra::rpc_timeout&) {
            // Release the deliberately blocked inner call on the unfixed
            // implementation so the regression fails without hanging teardown.
            sintra::s_mproc->unblock_rpc(sintra::process_of(service.instance_id()));
            std::fprintf(stderr, "RPC wait guard mode %d blocked its own dispatch reader\n", mode);
            return false;
        }
    }
    return service.finish_pending() &&
        Guard_service::rpc_async_exercise(service.instance_id(), 7).get_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(3)) == k_reply;
}

int client()
{
    Guard_service service;
    sintra::barrier("rpc-wait-guard-ready", "_sintra_all_processes");
    bool passed = check_local_reader(service);
    sintra::barrier("rpc-wait-guard-local-finished", "_sintra_all_processes");
    if (passed) {
        // The coordinator dispatches this call on the client's ring reader.
        // Its own-ring reader remains available to serve the nested local RPC.
        const auto coordinator = sintra::get_instance_id<Guard_service>(std::string(k_coordinator_name));
        passed = Guard_service::rpc_async_exercise(coordinator, 6).get_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(3)) == k_reply;
    }
    sintra::test::Shared_directory shared("SINTRA_RPC_WAIT_GUARD_DIR", "rpc_wait_guard");
    sintra::test::append_line_or_throw(shared.path() / "client.txt", passed ? "pass" : "fail");
    sintra::barrier("rpc-wait-guard-finished", "_sintra_all_processes");
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc, argv, "SINTRA_RPC_WAIT_GUARD_DIR", "rpc_wait_guard", {client},
        [](const std::filesystem::path&) {
            Guard_service service;
            service.assign_name(k_coordinator_name);
            sintra::barrier("rpc-wait-guard-ready", "_sintra_all_processes");
            const bool passed = check_local_reader(service);
            sintra::barrier("rpc-wait-guard-local-finished", "_sintra_all_processes");
            sintra::barrier("rpc-wait-guard-finished", "_sintra_all_processes");
            return passed ? 0 : 1;
        },
        [](const std::filesystem::path& shared) {
            return sintra::test::read_lines(shared / "client.txt") == std::vector<std::string>{"pass"} ? 0 : 1;
        });
}
