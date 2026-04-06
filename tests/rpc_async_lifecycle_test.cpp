#include <sintra/sintra.h>

#include "test_utils.h"

#include <cstdio>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

constexpr const char* k_service_name = "async lifecycle service";
constexpr const char* k_ready_barrier = "async-lifecycle-service-ready";
constexpr const char* k_finished_barrier = "async-lifecycle-test-finished";
std::filesystem::path owner_events_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "owner_events.txt";
}

std::filesystem::path client_events_path(const std::filesystem::path& shared_dir)
{
    return shared_dir / "client_events.txt";
}

std::filesystem::path done_marker_path(const std::filesystem::path& shared_dir, const std::string& tag)
{
    return shared_dir / (tag + "_done.txt");
}

void touch_done_marker(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create marker file: " + path.string());
    }
    out << "done\n";
}

template <typename Handle>
bool get_throws_runtime_error_not_cancelled(Handle& handle)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return false;
    }
    catch (const std::runtime_error&) {
        return true;
    }
    catch (...) {
        return false;
    }

    return false;
}

template <typename Handle>
bool get_throws_cancelled(Handle& handle)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return true;
    }
    catch (...) {
        return false;
    }

    return false;
}

template <typename Handle>
bool get_throws_runtime_error_message(Handle& handle, const std::string& expected_message)
{
    try {
        (void)handle.get();
    }
    catch (const sintra::rpc_cancelled&) {
        return false;
    }
    catch (const std::runtime_error& ex) {
        return ex.what() == expected_message;
    }
    catch (...) {
        return false;
    }

    return false;
}

void print_event_mismatch(
    const char* label,
    const std::vector<std::string>& expected,
    const std::vector<std::string>& actual)
{
    std::fprintf(stderr, "%s mismatch\n", label);
    std::fprintf(stderr, "expected:\n");
    for (const auto& entry : expected) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
    std::fprintf(stderr, "actual:\n");
    for (const auto& entry : actual) {
        std::fprintf(stderr, "  %s\n", entry.c_str());
    }
}

struct Lifecycle_service : sintra::Derived_transceiver<Lifecycle_service>
{
    void unicast_marker(
        std::string tag,
        std::string shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("unicast:") + tag);
        touch_done_marker(done_marker_path(shared_dir, "unicast_" + tag));
    }

    int immediate_reply(int value)
    {
        return value + 1;
    }

    int delayed_reply(
        int value,
        int delay_ms,
        std::string tag,
        std::string shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("start:") + tag);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("finish:") + tag);
        touch_done_marker(done_marker_path(shared_dir, tag));
        return value;
    }

    int throwing_reply(
        std::string message,
        std::string tag,
        std::string shared_dir_str)
    {
        const auto shared_dir = std::filesystem::path(shared_dir_str);
        sintra::test::append_line_or_throw(owner_events_path(shared_dir), std::string("throw:") + tag);
        touch_done_marker(done_marker_path(shared_dir, tag));
        throw std::runtime_error(message);
    }

    SINTRA_UNICAST(unicast_marker)
    SINTRA_RPC(immediate_reply)
    SINTRA_RPC_STRICT(delayed_reply)
    SINTRA_RPC_STRICT(throwing_reply)
};

