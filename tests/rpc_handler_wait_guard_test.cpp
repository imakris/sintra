#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Test-only access to hold a real registered reply before its delivery.
#define private public
#include <sintra/sintra.h>
#undef private

#include "test_utils.h"

namespace {

constexpr int k_reply = 17;
constexpr const char* k_coordinator_name = "rpc_wait_guard_coordinator";

std::mutex s_reply_mutex;
std::condition_variable s_reply_condition;
bool s_invoked = false;
bool s_finish_invocation = false;
bool s_reply_held = false;
bool s_release_reply = false;
thread_local bool s_wait_for_earlier = false;

void release_earlier_reply()
{
    if (s_wait_for_earlier) {
        std::lock_guard<std::mutex> lock(s_reply_mutex);
        // The wait hook holds the handle mutex. The independent reply reader
        // can proceed now, but can deliver only after get() actually waits.
        s_release_reply = true;
        s_reply_condition.notify_all();
    }
}

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

    int held_echo()
    {
        std::unique_lock<std::mutex> lock(s_reply_mutex);
        s_invoked = true;
        s_reply_condition.notify_all();
        if (!s_reply_condition.wait_for(lock, std::chrono::seconds(5), [] { return s_finish_invocation; })) {
            throw std::runtime_error("Earlier invocation was not released");
        }
        return k_reply;
    }

    SINTRA_RPC_STRICT(held_echo)

    void start_earlier()
    {
        m_pending.emplace(rpc_async_held_echo(instance_id()));
    }

    SINTRA_RPC_STRICT(start_earlier)

    void prepare_earlier()
    {
        rpc_async_start_earlier(instance_id()).get_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(3));
        {
            std::unique_lock<std::mutex> lock(s_reply_mutex);
            if (!s_reply_condition.wait_for(lock, std::chrono::seconds(5), [] { return s_invoked; })) {
                throw std::runtime_error("Earlier invocation did not dispatch");
            }
        }
        {
            std::lock_guard<std::mutex> lock(sintra::s_mproc->m_return_handlers_mutex);
            for (auto& entry : sintra::s_mproc->m_active_return_handlers) {
                auto& handler = entry.second;
                if (handler.instance_id != instance_id()) {
                    continue;
                }
                auto deliver = handler.return_handler;
                handler.return_handler = [deliver](const sintra::Message_prefix& message) {
                    std::unique_lock<std::mutex> lock(s_reply_mutex);
                    s_reply_held = true;
                    s_reply_condition.notify_all();
                    if (!s_reply_condition.wait_for(lock, std::chrono::seconds(5), [] { return s_release_reply; })) {
                        std::fprintf(stderr, "Earlier reply was not released\n");
                    }
                    lock.unlock();
                    deliver(message);
                };
            }
        }
        std::unique_lock<std::mutex> lock(s_reply_mutex);
        s_finish_invocation = true;
        s_reply_condition.notify_all();
        if (!s_reply_condition.wait_for(lock, std::chrono::seconds(5), [] { return s_reply_held; })) {
            throw std::runtime_error("Earlier reply did not reach its reply reader");
        }
    }

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
            case 2: {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
                try {
                    (void)rpc_async_echo(instance_id()).get_until(deadline);
                }
                catch (const sintra::rpc_timeout&) {
                    return std::chrono::steady_clock::now() >= deadline ? k_reply : 0;
                }
                return 0;
            }
            case 3:
                return rpc_direct_echo(instance_id());
            case 4:
                return m_completed->get();
            case 8:
                s_wait_for_earlier = true;
                try {
                    const int value = m_pending->get();
                    s_wait_for_earlier = false;
                    return value;
                }
                catch (...) {
                    s_wait_for_earlier = false;
                    throw;
                }
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

    SINTRA_RPC_STRICT(exercise)

private:
    std::atomic<unsigned> m_echo_calls{0};
    std::optional<sintra::Rpc_handle<int>> m_completed;
    std::optional<sintra::Rpc_handle<int>> m_pending;
};

bool check_local_reader(Guard_service& service)
{
    service.prepare_completed();
    bool passed = true;
    for (int mode : {0, 2, 3, 4, 7, 8}) {
        try {
            if (mode == 8) {
                service.prepare_earlier();
                sintra::detail::test_hooks::s_rpc_wait_pending.store(release_earlier_reply);
            }
            const auto previous_calls = service.echo_calls();
            const int actual = Guard_service::rpc_async_exercise(service.instance_id(), mode).get_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
            sintra::detail::test_hooks::s_rpc_wait_pending.store(nullptr);
            const int expected = mode == 0 ? 1 : k_reply;
            if (actual != expected) {
                std::fprintf(stderr, "RPC wait guard mode %d returned %d, expected %d\n",
                    mode, actual, expected);
                passed = false;
            }
            if (mode == 0 || mode == 2 || mode == 7) {
                // A later request proves synchronous rejection precedes submission,
                // while an async timeout leaves its transported invocation queued.
                (void)Guard_service::rpc_async_echo(service.instance_id()).get_until(
                    std::chrono::steady_clock::now() + std::chrono::seconds(3));
                const unsigned expected_calls = previous_calls + (mode == 0 ? 1 : 2);
                if (service.echo_calls() != expected_calls) {
                    std::fprintf(stderr, "RPC wait mode %d changed request submission/execution\n", mode);
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
    sintra::detail::test_hooks::s_rpc_wait_pending.store(nullptr);
    return passed;
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