int process_owner()
{
    Lifecycle_service service;
    service.assign_name(k_service_name);

    sintra::barrier(k_ready_barrier);
    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int process_client()
{
    using namespace std::chrono_literals;

    static_assert(!std::is_copy_constructible_v<sintra::Rpc_handle<int>>);
    static_assert(!std::is_copy_assignable_v<sintra::Rpc_handle<int>>);

    const sintra::test::Shared_directory shared("SINTRA_TEST_SHARED_DIR", "rpc_async_lifecycle");
    const auto& shared_dir = shared.path();
    const auto client_path = client_events_path(shared_dir);

    sintra::barrier(k_ready_barrier);

    {
        Lifecycle_service local_service;
        bool rejected = false;

        try {
            auto handle = Lifecycle_service::rpc_async_immediate_reply(local_service.instance_id(), 17);
            (void)handle;
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }

        if (!rejected) {
            sintra::test::append_line_or_throw(client_path, "local_async_non_strict:unexpected");
            return 1;
        }

        sintra::test::append_line_or_throw(client_path, "local_async_non_strict:rejected");
    }

    {
        Lifecycle_service local_service;
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            local_service.instance_id(),
            88,
            80,
            "local_strict",
            shared_dir.string());

        if (handle.ready()) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_ready_initial:unexpected");
            return 1;
        }

        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 2s);
        if (wait_status != sintra::Rpc_wait_status::completed) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_wait:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_wait:completed");

        if (!handle.ready()) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_ready_final:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_ready_final:ready");

        if (handle.state() != sintra::Rpc_completion_state::returned) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_state:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_state:returned");

        if (handle.get() != 88) {
            sintra::test::append_line_or_throw(client_path, "local_async_strict_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "local_async_strict_get:returned");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            303,
            1,
            "fast_complete",
            shared_dir.string());

        std::this_thread::sleep_for(40ms);

        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 100ms);
        if (wait_status != sintra::Rpc_wait_status::completed) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_wait:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_wait:completed");

        if (sintra::s_mproc->unblock_rpc() != 0) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_unblock:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_unblock:clean");

        if (handle.state() != sintra::Rpc_completion_state::returned) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_state:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_state:returned");

        if (!handle.ready()) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_ready:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_ready:ready");

        if (handle.get() != 303) {
            sintra::test::append_line_or_throw(client_path, "fast_complete_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "fast_complete_get:returned");
    }

    {
        auto handle = Lifecycle_service::rpc_async_throwing_reply(
            k_service_name,
            "remote exception observed",
            "remote_exception",
            shared_dir.string());

        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 2s);
        if (wait_status != sintra::Rpc_wait_status::completed) {
            sintra::test::append_line_or_throw(client_path, "remote_exception_wait:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "remote_exception_wait:completed");

        if (handle.state() != sintra::Rpc_completion_state::remote_exception) {
            sintra::test::append_line_or_throw(client_path, "remote_exception_state:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "remote_exception_state:remote_exception");

        if (!get_throws_runtime_error_message(handle, "remote exception observed")) {
            sintra::test::append_line_or_throw(client_path, "remote_exception_get:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "remote_exception_get:threw");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            101,
            250,
            "abandon",
            shared_dir.string());

        if (handle.ready()) {
            sintra::test::append_line_or_throw(client_path, "abandon_ready_initial:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_ready_initial:pending");

        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 40ms);
        if (wait_status != sintra::Rpc_wait_status::deadline_exceeded) {
            sintra::test::append_line_or_throw(client_path, "abandon_wait:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_wait:deadline_exceeded");

        if (!handle.abandon()) {
            sintra::test::append_line_or_throw(client_path, "abandon_call_first:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_call_first:transitioned");

        if (handle.abandon()) {
            sintra::test::append_line_or_throw(client_path, "abandon_call_second:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_call_second:stable");

        if (handle.state() != sintra::Rpc_completion_state::abandoned) {
            sintra::test::append_line_or_throw(client_path, "abandon_state_initial:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_state_initial:abandoned");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "abandon"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "abandon_late_reply:missing");
            return 1;
        }

        if (handle.state() != sintra::Rpc_completion_state::abandoned) {
            sintra::test::append_line_or_throw(client_path, "abandon_state_final:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_state_final:abandoned");

        if (!get_throws_runtime_error_not_cancelled(handle)) {
            sintra::test::append_line_or_throw(client_path, "abandon_get:returned");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "abandon_get:threw");
    }

    {
        {
            auto handle = Lifecycle_service::rpc_async_delayed_reply(
                k_service_name,
                404,
                250,
                "scope_abandon",
                shared_dir.string());

            const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 40ms);
            if (wait_status != sintra::Rpc_wait_status::deadline_exceeded) {
                sintra::test::append_line_or_throw(client_path, "scope_abandon_wait:unexpected");
                return 1;
            }
            sintra::test::append_line_or_throw(client_path, "scope_abandon_wait:deadline_exceeded");
        }

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "scope_abandon"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "scope_abandon_late_reply:missing");
            return 1;
        }

        if (sintra::s_mproc->unblock_rpc() != 0) {
            sintra::test::append_line_or_throw(client_path, "scope_abandon_cleanup:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "scope_abandon_cleanup:clean");
    }

    {
        auto handle = Lifecycle_service::rpc_async_delayed_reply(
            k_service_name,
            202,
            350,
            "cancel",
            shared_dir.string());

        size_t cancelled_count = 0;
        const auto cancel_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < cancel_deadline) {
            cancelled_count = sintra::s_mproc->unblock_rpc();
            if (cancelled_count != 0) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }

        if (cancelled_count != 1) {
            sintra::test::append_line_or_throw(client_path, "cancel_unblock:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_unblock:observed");

        const auto wait_status = handle.wait_until(std::chrono::steady_clock::now() + 200ms);
        if (wait_status != sintra::Rpc_wait_status::completed) {
            sintra::test::append_line_or_throw(client_path, "cancel_wait:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_wait:completed");

        if (handle.state() != sintra::Rpc_completion_state::cancelled) {
            sintra::test::append_line_or_throw(client_path, "cancel_state_initial:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_state_initial:cancelled");

        if (!get_throws_cancelled(handle)) {
            sintra::test::append_line_or_throw(client_path, "cancel_get:returned");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_get:threw");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "cancel"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "cancel_late_reply:missing");
            return 1;
        }

        if (handle.state() != sintra::Rpc_completion_state::cancelled) {
            sintra::test::append_line_or_throw(client_path, "cancel_state_final:unexpected");
            return 1;
        }
        sintra::test::append_line_or_throw(client_path, "cancel_state_final:cancelled");
    }

    {
        Lifecycle_service::rpc_unicast_marker(
            k_service_name,
            "remote_unicast",
            shared_dir.string());
        sintra::test::append_line_or_throw(client_path, "unicast_call:returned");

        if (!sintra::test::wait_for_file(done_marker_path(shared_dir, "unicast_remote_unicast"), 2s, 5ms)) {
            sintra::test::append_line_or_throw(client_path, "unicast_delivery:missing");
            return 1;
        }

        sintra::test::append_line_or_throw(client_path, "unicast_delivery:observed");
    }

    sintra::barrier(k_finished_barrier, "_sintra_all_processes");
    return 0;
}

int verify_results(const std::filesystem::path& shared_dir)
{
    const auto owner_events = sintra::test::read_lines(owner_events_path(shared_dir));
    const auto client_events = sintra::test::read_lines(client_events_path(shared_dir));

    const std::vector<std::string> expected_owner_events = {
        "start:local_strict",
        "finish:local_strict",
        "start:fast_complete",
        "finish:fast_complete",
        "throw:remote_exception",
        "start:abandon",
        "finish:abandon",
        "start:scope_abandon",
        "finish:scope_abandon",
        "start:cancel",
        "finish:cancel",
        "unicast:remote_unicast",
    };

    const std::vector<std::string> expected_client_events = {
        "local_async_non_strict:rejected",
        "local_async_strict_wait:completed",
        "local_async_strict_ready_final:ready",
        "local_async_strict_state:returned",
        "local_async_strict_get:returned",
        "fast_complete_wait:completed",
        "fast_complete_unblock:clean",
        "fast_complete_state:returned",
        "fast_complete_ready:ready",
        "fast_complete_get:returned",
        "remote_exception_wait:completed",
        "remote_exception_state:remote_exception",
        "remote_exception_get:threw",
        "abandon_ready_initial:pending",
        "abandon_wait:deadline_exceeded",
        "abandon_call_first:transitioned",
        "abandon_call_second:stable",
        "abandon_state_initial:abandoned",
        "abandon_state_final:abandoned",
        "abandon_get:threw",
        "scope_abandon_wait:deadline_exceeded",
        "scope_abandon_cleanup:clean",
        "cancel_unblock:observed",
        "cancel_wait:completed",
        "cancel_state_initial:cancelled",
        "cancel_get:threw",
        "cancel_state_final:cancelled",
        "unicast_call:returned",
        "unicast_delivery:observed",
    };

    if (owner_events != expected_owner_events) {
        print_event_mismatch("owner_events", expected_owner_events, owner_events);
        return 1;
    }

    if (client_events != expected_client_events) {
        print_event_mismatch("client_events", expected_client_events, client_events);
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    return sintra::test::run_multi_process_test(
        argc,
        argv,
        "SINTRA_TEST_SHARED_DIR",
        "rpc_async_lifecycle",
        {process_owner,
         process_client},
        [](const std::filesystem::path&) {
            sintra::barrier(k_finished_barrier, "_sintra_all_processes");
            return 0;
        },
        verify_results);
}
